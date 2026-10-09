#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A motion study films to every format, and the refusals refuse.

`export_motion_video` (docs/assemblies/README.md#motion-video) plays the
study as the Motion window's Play does, captures each frame as the viewport
renders it and hands the frames to ffmpeg (mp4, webm, gif) or writes them as
numbered PNGs into a folder. What must hold, on a small slider-crank:

  - png: as many frames as Play takes at 60 fps, each exactly the size asked
    for, and the mechanism MOVES between them (the first and a middle frame
    differ);
  - a frame IS the viewport: the PNG of a frame that falls on a sample is,
    pixel for pixel, the screenshot of the assembly posed at that sample;
  - gif, mp4, webm: ffprobe reads back the frame count, the size and the
    codec (gif, h264, vp9) — decoded, not trusted from our own answer;
  - the assembly is left at the pose it had before, mid-stroke;
  - refused, naming what is accepted: an unknown format, a path whose
    extension disagrees with the format, png given a FILE, an odd size for
    mp4, one frame, ffmpeg failing (its own words come back), and — on an
    instance whose PATH has no ffmpeg — every video format, by name, while
    `ping` says features.ffmpeg is false;
  - async: a job of kind motion-video that cancels and leaves no file.

Needs a renderer (it runs --offscreen), not ray tracing.

Usage: motion_video_smoke.py <path-to-designer-binary>
"""
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
sys.path.insert(0, os.path.join(HERE, "..", "generators"))
sys.path.insert(0, HERE)
from designer_remote import RemoteError  # noqa: E402
from designer_sdk import connect  # noqa: E402
from slider_crank import build  # noqa: E402
from smoke_app import launch_app  # noqa: E402

W, H = 160, 120
SAMPLES = 25
FPS = 60.0
PLAY_S = (SAMPLES - 1) / 24.0  # the Motion window plays 24 samples a second
FRAMES = round(PLAY_S * FPS) + 1


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    assert head[:8] == b"\x89PNG\r\n\x1a\n", f"{path} is not a PNG"
    return struct.unpack(">II", head[16:24])


def sha(path):
    with open(path, "rb") as f:
        return hashlib.sha1(f.read()).hexdigest()


def probe(path):
    """ffprobe's own reading of the first video stream, frames counted."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
         "-show_entries", "stream=codec_name,width,height,nb_read_frames",
         "-of", "json", path],
        check=True, capture_output=True, text=True).stdout
    s = json.loads(out)["streams"][0]
    return s["codec_name"], int(s["width"]), int(s["height"]), \
        int(s["nb_read_frames"])


