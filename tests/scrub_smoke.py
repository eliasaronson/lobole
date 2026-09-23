#!/usr/bin/env python3
"""A scrub lands on the branch the run walked.

Found building the single-cylinder engine
(generators/README.md#single-cylinder-and-air-cooled-engines). A mechanism's pose is NOT a
function of its driver alone: a slider-crank at 180 degrees has the crank and
rod collinear, and both branches close the loop exactly — the piston at
`rod - crank` and the piston at `-(rod + crank)` are equally valid answers to
"crank at 180". Which one comes back depends on where the solver started, and
jumping a quarter turn straight into that singularity put the piston out
through the bottom of the crankcase and reported it as a clean solve, because
it IS one.

So the scrub walks to its target at the study's own sample spacing, the same
path the run takes. This test scrubs to angles the analytic slider-crank
answers in closed form, in an order full of long jumps and reversals, and
holds every pose to a micrometre of it.

Usage: scrub_smoke.py <path-to-designer-binary>
"""
import math
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from designer_sdk import connect  # noqa: E402
from slider_crank import build  # noqa: E402
from smoke_app import launch_app  # noqa: E402

CRANK_R = 20.0
ROD_L = 70.0


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_scrub_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        a = connect(app.port)
        rig, axle, _, _ = build(a, CRANK_R, ROD_L)
        study = a.add_motion_study(driver=axle, dof="rotation", **{"from": 0},
                                   to=360, samples=49, name="crank")
        piston = rig.occurrence("piston")

        def piston_x():
            for o in a.list_occurrences()["occurrences"]:
                if o["id"] == piston.id:
                    return o["transform"]["position"][0]
            raise AssertionError("the piston left the assembly")

        # Long jumps, a reversal, and both collinear poses (180 is the one
        # that used to mirror; 0 and 360 are the other singularity).
        order = [0, 90, 180, 270, 360, 180, 0, 270, 90, 180]
        for deg in order:
            a.set_motion_pose(study=study["id"], t=deg / 360.0)
            t = math.radians(deg)
            want = (CRANK_R * math.cos(t)
                    + math.sqrt(ROD_L ** 2 - (CRANK_R * math.sin(t)) ** 2))
            got = piston_x()
            expect(abs(got - want) < 1e-3,
                   f"crank {deg} deg: piston at {got:.4f}, the slider-crank "
                   f"puts it at {want:.4f}"
                   + (" — the MIRRORED branch" if got < 0 else ""))
            # And never the mirror, which is what the failure looked like.
            expect(got > 0, f"crank {deg} deg: piston at {got:.4f} is behind "
                            f"the crank axis — the mirrored branch")
        print("scrub_smoke: ok")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
