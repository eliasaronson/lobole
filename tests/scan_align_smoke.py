#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a scan laid onto a body through the pipeline
(docs/interchange/README.md#laying-a-scan-onto-a-body), driven headless.

A B-Rep box is modelled; a scan of it -- written here as a noisy STL, turned
and moved off where the box stands -- is imported as a mesh body and laid
onto the box by `align_import {mode: best_fit}`, which runs as an `align`
job and answers when it lands, as one undo step. The box is its own turn
four ways, so the reply's `note` says another attitude fits as well, and
whichever it chose, the scan's bounding box is the box's. A datum is then
fitted on the laid scan's top, and the placement and the datum read the
same after save and reopen. The laid scan is compared with the box
(`compare_scan`, docs/inspect/README.md#how-far-a-body-is-from-a-scan, a
`deviation` job): nearly all of it within its noise, no feature past the
tolerance, and the same numbers after save and reopen; its map is shown
and cleared (`set_scan_deviation`). `cancel_align`: a cancelled fit answers
with a refusal and leaves the import where it stood; a cancelled compare
answers with a refusal too.

Usage: scan_align_smoke.py <path-to-designer-binary>
"""
import math
import os
import random
import struct
import sys
import tempfile
import threading
import time

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402

SIZE = (40.0, 30.0, 20.0)
NOISE = 0.03


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def scan_of_box(path, turn_deg, move_mm, steps=24):
    """The box's six faces as a grid of `steps` squares a side, each vertex
    moved along its face's normal by a noise of at most NOISE, then turned
    about X, Y and Z by `turn_deg` and moved by `move_mm`: a binary STL."""
    rng = random.Random(7)
    noise = {}

    def jitter(key):
        if key not in noise:
            noise[key] = rng.uniform(-NOISE, NOISE)
        return noise[key]

    ax, ay, az = (math.radians(a) for a in turn_deg)

    def place(p):
        x, y, z = p
        y, z = y * math.cos(ax) - z * math.sin(ax), y * math.sin(ax) + z * math.cos(ax)
        x, z = x * math.cos(ay) + z * math.sin(ay), -x * math.sin(ay) + z * math.cos(ay)
        x, y = x * math.cos(az) - y * math.sin(az), x * math.sin(az) + y * math.cos(az)
        return (x + move_mm[0], y + move_mm[1], z + move_mm[2])

    tris = []
    for axis in range(3):
        u, v = (axis + 1) % 3, (axis + 2) % 3
        for side in (0, 1):
            def vertex(i, j):
                p = [0.0, 0.0, 0.0]
                p[axis] = side * SIZE[axis]
                p[u] = SIZE[u] * i / steps
                p[v] = SIZE[v] * j / steps
                # Edges and corners are shared by faces: noise along this
                # face's normal only where the vertex is inside it.
                if 0 < i < steps and 0 < j < steps:
                    p[axis] += (1 if side else -1) * jitter((axis, side, i, j))
                return place(p)
            for i in range(steps):
                for j in range(steps):
                    a, b = vertex(i, j), vertex(i + 1, j)
                    c, d = vertex(i + 1, j + 1), vertex(i, j + 1)
                    # u x v is +axis: outward on the + side.
                    tris += [(a, b, c), (a, c, d)] if side else \
                        [(a, c, b), (a, d, c)]
    with open(path, "wb") as f:
        f.write(b"\0" * 80)
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<3f", 0, 0, 0))
            for p in t:
                f.write(struct.pack("<3f", *p))
            f.write(b"\0\0")


def scan_body(d, import_id):
    for b in d.call("get_state")["bodies"]:
        if b.get("created_by") == import_id:
            return b
    raise AssertionError(f"the import {import_id} made no body")


def import_scan(d, path):
    got = d.call("import_step", path=path, representation="mesh",
                 up_axis="none", keep_origin=True)
    expect(not got.get("regen", {}).get("errors"), f"import failed: {got}")
    return d.call("get_state")["features"][-1]["id"]


def cancel_align(d, port, import_id, box_id):
    """A best fit is an `align` job with progress and a cancel; cancelled,
    the waiting call is refused and the import stays where it stood."""
    before = d.call("get_state")["features"]
    other = Client.wait_for(port)
    out = {}

    def go():
        try:
            out["answer"] = d.call("align_import", feature_id=import_id,
                                   mode="best_fit", onto=box_id)
        except RuntimeError as e:
            out["error"] = str(e)

    worker = threading.Thread(target=go)
    worker.start()
    job, deadline = None, time.time() + 30
    while job is None:
        job = next((j for j in other.call("background_jobs")["jobs"]
                    if j["kind"] == "align"), None)
        if job is None:
            expect(worker.is_alive() and time.time() < deadline,
                   f"no align job while the call waited: {out}")
            time.sleep(0.02)
    expect(job["cancellable"] is True, f"a best fit can be cancelled: {job}")
    expect("Best fit" in job["label"], f"the job says what it does: {job}")
    other.call("cancel_background_job", kind="align")
    worker.join(60)
    expect(not worker.is_alive(), "the cancelled best fit never answered")
    expect("cancelled" in out.get("error", ""),
           f"a cancelled best fit is refused, saying so: {out}")
    expect(d.call("get_state")["features"] == before,
           "a cancelled best fit changed the document")
    deadline = time.time() + 5
    while any(j["kind"] == "align"
              for j in other.call("background_jobs")["jobs"]):
        expect(time.time() < deadline,
               "the align job is still listed 5 s after it answered")
        time.sleep(0.05)
    other.close()


def cancel_compare(d, port, import_id, box_id):
    """A compare is a `deviation` job with a cancel; cancelled, the waiting
    call is refused, saying so."""
    other = Client.wait_for(port)
    out = {}

    def go():
        try:
            out["answer"] = d.call("compare_scan", scan=import_id, body=box_id)
        except RuntimeError as e:
            out["error"] = str(e)

    worker = threading.Thread(target=go)
    worker.start()
    job, deadline = None, time.time() + 30
    while job is None:
        job = next((j for j in other.call("background_jobs")["jobs"]
                    if j["kind"] == "deviation"), None)
        if job is None:
            expect(worker.is_alive() and time.time() < deadline,
                   f"no deviation job while the call waited: {out}")
            time.sleep(0.02)
    expect(job["cancellable"] is True, f"a compare can be cancelled: {job}")
    other.call("cancel_background_job", kind="deviation")
    worker.join(60)
    expect(not worker.is_alive(), "the cancelled compare never answered")
    expect("cancelled" in out.get("error", ""),
           f"a cancelled compare is refused, saying so: {out}")
    other.close()


def compared(d, import_id, box_id):
    r = d.call("compare_scan", scan=import_id, body=box_id)
    print(f"compare: { {k: v for k, v in r.items() if k != 'hotspots'} }")
    expect(r["within"] > 0.95, f"the laid scan is within its noise: {r}")
    expect(r["tolerance_from"] in ("scan_noise", "chord"), r)
    expect(r["floor_mm"] > 0, f"a B-Rep body states its chord: {r}")
    expect(r["no_scan"] < 0.05, f"the scan covers the box: {r}")
    expect(r["feature_count"] == 0,
           f"nothing of the box is past the tolerance: {r['hotspots'][:3]}")
    expect(r["receipt"]["standing"] == "verified", r["receipt"])
    return {k: r[k] for k in ("within", "mean_mm", "rms_mm", "p95_mm",
                              "min_mm", "max_mm", "no_scan", "tolerance_mm",
                              "hotspot_count")}


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="lobole_scan_align_")
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    # Holds each fit before it computes (tests only), so the smoke sees the
    # job and cancels it while it runs.
    env["LOBOLE_TEST_ALIGN_MS"] = "1500"
    env["LOBOLE_TEST_DEVIATION_MS"] = "1000"
    stl = os.path.join(tmp, "scan.stl")
    scan_of_box(stl, (23.0, -38.0, 61.0), (31.0, -12.0, 17.0))
    app = launch_app(binary, env=env)
    try:
        d = Client.wait_for(app.port)
        d.call("new_project")
        box(d, (0, 0, 0), SIZE, "Box")
        box_id = d.call("get_state")["features"][-1]["id"]
        import_id = import_scan(d, stl)

        # Cancelled first: the scan is still where it was read.
        cancel_align(d, app.port, import_id, box_id)

        depth = d.call("get_state").get("undo_depth")
        r = d.call("align_import", feature_id=import_id, mode="best_fit",
                   onto=box_id)
        fit = r["fit"]
        print(f"best fit: {fit}")
        expect(r["scale"] == 1.0, f"a best fit never scales: {r}")
        expect(fit["mean_mm"] < 2 * NOISE,
               f"the scan lies on the box within its noise: {fit}")
        expect(fit["within"] > 0.95, f"nearly every sample within: {fit}")
        expect(fit["winner"] in ("principal", "placement"), fit)
        expect("note" in fit and fit.get("runner_up", {}).get("turn_deg", 0)
               > 170, f"a box is its own turn: the reply says so: {fit}")
        expect(fit.get("chord_mm", 0) > 0,
               f"a B-Rep body is laid onto by its tessellation: {fit}")
        lo = scan_body(d, import_id)["bbox_min"]
        hi = scan_body(d, import_id)["bbox_max"]
        for i in range(3):
            expect(abs(lo[i]) < 0.1 and abs(hi[i] - SIZE[i]) < 0.1,
                   f"whichever turn it chose, the scan stands on the box: "
                   f"{lo} {hi}")
        state = d.call("get_state")
        if depth is not None:
            expect(state.get("undo_depth") == depth + 1,
                   f"one undo step: {depth} -> {state.get('undo_depth')}")

        # A datum on the laid scan's top, then save and reopen.
        top = [SIZE[0] * 0.4, SIZE[1] * 0.6, SIZE[2]]
        got = d.call("add_feature", feature={
            "type": "DatumPlane", "name": "Top", "kind": "from_fit",
            "fit": {"body_id": import_id, "point_mm": top, "kind": "plane"}})
        expect(not got.get("regen", {}).get("errors"), f"datum: {got}")

        def read():
            s = d.call("get_state")
            imp = next(f for f in s["features"] if f["id"] == import_id)
            dat = next(f for f in s["features"] if f.get("name") == "Top")
            return imp, dat

        imp, dat = read()
        print(f"datum: {dat}")

        # How far the box is from the laid scan, and its map.
        cancel_compare(d, app.port, import_id, box_id)
        numbers = compared(d, import_id, box_id)
        shown = d.call("set_scan_deviation", scan=import_id, body=box_id)
        expect(shown.get("shown") is True, f"the map is shown: {shown}")
        expect(d.call("set_scan_deviation", clear=True)["cleared"] is True,
               "clear removes the shown map")
        expect(d.call("set_scan_deviation", clear=True)["cleared"] is False,
               "a second clear has nothing to remove")
        expect("placement" in imp, f"get_state shows the placement: {imp}")
        proj = os.path.join(tmp, "laid.lob")
        d.call("save_project", path=proj)
        d.call("new_project")
        d.call("open_project", path=proj)
        imp2, dat2 = read()
        expect(imp2.get("placement") == imp.get("placement"),
               f"the placement reads back: {imp.get('placement')} vs "
               f"{imp2.get('placement')}")
        expect(dat2 == dat, f"the datum reads back: {dat} vs {dat2}")
        again = compared(d, import_id, box_id)
        expect(again == numbers,
               f"the compare reads the same after reopen: {numbers} vs "
               f"{again}")
        print("scan_align_smoke: OK")
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    main()