def pixels_differing(a_path, b_path):
    """The share of pixels whose colour differs by more than 8 in any
    channel, both PNGs decoded by ffmpeg (no imaging library needed)."""
    def rgb(path):
        return subprocess.run(
            ["ffmpeg", "-v", "error", "-i", path, "-f", "rawvideo",
             "-pix_fmt", "rgb24", "-"], check=True,
            capture_output=True).stdout
    a, b = rgb(a_path), rgb(b_path)
    if len(a) != len(b):
        return 1.0
    off = 0
    for i in range(0, len(a), 3):
        if (abs(a[i] - b[i]) > 8 or abs(a[i + 1] - b[i + 1]) > 8
                or abs(a[i + 2] - b[i + 2]) > 8):
            off += 1
    return off / (len(a) // 3)


def refused(app, needles, **kw):
    try:
        r = app.export_motion_video(_timeout=600, **kw)
    except RemoteError as e:
        text = str(e)
        missing = [n for n in needles if n not in text]
        if missing:
            raise AssertionError(f"refused, but without {missing}: {text}")
        return text
    raise AssertionError(f"export_motion_video{kw} was not refused: {r}")


def pose(app):
    """Every occurrence's placement and every mate's value, rounded."""
    occ = {o["id"]: [round(v, 6) for v in o["transform"]["position"]]
           for o in app.list_occurrences()["occurrences"]}
    mates = {m["id"]: round(m.get("rotation_deg", 0.0), 6)
             for m in app.list_mates()["mates"]}
    return occ, mates


def stop(app):
    app.terminate()
    try:
        app.wait(timeout=10)
    except Exception:
        app.kill()


def scratch_env(root):
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    env["XDG_STATE_HOME"] = os.path.join(root, "state")
    env["XDG_CONFIG_HOME"] = os.path.join(root, "config")
    return env


def without_ffmpeg(env):
    """PATH with every directory holding an ffmpeg taken out."""
    keep = [d for d in env.get("PATH", "").split(os.pathsep)
            if d and not os.path.exists(os.path.join(d, "ffmpeg"))]
    env = dict(env)
    env["PATH"] = os.pathsep.join(keep)
    return env


def main():
    binary = sys.argv[1]
    root = tempfile.mkdtemp(prefix="designer_video_")
    have_ffmpeg = shutil.which("ffmpeg") is not None and \
        shutil.which("ffprobe") is not None

    # --- ffmpeg missing: refused by name, never substituted -----------------
    # A --headless instance: the ffmpeg check comes before the renderer's,
    # so the refusal is the one a real user without ffmpeg would read.
    print("instance without ffmpeg on PATH:", flush=True)
    bare = launch_app(binary, env=without_ffmpeg(scratch_env(root)))
    try:
        b = connect(bare.port)
        expect(b.ping()["features"]["ffmpeg"] is False,
               "ping says features.ffmpeg false with no ffmpeg on PATH")
        _, _, _, _ = build(b, 20.0, 70.0)
        sid = b.add_motion_study(driver=b.list_mates()["mates"][0]["id"],
                                 dof="rotation", **{"from": 0}, to=360,
                                 samples=SAMPLES, name="crank")["id"]
        for fmt in ("mp4", "webm", "gif"):
            refused(b, [f"format {fmt} needs ffmpeg on PATH",
                        "or use format png"],
                    study=sid, path=os.path.join(root, f"x.{fmt}"),
                    width=W, height=H)
        expect(True, "mp4, webm and gif refused by name without ffmpeg")
    finally:
        stop(bare)

    print("offscreen instance:", flush=True)
    app_proc = launch_app(binary, extra_args=["--offscreen"],
                          env=scratch_env(root), headless=False)
    try:
        a = connect(app_proc.port)
        expect(a.ping()["features"]["ffmpeg"] is have_ffmpeg,
               f"ping says features.ffmpeg {have_ffmpeg}, as PATH has it")
        _, axle, _, _ = build(a, 20.0, 70.0)
        study = a.add_motion_study(driver=axle, dof="rotation", **{"from": 0},
                                   to=360, samples=SAMPLES, name="crank")
        sid = study["id"]
        a.fit_view(width=W, height=H)
        # Mid-stroke, so "left where it was" is not also "at the start".
        a.set_motion_pose(study=sid, t=0.3)
        before = pose(a)

        # --- png: numbered frames in a folder -------------------------------
        frames_dir = os.path.join(root, "frames")
        r = a.export_motion_video(_timeout=600, study=sid, path=frames_dir, format="png",
                                  width=W, height=H)
        names = sorted(n for n in os.listdir(frames_dir) if n.endswith(".png"))
        expect(len(names) == FRAMES and r["frames"] == FRAMES,
               f"png: {len(names)} frames for {SAMPLES} samples played in "
               f"{PLAY_S} s at {FPS} fps")
        expect(all(png_size(os.path.join(frames_dir, n)) == (W, H)
                   for n in names), f"png: every frame is {W}x{H}")
        expect(sha(os.path.join(frames_dir, names[0])) !=
               sha(os.path.join(frames_dir, names[FRAMES // 4])),
               "png: the mechanism moves between frames")
        expect(r["bytes"] > 0 and r["fps"] > 0, f"png: answered {r}")
        expect(pose(a) == before, "png: the pose is back where it was")

        # The defaults are derived: no size given is the viewport's, the rate
        # a display's, and the length what Play takes.
        d = a.export_motion_video(_timeout=600, study=sid, format="png",
                                  path=os.path.join(root, "defaults"))
        expect(d["fps"] == FPS and d["frames"] == FRAMES,
               f"default {d['frames']} frames at {d['fps']} fps: {SAMPLES} "
               f"samples played in {PLAY_S} s")
        expect(d["width"] > 0 and d["height"] > 0,
               f"default size is the viewport's ({d['width']}x{d['height']})")

        # --- refusals that need no encoder -----------------------------------
        refused(a, ["'avi' is not one of", "mp4, webm, gif, png"],
                study=sid, path=os.path.join(root, "x.avi"), format="avi")
        refused(a, ["ends in '.gif' but format is mp4"],
                study=sid, path=os.path.join(root, "x.gif"), format="mp4")
        refused(a, ["does not say a format", "mp4, webm, gif, png"],
                study=sid, path=os.path.join(root, "noext"))
        refused(a, ["png writes numbered frames into a FOLDER"],
                study=sid, path=os.path.join(root, "x.png"), format="png")
        refused(a, ["frames must be at least 2"],
                study=sid, path=frames_dir, format="png", frames=1)
        refused(a, ["quality 'best' is not one of", "low, medium, high"],
                study=sid, path=os.path.join(root, "q.gif"), quality="best")
        refused(a, ["width and height go together"],
                study=sid, path=frames_dir, format="png", width=W)
        expect(pose(a) == before, "a refusal moves nothing")

        if not have_ffmpeg:
            print("SKIP: no ffmpeg/ffprobe on PATH — the video formats were "
                  "not exercised", flush=True)
            return 77

        refused(a, ["mp4 needs an even width and height"],
                study=sid, path=os.path.join(root, "odd.mp4"),
                width=W + 1, height=H)

        # --- the three encoded formats, read back by ffprobe ----------------
        for fmt, codec in (("gif", "gif"), ("mp4", "h264"), ("webm", "vp9")):
            path = os.path.join(root, f"crank.{fmt}")
            # gif by its extension alone; the other two say it as well.
            kw = {} if fmt == "gif" else {"format": fmt}
            r = a.export_motion_video(_timeout=600, study=sid, path=path, width=W, height=H,
                                      fps=10, **kw)
            got = probe(path)
            want = (codec, W, H, r["frames"])
            expect(r["frames"] == round(PLAY_S * 10) + 1 and got == want,
                   f"{fmt}: ffprobe reads {got}, want {want}")
            expect(r["bytes"] == os.path.getsize(path),
                   f"{fmt}: answered bytes {r['bytes']} = the file's")
            expect(pose(a) == before, f"{fmt}: the pose is back where it was")

        # --- ffmpeg failing: its own words come back ------------------------
        locked = os.path.join(root, "locked")
        os.mkdir(locked, 0o500)
        text = refused(a, ["ffmpeg"], study=sid,
                       path=os.path.join(locked, "no.mp4"), width=W, height=H)
        expect("Permission denied" in text,
               f"ffmpeg's failure is reported in its own words: {text}")
        expect(pose(a) == before, "a failed export puts the pose back too")

        # --- async: a job, cancellable, leaving nothing behind --------------
        slow = os.path.join(root, "slow.mp4")
        r = a.export_motion_video(_timeout=600, study=sid, path=slow, width=W, height=H,
                                  frames=6000, **{"async": True})
        expect(r["started"] and r["job"] == "motion-video",
               f"async answers at once: {r}")
        kinds = [j["kind"] for j in a.background_jobs()["jobs"]]
        expect("motion-video" in kinds, f"background_jobs lists it: {kinds}")
        refused(a, ["already being exported"], study=sid,
                path=os.path.join(root, "second.mp4"), width=W, height=H)
        a.cancel_background_job(kind="motion-video")
        deadline = time.time() + 120
        while time.time() < deadline:
            st = a.motion_video_status()
            if not st["running"]:
                break
            time.sleep(0.1)
        last = st.get("last", {})
        expect(not st["running"] and last.get("cancelled") and
               not last.get("ok"), f"cancelled: {st}")
        expect(not os.path.exists(slow), "a cancelled video leaves no file")
        expect(pose(a) == before, "cancelled: the pose is back where it was")

        # --- a frame IS the viewport ----------------------------------------
        # Frame k sits on sample k * (SAMPLES - 1) / (FRAMES - 1); the one on
        # sample 6 (a quarter turn in) is compared with a screenshot of the
        # assembly posed there. This --offscreen instance's viewport is its
        # whole 1280 x 720 framebuffer, so the frame is taken at that size:
        # the same camera, the same pixels. Last, because the pose it leaves
        # is sample 6's.
        on_sample = 6 * (FRAMES - 1) // (SAMPLES - 1)
        big = os.path.join(root, "big")
        a.export_motion_video(_timeout=600, study=sid, path=big, format="png",
                              width=1280, height=720)
        a.set_motion_pose(study=sid, t=6 / (SAMPLES - 1))
        shot = os.path.join(root, "shot.png")
        a.screenshot(path=shot)
        frame = os.path.join(big, f"frame_{on_sample:04d}.png")
        off = pixels_differing(shot, frame)
        expect(off < 0.001,
               f"the frame on sample 6 is the screenshot there: "
               f"{100 * off:.3f}% of pixels differ by more than 8")
    finally:
        stop(app_proc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
