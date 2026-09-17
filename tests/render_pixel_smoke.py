#!/usr/bin/env python3
"""Pixel regression: does a renderer change move any pixel it did not mean to?

The gap this closes. The 2026-08-24 architecture review rewrote shared Vulkan
code and verified it by re-rendering a fixed scene and diffing the PNG by hand
-- "0 of 2,764,800 bytes differ". That was the strongest check in the round and
it existed nowhere in the suite, so the next refactor has to do it by hand
again or not at all. `render_ao_smoke.py` asserts a screen-space INVARIANT,
which is a good and different thing: it catches a mapping error whatever caused
it, but only for ambient occlusion.

Why a committed baseline works here, when the obvious objection is that it
cannot. GPUs are not bit-reproducible across vendors, and this machine alone
has three drivers. Measured before writing any of this (2026-09-01, lavapipe):
the same scene rendered in three separate processes gave byte-identical PNGs,
including the first-render transient, which differs from the steady state and
differs the SAME way every time. So the output is deterministic per driver, and
the baseline is keyed to the driver that produced it -- `ping` reports it, down
to the LLVM version for lavapipe.

One thing that measurement did not answer, because it ran three processes on
ONE machine: whether the baseline survives a different CPU. llvmpipe JITs its
shaders, so it could in principle. Measured 2026-09-17 in the runner's own
image (ubuntu:24.04, mesa 25.2.8, LLVM 20.1.2): the same scene rendered with
AVX2 and FMA DISABLED (GALLIUM_OVERRIDE_CPU_CAPS=avx) is byte-identical to the
native render. Fused multiply-add is the classic way two x86 machines disagree
about a float, and it changes nothing here -- so the key names everything the
output depends on, and a baseline recorded in that image is CI's baseline
whatever core the runner lands on. The vector width is part of the key and is
not a CPU property either: llvmpipe caps itself at 256 bits unless
LP_NATIVE_VECTOR_WIDTH says otherwise.

That key is what makes exact byte equality usable instead of a tolerance. A
tolerance is a number nobody can justify, and it hides exactly the small real
regression this is for. A driver the baseline does not cover is REFUSED as
"no baseline for this device" rather than compared loosely -- which is the
same rule the document loader follows for an input it does not recognise.

Consequence worth stating: on a Mesa or driver bump the key changes and the
gate stops applying, which must not happen silently. Locally that is a skip.
In CI, where the driver is pinned by the container image, `--require-baseline`
turns it into a failure -- so a bump surfaces as a red build and someone
re-records deliberately.

    tests/render_pixel_smoke.py <designer>                  # compare, skip if unkeyed
    tests/render_pixel_smoke.py <designer> --require-baseline
    tests/render_pixel_smoke.py <designer> --record         # write the baseline

CI's baseline belongs to CI's driver, so recording it on a developer GPU
records a different file. `tools/ci-local.sh --pixel` runs this gate in a
container matching the runner, and `--pixel-record` writes that baseline.
"""

import hashlib
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE_DIR = os.path.join(HERE, "golden", "render")

# ctest reads 77 as "skipped".
SKIP = 77


def driver_slug(gpu):
    """A filename-safe key for the device that rendered.

    The whole string, not a family name: lavapipe reports its LLVM version and
    a driver update changes the output, so the version belongs in the key. A
    bump then reads as "no baseline for this device", which is the truth, and
    not as a regression in our code.
    """
    keep = [c.lower() if c.isalnum() else "-" for c in gpu]
    slug = "".join(keep)
    while "--" in slug:
        slug = slug.replace("--", "-")
    return slug.strip("-")


def build_scene(d):
    """One scene, exercising the paths a renderer refactor puts at risk.

    Deliberately not a single box: shading needs curvature, the outline pass
    needs silhouettes at several depths, and the shadow and AO passes need one
    shape standing on another. Deliberately not the whole app either -- every
    element here is placed by explicit coordinates, because a scene that
    depends on framing or on a default would change when those change and the
    failure would read as a renderer bug.
    """
    d.call("new_project")
    d.call("add_feature", feature={
        "type": "Box", "name": "Plate", "position": [0, 0, 0],
        "width": 160, "height": 110, "depth": 12})
    d.call("add_feature", feature={
        "type": "Cylinder", "name": "Post", "center": [52, 55, 12],
        "direction": [0, 0, 1], "radius": 16, "height": 54})
    d.call("add_feature", feature={
        "type": "Box", "name": "Block", "position": [104, 30, 12],
        "width": 34, "height": 50, "depth": 30})
    # An explicit camera, never fit_view: fitting is derived from the
    # framebuffer and from the model bounds, so it would silently re-frame the
    # shot whenever either moved and every pixel would differ for a reason
    # that is not a rendering change.
    d.call("set_camera", target=[70, 55, 20], eye=[300, -210, 190])


