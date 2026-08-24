#!/usr/bin/env python3
"""Screen-space rendering invariants, on a real GPU.

The renderers are otherwise untested: CI has no GPU, and `gui_smoke` checks
that widgets respond, not what pixels look like. That gap let a screen-space
bug ship — the SSAO map was sampled at `gl_FragCoord / framebufferSize` while
the scene is drawn into the 3D viewport SUB-RECT, so ambient occlusion landed
displaced from the geometry that caused it, painting ghost copies of railings
onto a flat deck (2026-08-18).

The shape of the test is what matters more than the numbers: build a scene
whose CORRECT ambient occlusion is confined to one known place, then assert
there is none anywhere else. Any mapping error moves occlusion somewhere it
does not belong, whatever the cause — a wrong divide, a half-res offset, a
stale buffer.

Run: python3 render_ao_smoke.py <path-to-designer>   (needs a display + GPU;
ctest supplies both through tools/xvfb-exec.sh).
"""
import os
import socket
import subprocess
import tempfile
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
from designer_remote import Designer  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pillow is not a hard dependency of the project
    print("render_ao_smoke: pillow not installed — skipping")
    sys.exit(77)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


# Cell size for the "is this occlusion near geometry?" test: one cell either
# side is the tolerance, so ~48 px at CELL=16 — far larger than any legitimate
# AO bleed, far smaller than the tens of pixels a mapping error displaces by.
CELL = 16


def bright_mask(img, threshold=140):
    """Pixels belonging to the model: it is near-white, the background and the
    docked panels are dark. Segmenting the image instead of hard-coding screen
    fractions keeps the test valid when the layout or the framing moves."""
    w, h = img.size
    px = img.load()
    return [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
            if px[x, y] > threshold]


def main():
    binary = sys.argv[1]
    port = free_port()
    tmp = os.environ.get("TMPDIR", "/tmp")
    shot_on = os.path.join(tmp, f"ao_on_{port}.png")
    shot_off = os.path.join(tmp, f"ao_off_{port}.png")
    shot_bare = os.path.join(tmp, f"ao_bare_{port}.png")
    # Its own state AND config dirs: autosaves and the recent-projects list
    # stay out of $HOME, and the run cannot edit the developer's real
    # preferences (set_settings persists immediately — headless runs no GUI
    # loop to debounce it, so a suite once reset their font size to 120%).
    scratch = tempfile.mkdtemp(prefix="designer_ao_")
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    app = subprocess.Popen([binary, "--hidden", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port, timeout=90)

        # A wide flat plate with one tall post at the FAR end. Correct AO
        # darkens only where the post meets the plate; everything else is open
        # sky. A post is the shape that made the original bug visible: tall,
        # thin, and far from the region it wrongly darkened.
        d.call("new_project")
        d.call("add_feature", feature={
            "type": "Box", "name": "Plate", "position": [0, 0, 0],
            "width": 400, "height": 10, "depth": 200})
        # Two walls 2 mm apart at one end: a slot narrower than the AO
        # kernel, so occlusion there is strong and LOCAL. A lone post gives
        # almost nothing to measure — the radius is crevice-scale (capped at
        # 2.5 model units), which is the whole point of the effect.
        d.call("add_feature", feature={
            "type": "Box", "name": "Wall A", "position": [330, 10, 40],
            "width": 14, "height": 70, "depth": 120})
        d.call("add_feature", feature={
            "type": "Box", "name": "Wall B", "position": [346, 10, 40],
            "width": 14, "height": 70, "depth": 120})

        # A fixed camera: this test compares regions by screen fraction, so it
        # must not depend on fit heuristics that change with the model.
        d.call("set_camera", target=[200, 40, 100],
               direction=[-0.35, 0.55, 0.75], distance=520)
        # Shadows OFF in both frames. With them on, the difference between
        # AO-on and AO-off is largest inside shadows — not because AO is wrong
        # there, but because ACES is steeper in dark regions. That artifact of
        # measurement is exactly what made the original report hard to read.
        d.call("set_render_options", shadows=False, ssao="full")
        vp = d.call("screenshot", path=shot_on)["viewport"]
        d.call("set_render_options", shadows=False, ssao="off")
        d.call("screenshot", path=shot_off)
        # A third frame with the walls suppressed locates them on screen
        # without projecting anything by hand: whatever changes between this
        # and the frame above IS the walls.
        walls_ids = [f["id"] for f in d.call("get_state")["features"]
                     if f["name"].startswith("Wall")]
        for fid in walls_ids:
            d.call("suppress_feature", feature_id=fid, suppressed=True)
        d.call("screenshot", path=shot_bare)
        for fid in walls_ids:
            d.call("suppress_feature", feature_id=fid, suppressed=False)

        # Crop to the 3D viewport the app reports: the rest of the frame is
        # panel chrome, whose white text would otherwise segment as "model".
        box = (vp["x"], vp["y"], vp["x"] + vp["width"], vp["y"] + vp["height"])
        on = Image.open(shot_on).convert("L").crop(box)
        off = Image.open(shot_off).convert("L").crop(box)
        bare = Image.open(shot_bare).convert("L").crop(box)
        expect(on.size == off.size == bare.size,
               f"same viewport in all three frames {on.size}")
        w, h = on.size
        onpx, offpx, barepx = on.load(), off.load(), bare.load()

        # Where AO darkens the image, and where the walls are.
        occluded = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
                    if offpx[x, y] - onpx[x, y] >= 4]
        wall_px = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
                   if abs(barepx[x, y] - offpx[x, y]) > 8]
        walls = {(x // CELL, y // CELL) for (x, y) in wall_px}
        expect(len(occluded) > 50,
               f"ambient occlusion is doing something ({len(occluded)} px "
               f"darkened by >=4/255)")
        expect(len(wall_px) > 200,
               f"the walls are on screen ({len(wall_px)} px)")

        # THE INVARIANT: occlusion belongs to the geometry that casts it. A
        # screen-space pass that maps the wrong rectangle moves it somewhere
        # else entirely — that is the whole bug class, and it is what a
        # threshold on "how dark" would miss.
        def near_geometry(p):
            cx, cy = p[0] // CELL, p[1] // CELL
            return any((cx + dx, cy + dy) in walls
                       for dx in (-1, 0, 1) for dy in (-1, 0, 1))

        stray = [p for p in occluded if not near_geometry(p)]
        stray_fraction = len(stray) / float(len(occluded))
        expect(stray_fraction < 0.15,
               f"occlusion lands on the geometry that causes it "
               f"({stray_fraction * 100:.1f}% stray)")

        # THE INVARIANT, sharply: the slot between the walls is occluded along
        # its whole height, so the occlusion must REACH the top of the walls.
        # A screen-space mapping error slides it down (or up) the image while
        # leaving it "near geometry" — on the layout this bug was found on it
        # slid 44 px, and the wall is taller than that.
        occ_top = min(y for _, y in occluded)
        wall_top = min(y for _, y in wall_px)
        print(f"  occluded {len(occluded)} px; top of occlusion y={occ_top}, "
              f"top of geometry y={wall_top}, gap {occ_top - wall_top} px",
              flush=True)
        expect(occ_top - wall_top < 25,
               f"occlusion reaches the geometry it belongs to (starts "
               f"{occ_top - wall_top} px below the walls' top)")
        print("render_ao_smoke: all assertions passed")
    finally:
        app.terminate()
        try:
            app.wait(timeout=20)
        except subprocess.TimeoutExpired:
            app.kill()
        for p in (shot_on, shot_off, shot_bare):
            if os.path.exists(p):
                os.remove(p)


if __name__ == "__main__":
    main()
