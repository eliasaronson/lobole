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
from smoke_app import launch_app  # noqa: E402
import smoke_shapes as shapes  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pillow is not a hard dependency of the project
    print("render_ao_smoke: pillow not installed — skipping")
    sys.exit(77)




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
    tmp = os.environ.get("TMPDIR", "/tmp")
    # Its own state AND config dirs: autosaves and the recent-projects list
    # stay out of $HOME, and the run cannot edit the developer's real
    # preferences (set_settings persists immediately — headless runs no GUI
    # loop to debounce it, so a suite once reset their font size to 120%).
    scratch = tempfile.mkdtemp(prefix="designer_ao_")
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    app = launch_app(binary, extra_args=["--hidden"], env=env,
                     headless=False)  # a real renderer: this is about SSAO
    # Unique per run: two of these in parallel must not fight over one file,
    # and the port the kernel gave us is the handiest unique number around.
    shot_on = os.path.join(tmp, f"ao_on_{app.port}.png")
    shot_off = os.path.join(tmp, f"ao_off_{app.port}.png")
    shot_bare = os.path.join(tmp, f"ao_bare_{app.port}.png")
    shot_half = os.path.join(tmp, f"ao_half_{app.port}.png")
    try:
        d = Designer.wait_for(app.port, timeout=90)

        # A wide flat plate with one tall post at the FAR end. Correct AO
        # darkens only where the post meets the plate; everything else is open
        # sky. A post is the shape that made the original bug visible: tall,
        # thin, and far from the region it wrongly darkened.
        #
        # The scene is a quarter scale, 100 mm across, and that is load-
        # bearing. The AO kernel is capped at 2.5 model units (crevice
        # scale, which is the point of the effect), and the frame below must
        # RESOLVE it: at 400 mm across a pixel spanned 0.74 mm, the kernel 3
        # px, and reading the depth at the nearest pixel put every open face
        # under a uniform 3% self-occlusion floor the same size as the slot's
        # own AO. The old world-fixed light rig happened to saturate the
        # open faces and hide it; the camera-relative one did not
        # (docs/rendering/README.md#the-light-rig). Here the kernel is ~13 px
        # and the floor is gone, so what the test measures is occlusion.
        d.call("new_project")
        shapes.box(d, (0, 0, 0), (100, 2.5, 50), "Plate")
        # Two walls 0.5 mm apart at one end: a slot narrower than the AO
        # kernel, so occlusion there is strong and LOCAL. A lone post gives
        # almost nothing to measure — the radius is crevice-scale.
        walls_ids = [
            shapes.box(d, (82.5, 2.5, 10), (3.5, 17.5, 30), "Wall A")["id"],
            shapes.box(d, (86.5, 2.5, 10), (3.5, 17.5, 30), "Wall B")["id"]]
        # Mid grey (linear 0.2), not the default near-white: AO darkens the
        # AMBIENT term, and a face the lights drive to 255 shows none of it,
        # so which faces could show occlusion would depend on where the
        # lights stand rather than on where the occlusion is.
        for i, _ in enumerate(d.call("get_state")["bodies"]):
            d.call("set_appearance", body=i, color=[0.2, 0.2, 0.2])

        # A fixed camera: this test compares regions by screen fraction, so it
        # must not depend on fit heuristics that change with the model. The
        # scene above is staged with its slot rising along +Y, so the up is
        # pinned explicitly rather than inheriting the world default. The
        # distance frames the scene as 130 mm did through the old 45 deg
        # lens: at 30 deg the walls filled the frame, and the half-res
        # upsample then straddled far more depth edges than it does in any
        # real view, and the diff crossed the 1.0 bound below.
        d.call("set_camera", target=[50, 10, 25],
               direction=[-0.35, 0.55, 0.75], up=[0, 1, 0], distance=201)
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
        # (The walls' solids: each stands on a datum plane and a sketch of
        # its own, which are not the walls.)
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

        # --- Half tier = half-res AO + joint-bilateral upsample
        # (docs/rendering/README.md#ssao). Two contracts: it must look LIKE
        # native full (a regression bound on the mean difference), and its
        # occlusion must still land ON the geometry — a wrong upsample UV or
        # a dead depth guide displaces AO exactly like the mapping bug this
        # smoke was built for, so the same invariant catches it.
        d.call("set_render_options", shadows=False, ssao="half")
        d.call("screenshot", path=shot_half)
        half = Image.open(shot_half).convert("L").crop(box)
        expect(half.size == on.size, "same viewport in the half frame")
        from PIL import ImageChops
        hist = ImageChops.difference(on, half).histogram()
        total = float(sum(hist))
        mean_diff = sum(i * c for i, c in enumerate(hist)) / total
        print(f"  half-vs-full mean abs diff {mean_diff:.3f}/255", flush=True)
        # Measured 0.49-0.58 on the RTX rig at a 30 deg lens; 1.0 is above noise
        # and far below a visible divergence.
        expect(mean_diff < 1.0,
               f"upsampled half matches native full ({mean_diff:.3f}/255)")

        halfpx = half.load()
        # >=8, not the >=4 the full tier uses: half-RES AO carries a faint
        # (4-7/255) wash on open surfaces — coarse-depth self-occlusion bias,
        # measured IDENTICAL before and after the upsample landed (18.9k px
        # in both, at the old 400 mm scale), so it is the AO computation, not
        # the upsample. The
        # locality invariant is about REAL occlusion being displaced, and the
        # strong pixels are where half matches full (2.9k px in both).
        occluded_h = [(x, y) for y in range(0, h, 2) for x in range(0, w, 2)
                      if offpx[x, y] - halfpx[x, y] >= 8]
        expect(len(occluded_h) > 50,
               f"half-tier occlusion is doing something ({len(occluded_h)} px)")
        stray_h = [q for q in occluded_h if not near_geometry(q)]
        frac_h = len(stray_h) / float(len(occluded_h))
        expect(frac_h < 0.15,
               f"half-tier occlusion lands on its geometry "
               f"({frac_h * 100:.1f}% stray)")
        occ_top_h = min(y for _, y in occluded_h)
        expect(occ_top_h - wall_top < 25,
               f"half-tier occlusion reaches its geometry (gap "
               f"{occ_top_h - wall_top} px)")
        print("render_ao_smoke: all assertions passed")
    finally:
        app.terminate()
        try:
            app.wait(timeout=20)
        except subprocess.TimeoutExpired:
            app.kill()
        for p in (shot_on, shot_off, shot_bare, shot_half):
            if os.path.exists(p):
                os.remove(p)


if __name__ == "__main__":
    main()