def main():
    args = sys.argv[1:]
    record = "--record" in args
    require = "--require-baseline" in args
    positional = [a for a in args if not a.startswith("--")]
    if not positional:
        print(__doc__)
        return 2
    binary = positional[0]

    scratch = tempfile.mkdtemp(prefix="designer_pixel_")
    # Its own state and config dirs, like every other smoke: a suite must not
    # touch the developer's autosaves, recent projects or preferences.
    env = dict(os.environ, XDG_STATE_HOME=scratch,
               XDG_CONFIG_HOME=os.path.join(scratch, "config"))
    # --offscreen, not --headless: this needs a real renderer, and offscreen
    # has one without a window or a display server.
    #
    # A real renderer still needs a DEVICE, and a stock-Ubuntu container has
    # no Vulkan ICD at all: the app aborts during boot, before it ever reports
    # a port, so the `ping`-reports-no-GPU skip below never gets to run. No ICD
    # is an environment, not a defect -- offscreen_smoke's precedent, same
    # condition, same 77.
    try:
        app = launch_app(binary, extra_args=["--offscreen"], env=env,
                         headless=False)
    except RuntimeError as e:
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            msg = f"no usable Vulkan device here ({e})"
            # Same rule as a missing baseline below: a skip on a machine that
            # simply has no ICD, a failure under --require-baseline, where the
            # driver is pinned and a vanished device means the gate stopped
            # applying rather than passing.
            if require:
                print(f"render_pixel_smoke: {msg}")
                return 1
            print(f"render_pixel_smoke: {msg} — skipping")
            return SKIP
        raise
    try:
        d = Designer.wait_for(app.port, timeout=120)
        gpu = d.call("ping").get("gpu", "")
        if not gpu:
            print("render_pixel_smoke: instance reports no GPU — skipping")
            return SKIP
        slug = driver_slug(gpu)
        baseline = os.path.join(BASELINE_DIR, f"scene__{slug}.png")
        print(f"  device: {gpu}")
        print(f"  baseline: {os.path.relpath(baseline, HERE)}")

        build_scene(d)
        # Render twice and keep the SECOND. The first differs from the steady
        # state — reproducibly, but it is a warm-up artifact (the outline
        # settle pass and the AO buffer), and the steady state is both what a
        # person sees and the thing worth pinning.
        shot = os.path.join(scratch, "shot.png")
        d.call("screenshot", path=shot)
        d.call("screenshot", path=shot)
        actual = open(shot, "rb").read()

        if record:
            os.makedirs(BASELINE_DIR, exist_ok=True)
            with open(baseline, "wb") as f:
                f.write(actual)
            print(f"  recorded {len(actual)} bytes for {gpu}")
            return 0

        if not os.path.exists(baseline):
            msg = (f"no baseline for this device ({gpu}).\n"
                   f"  Record one:  tests/render_pixel_smoke.py "
                   f"<designer> --record\n"
                   f"  For CI's driver, not this machine's: "
                   f"tools/ci-local.sh --pixel-record\n"
                   f"  A driver update changes the key on purpose — the "
                   f"output changes with it, so the old baseline would be "
                   f"comparing against a different renderer.")
            if require:
                print(f"render_pixel_smoke: {msg}")
                return 1
            print(f"render_pixel_smoke: {msg} — skipping")
            return SKIP

        expected = open(baseline, "rb").read()
        if actual == expected:
            print(f"  ok: {len(actual)} bytes identical to the baseline")
            return 0

        # Keep the evidence next to the baseline; a diff nobody can look at is
        # a failure nobody can act on.
        got = baseline.replace(".png", ".actual.png")
        shutil.copyfile(shot, got)
        print(f"render_pixel_smoke: the render CHANGED for {gpu}")
        print(f"  baseline {len(expected)} bytes, "
              f"sha {hashlib.sha256(expected).hexdigest()[:16]}")
        print(f"  actual   {len(actual)} bytes, "
              f"sha {hashlib.sha256(actual).hexdigest()[:16]}")
        print(f"  wrote {got}")
        print("  Two things this can mean, and they are not the same:")
        print("   1. a rendering change — intended or not. Look at the two "
              "images before deciding which.")
        print("   2. the driver moved under us. `ping` reports the device; if "
              "it no longer matches the baseline's key the file name would "
              "have changed, so this is NOT that — the key matched.")
        return 1
    finally:
        app.terminate()


if __name__ == "__main__":
    sys.exit(main())
