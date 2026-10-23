#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A start that cannot draw says why, and the starts after it cope
(app/StartupReport.h, docs/architecture.md#when-the-renderer-cannot-start).

All in one scratch XDG_STATE_HOME / XDG_CONFIG_HOME, shared by the launches
below and by nothing else, with LOBOLE_NO_DIALOG=1 (smoke_app's default) so
no box waits for a click:

  1. no Vulkan driver at all (VK_ICD_FILENAMES=/nonexistent): exit 3, the
     step and VK_ERROR_INCOMPATIBLE_DRIVER on stderr and in
     last-startup-failure.txt, no crash report, no marker left; the next
     start's crash_reports names it;
  2. LOBOLE_RENDERER=gpu is refused naming software and hardware (exit 2);
  3. LOBOLE_FAULT=device_create: with two devices the start lands on the
     second and ping says fallback and why; with one it ends at exit 3,
     naming the attempt. Without LOBOLE_ALLOW_DEBUG_CRASH=1 it is refused;
  4. LOBOLE_FAULT=renderer_init:outline: the start goes on, ping names the
     renderer missing; renderer_init:mesh ends at exit 3;
  5. LOBOLE_RENDERER=software: ping says software (no fallback), and the
     path-traced still is refused by name while the quick one renders;
  6. needs a display (tools/xvfb-exec.sh): LOBOLE_FAULT=swapchain lands on
     the next device or ends at exit 3 naming the swapchain; then two
     windowed starts that crash before their first frame (--crash-test) make
     the third start safe mode, which leaves settings.json and imgui.ini
     byte for byte as they were, and says the startup failure of (1) once.

Usage: startup_failure_smoke.py <path-to-designer-binary>
"""
import os
import resource
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)
    print(f"  ok: {message}", flush=True)


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def run(binary, args, env, timeout=60):
    """A start expected to END: (exit code, what it said)."""
    p = subprocess.run([binary, *args], env=env, capture_output=True,
                       text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    binary = sys.argv[1]
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))  # crashes on purpose
    scratch = tempfile.mkdtemp(prefix="lobole_startup_smoke_")
    state = os.path.join(scratch, "state")
    config = os.path.join(scratch, "config")
    lobole_state = os.path.join(state, "lobole")
    base = dict(os.environ, XDG_STATE_HOME=state, XDG_CONFIG_HOME=config,
                LOBOLE_NO_DIALOG="1")
    for k in ("LOBOLE_RENDERER", "LOBOLE_FAULT", "LOBOLE_SAFE_MODE",
              "LOBOLE_ALLOW_DEBUG_CRASH"):
        base.pop(k, None)
    headless_env = dict(base)
    headless_env.pop("DISPLAY", None)
    headless_env.pop("WAYLAND_DISPLAY", None)
    offscreen = ["--offscreen", "--remote", "0", "--autosave", "0"]
    failure_file = os.path.join(lobole_state, "last-startup-failure.txt")
    try:
        # 1 -------------------------------------------------------------
        env = dict(headless_env, VK_ICD_FILENAMES="/nonexistent",
                   VK_DRIVER_FILES="/nonexistent")
        code, said = run(binary, offscreen, env)
        expect(code == 3, f"no driver: exit 3 ({code})")
        expect("could not create the Vulkan instance" in said and
               "VK_ERROR_INCOMPATIBLE_DRIVER" in said,
               "stderr names the step and the result")
        expect(os.path.exists(failure_file), "last-startup-failure.txt written")
        text = read(failure_file)
        expect("could not create the Vulkan instance" in text and
               "--safe-mode" in text and "The log of this start:" in text,
               "the file says the same, with the way out and the log")
        crashes = os.path.join(lobole_state, "crashes")
        expect(not os.path.isdir(crashes) or not os.listdir(crashes),
               "no crash report: a start that said why has not crashed")
        sessions = os.path.join(lobole_state, "sessions")
        expect(not [n for n in os.listdir(sessions) if n.endswith(".json")],
               "and its session marker is gone, as a clean exit's is")
        with launch(binary, extra_args=["--autosave", "0"],
                    env=dict(base)) as app:
            r = app.client.call("crash_reports")
            expect(r["reports"] == [], "the next start collects no crash")
            lsf = r["last_startup_failure"]
            expect(lsf and "could not create the Vulkan instance" in lsf["text"],
                   "crash_reports names the startup failure")

        # 2 -------------------------------------------------------------
        code, said = run(binary, ["--headless", "--remote", "0"],
                         dict(headless_env, LOBOLE_RENDERER="gpu"))
        expect(code == 2 and "software" in said and "hardware" in said,
               f"LOBOLE_RENDERER=gpu refused naming both ({code})")

        # 3 -------------------------------------------------------------
        code, said = run(binary, offscreen,
                         dict(headless_env, LOBOLE_FAULT="device_create"))
        expect(code == 2 and "LOBOLE_ALLOW_DEBUG_CRASH=1" in said,
               "LOBOLE_FAULT without LOBOLE_ALLOW_DEBUG_CRASH is refused")
        faulty = dict(base, LOBOLE_ALLOW_DEBUG_CRASH="1")
        faulty_headless = dict(headless_env, LOBOLE_ALLOW_DEBUG_CRASH="1")
        try:
            with launch(binary, extra_args=["--autosave", "0"],
                        env=dict(faulty, LOBOLE_FAULT="device_create"),
                        headless="offscreen") as app:
                ping = app.client.call("ping")
                renderer = ping["renderer"]
                expect(renderer["fallback"] and
                       "VK_ERROR_INITIALIZATION_FAILED at device creation"
                       in renderer["reason"],
                       f"device_create: landed on {renderer['device']}, "
                       f"fallback because {renderer['reason']}")
                expect(not renderer["reason"].startswith(renderer["device"]),
                       "on a device other than the one that failed")
        except RuntimeError as e:
            # One device only (CI's lavapipe): the ladder has no next rung.
            msg = str(e)
            expect("exit=3" in msg and "Tried, in order" in msg and
                   "at device creation" in msg,
                   "device_create on a one-device machine: exit 3, naming the "
                   "attempt")

        # 4 -------------------------------------------------------------
        with launch(binary, extra_args=["--autosave", "0"],
                    env=dict(faulty, LOBOLE_FAULT="renderer_init:outline"),
                    headless="offscreen") as app:
            renderer = app.client.call("ping")["renderer"]
            expect(renderer["missing"] == ["outline"],
                   f"a renderer that failed is named ({renderer['missing']})")
            shot = os.path.join(scratch, "no-outline.png")
            app.client.call("add_feature", feature={
                "type": "Box", "name": "B", "on": "Top",
                "width": 10, "depth": 10, "height": 10})
            app.client.call("screenshot", path=shot, view="iso")
            expect(os.path.getsize(shot) > 0, "and the rest still draws")
        code, said = run(binary, offscreen,
                         dict(faulty_headless,
                              LOBOLE_FAULT="renderer_init:mesh"))
        expect(code == 3 and "initialise the mesh renderer" in said,
               f"no mesh renderer ends the start, exit 3 ({code})")

        # 5 -------------------------------------------------------------
        try:
            with launch(binary, extra_args=["--autosave", "0"],
                        env=dict(base, LOBOLE_RENDERER="software"),
                        headless="offscreen") as app:
                renderer = app.client.call("ping")["renderer"]
                expect(renderer["software"] and renderer["type"] == "software"
                       and not renderer["fallback"] and
                       renderer["forced"] == "software",
                       f"LOBOLE_RENDERER=software draws on {renderer['device']}"
                       ", not called a fallback")
                expect(not renderer["ray_tracing"], "with no path tracer")
                app.client.call("add_feature", feature={
                    "type": "Box", "name": "B", "on": "Top",
                    "width": 10, "depth": 10, "height": 10})
                r = app.client.call("render_still", width=32, height=24,
                                    samples=1, fit=True,
                                    path=os.path.join(scratch, "rt.png"))
                expect(not r["ok"] and "software device" in r["error"],
                       f"the path-traced still says why ({r['error']})")
                quick = os.path.join(scratch, "quick.png")
                r = app.client.call("render_still", mode="quick", width=32,
                                    height=24, fit=True, path=quick)
                expect(r["ok"] and os.path.getsize(quick) > 0,
                       "the quick still renders")
        except RuntimeError as e:
            if "exit=3" in str(e) and "no software device" in str(e):
                print("  skip: no software Vulkan device on this machine")
            else:
                raise

        # 6 -------------------------------------------------------------
        if not os.environ.get("DISPLAY"):
            print("  skip: the windowed half needs a display "
                  "(tools/xvfb-exec.sh)")
            return
        windowed = ["--hidden", "--remote", "0", "--autosave", "0"]
        # The box itself, through a stand-in `zenity` that records what it
        # was asked and "clicks" OK; then with no box tool at all and no
        # session bus, where the chain must end at the text file, not hang.
        # (A real desktop notification would reach the developer's session:
        # the bus address is pointed nowhere.)
        fake = os.path.join(scratch, "bin")
        os.makedirs(fake)
        asked = os.path.join(scratch, "zenity-asked.txt")
        with open(os.path.join(fake, "zenity"), "w") as f:
            f.write(f"#!/bin/sh\nprintf '%s\\n' \"$@\" > '{asked}'\nexit 0\n")
        os.chmod(os.path.join(fake, "zenity"), 0o755)
        boxed = dict(base, LOBOLE_NO_DIALOG="0", VK_ICD_FILENAMES="/nonexistent",
                     VK_DRIVER_FILES="/nonexistent",
                     DBUS_SESSION_BUS_ADDRESS="unix:path=/nonexistent")
        # PATH is the stand-in alone: a real zenity on this machine would put
        # a box on the virtual display that nobody clicks.
        p = subprocess.Popen([binary, *windowed], env=dict(boxed, PATH=fake),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        p.communicate(timeout=60)
        expect(p.returncode == 3, f"windowed, no driver: exit 3 "
               f"({p.returncode})")
        expect(os.path.exists(asked) and "--error" in read(asked) and
               "could not" in read(asked) and "--safe-mode" in read(asked),
               "the box was shown, with the message and the way out")
        empty = os.path.join(scratch, "nobin")
        os.makedirs(empty)
        os.remove(failure_file)
        p = subprocess.Popen(
            [binary, *windowed],
            env=dict(boxed, PATH=empty),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        p.communicate(timeout=60)
        expect(p.returncode == 3 and os.path.exists(failure_file),
               "no box tool, no bus: exit 3, the text file, no hang")
        try:
            with launch(binary, extra_args=["--autosave", "0", "--hidden"],
                        env=dict(faulty, LOBOLE_FAULT="swapchain"),
                        headless=False) as app:
                renderer = app.client.call("ping")["renderer"]
                expect(renderer["fallback"] and
                       "at swapchain setup" in renderer["reason"],
                       f"swapchain: landed on {renderer['device']}")
        except RuntimeError as e:
            expect("exit=3" in str(e) and "at swapchain setup" in str(e),
                   "swapchain on a one-device display: exit 3, naming it")
        # The fault above may have written a failure of its own; (1)'s is the
        # one the safe-mode start below should say.
        with open(failure_file, "w") as f:
            f.write(text)

        os.makedirs(os.path.join(config, "lobole"), exist_ok=True)
        settings_path = os.path.join(config, "lobole", "settings.json")
        ini_path = os.path.join(config, "lobole", "imgui.ini")
        with open(settings_path, "w") as f:
            f.write('{\n  "ui_font_scale": 1.2,\n  "shadows": false\n}\n')
        with open(ini_path, "w") as f:
            f.write("[Window][Debug##Default]\nPos=60,60\nSize=400,400\n")
        before = (open(settings_path, "rb").read(), open(ini_path, "rb").read())
        for n in (1, 2):
            code, said = run(binary, windowed + ["--crash-test"], base)
            expect(code < 0 or code > 128,
                   f"windowed start {n} crashed before its first frame "
                   f"({code})")
        with launch(binary, extra_args=["--autosave", "0", "--hidden"],
                    env=dict(base), headless=False) as app:
            ping = app.client.call("ping")
            expect(ping["safe_mode"], "the third start is safe mode")
            log = app.log()
            expect("the last 2 starts crashed before the window drew" in log,
                   "and says why")
            expect("The last start found no renderer" in log,
                   "and says the startup failure once")
            # A setting changed in safe mode lasts the session only; and
            # ImGui writes its layout every 5 s, which must land elsewhere.
            app.client.call("set_settings", ui_font_scale=1.4)
            time.sleep(6)
        after = (open(settings_path, "rb").read(), open(ini_path, "rb").read())
        expect(after[0] == before[0], "settings.json is byte-identical")
        expect(after[1] == before[1], "imgui.ini is byte-identical")
        good = os.path.join(lobole_state, "last-good-start")
        expect(os.path.exists(good), "the safe-mode window drew: a good start")
        with launch(binary, extra_args=["--autosave", "0", "--hidden"],
                    env=dict(base), headless=False) as app:
            expect(not app.client.call("ping")["safe_mode"],
                   "the next start is normal again")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    main()
