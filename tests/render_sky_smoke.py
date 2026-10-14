#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A path-traced still against an outdoor sky (`render_still backdrop: sky`).

Every still used to be lit and backed by one indoor studio HDRI. `sky` is a
procedural outdoor sky — a zenith-to-horizon gradient, a ground below the
horizon, a sun — that backs the still and lights it, its sun the key light
(docs/rendering/still-render.md#the-sky).

The shape of the test: one block, the camera low and level so the top of the
frame is above the horizon and the bottom below it. Against the sky the top
must be sky BLUE and the bottom must not be; the studio still of the same
view is a grey sweep, which is what makes the colour mean something. Then
the sun the reply names — derived, and placed — and the refusals: an unknown
backdrop names all three, a sun without `sky` or in quick mode is refused,
and so is a sun at or under the horizon.

Needs a GPU with ray tracing; skips (77) without one, as the other still
tests do. No image library: render_glass_smoke's zlib PNG reader.

Usage: render_sky_smoke.py <path-to-designer-binary>
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lobole.client import Client, RemoteError  # noqa: E402
from render_glass_smoke import read_png  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def refused(d, params, needle):
    try:
        r = d.request("render_still", params)
    except RemoteError as e:
        return needle in str(e)
    return not r.get("ok") and needle in (r.get("error") or "")


def band(rows, y0, y1):
    """Mean (r, g, b) over rows [y0, y1) of the whole width."""
    px = [p for y in range(y0, y1) for p in rows[y]]
    return tuple(sum(p[i] for p in px) / len(px) for i in range(3))


def is_blue(c):
    r, g, b = c
    return b > r + 25 and b > g + 5


def main():
    binary = sys.argv[1]
    scratch = tempfile.mkdtemp(prefix="designer_sky_")
    env = dict(os.environ)
    env["XDG_STATE_HOME"] = os.path.join(scratch, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(scratch, "config")

    app = launch_app(binary, extra_args=["--offscreen"], env=env,
                     headless=False)
    try:
        d = Client(app.port, timeout=600)
        d.request("new_project", {})
        box(d, (0, 0, 0), (40, 30, 20), "Block")
        # Low and level, looking at the block from the front: the horizon
        # runs across the middle of the frame.
        d.request("set_camera", {"target": [20, 15, 10],
                                 "eye": [20, -160, 12]})

        common = {"width": 160, "height": 90, "samples": 8, "seed": 1}
        got = {}
        replies = {}
        for backdrop in ("studio", "sky"):
            out = os.path.join(scratch, f"{backdrop}.png")
            r = d.request("render_still", dict(common, path=out,
                                               backdrop=backdrop))
            if not r.get("ok"):
                print(f"render_sky: no ray tracing here ({r.get('error')})"
                      " — skipping")
                return 77
            replies[backdrop] = r
            w, h, rows = read_png(out)
            got[backdrop] = (band(rows, 0, h // 8), band(rows, h - h // 8, h))
            print(f"  {backdrop}: top {tuple(round(v) for v in got[backdrop][0])}"
                  f", bottom {tuple(round(v) for v in got[backdrop][1])}",
                  flush=True)

        top, bottom = got["sky"]
        expect(is_blue(top), f"against the sky the top of the frame is sky "
               f"blue ({top})")
        expect(not is_blue(bottom), f"and the bottom, below the horizon, is "
               f"ground, not sky ({bottom})")
        expect(not is_blue(got["studio"][0]),
               f"the studio still of the same view is not blue — the colour "
               f"is the sky's doing ({got['studio'][0]})")

        expect(replies["sky"].get("backdrop") == "sky"
               and "sun" not in replies["studio"],
               "a sky reply names its backdrop; a studio one has no sun")
        sun = replies["sky"].get("sun") or {}
        expect(sun.get("elevation_deg") == 40.0
               and 0.0 <= sun.get("azimuth_deg", -1) < 360.0,
               f"the derived sun is reported: 40 degrees up, a bearing from "
               f"the camera ({sun})")

        out = os.path.join(scratch, "placed.png")
        r = d.request("render_still", dict(common, path=out, backdrop="sky",
                                           sun_azimuth_deg=-60,
                                           sun_elevation_deg=25))
        expect(r.get("ok") and r.get("sun") == {"azimuth_deg": 300.0,
                                                "elevation_deg": 25.0},
               f"a placed sun is used as given, the bearing folded into "
               f"[0, 360) ({r.get('sun')})")

        expect(refused(d, {"backdrop": "outdoor"}, "studio, environment, sky"),
               "an unknown backdrop is refused, naming all three")
        expect(refused(d, {"backdrop": "studio", "sun_elevation_deg": 30},
                       "sun_elevation_deg"),
               "a sun without the sky is refused, by name")
        expect(refused(d, {"backdrop": "sky", "sun_elevation_deg": 0},
                       "sun_elevation_deg"),
               "a sun on the horizon is refused")
        expect(refused(d, {"backdrop": "sky", "sun_elevation_deg": 95},
                       "sun_elevation_deg"),
               "a sun past the zenith is refused")
        expect(refused(d, {"mode": "quick", "sun_azimuth_deg": 10},
                       "sun_azimuth_deg"),
               "a sun in quick mode is refused, by name")
        return 0
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
