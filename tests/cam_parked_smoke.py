#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""CAM requests wait OFF the frame (docs/cam/README.md#requests-that-wait).

cam_toolpath, cam_post, cam_verify and cam_simulate used to cut every pass
they read on the main thread -- 60 to 330 s with the app stopped on a real
job. They PARK now: the passes cut on the "cam" worker, cam_verify's
simulation on a "cam-verify" one, and the request is answered when they
land. This drives a job heavy enough to take seconds and proves, headless:

- a parked cam_verify is answered with EXACTLY what the synchronous handler
  says (a `batch` still runs it inline, which is the comparison);
- a second connection's ping and get_state are answered while it waits, and
  `background_jobs` shows the work it waits on;
- one connection's own requests keep their order: a ping pipelined behind a
  parked cam_toolpath is answered after it, not before;
- an edit from another connection while a request waits is what the answer
  describes (the job as it stands when it answers);
- cancelling the passes answers the waiting request with a refusal that
  says so, rather than leaving it hanging or re-queueing them;
- refusals are the handler's own words, parked or not;
- the frame watchdog never reports a stall.
"""
import json
import os
import select
import socket
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


class Raw:
    """A bare connection that can SEND without waiting for the answer --
    what a pipelining client does, and what Client.call cannot."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=300)
        self.file = self.sock.makefile("rw")
        self.next_id = 1000

    def send(self, method, **params):
        self.next_id += 1
        self.file.write(json.dumps({"id": self.next_id, "method": method,
                                    "params": params}) + "\n")
        self.file.flush()
        return self.next_id

    def read(self):
        line = self.file.readline()
        if not line:
            raise ConnectionError("designer disconnected")
        return json.loads(line)

    def ready(self):
        """True when an answer has arrived. Only asked while nothing has
        been read since the send, so the file's buffer is empty and the
        socket is the whole story."""
        r, _, _ = select.select([self.sock], [], [], 0)
        return bool(r)

    def close(self):
        self.file.close()
        self.sock.close()


