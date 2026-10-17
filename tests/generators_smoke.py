#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The generators build what they say, headless.

generators/gear.py is the answer to "would we need scripted features for a
gear" (docs/roadmap.md#not-planned): a Python generator against the
API, an ordinary part out. This runs it against a headless app and checks
the part by numbers — no display, no GPU.

generators/smart_speaker builds its CARCASS here — the panels, coverings,
braces, port and hardware, and none of the bought STEP, which lives in
scratch/ and is not tracked because the projects are regenerable. What that
case is really for is the joinery: the panels used to drill their own dowels
independently and the holes did not meet (side walls at +/-126, the top wall
at +/-110), so nothing could be mated to anything. It asserts they line up.

And a generator's `--fresh` (generators/_lib.py fresh_project): an untitled
scratch project is discarded, saying so; unsaved work in a project with a
path is refused.

Usage: generators_smoke.py <path-to-designer-binary>
"""
import math
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from smoke_app import launch_app  # noqa: E402


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def fresh(port, state):
    """A generator's `--fresh` (generators/_lib.py fresh_project) discards
    an untitled scratch project, saying so, and refuses unsaved work in a
    project that has a path."""
    from lobole.client import Client
    sys.path.insert(0, os.path.join(ROOT, "generators"))
    from _lib import fresh_project
    from smoke_shapes import box
    import contextlib
    import io
    d = Client.wait_for(port)
    try:
        d.call("new_project")
        box(d, (0, 0, 0), (10, 10, 10), "scratch")
        ping = d.call("ping")
        expect(ping["dirty"] and not ping["project"],
               f"an untitled project with work in it: {ping}")
        said = io.StringIO()
        with contextlib.redirect_stdout(said):
            fresh_project(d)
        expect("discarded" in said.getvalue(),
               f"--fresh says it discarded the scratch project: "
               f"{said.getvalue()!r}")
        expect(not d.call("ping")["dirty"], "--fresh left a new project")

        path = os.path.join(state, "kept.dsgn")
        box(d, (0, 0, 0), (10, 10, 10), "kept")
        d.call("save_project", path=path)
        box(d, (20, 0, 0), (10, 10, 10), "unsaved")
        try:
            fresh_project(d)
        except SystemExit as e:
            expect("unsaved" in str(e), f"the refusal names why: {e}")
        else:
            raise AssertionError("--fresh discarded unsaved work in a saved "
                                 "project")
        expect(d.call("ping")["project"], "the saved project is still open")
    finally:
        d.close()


def main():
    binary = sys.argv[1]
    state = tempfile.mkdtemp(prefix="designer-generators-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, env=env)  # headless
    try:
        from lobole.client import Client
        Client.wait_for(app.port).close()

        m, z, t, bore = 2.0, 24, 8.0, 6.0
        gear = os.path.join(ROOT, "generators", "gear.py")
        out = subprocess.run(
            [sys.executable, gear, "--port", str(app.port), "--module", str(m),
             "--teeth", str(z), "--thickness", str(t), "--bore", str(bore)],
            capture_output=True, text=True, env=env, timeout=120)
        expect(out.returncode == 0,
               f"gear.py failed:\n{out.stdout}\n{out.stderr}")
        expect("192 sketch entities" in out.stdout,
               f"24 teeth is 8 entities a tooth ({out.stdout.strip()})")

        d = Client(app.port)
        try:
            s = d.call("get_state")
            errs = [(f["name"], f["error"]) for f in s["features"]
                    if f.get("error")]
            expect(not errs, f"features with errors: {errs}")
            expect(len(s["bodies"]) == 1, f"one body, got {len(s['bodies'])}")
            b = s["bodies"][0]
            rp = m * z / 2
            ra, rf = rp + m, rp - 1.25 * m
            v_bore = math.pi * (bore / 2) ** 2 * t
            lo_v = math.pi * rf ** 2 * t - v_bore
            hi_v = math.pi * ra ** 2 * t - v_bore
            expect(lo_v < b["volume_mm3"] < hi_v,
                   f"volume {b['volume_mm3']:.1f} outside root..tip "
                   f"({lo_v:.1f}..{hi_v:.1f})")
            # Roughly half the annulus between root and tip is tooth — a
            # profile that came out as petals or as a full disc fails this.
            frac = (b["volume_mm3"] - lo_v) / (hi_v - lo_v)
            expect(0.35 < frac < 0.65,
                   f"tooth fill fraction {frac:.2f} is not gear-like")
            for i, (name, want) in enumerate(
                    (("x", ra), ("y", ra), ("z", t / 2))):
                half = (b["bbox_max"][i] - b["bbox_min"][i]) / 2
                expect(abs(half - want) < 1e-3,
                       f"{name} half-extent {half:.4f}, expected {want:.4f}")
            # The live half: thickness is an expression over a variable.
            v = d.call("set_variable", name="gear_thickness", expression="12")
            expect(abs(v["value"] - 12.0) < 1e-9, "variable set")
            s2 = d.call("get_state")
            expect(abs(s2["bodies"][0]["bbox_max"][2] - 12.0) < 1e-3,
                   "the gear thickened with its variable")
        finally:
            d.close()
        # ---- smart_speaker's carcass ---------------------------------
        cabinet = os.path.join(ROOT, "generators", "smart_speaker",
                               "smart_speaker.py")
        out = subprocess.run(
            [sys.executable, cabinet, "--port", str(app.port),
             "--stage", "carcass", "--no-save"],
            capture_output=True, text=True, env=env, timeout=300)
        expect(out.returncode == 0,
               f"smart_speaker carcass failed:\n{out.stdout}\n{out.stderr}")
        expect("no failures" in out.stdout,
               f"every part must build:\n{out.stdout}")
        # The parametric gate inside the run prints this when a part did not
        # move at a second parameter set — a literal that survived.
        expect("unchanged, so not driven" not in out.stdout,
               f"a part stopped following its table:\n{out.stdout}")

        d = Client(app.port)
        try:
            slots = {s["name"]: s for s in d.call("list_parts")["parts"]}
            for want in ("side_wall", "front_wall", "top_wall", "veneer_top"):
                expect(want in slots, f"no {want} tab: {sorted(slots)}")
            expect("Part 1" not in slots,
                   "the empty tab new_project opens with must be gone")
            # THE joinery check: a dowel bored into one panel must have a
            # partner in the panel it meets. Both come from joinery.py, and
            # before that they did not agree.
            def dowels(name):
                # `list_elements` nests faces under each BODY, not at the
                # top level.
                els = d.call("list_elements", part=slots[name]["id"])
                return [f for body in els["bodies"]
                        for f in body.get("faces", [])
                        if f.get("kind") == "cylinder"
                        and abs((f.get("radius") or 0) - 4.0) < 0.05]
            side, top = dowels("side_wall"), dowels("top_wall")
            expect(len(side) == 8, f"the side wall takes 8 dowels: {len(side)}")
            expect(len(top) == 8, f"the top wall takes 8 dowels: {len(top)}")
            # The side wall's top-joint pair and the top wall's side-joint
            # pair are the same joint, so they sit at the same depth across
            # the cabinet: +/-(internal_d/2 - plug_side_dist) = +/-110.
            ys = sorted({round(f["centroid"][1], 1) for f in top})
            expect(any(abs(abs(y) - 110.0) < 0.2 for y in ys),
                   f"the top wall's side-joint dowels sit at +/-110: {ys}")
        finally:
            d.close()

        fresh(app.port, state)
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    print("generators_smoke: gear.py built a gear and it stays parametric; "
          "smart_speaker's carcass builds and its joinery meets")
    return 0


if __name__ == "__main__":
    sys.exit(main())
