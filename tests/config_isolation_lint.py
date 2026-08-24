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
"""
import pathlib
import re
import sys

# Spawning the app: subprocess.Popen([...binary..., "--headless"/"--hidden"/
# "--remote"...]). Matching on the FLAGS rather than the variable name, since
# the binary arrives as sys.argv[1] under a dozen different names.
LAUNCH = re.compile(r'subprocess\.Popen\(\s*\[[^\]]*?"--(?:remote|headless|hidden)"',
                    re.DOTALL)

TESTS = pathlib.Path(__file__).resolve().parent


def main() -> int:
    offenders = []
    checked = 0
    for path in sorted(TESTS.glob("*.py")):
        if path.name == pathlib.Path(__file__).name:
            continue
        source = path.read_text()
        if not LAUNCH.search(source):
            continue
        checked += 1
        if "XDG_CONFIG_HOME" not in source:
            offenders.append(path.name)

    if checked == 0:
        # The regex stopped matching anything — a silent pass would be worse
        # than a loud failure, because this lint would then be dead.
        print("config_isolation_lint: matched no launchers at all; the "
              "Popen pattern must have drifted", file=sys.stderr)
        return 1

    if offenders:
        print("These tests launch Designer without their own XDG_CONFIG_HOME,\n"
              "so a set_settings anywhere in them rewrites the developer's\n"
              "real ~/.config/designer/settings.json:\n", file=sys.stderr)
        for name in offenders:
            print(f"  tests/{name}", file=sys.stderr)
        print("\nGive each one a scratch dir beside its XDG_STATE_HOME:\n"
              '  env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")',
              file=sys.stderr)
        return 1

    print(f"config_isolation_lint: {checked} launchers, all isolated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
