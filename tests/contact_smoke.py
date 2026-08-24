#!/usr/bin/env python3
"""Integration test: V1 contact & connectivity over RPC, driven headless.

Covers: interference's distance/state fields on part and assembly tabs, and
contact_report — components (a floating occurrence is a second component),
near-miss detection, clash listing, and the touch/near tolerances. This is
the RPC-level gate for the class of defect the dock dogfooding round hit:
checks that passed BECAUSE nothing touched.

Usage: contact_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_contact_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        d.call("new_project")

        # --- Part tab: two bodies, touching then apart ---------------------
        d.call("add_feature", feature={
            "type": "Box", "name": "A", "position": [0, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        d.call("add_feature", feature={
            "type": "Box", "name": "B", "position": [10, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        r = d.call("interference", a=0, b=1)
        expect(r["state"] == "touching", f"tangent boxes: {r}")
        expect(abs(r["distance"]) < 1e-6, f"tangent distance: {r}")
        expect(not r["clash"], f"tangent clash: {r}")

        d.call("add_feature", feature={
            "type": "Box", "name": "C", "position": [25, 0, 0],
            "width": 10, "height": 10, "depth": 10})
        r = d.call("interference", a=1, b=2)
        expect(r["state"] == "clear", f"apart boxes: {r}")
        expect(abs(r["distance"] - 5.0) < 1e-6, f"apart distance: {r}")

        # Round 7: BODY-level connectivity on a part tab — the class the
        # occurrence sweep cannot see (a bracket floating inside one part).
        rep = d.call("contact_report")
        expect(rep["scope"] == "bodies", f"part scope: {rep}")
        expect(not rep["connected"], "C floats 5 mm off: two components")
        sizes = sorted(len(c) for c in rep["components"])
        expect(sizes == [1, 2], f"components: {rep['components']}")

        # ...and check_geometry warns about it BY DEFAULT (round-8 retro:
        # the floating-plate class must be opt-out, not opt-in).
        cg = d.call("check_geometry")
        expect(cg["ok"], "geometry itself is valid")
        expect(not cg["connected"], f"floating body flagged: {cg}")
        expect(any("touch nothing" in w for w in cg.get("warnings", [])),
               f"warning names the island: {cg.get('warnings')}")

        # --- Assembly: bolted / floating / near-miss / clash ---------------
        part = d.call("list_parts")["parts"][0]["name"]
        d.call("add_assembly", name="Rig")
        d.call("set_active_part", name="Rig")

        def place(name, x):
            return d.call("add_occurrence", part=part, name=name, fixed=True,
                          transform={"position": [x, 0, 0],
                                     "orientation": [0, 0, 0, 1]})
        # The part spans x 0..35 (three boxes). Chain: P1 at 0, P2 at 35
        # (tangent), P3 at 70.5 (0.5 near-miss), P4 at 200 (floating).
        place("P1", 0)
        place("P2", 35)
        place("P3", 70.5)
        p4_id = place("P4", 200)["id"]

        r = d.call("interference", a="P1", b="P2")
        expect(r["state"] == "touching", f"P1/P2: {r}")
        r = d.call("interference", a="P1", b="P4")
        expect(r["state"] == "clear" and r["distance"] > 100, f"P1/P4: {r}")

        rep = d.call("contact_report")
        expect(rep["occurrences"] == 4, f"count: {rep['occurrences']}")
        expect(not rep["connected"], "P3/P4 float, must not be connected")
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P1", "P2"] in comps, f"components: {comps}")
        expect(["P3"] in comps and ["P4"] in comps, f"components: {comps}")
        near = [(m["a"], m["b"]) for m in rep["near_misses"]]
        expect(("P2", "P3") in near or ("P3", "P2") in near,
               f"near_misses: {rep['near_misses']}")
        expect(rep["clashes"] == [], f"clashes: {rep['clashes']}")
        expect("unmated_contacts" not in rep,
               "mate diff must be absent without mates")

        # Overlap P4 onto P3's tail: one clash, and the graph unites them.
        d.call("set_occurrence_transform", id=p4_id,
               transform={"position": [100, 0, 0],
                          "orientation": [0, 0, 0, 1]})
        rep = d.call("contact_report")
        expect(len(rep["clashes"]) == 1, f"clashes: {rep['clashes']}")
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P3", "P4"] in comps, f"clash unites: {comps}")

        # A generous near_tol turns the 0.5 gap into a connection? No —
        # near_tol only widens REPORTING; connectivity stays touch_tol.
        rep = d.call("contact_report", near_tol=50.0)
        expect(not rep["connected"], "near_tol must not fake connectivity")
        # ...but touch_tol does: 1.0 swallows the 0.5 near-miss gap.
        rep = d.call("contact_report", touch_tol=1.0)
        comps = sorted(sorted(c) for c in rep["components"])
        expect(["P1", "P2", "P3", "P4"] in comps,
               f"touch_tol widens contact: {comps}")

        print("contact_smoke: all checks passed")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
