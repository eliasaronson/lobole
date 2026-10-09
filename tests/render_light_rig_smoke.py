#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The light rig follows the camera: the back of a part is lit like its front.

The viewport's rig was fixed in the world, with the key in front of the home
view, so orbiting to the far side of a part showed a face lit by the dim fill
alone. It is now derived from the camera (core/LightRig.h,
docs/rendering/README.md#the-light-rig) and Preferences > Lock lights to
model puts the world-fixed rig back.

The scene makes "lit the same" a measurement rather than a judgement: one
square block, seen from in front and from behind at the same elevation. The
block is symmetric under that half turn, so the two pictures hold the same
geometry, and a camera-relative rig (lights AND the environment's turn) must
light them the same; only the shadow map's texel grid may differ. Locked, the
same two views must differ — the switch reaches the renderers, through
set_settings, as the Preferences window's checkbox does. Straight down the
up axis, where a rig built on "horizontal" has nothing to swing about, the
top must be lit and finite.

Run: python3 render_light_rig_smoke.py <path-to-designer>   (needs a GPU;
offscreen, so no display server).
"""

import json
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

SKIP = 77

try:
    from PIL import Image
except ImportError:  # pillow is not a hard dependency of the project
    print("render_light_rig_smoke: pillow not installed — skipping")
    sys.exit(SKIP)

# Far from the origin so the origin triad is out of every frame.
CORNER = [500.0, 500.0, 0.0]
SIZE = 80.0
HEIGHT = 40.0
CENTRE = [CORNER[0] + SIZE / 2, CORNER[1] + SIZE / 2, HEIGHT / 2]

# How far apart the front and the back may read, camera-relative. The two
# frames hold the same geometry under the same rig; what differs is the
# shadow map's texel alignment and the SSAO noise, a fraction of a percent.
SAME_WITHIN = 0.03
# Locked, the far side faces away from the key: it must read clearly
# different. Measured 14% darker on an RTX 4060, where camera-relative the
# two agree to the first decimal.
LOCKED_APART = 0.05


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def part_luminance(path):
    """Mean Rec.709 luminance of the pixels that are not the background
    (the clear colour, read at a corner), and how many there were."""
    img = Image.open(path).convert("RGB")
    w, h = img.size
    bg = img.getpixel((0, 0))
    total, count = 0.0, 0
    px = img.tobytes()
    for i in range(0, len(px), 3):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if abs(r - bg[0]) + abs(g - bg[1]) + abs(b - bg[2]) <= 6:
            continue
        total += 0.2126 * r + 0.7152 * g + 0.0722 * b
        count += 1
    if count < 0.05 * w * h:
        raise AssertionError(f"{path}: the block covers only {count} of "
                             f"{w * h} pixels — the camera missed it")
    return total / count


def build_scene(d, scratch):
    # A stored Box primitive at an explicit position, as render_pixel_smoke
    # does: the scene must not depend on any default.
    d.call("new_project")
    path = os.path.join(scratch, "block.json")
    d.call("save_project", path=path)
    with open(path) as f:
        doc = json.load(f)
    doc["features"] = [{"type": "Box", "name": "Block", "position": CORNER,
                        "width": SIZE, "height": SIZE, "depth": HEIGHT}]
    with open(path, "w") as f:
        json.dump(doc, f, indent=2)
    d.call("open_project", path=path)


def shoot(d, scratch, name, eye_offset):
    eye = [CENTRE[i] + eye_offset[i] for i in range(3)]
    d.call("set_camera", target=CENTRE, eye=eye)
    out = os.path.join(scratch, f"{name}.png")
    # Twice, keeping the second: the first frame after a camera move is a
    # warm-up (render_pixel_smoke says why).
    d.call("screenshot", path=out)
    d.call("screenshot", path=out)
    return part_luminance(out)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    scratch = tempfile.mkdtemp(prefix="designer_lightrig_")
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    try:
        app = launch_app(sys.argv[1], extra_args=["--offscreen"], env=env,
                         headless=False)
    except RuntimeError as e:
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            print(f"render_light_rig_smoke: no usable Vulkan device ({e}) "
                  "— skipping")
            return SKIP
        raise
    try:
        d = Designer.wait_for(app.port, timeout=120)
        if not d.call("ping").get("gpu"):
            print("render_light_rig_smoke: no GPU — skipping")
            return SKIP
        expect(d.call("get_settings")["lock_lights_to_model"] is False,
               "the rig follows the camera by default")
        build_scene(d, scratch)

        # In front (-Y is the front) and behind, both 30 degrees up.
        front_eye = [0.0, -260.0, 150.0]
        back_eye = [0.0, 260.0, 150.0]
        front = shoot(d, scratch, "front", front_eye)
        back = shoot(d, scratch, "back", back_eye)
        print(f"  camera-relative: front {front:.1f}, back {back:.1f}")
        expect(abs(back - front) <= SAME_WITHIN * front,
               f"from behind the block reads within {SAME_WITHIN:.0%} of "
               f"from in front ({back:.1f} vs {front:.1f})")

        top = shoot(d, scratch, "top", [0.0, -1e-3, 300.0])
        print(f"  straight down: {top:.1f}")
        expect(top > 0.5 * front,
               f"straight down the up axis the top is lit ({top:.1f})")

        d.call("set_settings", lock_lights_to_model=True)
        lfront = shoot(d, scratch, "locked_front", front_eye)
        lback = shoot(d, scratch, "locked_back", back_eye)
        print(f"  locked: front {lfront:.1f}, back {lback:.1f}")
        expect(abs(lback - lfront) > LOCKED_APART * lfront,
               "locked to the model, the far side is lit differently "
               f"({lback:.1f} vs {lfront:.1f}): the switch reaches the "
               "renderers")
        d.call("set_settings", lock_lights_to_model=False)
        again = shoot(d, scratch, "back_again", back_eye)
        expect(abs(again - back) <= 0.005 * back,
               f"unlocking puts the camera-relative rig back ({again:.1f})")
        print("render_light_rig_smoke: PASS")
        return 0
    finally:
        app.terminate()
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