def build_job(d, stepover, tool_mm=4.0, stepdown=3.0, ops=3):
    """A plate with a rectangular pocket, cleared adaptively in layers by
    `ops` operations that differ only in stepover (so none is a cache hit
    on another): seconds of passes, and a fine verify on top."""
    d.call("new_project")
    box(d, (-40, -30, 0), (80, 60, 20), "Plate")
    sk = d.call("add_feature", feature={
        "type": "Sketch", "name": "Pocket sketch",
        "plane": {"origin": [0, 0, 20], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": [-32, -22], "c2": [32, 22]}]})
    d.call("add_feature", feature={
        "type": "Extrude", "name": "Pocket", "sketch_id": sk["id"],
        "distance": 12.0, "mode": "cut", "reversed": True})
    floor = None
    for body in d.call("list_elements")["bodies"]:
        for f in body["faces"]:
            n = f.get("normal") or [0, 0, 0]
            c = f.get("centroid") or [0, 0, 0]
            if f.get("kind") == "plane" and n[2] > 0.99 and abs(c[2] - 8.0) < 0.5:
                floor = {"feature_id": body["created_by"], "element": f["name"]}
    expect(floor is not None, "the pocket has a floor to target")
    d.call("machine_part")
    setup = d.call("add_cam_setup", name="Heavy")["id"]
    d.call("set_cam_setup", id=setup,
           tool={"number": 2, "diameter": tool_mm, "name": "small"})
    made = []
    for i in range(ops):
        made.append(d.call("add_cam_op", setup=setup, type="adaptive", tool=2,
                           target=floor, stepover=stepover + 0.01 * i,
                           stepdown=stepdown)["id"])
    return setup, made


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_camparked_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port, timeout=60)
        d.sock.settimeout(300)
        if not d.call("ping").get("features", {}).get("cam", True):
            print("cam_parked_smoke: built without CAM, nothing to test")
            return 0
        if not d.call("ping").get("features", {}).get("cam_adaptive", True):
            print("cam_parked_smoke: built without adaptive clearing, skipped")
            return 0
        setup, ops = build_job(d, stepover=0.5, tool_mm=6.0, stepdown=12.0,
                               ops=2)
        part = [p["name"] for p in d.call("list_parts")["parts"]
                if p.get("type") == "part"][0]

        # --- A parked cam_verify, with another client answered meanwhile.
        a = Raw(app.port)
        t0 = time.time()
        vid = a.send("cam_verify", setup=setup, resolution=0.1)
        pings = 0
        kinds = set()
        answer = None
        while answer is None:
            if a.ready():
                answer = a.read()
                break
            t = time.time()
            d.call("ping")
            d.call("get_state", part=part)
            pings += 1
            for j in d.call("background_jobs")["jobs"]:
                kinds.add(j["kind"])
            lag = time.time() - t
            time.sleep(0.05)
            expect(lag < 2.0, f"a ping took {lag:.2f} s while cam_verify "
                   "waited -- the frame is held")
        took = time.time() - t0
        expect(answer.get("id") == vid, f"the answer is cam_verify's: {answer}")
        expect("result" in answer, f"cam_verify answered: {answer}")
        print(f"cam_parked_smoke: cam_verify waited {took:.1f} s, "
              f"{pings} pings answered meanwhile, jobs seen {sorted(kinds)}")
        expect(pings >= 3, f"only {pings} pings were answered during a "
               f"{took:.1f} s cam_verify -- is the job heavy enough, or is "
               "the queue held?")
        expect("cam" in kinds, f"the passes ran on the cam worker: {kinds}")
        expect("cam-verify" in kinds,
               f"the simulation ran on a cam-verify job: {kinds}")
        parked = answer["result"]

        # --- Exactly what the synchronous handler says. A batch runs its
        # calls inline, so it is the synchronous path, now all from cache.
        sync = d.call("batch", calls=[{"method": "cam_verify", "params": {
            "setup": setup, "resolution": 0.1}}])
        sync = sync["results"][0] if "results" in sync else sync
        sync = sync.get("result", sync)
        expect(json.dumps(parked, sort_keys=True) ==
               json.dumps(sync, sort_keys=True),
               "the parked cam_verify differs from the synchronous one:\n"
               f"parked {json.dumps(parked, sort_keys=True)[:2000]}\n"
               f"sync   {json.dumps(sync, sort_keys=True)[:2000]}")

        # --- One connection's order is kept: a ping pipelined behind a
        # parked cam_toolpath is answered after it.
        d.call("set_cam_op", id=ops[0], stepover=0.45)
        tid = a.send("cam_toolpath", setup=setup)
        pid = a.send("ping")
        first, second = a.read(), a.read()
        expect(first.get("id") == tid and second.get("id") == pid,
               f"answers out of order on one connection: {first.get('id')}, "
               f"{second.get('id')} (sent {tid}, {pid})")
        expect("result" in first, f"cam_toolpath answered: {first}")

        # --- An edit from another client while one waits: the answer is the
        # job as it stands when it answers.
        d.call("set_cam_op", id=ops[1], stepover=0.47)
        tid = a.send("cam_toolpath", setup=setup)
        d.call("set_cam_op", id=ops[0], stepover=0.43)
        got = a.read()
        expect(got.get("id") == tid and "result" in got,
               f"cam_toolpath answered: {got}")
        again = d.call("cam_toolpath", setup=setup)
        moves = [o["moves"] for o in got["result"]["operations"]]
        expect(moves == [o["moves"] for o in again["operations"]],
               f"the parked answer is not the job as edited: {moves} vs "
               f"{[o['moves'] for o in again['operations']]}")

        # --- Cancel: the waiting request is REFUSED, saying why.
        d.call("set_cam_op", id=ops[0], stepover=0.41)
        d.call("set_cam_op", id=ops[1], stepover=0.42)
        tid = a.send("cam_post", setup=setup,
                     path=os.path.join(tmp, "heavy.nc"))
        deadline = time.time() + 30
        while "cam" not in {j["kind"]
                            for j in d.call("background_jobs")["jobs"]}:
            expect(time.time() < deadline, "the passes never started")
            time.sleep(0.02)
        d.call("cancel_background_job", kind="cam")
        got = a.read()
        expect(got.get("id") == tid, f"cam_post answered: {got}")
        expect("cancelled" in got.get("error", ""),
               f"a cancelled wait says so: {got}")
        expect(not os.path.exists(os.path.join(tmp, "heavy.nc")),
               "a cancelled cam_post wrote no file")

        # --- Refusals are the handler's, word for word.
        for kw, needle in (({"setup": 999999}, "no CAM setup"),
                           ({}, "setup")):
            rid = a.send("cam_verify", **kw)
            got = a.read()
            expect(got.get("id") == rid and needle in got.get("error", ""),
                   f"cam_verify {kw} refused naming '{needle}': {got}")
        rid = a.send("cam_verify", setup=setup, resolution=0.5, bogus=1)
        got = a.read()
        expect("bogus" in got.get("error", ""),
               f"an unknown key is refused on a parked request: {got}")

        a.close()
        # The one call that is SUPPOSED to hold the frame is the batch: it
        # runs cam_verify inline, which is the comparison above.
        stalls = [l for l in app.log().splitlines()
                  if "stalled" in l and "RPC batch" not in l]
        expect(not stalls, "the watchdog reported a stall:\n" +
               "\n".join(stalls))
        print("cam_parked_smoke: OK")
        return 0
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
