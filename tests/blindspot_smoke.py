#!/usr/bin/env python3
"""Blind-spot suite runner: tests/blindspots/NN_*.py, one file per KNOWN
AGENT MISTAKE (the numbered-blind-spot idea from the AI-CAD survey,
docs/architecture.md#tests). Each file's `run(d, expect)` must show
two things: the WRONG pattern is caught loudly by the validation surface
(warning, error, digest delta, contact_report — never a silent success),
and the RIGHT pattern works. This keeps the traps that live in session
memory (the V-axis sketch plane, the away-facing cut, hand-derived
quaternions, almost-touching assemblies) executable instead of anecdotal.

Add a new file when a session hits a new trap; never delete one — a trap
that stops being caught is a regression.

Usage: blindspot_smoke.py <path-to-designer-binary>
"""
import glob
import importlib.util
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_blindspot_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    try:
        d = Designer.wait_for(app.port)
        spots = sorted(glob.glob(os.path.join(os.path.dirname(__file__),
                                              "blindspots", "*.py")))
        expect(spots, "no blindspot files found")
        for path in spots:
            name = os.path.splitext(os.path.basename(path))[0]
            spec = importlib.util.spec_from_file_location(name, path)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
            d.call("new_project")
            try:
                mod.run(d, expect)
            except AssertionError as e:
                raise AssertionError(f"[{name}] {e}") from e
            print(f"  {name}: caught")
        print(f"blindspot_smoke: all {len(spots)} blind spots covered")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
