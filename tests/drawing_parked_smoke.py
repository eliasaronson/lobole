#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a drawing request waits for its projection off the frame.

On an --offscreen instance (a frame loop, unlike --headless, where the
handlers project inline), a part carrying a helical spring -- seconds of
exact hidden-line removal a view -- and a sheet of it:

  - update_drawing is PARKED while its view projects on a `drawing` job:
    background_jobs lists the job, a second connection is answered all the
    while, and the answer carries the projected view;
  - the same call inside a `batch` projects inline and answers the same;
  - cancel_background_job {kind: "drawing"} refuses a waiting request, in
    words that say so;
  - update_drawing {async: true} answers at once with the job's id, and the
    views land on their own.

docs/drawings/README.md#requests-that-wait. Skips (77) where there is no
Vulkan device at all.

Usage: drawing_parked_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer, RemoteError  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def drawing_jobs(d):
    return [j for j in d.call("background_jobs")["jobs"]
            if j["kind"] == "drawing"]


def wait_jobs(d, limit=240):
    t0 = time.monotonic()
    while drawing_jobs(d):
        expect(time.monotonic() - t0 < limit, "the projection lands")
        time.sleep(0.05)


class Waiting:
    """`method` called on its own connection, in a thread: the request that
    waits, while the test's own connection looks on."""

    def __init__(self, port, method, **params):
        self.d = Designer.wait_for(port)
        self.answer = None
        self.error = None
        self.seconds = 0.0

        def run():
            t0 = time.monotonic()
            try:
                self.answer = self.d.call(method, **params)
            except RemoteError as e:
                self.error = str(e)
            self.seconds = time.monotonic() - t0

        self.t = threading.Thread(target=run, daemon=True)
        self.t.start()

    def join(self, limit=240):
        self.t.join(limit)
        expect(not self.t.is_alive(), "the waiting request was answered")


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_dwgparked_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    try:
        app = launch_app(binary, env=env, headless="offscreen")
    except RuntimeError as e:
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            print(f"SKIP: no usable Vulkan device here ({e})")
            return 77
        raise
    try:
        d = Designer.wait_for(app.port)
        expect(d.call("ping")["mode"] == "offscreen", "an offscreen instance")
        d.call("new_project")
        d.call("add_part", name="spring")
        helix = d.call("add_feature", feature={
            "type": "Curve3D", "name": "Helix", "kind": "helix",
            "origin": [0, 0, 0], "axis": [0, 0, 1], "radius": 11,
            "pitch": 7, "turns": 4})
        d.call("add_feature", feature={
            "type": "Pipe", "name": "Spring", "path_id": helix["id"],
            "outer_diameter": 6.4})
        box(d, (30, -5, 0), (10, 10, 10), "block")
        d.call("create_drawing", name="sheet",
               sheet={"size": "a3", "landscape": True})
        front = d.call("add_drawing_view", source="spring",
                       orientation="front", scale=2.0,
                       position=[120, 150])["id"]

        # --- parked: the frame and the other connections carry on ----------
        w = Waiting(app.port, "update_drawing")
        seen, pings, worst = False, 0, 0.0
        while w.t.is_alive():
            t0 = time.monotonic()
            seen = seen or bool(drawing_jobs(d))
            worst = max(worst, time.monotonic() - t0)
            pings += 1
            time.sleep(0.05)
        w.join()
        expect(w.error is None, f"update_drawing answered: {w.error}")
        print(f"update_drawing waited {w.seconds:.1f} s; {pings} requests "
              f"answered meanwhile, the slowest in {worst * 1000:.0f} ms")
        expect(seen, "background_jobs listed the drawing job")
        expect(pings >= 5, f"the other connection was answered ({pings})")
        expect(worst < 1.0, f"no request waited behind the projection "
               f"({worst:.2f} s)")
        view = next(v for v in w.answer["views"] if v["id"] == front)
        expect(view["has_geometry"] and view["curves"] > 0,
               f"the answer carries the projected view: {view}")
        cost = next(v for v in d.call("list_drawing_views")["views"]
                    if v["id"] == front)["cost"]
        expect(cost["self_calls"] > 0, f"it projected: {cost}")

        # --- in a batch: inline, the same answer ---------------------------
        top = d.call("add_drawing_view", source="spring", orientation="top",
                     scale=2.0, position=[300, 150])["id"]
        batch = d.call("batch", calls=[
            {"method": "update_drawing", "params": {}}])
        expect(not drawing_jobs(d), "a batch projects inline, no job")
        answered = batch["results"][0]["result"]
        views = {v["id"]: v for v in answered["views"]}
        expect(views[top]["has_geometry"] and views[front]["curves"] ==
               view["curves"], f"the batch's answer: {answered}")

        # --- cancelled: the waiting request is refused ---------------------
        right = d.call("add_drawing_view", source="spring",
                       orientation="right", scale=2.0,
                       position=[120, 60])["id"]
        w = Waiting(app.port, "update_drawing")
        t0 = time.monotonic()
        while not drawing_jobs(d):
            expect(time.monotonic() - t0 < 30, "the right view projects")
            time.sleep(0.02)
        d.call("cancel_background_job", kind="drawing")
        w.join()
        expect(w.error is not None and "cancel" in w.error,
               f"the waiting request is refused, saying why: {w.error}")
        views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
        expect("projection cancelled" in views[right].get("error", ""),
               f"the view says it was cancelled: {views[right]}")

        # --- async: answered at once, landing on its own -------------------
        t0 = time.monotonic()
        a = d.call("update_drawing", **{"async": True})
        expect(time.monotonic() - t0 < 2.0, "async answers at once")
        expect(a.get("job") == "drawing" and "job_id" in a,
               f"async names its job: {a}")
        expect(any(v.get("projecting") for v in a["views"]),
               f"the view still to project is marked: {a}")
        wait_jobs(d)
        views = {v["id"]: v for v in d.call("list_drawing_views")["views"]}
        expect(all(v["has_geometry"] for v in views.values()),
               f"every view landed: {views}")
        expect(not views[right].get("error"), f"right landed: {views[right]}")
        print("drawing_parked_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
