#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A path-traced still must reach the whole part, however big it is.

The ray tracer's rays carried a hard-coded far limit of 10000 in raygen.rgen,
and that number is a LENGTH: CAD works in millimetres, so nothing more than
ten metres from the camera existed to the ray tracer. A part big enough to
need framing from further away than that — a 3 m floating dock, framed from
about eleven metres — rendered as pure environment, and the app reported it
as a successful save (2026-08-31).

The shape of the test: build a part big enough that framing it stands the
camera off further than the old limit, then assert on the reach the renderer
DERIVED — a reach shorter than the camera's own standoff renders the backdrop
and nothing else. Asserting the number rather than the pixels is deliberate:
a picture of the backdrop looks like a dozen other failures, and comparing
pixels needs an image library this project does not depend on.

Needs a GPU with ray tracing; skips (77) without one. No image library: the
still reports for itself whether the scene was in frame, which is also what
the GUI needs in order to stop saying "Saved" about a picture of nothing.

Usage: render_still_reach_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_rtreach_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")

    # --offscreen: a real renderer (and a real RT pipeline) with no window.
    app = launch_app(binary, extra_args=["--offscreen"], env=env,
                     headless=False)
    try:
        d = Designer(app.port, timeout=600)
        d.request("new_project", {})

        # Eight metres of part. The old limit was ten metres, and framing
        # something this size at the default lens stands the camera off
        # further than that — which is the whole case. (A LONG thin part
        # does not do it: 6 x 0.4 x 0.3 m fits at 5.1 m, inside the old
        # limit, because the fit is tight to the silhouette.)
        box(d, (0, 0, 0), (8000, 3000, 4000), "Span")
        d.request("fit_view", {})

        out = os.path.join(scratch, "reach.png")
        r = d.request("render_still", {"path": out, "width": 320,
                                       "height": 180, "samples": 8})
        if not r.get("ok"):
            # No RT device (CI, llvmpipe): nothing to assert, and a red
            # failure here would only mean "this machine has no ray tracing".
            print(f"render_still_reach: no ray tracing here "
                  f"({r.get('error')}) — skipping")
            return 77

        expect(os.path.getsize(out) > 0, "the still was written")

        # THE assertion, on the number that was wrong. A ray reach shorter
        # than the camera's own standoff renders the backdrop and nothing
        # else — which is indistinguishable, from the picture alone, from a
        # dozen other failures. The renderer logs what it derived.
        reach = away = None
        for line in app.log().splitlines():
            if "still reach" in line:
                body = line.split("still reach", 1)[1]
                reach = float(body.split(",")[0])
                away = float(body.split("camera")[1].split()[0])
        expect(reach is not None, "the still logged the reach it derived")
        print(f"  reach {reach:.0f} for a camera {away:.0f} away", flush=True)
        expect(reach > away,
               f"the rays reach past the camera's standoff ({reach} > {away})")
        # The old literal, for scale: this scene needs more than ten metres.
        expect(away > 10000.0,
               f"and this part really does need framing past the old 10000 "
               f"limit ({away:.0f})")

        warnings = r.get("warnings") or []
        expect(not warnings, f"the still reported no complaint ({warnings})")

        # And a still that frames nothing is REPORTED, not silently saved.
        box(d, (0, 0, 0), (8000, 3000, 4000), "Span")
        d.request("fit_view", {})
        d.request("set_camera", {"target": [400000, 0, 0]})
        empty = os.path.join(scratch, "empty.png")
        r2 = d.request("render_still", {"path": empty, "width": 160,
                                        "height": 90, "samples": 4})
        expect(r2.get("ok"), "a still of nothing still writes a file")
        expect(bool(r2.get("warnings")),
               "a still that framed no geometry says so")

        # Looking UP at the model, the shadow catcher must not be a lid. The
        # plane is opaque from both sides, so a camera below the scene's floor
        # got a picture of the underside of the floor with the model's shadow
        # lying on it, and nothing else (2026-09-01).
        d.request("fit_view", {})
        d.request("set_camera", {"target": [0, 0, 0],
                                 "eye": [9000, 9000, -6000]})
        under = os.path.join(scratch, "under.png")
        r3 = d.request("render_still", {"path": under, "width": 160,
                                        "height": 90, "samples": 4,
                                        "ground": True})
        expect(r3.get("ok"), "a still from below renders")
        expect(any("below the scene's floor" in ln for ln in
                   app.log().splitlines()),
               "and drops the shadow catcher rather than looking up at it")
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
