#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A lost graphics device is rebuilt in place, and the session goes on
(docs/rendering/README.md#rebuilding-the-device).

In --offscreen and in the windowed app (--hidden, on the display
tools/xvfb-exec.sh gives), each launched by smoke_app in its own scratch
XDG_STATE_HOME (a rebuild autosaves) with LOBOLE_ALLOW_DEBUG_CRASH=1:

  - an edit, then `debug_fault {kind: "device_lost"}`;
  - screenshot, select_face and a quick render_still answer -- on the
    rebuilt device, with no device_lost refusal seen anywhere;
  - ping counts the rebuild (renderer.rebuilds, last_rebuild {reason:
    device_lost, from, to, ms}) and the device is not lost;
  - the edit is still there, and a second one lands;
  - the loss left a report whose reason is "graphics device recovered";
  - set_settings {gpu_device} onto another device answers with the device it
    landed on, in its own response, and back again; with one device that can
    show the window, that half is skipped by name.

`--validate` runs each instance with the Khronos validation layer
(VK_INSTANCE_LAYERS, synchronization and GPU-assisted validation, as
DEV_WATCH_VALIDATE=1 does; docs/rendering/gpu-debugging.md) and fails on any
validation error in the app's log.

Usage: device_rebuild_smoke.py <designer> [offscreen|windowed|both]
                               [--validate]
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402

from lobole.errors import Refused  # noqa: E402

VALIDATION = re.compile(r"Validation (Error|Warning)|VUID-")
# The layer's notes about its own configuration, not about the app.
LAYER_SETTINGS = re.compile(r"VALIDATION-SETTINGS|WARNING-Setting-")


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def not_refused(call, what):
    try:
        return call()
    except Refused as e:
        code = (e.diagnostic or {}).get("code")
        raise AssertionError(f"{what} was refused ({code}): {e}") from e


class _Entered:
    """`with` over a context already entered: launch()'s, entered above so
    a failed start can be told apart from a failed test."""

    def __init__(self, ctx, app):
        self.ctx, self.app = ctx, app

    def __enter__(self):
        return self.app

    def __exit__(self, *exc):
        return self.ctx.__exit__(*exc)


def one(binary, mode, validate):
    print(f"device rebuild, {mode}{' (validated)' if validate else ''}:",
          flush=True)
    env = dict(os.environ, LOBOLE_ALLOW_DEBUG_CRASH="1", LOBOLE_NO_DIALOG="1")
    env.pop("XDG_STATE_HOME", None)  # smoke_app's scratch one, never the real
    env.pop("XDG_CONFIG_HOME", None)
    if validate:
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
        env["VK_LAYER_ENABLES"] = (
            "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,"
            "VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT")
    extra = ["--autosave", "0"]
    if mode == "windowed":
        extra.append("--hidden")
    try:
        ctx = launch(binary, extra_args=extra, env=env, timeout=120,
                     headless=False if mode == "windowed" else "offscreen")
        app = ctx.__enter__()
    except RuntimeError as e:
        # No ICD is an environment, not a defect (offscreen_smoke's rule).
        if "no usable Vulkan device" in str(e) or "VkResult" in str(e):
            print(f"SKIP: no usable Vulkan device here ({e})")
            sys.exit(77)
        raise
    with _Entered(ctx, app):
        d = app.client
        d.call("add_feature", feature={
            "type": "Box", "name": "BeforeTheLoss", "on": "Top",
            "width": 20, "depth": 20, "height": 20})
        d.call("fit_view")
        shot = os.path.join(app.tmp, "before.png")
        not_refused(lambda: d.call("screenshot", path=shot), "screenshot")
        before = d.call("ping")["renderer"]
        before_mesh = d.call("get_state")["bodies"][0].get("mesh")
        expect(before["rebuilds"] == 0 and before["last_rebuild"] is None,
               f"no rebuild yet, on {before['device']}")

        r = d.call("debug_fault", kind="device_lost")
        expect(r["lost"], "debug_fault marks the device lost")

        # The next request waits for the rebuild, as for any frame: none is
        # refused as device_lost.
        after = os.path.join(app.tmp, "after.png")
        not_refused(lambda: d.call("screenshot", path=after),
                    "screenshot after the loss")
        expect(os.path.getsize(after) > 0, "the screenshot was written")
        ping = d.call("ping")["renderer"]
        expect(ping["rebuilds"] == 1, "ping counts one rebuild")
        expect(not ping["lost"], "and the device is not lost")
        last = ping["last_rebuild"]
        expect(last and last["reason"] == "device_lost" and
               last["from"] == before["device"] and last["to"] and
               last["ms"] >= 0,
               f"last_rebuild: {last}")
        state = d.call("get_state")
        names = [f["name"] for f in state["features"]]
        expect("BeforeTheLoss" in names, "the edit survived the rebuild")
        # The mesh id is the rebuilt renderer's: ids start above the last
        # device's, so one kept from before names nothing.
        mesh_ids = [b.get("mesh") for b in state.get("bodies", [])]
        expect(mesh_ids and mesh_ids[0] is not None,
               f"get_state names the body's mesh again ({mesh_ids})")
        expect(mesh_ids[0] != before_mesh,
               f"a new id ({before_mesh} -> {mesh_ids[0]})")
        face = not_refused(
            lambda: d.call("select_face", mesh=mesh_ids[0], face=0),
            "select_face")
        expect(face is not None, "select_face answers")
        still = os.path.join(app.tmp, "still.png")
        not_refused(lambda: d.call("render_still", mode="quick", width=64,
                                   height=48, path=still),
                    "render_still")
        expect(os.path.getsize(still) > 0, "a quick still renders")
        d.call("add_feature", feature={
            "type": "Box", "name": "AfterTheLoss", "on": "Top",
            "centered": False, "offset": [40, 0],
            "width": 5, "depth": 5, "height": 5})
        expect("AfterTheLoss" in
               [f["name"] for f in d.call("get_state")["features"]],
               "and an edit lands")
        reports = d.call("crash_reports")["reports"]
        recovered = [x for x in reports
                     if x["reason"].startswith("graphics device recovered")]
        lost = [x for x in reports
                if x["reason"].startswith("graphics device lost")]
        expect(len(recovered) == 1 and not lost,
               "one report, of a recovered loss, none of a lost one")

        # The GPU preference rebuilds through the same path, and set_settings
        # answers once it has run, with where it landed.
        devices = d.call("get_settings")["devices"]
        start = ping["device"]
        others = [x for x in devices if x != start]
        switched = False
        for other in others:
            s = not_refused(lambda: d.call("set_settings", gpu_device=other),
                            "set_settings gpu_device")
            expect("renderer" in s, f"set_settings answers with renderer "
                   f"({other})")
            if s["renderer"]["device"] != other:
                print(f"  note: {other} is not a rung here "
                      f"(stayed on {s['renderer']['device']})")
                continue
            expect(s["renderer"]["last_rebuild"]["reason"] ==
                   "gpu_preference", f"rebuilt onto {other}")
            p2 = os.path.join(app.tmp, "other.png")
            not_refused(lambda: d.call("screenshot", path=p2),
                        f"screenshot on {other}")
            switched = True
            break
        back = not_refused(lambda: d.call("set_settings", gpu_device=""),
                           "set_settings gpu_device back")
        if switched:
            expect(back["renderer"]["device"] == start,
                   f"and back onto {start}")
        else:
            print(f"  skip: the GPU-preference switch -- only {start} can "
                  "show this window here", flush=True)
        expect(not d.call("ping")["renderer"]["lost"], "never lost")
        expect(app.proc.poll() is None, "the process is alive")
        log = app.log()
        expect("the graphics device was rebuilt" in log,
               "the log names the rebuild")
        if validate:
            bad = [line for line in log.splitlines()
                   if VALIDATION.search(line) and
                   not LAYER_SETTINGS.search(line)]
            for line in bad[:20]:
                print(f"  validation: {line}", flush=True)
            expect(not bad, f"no validation message ({len(bad)})")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    validate = "--validate" in sys.argv
    binary = args[0]
    which = args[1] if len(args) > 1 else "both"
    if which in ("offscreen", "both"):
        one(binary, "offscreen", validate)
    if which in ("windowed", "both"):
        if os.environ.get("DISPLAY"):
            one(binary, "windowed", validate)
        else:
            print("  skip: the windowed half needs a display "
                  "(tools/xvfb-exec.sh)")


if __name__ == "__main__":
    main()
