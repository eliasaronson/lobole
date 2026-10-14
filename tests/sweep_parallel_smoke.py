#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Integration test: a PARALLEL clash sweep answers exactly what a serial one
answers.

The sweep runs `Measure::ContactBetween` on up to twenty-two threads, because
one check is ~161 ms of OpenCASCADE boolean and a motion study on a real
engine is twenty thousand of them. Two things can go wrong with that, and
neither shows up as an error:

  * OpenCASCADE fills caches lazily ON the shape, and one body appears in
    many pairs. A race there corrupts an answer rather than crashing.
  * `record` runs in plan order precisely so the response does not depend on
    which thread finished first. If it ever stops doing that, the report
    becomes non-reproducible and every diff against a stored one is noise.

So this asserts the whole response is IDENTICAL between a one-thread sweep
and an all-threads sweep, and identical between two all-thread sweeps.
`elapsed_s` is the only field allowed to differ, and pairs are compared as an
ORDERED list — order is part of the contract.

Usage: sweep_parallel_smoke.py <path-to-designer-binary>
"""
import copy
import json
import os
import sys
import tempfile

from lobole.client import Client  # noqa: E402
from smoke_app import launch_app  # noqa: E402
from smoke_shapes import box  # noqa: E402


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def build(d, side):
    """A `side` x `side` GRID of overlapping boxes, 7 apart on a 10 box, so
    each one really intersects its neighbours in both axes. A single ROW only
    overlaps its two neighbours and left the sweep with eleven pairs to
    check, which proves nothing about running them on twenty-two threads."""
    d.call("new_project")
    n = side * side
    for i in range(n):
        d.call("add_part", name=f"blk{i}")
        d.call("set_active_part", part=f"blk{i}")
        box(d, (0, 0, 0), (10, 10, 10), "B")
    d.call("add_assembly", name="rig")
    d.call("set_active_part", part="rig")
    for i in range(n):
        d.call("add_occurrence", part=f"blk{i}",
               transform={"position": [7.0 * (i % side), 7.0 * (i // side), 0],
                          "orientation": [0, 0, 0, 1]})
    return d


def sweep(binary, env, threads, side):
    e = dict(env)
    if threads:
        e["DESIGNER_SWEEP_THREADS"] = str(threads)
    app = launch_app(binary, env=e)
    try:
        d = Client.wait_for(app.port)
        build(d, side)
        return d.call("contact_report", max_pairs=20000, deadline_s=600)
    finally:
        app.terminate()
        app.wait(timeout=10)


def comparable(report):
    r = copy.deepcopy(report)
    r.pop("elapsed_s", None)  # the one field that MUST differ
    return r


def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_sweep_")
    env["XDG_STATE_HOME"] = tmp
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    side = 5   # 25 occurrences, ~100 pairs past the box prune

    serial = sweep(binary, env, 1, side)
    par = sweep(binary, env, 0, side)
    again = sweep(binary, env, 0, side)

    expect(serial["pairs_checked"] > 20,
           f"the fixture must actually exercise the sweep, got "
           f"{serial['pairs_checked']} exact checks")
    expect(not serial.get("truncated"),
           "the fixture must run to completion or the comparison is between "
           "two different prefixes")

    for name, other in (("parallel", par), ("a second parallel", again)):
        a, b = comparable(serial), comparable(other)
        if a != b:
            for key in sorted(set(a) | set(b)):
                if a.get(key) != b.get(key):
                    raise AssertionError(
                        f"{name} sweep differs from serial on '{key}':\n"
                        f"  serial: {json.dumps(a.get(key))[:400]}\n"
                        f"  {name}: {json.dumps(b.get(key))[:400]}")
            raise AssertionError(f"{name} sweep differs from serial")

    print(f"OK sweep_parallel_smoke: {serial['pairs_checked']} exact checks, "
          f"serial == parallel == parallel, byte for byte")
    return 0


if __name__ == "__main__":
    sys.exit(main())
