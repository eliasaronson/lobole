#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A heavy feature builds off the frame, and its cancel follows the rule.

docs/ui/long-operations.md#heavy-features. A Delete Face of many faces (the
fan grill's 916: 14 s of Defeaturing) held the main thread for all of it,
twice for an add_feature. Now, over the RPC:

- add_feature of one answers when its `feature` job lands, with the regen
  and digest of the part as built, while the instance answers other calls
  and get_state says it is `building`;
- cancelled from another connection, the call is refused and rolled back:
  the features and the saved bytes are as before, no undo step;
- a reopen in the same session reuses the build (no job);
- an open that rebuilds it, cancelled, answers with the feature not built
  (`regen.unbuilt`, state `cancelled`), and build_feature builds it.

LOBOLE_TEST_HEAVY_FORCE sends every Delete Face to a job (a bore is two
faces, far under the estimate) and LOBOLE_TEST_HEAVY_DELAY_MS holds the
worker first, so "while it builds" is a state the test reaches.

Usage: heavy_feature_smoke.py <path-to-designer-binary>
"""
import os
import shutil
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "python"))
from smoke_app import launch  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402
from lobole.client import Client  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def env(delay_ms):
    e = dict(os.environ)
    e["LOBOLE_TEST_HEAVY_FORCE"] = "1"
    e["LOBOLE_TEST_HEAVY_DELAY_MS"] = str(delay_ms)
    return e


def feature_jobs(c):
    return [j for j in c.call("background_jobs")["jobs"]
            if j["kind"] == "feature"]


def wait_for(cond, what, timeout=60.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        got = cond()
        if got:
            return got
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {what}")


class Call:
    """One RPC on its own thread, so the test can watch while it waits."""

    def __init__(self, client, method, params):
        self.result = None
        self.error = None

        def go():
            try:
                self.result = client.request(method, params, timeout=600)
            except Exception as e:  # noqa: BLE001 -- the message is the point
                self.error = str(e)

        self.thread = threading.Thread(target=go)
        self.thread.start()

    def join(self):
        self.thread.join(timeout=600)
        expect(not self.thread.is_alive(), "the call never answered")
        return self


def drilled(d):
    """A 40x30x20 block with a D10 through bore; the body id and the bore."""
    blk = box(d, [0, 0, 0], [40, 30, 20], name="Block")["id"]
    cylinder(d, [20, 15, -1], "z", 5.0, 22.0, name="Bore", mode="cut")
    m = d.call("query_geometry", element_type="face", on_point=[25, 15, 10],
               max_distance=0.05, max_results=5)["matches"]
    expect(m, "no bore face found")
    return blk, {"feature_id": blk, "element": m[0]["element"]}


def delete_face(blk, bore):
    return {"type": "DeleteFace", "name": "Fill", "body_id": blk,
            "faces": [bore]}


def volume(d):
    return sum(b["volume_mm3"] for b in d.call("get_state")["bodies"])


def main():
    binary = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="lobole-heavy-") as tmp:
        project = os.path.join(tmp, "plate.lob")

        # It answers when the build lands, with the part as built; meanwhile
        # the instance answers, and says what is building.
        with launch(binary, env=env(2500)) as app:
            d = app.client
            other = Client.wait_for(app.port)
            blk, bore = drilled(d)
            v0 = volume(d)
            call = Call(d, "add_feature", {"feature": delete_face(blk, bore)})
            seen = wait_for(lambda: feature_jobs(other), "the feature job")
            expect(seen[0]["cancellable"], f"not cancellable: {seen[0]}")
            expect("Fill" in seen[0]["label"], f"label: {seen[0]}")
            st = other.call("get_state")
            fill = [f for f in st["features"] if f.get("name") == "Fill"]
            expect(fill and fill[0].get("build") == "building",
                   f"get_state while it builds: {fill}")
            expect(any(u["state"] == "building"
                       for u in st["regen"].get("unbuilt", [])),
                   f"regen while it builds: {st['regen']}")
            other.call("ping")
            call.join()
            expect(call.error is None, f"add_feature failed: {call.error}")
            r = call.result
            expect(r["regen"]["ok"] and "unbuilt" not in r["regen"],
                   f"the reply's regen is not the part as built: {r['regen']}")
            filled = r["digest"]["delta"]["volume_mm3"]
            expect(abs(filled - 3.14159265 * 25 * 20) < 1e-3,
                   f"the digest does not say the bore filled: {r['digest']}")
            expect(abs(volume(d) - (v0 + filled)) < 1e-6, "volume after")
            d.call("save_project", path=project)
            # Reopened in the same session: the build is reused, no job.
            for root, dirs, _ in os.walk(project):
                for name in dirs:
                    if name == "geometry":
                        shutil.rmtree(os.path.join(root, name))
            t = time.monotonic()
            o = d.call("open_project", path=project)
            expect(time.monotonic() - t < 2.0, "the reopen built it again")
            expect(not feature_jobs(other), "the reopen started a job")
            st = d.call("get_state")
            expect(st["regen"]["ok"], f"reopened: {st['regen']} {o}")
            other.close()

        # A cancel of the edit's build: refused, rolled back, no undo step.
        with launch(binary, env=env(60000)) as app:
            d = app.client
            other = Client.wait_for(app.port)
            blk, bore = drilled(d)
            before = d.call("get_state")
            call = Call(d, "add_feature", {"feature": delete_face(blk, bore)})
            wait_for(lambda: feature_jobs(other), "the feature job")
            other.call("cancel_background_job", kind="feature")
            call.join()
            expect(call.error is not None and "cancelled" in call.error and
                   "rolled back" in call.error,
                   f"the cancelled edit answered {call.result} {call.error}")
            after = d.call("get_state")
            expect(after["features"] == before["features"],
                   "the features are not as before the edit")
            expect(after["can_undo"] == before["can_undo"] and
                   not after["can_redo"], "an undo or redo step was left")
            expect(abs(volume(d) - sum(b["volume_mm3"]
                                       for b in before["bodies"])) < 1e-9,
                   "the bodies are not as before")
            wait_for(lambda: not feature_jobs(other), "the job to end")
            other.close()

        # An open that rebuilds it, cancelled: the feature is not built, and
        # says so in the open's own reply; build_feature builds it.
        with launch(binary, env=env(4000)) as app:
            d = app.client
            other = Client.wait_for(app.port)
            call = Call(d, "open_project", {"path": project})
            wait_for(lambda: feature_jobs(other), "the rebuild's job")
            other.call("cancel_background_job", kind="feature")
            call.join()
            expect(call.error is None, f"open_project failed: {call.error}")
            regen = d.call("get_state")["regen"]
            states = [u["state"] for u in regen.get("unbuilt", [])]
            expect(states == ["cancelled"] and not regen["ok"],
                   f"after the cancelled rebuild: {regen}")
            fid = [f for f in d.call("get_state")["features"]
                   if f.get("build") == "cancelled"][0]["id"]
            r = d.call("build_feature", feature_id=fid)
            expect(r["regen"]["ok"] and "unbuilt" not in r["regen"],
                   f"build_feature: {r}")
            try:
                d.call("build_feature", feature_id=fid)
                raise AssertionError("build_feature of a built feature")
            except Exception as e:  # noqa: BLE001
                expect("is built" in str(e), f"refusal: {e}")
            other.close()
    print("heavy_feature_smoke: OK")


if __name__ == "__main__":
    main()
