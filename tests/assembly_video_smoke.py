#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""An assembly sequence films as it goes together, and one that does not
check is refused (docs/assemblies/README.md#playing-and-filming-a-sequence).

A clevis -- a lug, a fork stacked on it, and a headed pin through both.
With the fork hidden, `export_assembly_video` of lug then pin to PNG
frames:

  - the frames are export_motion_video's: as many as the timeline takes at
    the fps asked, each the size asked;
  - the pin is hidden before its step: the frames before it appears are
    all one picture, and the one where it appears is not;
  - it travels DOWN its own axis to its seat: the pixels that are the pin
    (those differing from the frame before it appeared) have a top edge
    that only goes down the picture, and some way;
  - it stays home: the last frames are one picture;
  - the assembly is where it was; and with the pin put in before the fork
    the export is refused naming the step that fails (the fork).

Needs a renderer (it runs --offscreen).

Usage: assembly_video_smoke.py <path-to-designer-binary>
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from lobole.sdk import connect  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box, cylinder  # noqa: E402

W, H = 192, 144
FPS = 20


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def rgb(path):
    """A PNG's pixels, decoded by ffmpeg when there is one, else by a
    stdlib PNG reader good for the 8-bit RGB frames the export writes."""
    try:
        return subprocess.run(
            ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
             "-pix_fmt", "rgb24", "-"], check=True,
            capture_output=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return png_rgb(path)


def png_rgb(path):
    import struct
    import zlib
    with open(path, "rb") as f:
        data = f.read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride, out, prev = w * 3, bytearray(), bytearray(w * 3)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - 3] if i >= 3 else 0
            b = prev[i]
            c = prev[i - 3] if i >= 3 else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else b if pb <= pc else c
                line[i] = (line[i] + pr) & 255
        out += line
        prev = line
    return bytes(out)


def differing(a, b):
    """The pixels (x, y) whose colour differs by more than 8 anywhere."""
    out = []
    for i in range(0, len(a), 3):
        if (abs(a[i] - b[i]) > 8 or abs(a[i + 1] - b[i + 1]) > 8
                or abs(a[i + 2] - b[i + 2]) > 8):
            out.append(((i // 3) % W, (i // 3) // W))
    return out


def plate(d, name, z0):
    d.call("add_part", name=name)
    box(d, (-20, -20, z0), (40, 40, 10), f"{name} plate")
    cylinder(d, (0, 0, z0 - 1), "z", 3.0, 12, f"{name} hole", mode="cut")


def fastened(d, name, a, b):
    d.call("add_mate", type="fastened", name=name,
           a={"occurrence": a, "origin": True},
           b={"occurrence": b, "origin": True}, flipped=False)


def placements(d):
    return {o["id"]: [round(v, 6) for v in o["transform"]["position"]]
            for o in d.call("list_occurrences")["occurrences"]}


def scratch_env(root):
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = os.path.join(root, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(root, "config")
    return env


def main():
    binary = sys.argv[1]
    root = tempfile.mkdtemp(prefix="designer_asm_video_")
    app = launch_app(binary, extra_args=["--offscreen"],
                     env=scratch_env(root), headless=False)
    try:
        d = connect(app.port)
        d.call("new_project")
        plate(d, "lug", 0.0)
        plate(d, "fork", 10.0)
        d.call("add_part", name="pin")
        cylinder(d, (0, 0, -5), "z", 2.9, 30, "shank")
        cylinder(d, (0, 0, 25), "z", 5.0, 3, "head", mode="add")
        d.call("add_assembly", name="clevis")
        lug = d.call("add_occurrence", part="lug", name="lug",
                     fixed=True)["id"]
        fork = d.call("add_occurrence", part="fork", name="fork")["id"]
        pin = d.call("add_occurrence", part="pin", name="pin")["id"]
        fastened(d, "fork on lug", lug, fork)
        fastened(d, "pin in lug", lug, pin)
        # The film: the fork hidden (a hidden occurrence places no matter,
        # so no step need place it), the pin through the lug.
        d.call("set_occurrence_visible", occurrence=fork, visible=False)
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "lug", "note": "the base"},
                      {"occurrence": "pin"}])
        expect(d.call("check_assembly_sequence")["pass"],
               "the pin goes through the lug")
        d.call("fit_view", width=W, height=H)
        before = placements(d)

        frames_dir = os.path.join(root, "frames")
        r = d.call("export_assembly_video", path=frames_dir, format="png",
                   width=W, height=H, fps=FPS, _timeout=600)
        names = sorted(n for n in os.listdir(frames_dir)
                       if n.endswith(".png"))
        expect(len(names) == r["frames"] and r["frames"] > 20,
               f"png: {len(names)} frames, as answered ({r['frames']} at "
               f"{FPS} fps)")
        expect(placements(d) == before, "the assembly is where it was")
        pix = [rgb(os.path.join(frames_dir, n)) for n in names]
        expect(all(len(p) == W * H * 3 for p in pix),
               f"every frame is {W}x{H}")
        # Runs of identical frames: the lug alone, the pin standing at its
        # start, its travel (every frame its own), home.
        runs, k = [], 0
        while k < len(pix):
            j = k
            while j + 1 < len(pix) and pix[j + 1] == pix[k]:
                j += 1
            runs.append((k, j))
            k = j + 1
        expect(len(runs) >= 4 and runs[0][1] >= 4 and runs[1][1] > runs[1][0],
               f"the lug alone, then the pin held at its start: {runs[:3]}")
        expect(runs[-1][1] - runs[-1][0] >= FPS,
               f"it stays home: the last {runs[-1][1] - runs[-1][0] + 1} "
               "frames are one picture")
        base = pix[0]  # the pin not yet shown
        out = differing(base, pix[runs[1][0]])
        expect(len(out) > 20,
               f"the pin appears at its step: {len(out)} pixels are it")
        rows = []
        for k in range(runs[1][0], runs[-1][0] + 1):
            p = differing(base, pix[k])
            # Its top: the head, once in the picture (the view fits the
            # assembly as it is, so the pin starts clipped at the top). Its
            # foot goes into the hole, out of sight.
            rows.append(min((y for _, y in p), default=0))
        expect(all(b >= a - 0.75 for a, b in zip(rows, rows[1:])),
               f"the pin only goes down the picture: {[round(x, 1) for x in rows]} "
               f"({frames_dir})")
        expect(rows[-1] - rows[0] > 10,
               f"and some way: {rows[0]} px to {rows[-1]} px")

        # --- a sequence that does not check ---------------------------------
        d.call("set_occurrence_visible", occurrence=fork, visible=True)
        d.call("set_assembly_sequence",
               steps=[{"occurrence": "lug"}, {"occurrence": "pin"},
                      {"occurrence": "fork"}])
        try:
            d.call("export_assembly_video", path=os.path.join(root, "no"),
                   format="png", width=W, height=H, fps=FPS, _timeout=600)
            raise AssertionError("a sequence that does not check filmed")
        except AssertionError:
            raise
        except Exception as e:  # noqa: BLE001
            text = str(e)
            expect("does not check" in text and "step 3 ('fork')" in text,
                   f"refused, naming its first failing step: {text}")
        expect(not os.path.exists(os.path.join(root, "no"))
               or not os.listdir(os.path.join(root, "no")),
               "and no frames were written")
        expect(placements(d) == before, "the assembly is still where it was")
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:  # noqa: BLE001
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
