#!/usr/bin/env python3
"""A test that launches Designer must give it its own XDG_CONFIG_HOME.

`set_settings` persists immediately — deliberately, because a --headless
instance runs no GUI main loop to pump the settings-window debounce, so a
scripted change would otherwise apply and never reach the file. That makes any
test which touches preferences a WRITER of whatever config dir the process
inherits.

It shipped that way. `headless_smoke.py` calls

    d.call("set_settings", ui_font_scale=1.2)

to prove the RPC is not part-gated, and its launcher isolated XDG_STATE_HOME
only. So every run of the headless suite rewrote the developer's real
~/.config/designer/settings.json and their app came back at 120% text after the
next rebuild. docs/ui/settings.md had called for exactly this isolation ("Tests
point XDG_CONFIG_HOME at a scratch dir, so no test run can write the
developer's real preferences") — the GUI shard runner does it, the Python smoke
tests never did.

Nothing could have caught it from inside a test run: a suite that quietly edits
$HOME still passes, and the damage only shows up on the developer's next
launch. So the check is on the source, where the launcher is plainly visible.
It runs in a second and needs no display.

The rule: a file that spawns the designer binary must mention XDG_CONFIG_HOME.
That is deliberately shallow — it cannot prove the value is a scratch path —
but it puts the decision in front of whoever writes the next launcher, which is
the only moment it is cheap to get right.

**2026-08-25: the rule now has a better answer than "mention it".** Every smoke
script launches through `smoke_app.launch_app()`, which sets XDG_CONFIG_HOME
itself — so the isolation is structural rather than per-script discipline, and
it cannot be forgotten by the next person to copy a launcher. This lint gained
the second half of that: a script that spawns the binary DIRECTLY is now an
offender on its own, whatever it does about the environment.

Why that is worth failing over, beyond the config dir: those hand-rolled
launchers each picked a port by binding 0 and CLOSING the socket before the
child bound it — a race that surfaced as an occasional unexplained failure —
and each sent the app's output to DEVNULL, which is why fem_smoke could fail
intermittently for months without anyone once seeing what the app said.
"""
import pathlib
import re
import sys

# Spawning the app DIRECTLY: subprocess.Popen([...binary..., "--headless"/
# "--hidden"/"--remote"...]). Matching on the FLAGS rather than the variable
# name, since the binary arrives as sys.argv[1] under a dozen different names.
DIRECT = re.compile(
    r'subprocess\.Popen\(\s*\[[^\]]*?"--(?:remote|headless|hidden|offscreen)"',
    re.DOTALL)
# ...and the sanctioned way.
HELPER = re.compile(r'\blaunch_app\s*\(')

TESTS = pathlib.Path(__file__).resolve().parent


def main() -> int:
    offenders = []
    checked = 0
    for path in sorted(TESTS.glob("*.py")):
        # This file and the helper both TALK about the pattern they forbid.
        if path.name in (pathlib.Path(__file__).name, "smoke_app.py"):
            continue
        source = path.read_text()
        direct, helper = DIRECT.search(source), HELPER.search(source)
        if not direct and not helper:
            continue
        checked += 1
        if direct:
            offenders.append(
                f"{path.name}: spawns the binary directly — use "
                f"smoke_app.launch_app()")
        elif "XDG_CONFIG_HOME" not in source and not helper:
            offenders.append(
                f"{path.name}: launches without its own XDG_CONFIG_HOME")

    if checked == 0:
        # Neither pattern matched anything — a silent pass would be worse
        # than a loud failure, because this lint would then be dead.
        print("config_isolation_lint: matched no launchers at all; the "
              "patterns must have drifted", file=sys.stderr)
        return 1

    if offenders:
        print("A test must launch Designer through smoke_app.launch_app(),\n"
              "which gives it a scratch XDG_CONFIG_HOME (so set_settings\n"
              "cannot rewrite the developer's real settings.json), binds the\n"
              "port in the process that KEEPS it (no pick-then-hand-off race),\n"
              "and keeps the app's log so a failure says why:\n",
              file=sys.stderr)
        for name in offenders:
            print(f"  tests/{name}", file=sys.stderr)
        print("\n    from smoke_app import launch_app\n"
              "    app = launch_app(binary, env=env)   # app.port is the "
              "port it got\n\n"
              "It sets XDG_CONFIG_HOME itself, so passing your own env is "
              "optional\nand cannot lose the isolation.", file=sys.stderr)
        return 1

    print(f"config_isolation_lint: {checked} launchers, all isolated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
