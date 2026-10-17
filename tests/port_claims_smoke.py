#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every instance is visible to the port picker
(docs/scripting/README.md#the-instance-registry).

An instance run with a scratch XDG_STATE_HOME announces itself there, where
tools/scratch_port.py does not look, so a port it held could be handed out
again. Each instance now also claims its port machine-wide, one line holding
its pid, and the picker reads the claims:

- an instance launched with its own XDG_STATE_HOME (smoke_app's, as every
  smoke and generator test runs) writes its claim, with its pid;
- the picker never hands out a port a live claim holds, even one nothing is
  bound to at the moment (a relaunching instance), and drops a claim whose
  pid is gone;
- the claim goes when the instance stops.

The claims directory is this test's own (LOBOLE_PORT_CLAIMS_DIR), for the
app and the picker alike, so nothing here touches another run's.
"""
import os
import socket
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from smoke_app import launch  # noqa: E402

PICKER = os.path.join(ROOT, "tools", "scratch_port.py")


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def pick(env, port, cwd):
    """scratch_port.py over the one port: the port, or None."""
    r = subprocess.run([sys.executable, PICKER, "--range", str(port),
                        str(port)], env=env, cwd=cwd, capture_output=True,
                       text=True, timeout=30)
    return int(r.stdout) if r.returncode == 0 else None


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def dead_pid():
    p = subprocess.Popen([sys.executable, "-c", "pass"])
    p.wait()
    return p.pid


def main():
    binary = sys.argv[1]
    tmp = tempfile.mkdtemp(prefix="lobole_port_claims_")
    claims = os.path.join(tmp, "claims")
    env = dict(os.environ, LOBOLE_PORT_CLAIMS_DIR=claims)
    # The picker runs outside any claimed worktree, or it answers that
    # worktree's port whatever is asked.
    cwd = tmp
    with launch(binary, env=env) as app:
        port = app.port
        state = app.tmp  # its own XDG_STATE_HOME lives under here
        expect(state and not os.path.exists(os.path.expanduser(
            f"~/.local/state/lobole/instances/{port}.json")),
            "the instance should announce itself only in its scratch state")
        claim = os.path.join(claims, str(port))
        expect(os.path.isfile(claim), f"no claim at {claim}")
        with open(claim) as f:
            pid = int(f.read().split()[0])
        expect(pid == app.client.call("ping")["pid"],
               f"the claim holds {pid}, the instance is "
               f"{app.client.call('ping')['pid']}")
        expect(pick(env, port, cwd) is None,
               f"scratch_port handed out {port}, which an instance holds")

        # Claimed and unbound -- an instance between two binds: the claim
        # alone keeps it.
        other = free_port()
        with open(os.path.join(claims, str(other)), "w") as f:
            f.write(f"{pid}\n")
        expect(pick(env, other, cwd) is None,
               f"scratch_port handed out {other}, claimed by live pid {pid}")
        # The same claim of a process that is gone is dropped.
        with open(os.path.join(claims, str(other)), "w") as f:
            f.write(f"{dead_pid()}\n")
        expect(pick(env, other, cwd) == other,
               f"a dead pid's claim kept {other} from being handed out")
        expect(not os.path.exists(os.path.join(claims, str(other))),
               "the dead pid's claim was not removed")
    expect(not os.path.exists(claim),
           f"the claim outlived the instance: {claim}")
    print("port_claims_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
