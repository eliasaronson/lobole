#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Is the machine busy with OTHER work? -- the one probe tools/bench.py and
the smokes share (docs/performance/README.md#is-the-machine-busy).

A test that times something is one of two kinds
(docs/ui/testing.md#tests-that-time-things):

- a COST check ("not quadratic", "the cached path is cheaper") reads CPU
  time, which other work does not inflate, and never asks this module;
- a RESPONSIVENESS check ("a ping answers within 1 s while a job runs")
  needs the wall clock, which other work does inflate. It measures inside a
  `Window` (or `Timed`, which also times its body), and `judge` decides:

      with host_load.Window(app.pid) as busy:
          ... time the pings ...
      host_load.judge(busy, slowest < 1.0, f"slowest ping {slowest:.2f} s",
                      expect, "contact_smoke")

A check that holds passes, busy machine or not. One that fails is a failure
on a quiet machine, and SKIPPED, printing why ("18.3 of 22 CPUs busy with
other work"), on a busy one: when the CPU the REST of the machine used --
the machine's total less this process's and the named processes' (the app
under test) -- averaged over a quarter of the CPUs, or a compiler ran. The
1-minute load average cannot answer that: the app's own job threads raise
it. The rest of the test runs either way.
"""
import glob
import os
import time

HZ = os.sysconf("SC_CLK_TCK")

COMPILERS = {"cc1", "cc1plus", "cc1obj", "lto1", "clang", "clang++", "ld",
             "ld.lld", "ld.gold", "ld.bfd", "mold", "rustc", "ninja"}


def _read(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def load1():
    return os.getloadavg()[0]


def cpu_count():
    try:
        return len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        return os.cpu_count() or 1


def busy_limit(cpus=None):
    """How many CPUs of other work make the machine busy: a quarter."""
    return max(1.0, (cpus if cpus is not None else cpu_count()) / 4.0)


def cpu_busy_s():
    """Seconds of CPU every process on the machine has used since boot
    (/proc/stat: user, nice, system, irq, softirq, steal)."""
    f = (_read("/proc/stat") or "cpu 0").splitlines()[0].split()[1:]
    t = [int(x) for x in f] + [0] * 8
    return (t[0] + t[1] + t[2] + t[5] + t[6] + t[7]) / HZ


def proc_cpu_s(pid, children=False):
    """Seconds of CPU one process has used, every thread of it; with
    `children`, also what its waited-for children used."""
    st = _read(f"/proc/{pid}/stat")
    if not st:
        return 0.0
    f = st[st.rindex(")") + 2:].split()
    used = int(f[11]) + int(f[12])
    if children:
        used += int(f[13]) + int(f[14])
    return used / HZ


def self_cpu_s():
    """This process's CPU, not its children's: an instance it runs is
    counted by name (Foreign.instance, Window's pids), and counting it again
    once reaped would take it off twice."""
    t = os.times()
    return t.user + t.system


def _procs():
    for d in glob.glob("/proc/[0-9]*"):
        pid = int(os.path.basename(d))
        if pid == os.getpid():
            continue
        yield pid, d


def compilers():
    found = []
    for pid, d in _procs():
        comm = (_read(os.path.join(d, "comm")) or "").strip()
        if comm in COMPILERS:
            found.append(f"{comm}[{pid}]")
    return sorted(found)


class Foreign:
    """The CPU the REST of the machine used while a workload ran, as a load
    (CPUs busy on average): the machine's total less this process's and the
    instances'. The 1-minute load average cannot answer that, since the
    workload's own threads raise it; this can, so a workload is marked busy
    for what others did, never for itself."""

    def __init__(self):
        self.t0 = time.monotonic()
        self.busy0 = cpu_busy_s()
        self.self0 = self_cpu_s()
        self.instances = 0.0

    def instance(self, pid):
        """Count an instance's CPU; call just before it stops."""
        self.instances += proc_cpu_s(pid)

    def load(self):
        wall = time.monotonic() - self.t0
        if wall <= 0:
            return 0.0
        other = (cpu_busy_s() - self.busy0 - (self_cpu_s() - self.self0)
                 - self.instances)
        return max(0.0, other / wall)


class Window:
    """What the rest of the machine did while a responsiveness check ran.
    `pids` are processes under test (the app), alive across the window: what
    they use is theirs, not other work."""

    # /proc/stat counts in clock ticks (10 ms), so a window of a few
    # milliseconds reads a whole tick of anything as several CPUs. A shorter
    # window is judged on a fresh sample of this length instead.
    MIN_SAMPLE_S = 1.0

    def __init__(self, *pids):
        self.pids = [p for p in pids if p]

    def __enter__(self):
        self._start()
        return self

    def __exit__(self, *exc):
        self._stop()
        return False

    def _start(self):
        self.foreign = Foreign()
        self.pid0 = {p: proc_cpu_s(p, children=True) for p in self.pids}
        self.compilers0 = compilers()

    def _stop(self):
        for p in self.pids:
            self.foreign.instances += (proc_cpu_s(p, children=True)
                                       - self.pid0[p])
        self.wall = time.monotonic() - self.foreign.t0
        self.other_cpus = self.foreign.load()
        self.compilers = sorted(set(self.compilers0) | set(compilers()))

    def busy(self):
        """Why this window cannot judge wall time, or None."""
        if self.wall < self.MIN_SAMPLE_S:
            again = Window(*self.pids)
            again._start()
            time.sleep(self.MIN_SAMPLE_S)
            again._stop()
            self.other_cpus = again.other_cpus
            self.compilers = sorted(set(self.compilers) | set(again.compilers))
            self.wall = again.wall
        cpus = cpu_count()
        reasons = []
        if self.other_cpus > busy_limit(cpus):
            reasons.append(f"{self.other_cpus:.1f} of {cpus} CPUs busy with "
                           f"other work")
        if self.compilers:
            reasons.append("a compiler ran: " + ", ".join(self.compilers[:4])
                           + (f" and {len(self.compilers) - 4} more"
                              if len(self.compilers) > 4 else ""))
        return "; ".join(reasons) or None


class Timed(Window):
    """A Window that also times its body: `.seconds`, wall clock."""

    def __enter__(self):
        super().__enter__()
        self.t0 = time.monotonic()
        return self

    def __exit__(self, *exc):
        self.seconds = time.monotonic() - self.t0
        return super().__exit__(*exc)


def judge(window, cond, msg, expect, label):
    """A responsiveness check: passes when `cond` holds; when it does not,
    `expect(False, msg)` on a quiet machine, and on a busy one a printed
    skip naming what kept it busy. Returns whether the check was judged."""
    if cond:
        return True
    reason = window.busy()
    if reason:
        print(f"{label}: skipped a wall-clock check ({msg}): {reason}",
              flush=True)
        return False
    expect(False, msg)
    return True
