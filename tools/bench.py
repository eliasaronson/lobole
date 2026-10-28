#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""How fast Loböle is, measured the same way every time.

    tools/bench.py list                         the workloads, their inputs
    tools/bench.py run [WORKLOAD...]            run (globs: 'open/*'), record
    tools/bench.py run --dry-run                the plan, for free
    tools/bench.py run --profile WORKLOAD       one body under tools/profile.sh
    tools/bench.py fixtures [NAME...]           build the fixtures, time nothing
    tools/bench.py compare [--against last|REV|FILE]
    tools/bench.py compare --ab BIN_A BIN_B [WORKLOAD...]

Every workload runs in an instance of its own, launched here
(tests/smoke_app.py) in a systemd scope with a memory cap, pinned to the
P-cores, with a private config and state directory and `--autosave 0`; it
never drives anybody else's. A run refuses a busy machine (load, a compiler)
unless --busy-ok, and then marks its record busy. Records are appended to
~/.local/state/lobole/bench/<host-key>.jsonl, never to the repository: a
number is only comparable on the machine and load that produced it.

docs/performance/README.md is the manual: the workloads, how a compare
reads, and what each refusal means. The workload table is
tools/bench_workloads.py.
"""
import argparse
import contextlib
import fcntl
import fnmatch
import glob
import hashlib
import json
import math
import os
import re
import shlex
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for p in (os.path.join(ROOT, "python"), os.path.join(ROOT, "tests"), HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

from lobole.client import Client  # noqa: E402
import smoke_app  # noqa: E402
# The busy probe, shared with the smokes' wall-clock checks.
from host_load import (Foreign, compilers, cpu_busy_s, load1,  # noqa: E402
                       proc_cpu_s, self_cpu_s)
import bench_workloads as W  # noqa: E402

SCHEMA = 1
DEFAULT_BINARY = os.path.join(ROOT, "build", "lobole")
CACHE = os.path.join(os.environ.get("XDG_CACHE_HOME")
                     or os.path.expanduser("~/.cache"), "lobole", "bench")
STATE = os.path.join(os.environ.get("XDG_STATE_HOME")
                     or os.path.expanduser("~/.local/state"), "lobole", "bench")
# The environment the verification hooks read (src/core/Document.cpp): a run
# that sees one says so in its record, and compare says so beside it.
INJECTED = ("LOBOLE_TEST_REGEN_DELAY_MS", "LOBOLE_TEST_NO_REGEN_CACHE")
CALL_TIMEOUT_S = 3600.0
# A fixture build is untimed and runs on whatever the machine is doing, so
# its budget is a runaway guard at twice the generator test's (test.json).
FIXTURE_BUDGET_FACTOR = 2


class Refusal(Exception):
    """A request refused, naming why; the CLI prints it and exits 2."""


def say(*a):
    print(*a, flush=True)


# --------------------------------------------------------------- the host --

def _cpu_list(text):
    cpus = set()
    for part in text.strip().split(","):
        if not part:
            continue
        lo, _, hi = part.partition("-")
        cpus.update(range(int(lo), int(hi or lo) + 1))
    return cpus


def _fmt_cpus(cpus):
    cpus, out, i = sorted(cpus), [], 0
    while i < len(cpus):
        j = i
        while j + 1 < len(cpus) and cpus[j + 1] == cpus[j] + 1:
            j += 1
        out.append(str(cpus[i]) if i == j else f"{cpus[i]}-{cpus[j]}")
        i = j + 1
    return ",".join(out)


def _read(path, default=None):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return default


def _slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "none"


def cpu_sets():
    """(P-cores, E-cores) as CPU-number sets. On a machine that is not
    hybrid, every CPU is a P-core and the E set is empty."""
    p = _read("/sys/devices/cpu_core/cpus")
    e = _read("/sys/devices/cpu_atom/cpus")
    if p is None:
        return set(range(os.cpu_count() or 1)), set()
    return _cpu_list(p), _cpu_list(e or "")


def _gpu_cache():
    return os.path.join(CACHE, "gpu.json")


def probe_gpu(binary, scoped):
    """The GPU name an --offscreen instance reports in `ping`, cached per
    boot (it costs a launch). "" when offscreen cannot start, with why."""
    boot = (_read("/proc/sys/kernel/random/boot_id") or "").strip()
    try:
        with open(_gpu_cache()) as f:
            c = json.load(f)
        if c.get("boot") == boot:
            return c["gpu"], c.get("why", "")
    except (OSError, ValueError, KeyError):
        pass
    gpu, why = "", ""
    try:
        with launch(binary, "offscreen", scoped, None, "gpu") as app:
            gpu = app.client.call("ping").get("gpu", "") or ""
    except Exception as e:  # noqa: BLE001 -- the reason is the answer
        why = str(e).strip().splitlines()[0] if str(e).strip() else repr(e)
    os.makedirs(CACHE, exist_ok=True)
    with open(_gpu_cache(), "w") as f:
        json.dump({"boot": boot, "gpu": gpu, "why": why}, f)
    return gpu, why


def host_info(gpu):
    model = "unknown"
    for line in (_read("/proc/cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    pc, ec = cpu_sets()
    mem_kb = 0
    m = re.search(r"MemTotal:\s+(\d+)", _read("/proc/meminfo") or "")
    if m:
        mem_kb = int(m.group(1))
    ram_gb = round(mem_kb / 1024 / 1024)
    info = {"cpu": model, "p_cpus": len(pc), "e_cpus": len(ec),
            "ram_gb": ram_gb, "gpu": gpu or "none"}
    info["key"] = (f"{_slug(model)}-{len(pc)}p{len(ec)}e-{ram_gb}g-"
                   f"{_slug(gpu or 'none')}")
    return info


def governors():
    found = set()
    for path in glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/"
                          "scaling_governor"):
        g = (_read(path) or "").strip()
        if g:
            found.add(g)
    return sorted(found)


def on_ac():
    """True on mains, False on battery, None when the machine has no
    power-supply class (a desktop or a container)."""
    seen = False
    for d in glob.glob("/sys/class/power_supply/*"):
        if (_read(os.path.join(d, "type")) or "").strip() == "Mains":
            seen = True
            if (_read(os.path.join(d, "online")) or "").strip() == "1":
                return True
    return False if seen else None


def _procs():
    for d in glob.glob("/proc/[0-9]*"):
        pid = int(os.path.basename(d))
        if pid == os.getpid():
            continue
        yield pid, d


def lobole_instances(exclude=()):
    """Every running lobole of this user: [{pid, cwd}] (what
    tools/stray-instances.sh lists), less `exclude`."""
    found = []
    for pid, d in _procs():
        if pid in exclude:
            continue
        try:
            if os.stat(d).st_uid != os.getuid():
                continue
            exe = os.readlink(os.path.join(d, "exe"))
        except OSError:
            continue
        if os.path.basename(exe.removesuffix(" (deleted)")) not in (
                "lobole", "designer"):
            continue
        try:
            cwd = os.readlink(os.path.join(d, "cwd"))
        except OSError:
            cwd = "?"
        found.append({"pid": pid, "cwd": cwd})
    return found


def machine_state(pinned):
    """What the run sees of the machine before it starts, and whether that
    is busy: the 1-minute load against a quarter of the pinned CPUs, and any
    compiler running."""
    limit = max(1.0, len(pinned) / 4.0)
    busy0, self0, t0 = cpu_busy_s(), self_cpu_s(), time.monotonic()
    time.sleep(1.0)
    now = max(0.0, (cpu_busy_s() - busy0 - (self_cpu_s() - self0))
              / (time.monotonic() - t0))
    state = {"load1": round(load1(), 2), "load_limit": limit,
             "cpus_busy_now": round(now, 2),
             "compilers": compilers(),
             "other_instances": lobole_instances(),
             "governors": governors(), "ac_power": on_ac()}
    reasons = []
    if state["load1"] > limit:
        reasons.append(f"the 1-minute load average is {state['load1']:.2f}, "
                       f"above {limit:.2f} (a quarter of the "
                       f"{len(pinned)} pinned CPUs)")
    if now > limit:
        reasons.append(f"{now:.2f} CPUs were busy over the last second, "
                       f"above {limit:.2f}")
    if state["compilers"]:
        shown = ", ".join(state["compilers"][:6])
        more = len(state["compilers"]) - 6
        reasons.append("a compiler is running: " + shown
                       + (f" and {more} more" if more > 0 else ""))
    state["busy_reasons"] = reasons
    return state


# -------------------------------------------------------------- the build --

def git(*args):
    try:
        return subprocess.run(["git", "-C", ROOT, *args], text=True,
                              capture_output=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def build_info(binary):
    cache = os.path.join(os.path.dirname(os.path.abspath(binary)),
                         "CMakeCache.txt")
    m = re.search(r"^CMAKE_BUILD_TYPE:\w+=(.*)$", _read(cache) or "", re.M)
    try:
        mtime = os.path.getmtime(binary)
    except OSError:
        mtime = None
    return {"commit": git("rev-parse", "HEAD"),
            "dirty": bool(git("status", "--porcelain",
                              "--untracked-files=no")),
            "build_type": m.group(1).strip() if m else "unknown",
            "binary": os.path.abspath(binary), "binary_mtime": mtime}


@contextlib.contextmanager
def build_lock(binary):
    """build/'s lock, SHARED, as tools/ctest holds it: a merge or a build
    (which take it exclusively) cannot relink the binary under a run.
    tools/build-lock.sh is the other side."""
    path = os.path.join(os.path.dirname(os.path.abspath(binary)),
                        ".build.lock")
    if not os.path.isdir(os.path.dirname(path)):
        yield
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o644)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_SH | fcntl.LOCK_NB)
        except BlockingIOError:
            say(f"bench: waiting for the lock on {os.path.dirname(path)} "
                "(a build or a merge holds it)")
            fcntl.flock(fd, fcntl.LOCK_SH)
        yield
    finally:
        os.close(fd)


# ----------------------------------------------------------- the instance --

def scope_available():
    """Whether `systemd-run --user --scope` works here (a user manager is
    running); the reason when it does not."""
    try:
        r = subprocess.run(["systemd-run", "--user", "--scope", "--quiet",
                            "--collect", "--", "true"],
                           capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, str(e)
    return r.returncode == 0, (r.stderr or r.stdout).strip()


_seq = 0


@contextlib.contextmanager
def launch(binary, kind, scoped, cpus, what, env=None):
    """One instance, as tests/smoke_app.py launches every smoke's: its own
    config and state directory (so no preference of the user's changes a
    number, and nothing lands in their recent list or recovery copies),
    `--autosave 0`, dead with this process, and here in a scope of its
    own with an 8 GB cap, pinned to `cpus`."""
    global _seq
    _seq += 1
    scope = None
    if scoped:
        scope = smoke_app.Scope(
            unit=f"lobole-bench-{os.getpid()}-{_seq}-{_slug(what)[:40]}")
    e = dict(os.environ)
    e.update(env or {})
    with smoke_app.launch(binary, extra_args=("--autosave", "0"), env=e,
                          timeout=120.0,
                          headless=True if kind == "headless" else kind,
                          scope=scope, cpus=cpus, archive=False) as app:
        app.client.sock.settimeout(CALL_TIMEOUT_S)
        app.scope = scope
        yield app


def memory_peak(app):
    d = smoke_app.cgroup_dir(app.proc.pid)
    if d is None or app.scope is None:
        return None
    v = _read(os.path.join(d, "memory.peak"))
    try:
        return int(v)
    except (TypeError, ValueError):
        return None


def allowed_cpus(pid):
    m = re.search(r"Cpus_allowed_list:\s*(\S+)",
                  _read(f"/proc/{pid}/status") or "")
    return m.group(1) if m else "?"


# --------------------------------------------------------------- fixtures --

def fixture_dir(fx):
    return os.path.join(CACHE, "fixtures", f"{fx.name}-{fx.hash()}")


def fixture_ready(fx):
    return os.path.isfile(os.path.join(fixture_dir(fx), "meta.json"))


def fixture_meta(fx):
    with open(os.path.join(fixture_dir(fx), "meta.json")) as f:
        return json.load(f)


def build_fixture(fx, binary, scoped):
    """Build one fixture with its generator, once: into
    ~/.cache/lobole/bench/fixtures/<name>-<hash>/ (LOBOLE_SCRATCH pointed
    there), against an instance of its own, then probe it for what its
    workloads assert against. meta.json is written last, so a build that
    failed or was killed is rebuilt rather than used."""
    final = fixture_dir(fx)
    os.makedirs(os.path.dirname(final), exist_ok=True)
    lock = open(final + ".lock", "w")
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
        if fixture_ready(fx):
            return fixture_meta(fx)
        tmp = final + ".building"
        shutil.rmtree(tmp, ignore_errors=True)
        os.makedirs(tmp)
        out = os.path.join(tmp, "out")
        os.makedirs(out)
        log = os.path.join(tmp, "build.log")
        budget = FIXTURE_BUDGET_FACTOR * fx.decl["budget_s"]
        say(f"bench: building fixture {fx.name} ({fx.hash()}) -- "
            f"budget {budget} s, log {log}")
        t0 = time.monotonic()
        env = dict(os.environ, LOBOLE_SCRATCH=out, PYTHONUNBUFFERED="1",
                   PYTHONPATH=os.pathsep.join(
                       [os.path.join(ROOT, "python")]
                       + [p for p in os.environ.get("PYTHONPATH", "").split(
                           os.pathsep) if p]),
                   **fx.decl.get("env", {}))
        with launch(binary, fx.decl["instance"], scoped, None,
                    f"fixture-{fx.name}") as app:
            cmd, markers, own_log = W.fixture_command(fx, app.port, out,
                                                      sys.executable)
            with open(log, "w") as f:
                f.write(f"$ {shlex.join(cmd)}\n")
                f.flush()
                proc = subprocess.Popen(cmd, env=env, stdout=f,
                                        stderr=subprocess.STDOUT,
                                        start_new_session=True)
                try:
                    code = proc.wait(timeout=budget)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait()
                    raise Refusal(f"fixture {fx.name}: over its budget of "
                                  f"{budget} s -- killed; log in {log}")
                except BaseException:
                    with contextlib.suppress(OSError):
                        os.killpg(proc.pid, signal.SIGKILL)
                    raise
            text = _read(log, "") + (_read(own_log, "") if own_log else "")
            if code != 0:
                raise Refusal(f"fixture {fx.name}: the generator exited "
                              f"{code}; log in {log}\n"
                              + "".join(text.splitlines(True)[-15:]))
            missing = [m for m in markers if not re.search(m, text)]
            if missing:
                raise Refusal(f"fixture {fx.name}: exit 0 but no success "
                              f"marker {missing}: it stopped early; log in "
                              f"{log}")
            project = os.path.join(out, fx.project)
            if not os.path.exists(project):
                raise Refusal(f"fixture {fx.name}: the generator did not "
                              f"save {fx.project}; log in {log}")
            probes = fx.probe(app.client, project) if fx.probe else {}
        meta = {"name": fx.name, "hash": fx.hash(), "files": fx.files(),
                "project": os.path.join("out", fx.project),
                "built_by": build_info(binary),
                "built_s": round(time.monotonic() - t0, 1),
                "probes": probes}
        with open(os.path.join(tmp, "meta.json"), "w") as f:
            json.dump(meta, f, indent=1)
        shutil.rmtree(final, ignore_errors=True)
        os.rename(tmp, final)
        say(f"bench: fixture {fx.name} built in {meta['built_s']:.0f} s")
        return meta
    finally:
        fcntl.flock(lock, fcntl.LOCK_UN)
        lock.close()


# ------------------------------------------------------------------ stats --

def median(xs):
    return statistics.median(xs) if xs else None


def spread(xs):
    """1.4826 x the median absolute deviation: a standard deviation's
    estimate that one outlier cannot move."""
    if len(xs) < 2:
        return 0.0
    m = statistics.median(xs)
    return 1.4826 * statistics.median(abs(x - m) for x in xs)


def threshold(new, base):
    """How far a median may move before it is called a change: three
    combined spreads, 5 % of the baseline or 5 ms, whichever is largest."""
    return max(3.0 * math.hypot(new["spread_s"], base["spread_s"]),
               0.05 * base["median_s"], 0.005)


def verdict_paired(a, b):
    """B against A from an interleaved run: the paired differences b_i - a_i
    (each pair ran back to back, under the same load), whose median must
    pass the same threshold with their own spread, AND every pair must
    agree in sign. A load spike lands on one side of one pair; it moves one
    difference, not all of them."""
    d = [y - x for x, y in zip(a["samples_s"], b["samples_s"])]
    md, sd = statistics.median(d), spread(d)
    t = max(3.0 * sd, 0.05 * a["median_s"], 0.005)
    if md > t and all(x > 0 for x in d):
        return "SLOWER", md, t
    if -md > t and all(x < 0 for x in d):
        return "faster", md, t
    return "same", md, t


def verdict(new, base):
    d = new["median_s"] - base["median_s"]
    t = threshold(new, base)
    if d > t:
        return "SLOWER", d, t
    if -d > t:
        return "faster", d, t
    return "same", d, t


# -------------------------------------------------------------- a workload --

class Ctx:
    """What a workload row's functions see (tools/bench_workloads.py)."""

    def __init__(self, runner, wl, work):
        self.runner = runner
        self.wl = wl
        self.work = work
        self.app = None
        self.client = None
        self.state = {}
        self.scale = runner.scale
        self.failures = []
        self.sample = None  # {"total": s, "phases": {...}} while timing
        self.fixture = None
        self.fixture_meta = None
        self.foreign = None
        self.cpus_seen = set()

    # -- calls
    def call(self, method, **params):
        """An untimed call (setup, checks)."""
        return self.client.request(method, params, timeout=CALL_TIMEOUT_S)

    def timed(self, label, method, **params):
        """A timed call: its wall time is added to the sample and kept as
        phase `label`."""
        t0 = time.perf_counter()
        r = self.client.request(method, params, timeout=CALL_TIMEOUT_S)
        dt = time.perf_counter() - t0
        if self.sample is not None:
            self.sample["total"] += dt
            ph = self.sample["phases"]
            ph[label] = round(ph.get(label, 0.0) + dt, 6)
        return r

    def note(self, key, value):
        """A phase the app reported (a count, a log figure). None is kept as
        None: a phase the app stopped reporting is missing, not zero."""
        if self.sample is not None:
            self.sample["phases"][key] = value

    def expect(self, ok, message):
        if not ok and message not in self.failures:
            self.failures.append(message)
        return ok

    # -- the app's log
    def log_mark(self):
        return len(self.app.log()) if self.app else 0

    def log_since(self, mark):
        return self.app.log()[mark:] if self.app else ""

    # -- fixtures
    def working_copy(self):
        """The fixture copied into this workload's directory, opened and
        saved there (untimed), so it is baked by the binary under test and
        `open/baked` measures a load. Once per workload."""
        if "copy" in self.state:
            return self.state["copy"]
        src = os.path.join(fixture_dir(self.fixture),
                           self.fixture_meta["project"])
        dst = os.path.join(self.work, os.path.basename(src))
        if os.path.isdir(src):
            shutil.copytree(src, dst, symlinks=True)
        else:
            shutil.copy2(src, dst)
        self.call("open_project", path=dst)
        self.call("save_project", path=dst)
        self.state["copy"] = dst
        return dst

    def launch_timed(self, kind):
        """For the startup workloads: launch an instance of `kind`, timed
        from the launch to the first `ping` answered, and stop it."""
        r = self.runner
        t0 = time.perf_counter()
        # No scope: systemd-run's round trip to the user manager would be
        # timed as the app's startup. The launch is brief and needs no cap.
        with launch(r.binary, kind, False, r.cpus, self.wl.name,
                    r.env) as app:
            ping = app.client.call("ping")
            dt = time.perf_counter() - t0
            self.cpus_seen.add(allowed_cpus(app.proc.pid))
            if self.foreign:
                self.foreign.instance(app.proc.pid)
        if self.sample is not None:
            self.sample["total"] += dt
            self.sample["phases"]["launch_to_ping"] = round(dt, 6)
        return ping


def _summarize(samples):
    totals = [s["total"] for s in samples]
    phases = {}
    for s in samples:
        for k, v in s["phases"].items():
            phases.setdefault(k, []).append(v)
    summary = {}
    for k, vs in phases.items():
        nums = [v for v in vs if isinstance(v, (int, float))
                and not isinstance(v, bool)]
        if len(vs) < len(samples):
            summary[k] = None  # a repeat did not report it: missing
        elif len(nums) == len(vs):
            summary[k] = round(statistics.median(nums), 6)
        elif any(v is None for v in vs):
            summary[k] = None  # missing in at least one repeat
        else:
            summary[k] = vs[-1]
    return {"samples_s": [round(t, 6) for t in totals],
            "median_s": round(median(totals), 6) if totals else None,
            "spread_s": round(spread(totals), 6) if totals else None,
            "phases": summary,
            "phase_samples": [s["phases"] for s in samples]}


class Runner:
    def __init__(self, binary, scoped, cpus, scale, repeats, env=None):
        self.binary = binary
        self.scoped = scoped
        self.cpus = cpus
        self.scale = scale
        self.repeats = repeats
        self.env = env or {}
        self.gpu_why = ""
        self.unbuilt = {}  # fixture name -> why it could not be built

    def skip_reason(self, wl):
        for path in wl.inputs:
            if not os.path.exists(os.path.expanduser(path)):
                return f"input missing: {path}"
        if wl.kind == "offscreen" and self.gpu_why:
            return f"--offscreen cannot start here: {self.gpu_why}"
        return None

    def instance(self, ctx, wl):
        return launch(self.binary, wl.kind, self.scoped, self.cpus, wl.name,
                      self.env)

    def _attach(self, ctx, app):
        ctx.app = app
        ctx.client = app.client
        mode = app.client.call("ping").get("mode")
        ctx.expect(mode == wl_mode(ctx.wl.kind),
                   f"the instance is {mode}, not {ctx.wl.kind}")

    def _one(self, ctx, wl, timed):
        ctx.sample = {"total": 0.0, "phases": {}}
        wl.body(ctx)
        s = ctx.sample
        ctx.sample = None
        return s if timed else None

    def run(self, wl, repeats=None):
        """Run one workload: setup (untimed), a warm-up thrown away, then the
        repeats, with the row's reset between them. Returns its result."""
        n = repeats or self.repeats or wl.repeats
        res = {"kind": wl.kind, "repeats": n, "status": "ok",
               "fixture": None, "inputs": {}}
        why = self.skip_reason(wl)
        if why:
            res.update(status="skipped", reason=why)
            return res
        for path in wl.inputs:
            res["inputs"][path] = sha256_file(os.path.expanduser(path))
        work = workdir("run")
        ctx = Ctx(self, wl, work)
        if wl.fixture:
            fx = W.FIXTURES[wl.fixture]
            res["fixture"] = {"name": fx.name, "hash": fx.hash()}
            ctx.fixture = fx
            if fx.name in self.unbuilt:
                res.update(status="error", reason=self.unbuilt[fx.name])
                shutil.rmtree(work, ignore_errors=True)
                return res
            try:
                ctx.fixture_meta = build_fixture(fx, self.binary,
                                                 self.scoped)
            except Refusal as e:
                self.unbuilt[fx.name] = str(e)
                res.update(status="error", reason=str(e))
                shutil.rmtree(work, ignore_errors=True)
                return res
        res["load_before"] = round(load1(), 2)
        samples, peaks, cpus_seen = [], [], ctx.cpus_seen
        ctx.foreign = Foreign()
        try:
            if wl.launches:
                wl.setup(ctx)
                self._one(ctx, wl, False)
                for _ in range(n):
                    samples.append(self._one(ctx, wl, True))
            elif wl.fresh_instance:
                # One instance per repeat (and per warm-up): the first
                # capture of a process is what is measured.
                for i in range(n + 1):
                    with self.instance(ctx, wl) as app:
                        self._attach(ctx, app)
                        cpus_seen.add(allowed_cpus(app.proc.pid))
                        wl.setup(ctx)
                        s = self._one(ctx, wl, i > 0)
                        if s:
                            samples.append(s)
                        if wl.finish:
                            wl.finish(ctx)
                        peaks.append(memory_peak(app))
                        ctx.foreign.instance(app.proc.pid)
            else:
                with self.instance(ctx, wl) as app:
                    self._attach(ctx, app)
                    cpus_seen.add(allowed_cpus(app.proc.pid))
                    wl.setup(ctx)
                    self._one(ctx, wl, False)
                    for _ in range(n):
                        if wl.reset:
                            wl.reset(ctx)
                        samples.append(self._one(ctx, wl, True))
                    if wl.finish:
                        wl.finish(ctx)
                    peaks.append(memory_peak(app))
                    ctx.foreign.instance(app.proc.pid)
        except Refusal:
            raise
        except Exception as e:  # noqa: BLE001 -- recorded, the run goes on
            res["status"] = "error"
            res["reason"] = f"{type(e).__name__}: {e}"
        finally:
            shutil.rmtree(work, ignore_errors=True)
        res.update(_summarize(samples))
        res["load_after"] = round(load1(), 2)
        res["foreign_load"] = round(ctx.foreign.load(), 2)
        peaks = [p for p in peaks if p is not None]
        res["memory_peak_bytes"] = max(peaks) if peaks else None
        res["cpus"] = sorted(cpus_seen)
        res["failures"] = ctx.failures
        if ctx.failures and res["status"] == "ok":
            res["status"] = "failed"
        return res


def workdir(what):
    """A working directory for one workload, named for this process, so the
    next run can remove the ones a killed run left (a copy of a fixture
    each): /tmp/lobole-bench-<pid>-<what>-*. Its own are removed as it
    goes."""
    root = tempfile.gettempdir()
    for path in glob.glob(os.path.join(root, "lobole-bench-*-*")):
        m = re.match(r"lobole-bench-(\d+)-", os.path.basename(path))
        if not m or int(m.group(1)) == os.getpid():
            continue
        try:
            os.kill(int(m.group(1)), 0)
        except ProcessLookupError:
            shutil.rmtree(path, ignore_errors=True)
        except PermissionError:
            pass
    return tempfile.mkdtemp(prefix=f"lobole-bench-{os.getpid()}-{what}-")


def wl_mode(kind):
    return {"headless": "headless", "offscreen": "offscreen"}[kind]


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------- history --

def history_path(args, host_key):
    return args.history or os.path.join(STATE, f"{host_key}.jsonl")


def read_history(path):
    records = []
    try:
        with open(path) as f:
            for i, line in enumerate(f, 1):
                line = line.strip()
                if not line:
                    continue
                try:
                    records.append(json.loads(line))
                except json.JSONDecodeError as e:
                    raise Refusal(f"{path}:{i} is not a record: {e}")
    except FileNotFoundError:
        pass
    return records


def append_history(path, record):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "a") as f:
        fcntl.flock(f, fcntl.LOCK_EX)
        f.write(json.dumps(record, sort_keys=True) + "\n")


# -------------------------------------------------------------- selection --

def select(names, wset):
    rows = W.workloads(wset)
    if not names:
        return rows
    chosen, every = [], W.workloads("all")
    for pat in names:
        hit = [w for w in every if fnmatch.fnmatchcase(w.name, pat)]
        if not hit:
            raise Refusal(f"no workload {pat!r}: `tools/bench.py list` names "
                          "every one (globs work: 'open/*')")
        chosen += [w for w in hit if w not in chosen]
    return chosen


def pinned_cpus(args):
    p, _ = cpu_sets()
    if args.cpus:
        return _cpu_list(args.cpus)
    return p


def estimate(wl, history, scale, repeats):
    """Seconds this workload is expected to take: from the last quiet record
    of it on this host, else the last busy one (an upper bound), else the
    row's rough figure."""
    busy = None
    for rec in reversed(history):
        r = rec.get("workloads", {}).get(wl.name)
        if (not r or r.get("median_s") is None
                or rec.get("options", {}).get("scale") != scale):
            continue
        secs = r["median_s"] * ((repeats or wl.repeats) + 1) + wl.setup_s
        if not rec.get("busy"):
            return secs, "history"
        if busy is None:
            busy = secs
    if busy is not None:
        return busy, "busy history: an upper bound"
    return wl.estimate_s, "the row's rough figure"


# ---------------------------------------------------------------- commands --

def cmd_list(args):
    p, _ = cpu_sets()
    for wl in W.workloads("all"):
        status = []
        for path in wl.inputs:
            if not os.path.exists(os.path.expanduser(path)):
                status.append(f"missing {path}")
        if wl.fixture:
            fx = W.FIXTURES[wl.fixture]
            status.append(f"fixture {fx.name}-{fx.hash()}"
                          + ("" if fixture_ready(fx) else " (not built)"))
        say(f"{wl.name:34} {wl.kind:9} {wl.set:7} x{wl.repeats}  "
            + "; ".join(status))
        say(f"{'':34} {wl.doc}")
    return 0


def _record_head(args, host, machine, pinned, scoped):
    return {"schema": SCHEMA, "started": time.time(),
            "host_key": host["key"], "host": host,
            "build": build_info(args.binary),
            "machine": machine, "pinned_cpus": _fmt_cpus(pinned),
            "scope": {"memory_max": "8G"} if scoped else None,
            "injected": {k: os.environ[k] for k in INJECTED
                         if os.environ.get(k)},
            "options": {"scale": args.scale, "set": args.set,
                        "repeats": args.repeats}}


def _scoped(args):
    if args.no_scope:
        return False
    ok, why = scope_available()
    if not ok:
        raise Refusal("systemd-run --user --scope does not work here ("
                      + (why or "no user manager") + "); --no-scope runs the "
                      "instances without a memory cap or a memory peak, and "
                      "says so in the record")
    return True


def cmd_run(args):
    pinned = pinned_cpus(args)
    rows = select(args.workloads, args.set)
    if args.profile:
        return cmd_profile(args, rows)
    gpu, why = "", ""
    cache = None
    with contextlib.suppress(OSError, ValueError):
        with open(_gpu_cache()) as f:
            cache = json.load(f)
    if args.dry_run:
        gpu = (cache or {}).get("gpu", "") if cache else ""
        host = host_info(gpu)
        hist = read_history(history_path(args, host["key"]))
        machine = machine_state(pinned)
        say(f"host {host['key']}"
            + ("" if cache else " (GPU not probed yet: a run launches "
               "--offscreen once to name it)"))
        b = build_info(args.binary)
        say(f"binary {args.binary}: {b['build_type']}, "
            f"{b['commit'][:10]}{' (dirty)' if b['dirty'] else ''}")
        say(f"pinned to CPUs {_fmt_cpus(pinned)}; machine: load "
            f"{machine['load1']} (limit {machine['load_limit']:.2f})"
            + ("; BUSY: " + "; ".join(machine["busy_reasons"])
               if machine["busy_reasons"] else "; quiet"))
        total = 0.0
        builds = {}
        for wl in rows:
            skip = Runner(args.binary, False, pinned, args.scale,
                          args.repeats).skip_reason(wl)
            if skip:
                say(f"  {wl.name:34} SKIPPED: {skip}")
                continue
            if wl.fixture and not fixture_ready(W.FIXTURES[wl.fixture]):
                fx = W.FIXTURES[wl.fixture]
                builds[fx.name] = FIXTURE_BUDGET_FACTOR * fx.decl["budget_s"]
            secs, src = estimate(wl, hist, args.scale, args.repeats)
            total += secs
            say(f"  {wl.name:34} ~{secs:6.0f} s ({src}), "
                f"x{args.repeats or wl.repeats}")
        for name, budget in builds.items():
            fx = W.FIXTURES[name]
            say(f"  fixture {name}-{fx.hash()} to build: up to {budget} s "
                f"({fixture_dir(fx)})")
        say(f"expected: ~{total / 60:.0f} min of workloads"
            + (f", plus up to {sum(builds.values()) / 60:.0f} min of fixture "
               "builds" if builds else "")
            + f"; history {history_path(args, host['key'])}")
        return 0

    scoped = _scoped(args)
    machine = machine_state(pinned)
    if machine["busy_reasons"] and not args.busy_ok:
        raise Refusal("the machine is busy: "
                      + "; ".join(machine["busy_reasons"])
                      + ". A number taken now measures the load. Wait, or "
                      "--busy-ok to run anyway (the record is marked busy "
                      "and is never used as a baseline)")
    with build_lock(args.binary):
        if not os.access(args.binary, os.X_OK):
            raise Refusal(f"no binary at {args.binary}: build it "
                          "(tools/build.sh) or name one with --binary")
        # Always, so the host key is the same whether or not this run has
        # a render workload; cached per boot, so it costs one launch a boot.
        gpu, why = probe_gpu(args.binary, scoped)
        host = host_info(gpu)
        record = _record_head(args, host, machine, pinned, scoped)
        record["busy"] = bool(machine["busy_reasons"])
        record["busy_reasons"] = list(machine["busy_reasons"])
        if record["injected"]:
            say("bench: injected slowdown in effect: "
                + ", ".join(f"{k}={v}" for k, v in record["injected"].items()))
        runner = Runner(args.binary, scoped, pinned, args.scale, args.repeats)
        runner.gpu_why = why
        record["workloads"] = {}
        say(f"bench: {len(rows)} workload(s) on {host['key']}, CPUs "
            f"{_fmt_cpus(pinned)}" + (", BUSY" if record["busy"] else ""))
        for wl in rows:
            say(f"  {wl.name} ...")
            res = runner.run(wl)
            limit = machine["load_limit"]
            if res.get("foreign_load", 0.0) > limit:
                res["busy"] = True
                record["busy"] = True
                note = (f"{wl.name}: the rest of the machine kept "
                        f"{res['foreign_load']:.2f} CPUs busy during it "
                        f"(limit {limit:.2f})")
                if note not in record["busy_reasons"]:
                    record["busy_reasons"].append(note)
            record["workloads"][wl.name] = res
            say("    " + describe_result(res))
        record["finished"] = time.time()
        record["machine"]["load_after"] = round(load1(), 2)
        path = history_path(args, host["key"])
        append_history(path, record)
    say(f"bench: recorded to {path}"
        + (" (BUSY: never a baseline)" if record["busy"] else ""))
    bad = [n for n, r in record["workloads"].items()
           if r["status"] in ("failed", "error")]
    if bad:
        say("bench: assertions failed in " + ", ".join(bad))
    return 1 if bad else 0


def describe_result(r):
    if r["status"] == "skipped":
        return f"skipped: {r['reason']}"
    if r["status"] == "error":
        return f"ERROR: {r['reason']}"
    if r.get("median_s") is None:
        return f"{r['status']}: no samples"
    s = (f"median {fmt_s(r['median_s'])} +- {fmt_s(r['spread_s'])} over "
         f"{len(r['samples_s'])}")
    if r.get("memory_peak_bytes"):
        s += f", peak {r['memory_peak_bytes'] / 2**20:.0f} MiB"
    if r.get("busy"):
        s += ", BUSY"
    if r["failures"]:
        s += "\n      FAILED: " + "\n      FAILED: ".join(r["failures"])
    return s


def fmt_s(x):
    if x is None:
        return "-"
    if x < 1.0:
        return f"{x * 1000:.1f} ms"
    return f"{x:.2f} s"


def _baseline(args, records, latest_index):
    against = args.against
    prior = records[:latest_index]
    if against == "last":
        if not prior:
            raise Refusal("no earlier record to compare with: run twice, or "
                          "--against REV|FILE")
        return prior[-1], "the previous record"
    if os.path.isfile(against):
        recs = read_history(against)
        if not recs:
            raise Refusal(f"{against} holds no record")
        return recs[-1], against
    rev = git("rev-parse", "--verify", "--quiet", against + "^{commit}")
    if not rev:
        raise Refusal(f"--against {against!r} is neither last, a file nor a "
                      "revision")
    for rec in reversed(prior):
        if rec.get("build", {}).get("commit") == rev:
            return rec, f"the last record of {against} ({rev[:10]})"
    raise Refusal(f"no record of {against} ({rev[:10]}) in this history: "
                  "check it out, build it and `run` it first")


def cmd_compare(args):
    if args.ab:
        return cmd_ab(args)
    if args.new:
        recs = read_history(args.new)
        if not recs:
            raise Refusal(f"{args.new} holds no record")
        new = recs[-1]
        records = read_history(history_path(args, new["host_key"]))
        index = len(records)
    else:
        path = args.history
        if not path:
            gpu = ""
            with contextlib.suppress(OSError, ValueError, KeyError):
                with open(_gpu_cache()) as f:
                    gpu = json.load(f)["gpu"]
            path = os.path.join(STATE, f"{host_info(gpu)['key']}.jsonl")
        records = read_history(path)
        if not records:
            raise Refusal(f"no record in {path}: `tools/bench.py run` first")
        new, index = records[-1], len(records) - 1
    base, named = _baseline(args, records, index)
    if base.get("busy"):
        raise Refusal(f"the baseline ({named}, {base['build']['commit'][:10]}"
                      ") is BUSY ("
                      + "; ".join(base.get("busy_reasons", []))
                      + "): a busy record is never a baseline")
    for what, a, b in (("host", new["host_key"], base["host_key"]),
                       ("build type", new["build"]["build_type"],
                        base["build"]["build_type"]),
                       ("scale", new["options"]["scale"],
                        base["options"]["scale"])):
        if a != b:
            raise Refusal(f"the two records differ in {what}: {a} against "
                          f"{b}. Numbers from different {what}s are not "
                          "comparable")
    say(f"compare: {new['build']['commit'][:10]}"
        f"{' (dirty)' if new['build']['dirty'] else ''} against {named}, "
        f"{base['build']['commit'][:10]}"
        f"{' (dirty)' if base['build']['dirty'] else ''}")
    if new.get("busy"):
        say("  the NEW record is busy (" + "; ".join(new["busy_reasons"])
            + "): its numbers carry that load")
    for rec, who in ((new, "new"), (base, "baseline")):
        if rec.get("injected"):
            say(f"  the {who} record ran with an injected slowdown: "
                + ", ".join(f"{k}={v}" for k, v in rec["injected"].items()))
    regressions, refused = [], []
    for name, n in new["workloads"].items():
        b = base["workloads"].get(name)
        if b is None:
            say(f"  {name:34} new: no baseline")
            continue
        if n["status"] == "skipped" or b["status"] == "skipped":
            say(f"  {name:34} skipped: "
                + (n.get("reason") or b.get("reason", "")))
            continue
        if (n.get("fixture") or {}).get("hash") != (
                b.get("fixture") or {}).get("hash"):
            refused.append(name)
            say(f"  {name:34} REFUSED: the fixture differs "
                f"({(b.get('fixture') or {}).get('hash')} -> "
                f"{(n.get('fixture') or {}).get('hash')}): its generator "
                "changed")
            continue
        if n.get("inputs") != b.get("inputs"):
            refused.append(name)
            say(f"  {name:34} REFUSED: its input file differs")
            continue
        if n.get("median_s") is None or b.get("median_s") is None:
            say(f"  {name:34} no samples on one side ({n['status']}, "
                f"{b['status']})")
            continue
        v, d, t = verdict(n, b)
        line = (f"  {name:34} {fmt_s(b['median_s']):>9} -> "
                f"{fmt_s(n['median_s']):>9}  {d / b['median_s'] * 100:+6.1f}%"
                f"  (threshold {fmt_s(t)})  {v}")
        if n["failures"]:
            line += "  ASSERTIONS FAILED: " + "; ".join(n["failures"])
        say(line)
        if v == "SLOWER":
            regressions.append(name)
    if regressions:
        say("regressed: " + ", ".join(regressions))
        return 1
    if refused:
        say("not compared: " + ", ".join(refused))
        return 2
    say("no regression")
    return 0


def cmd_ab(args):
    """A against B on the same machine at the same time: the two binaries'
    instances alternate, ABBA ABBA..., so drift in load or clock speed falls
    on both, and the verdict is read from the pairs (verdict_paired)."""
    pinned = pinned_cpus(args)
    scoped = _scoped(args)
    bins = [os.path.abspath(b) for b in args.ab]
    for b in bins:
        if not os.access(b, os.X_OK):
            raise Refusal(f"no binary at {b}")
    rows = select(args.workloads, args.set)
    machine = machine_state(pinned)
    if machine["busy_reasons"]:
        say("ab: the machine is busy (" + "; ".join(machine["busy_reasons"])
            + "); the alternation spreads it over both sides")
    runners = [Runner(b, scoped, pinned, args.scale, args.repeats)
               for b in bins]
    if any(wl.kind == "offscreen" for wl in rows):
        _, why = probe_gpu(bins[0], scoped)
        for r in runners:
            r.gpu_why = why
    out = {"a": bins[0], "b": bins[1], "workloads": {}}
    slower = []
    say(f"ab: A = {bins[0]}\n    B = {bins[1]}")
    for wl in rows:
        res = ab_one(runners, wl)
        out["workloads"][wl.name] = res
        if res.get("status") == "skipped":
            say(f"  {wl.name:34} skipped: {res['reason']}")
            continue
        if res.get("status") == "error":
            say(f"  {wl.name:34} ERROR: {res['reason']}")
            continue
        a, b = res["a"], res["b"]
        v, d, t = verdict_paired(a, b)
        say(f"  {wl.name:34} A {fmt_s(a['median_s'])} "
            f"[{fmt_s(min(a['samples_s']))}..{fmt_s(max(a['samples_s']))}]"
            f"  B {fmt_s(b['median_s'])} "
            f"[{fmt_s(min(b['samples_s']))}..{fmt_s(max(b['samples_s']))}]"
            f"  {d / a['median_s'] * 100:+6.1f}%  B is {v}"
            + ("  ASSERTIONS FAILED: A " + "; ".join(a["failures"])
               if a["failures"] else "")
            + ("  ASSERTIONS FAILED: B " + "; ".join(b["failures"])
               if b["failures"] else ""))
        res["verdict"] = v
        if v == "SLOWER":
            slower.append(wl.name)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)
    if slower:
        say("B is slower in: " + ", ".join(slower))
        return 1
    say("no difference")
    return 0


def _order(i):
    """ABBA ABBA...: which side goes first alternates by round, so a cost
    that falls on whichever runs first (or second) falls on both."""
    return (0, 1) if i % 2 == 0 else (1, 0)


def ab_one(runners, wl):
    skip = runners[0].skip_reason(wl)
    if skip:
        return {"status": "skipped", "reason": skip}
    n = runners[0].repeats or wl.repeats
    works = [workdir("ab") for _ in runners]
    ctxs = [Ctx(r, wl, w) for r, w in zip(runners, works)]
    if wl.fixture:
        fx = W.FIXTURES[wl.fixture]
        meta = build_fixture(fx, runners[0].binary, runners[0].scoped)
        for c in ctxs:
            c.fixture, c.fixture_meta = fx, meta
    samples = [[], []]
    try:
        # A fresh instance per side per round. One instance per side would
        # judge the two processes rather than the two binaries: on a loaded
        # machine one instance of a binary ran a workload 20 % slower than
        # another instance of the SAME binary, steadily, over every pair.
        if wl.launches:
            for c in ctxs:
                wl.setup(c)
        for i in range(n + 1 if wl.launches else n):
            for k in _order(i):
                r, c = runners[k], ctxs[k]
                if wl.launches:
                    s = r._one(c, wl, i > 0)
                else:
                    with r.instance(c, wl) as app:
                        r._attach(c, app)
                        wl.setup(c)
                        r._one(c, wl, False)  # its warm-up
                        if wl.reset:
                            wl.reset(c)
                        s = r._one(c, wl, True)
                if s:
                    samples[k].append(s)
    except Exception as e:  # noqa: BLE001
        return {"status": "error", "reason": f"{type(e).__name__}: {e}"}
    finally:
        for w in works:
            shutil.rmtree(w, ignore_errors=True)
    res = {"status": "ok"}
    for k, key in enumerate("ab"):
        res[key] = _summarize(samples[k])
        res[key]["failures"] = ctxs[k].failures
    return res


PROFILE_DRIVER = '''\
#!/usr/bin/env python3
# Written by tools/bench.py run --profile: {name}'s setup and one timed body,
# against the instance tools/profile.sh launched (LOBOLE_PORT).
import os, sys
sys.path[:0] = {paths!r}
import bench
sys.exit(bench.profile_body({name!r}, int(os.environ["LOBOLE_PORT"]),
                            {scale!r}))
'''


def profile_body(name, port, scale):
    """The driver's half of `run --profile`: one workload's setup and one
    body, against an instance somebody else launched (no log, no scope)."""
    wl = next(w for w in W.workloads("all") if w.name == name)
    runner = Runner(DEFAULT_BINARY, False, None, scale, 1)
    work = workdir("profile")
    ctx = Ctx(runner, wl, work)
    try:
        ctx.client = Client.wait_for(port)
        ctx.client.sock.settimeout(CALL_TIMEOUT_S)
        if wl.fixture:
            ctx.fixture = W.FIXTURES[wl.fixture]
            ctx.fixture_meta = fixture_meta(ctx.fixture)
        wl.setup(ctx)
        s = runner._one(ctx, wl, True)
        say(f"profile: {name} body {fmt_s(s['total'])}: {s['phases']}")
        for f in ctx.failures:
            say(f"profile: FAILED: {f}")
        return 1 if ctx.failures else 0
    finally:
        shutil.rmtree(work, ignore_errors=True)


def cmd_profile(args, rows):
    if len(rows) != 1:
        raise Refusal("--profile takes exactly one workload, got "
                      + ", ".join(w.name for w in rows))
    wl = rows[0]
    skip = Runner(args.binary, False, None, args.scale, 1).skip_reason(wl)
    if skip:
        raise Refusal(f"{wl.name} would be skipped: {skip}")
    if wl.kind != "headless" or wl.launches:
        flag = "--offscreen" if wl.kind == "offscreen" else "--headless"
        say(f"profile: {wl.name} needs {'its own launch' if wl.launches else 'an --offscreen instance'}"
            ", which tools/profile.sh --app cannot start (it launches "
            "--headless). Profile the instance yourself:\n"
            f"  tools/profile.sh -- {args.binary} {flag} --remote "
            "$(tools/scratch_port.py) --autosave 0 --exit-with-parent\n"
            f"and drive it with the workload's body from "
            "tools/bench_workloads.py.")
        return 0
    if wl.fixture:
        build_fixture(W.FIXTURES[wl.fixture], args.binary, _scoped(args))
    os.makedirs(os.path.join(CACHE, "profile"), exist_ok=True)
    driver = os.path.join(CACHE, "profile", _slug(wl.name) + ".py")
    with open(driver, "w") as f:
        f.write(PROFILE_DRIVER.format(
            name=wl.name, scale=args.scale,
            paths=[HERE, os.path.join(ROOT, "python"),
                   os.path.join(ROOT, "tests")]))
    say(f"profile: driver {driver}")
    cmd = [os.path.join(HERE, "profile.sh"), "--app", driver]
    say("profile: " + shlex.join(cmd))
    return subprocess.call(cmd)


def cmd_fixtures(args):
    names = args.names or [n for n, fx in W.FIXTURES.items()
                           if args.set == "all" or fx.set == "default"
                           or fx.set == args.set]
    scoped = _scoped(args)
    for n in names:
        if n not in W.FIXTURES:
            raise Refusal(f"no fixture {n!r}; there are "
                          + ", ".join(W.FIXTURES))
    failed = []
    with build_lock(args.binary):
        for n in names:
            fx = W.FIXTURES[n]
            if fixture_ready(fx):
                say(f"{n}: ready, {fixture_dir(fx)}")
                continue
            try:
                build_fixture(fx, args.binary, scoped)
            except Refusal as e:
                say(f"bench: {e}")
                failed.append(n)
    if failed:
        raise Refusal("not built: " + ", ".join(failed))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--binary", default=DEFAULT_BINARY,
                       help="the lobole to measure (default build/lobole)")
        p.add_argument("--set", default="default",
                       choices=("default", "big", "mesh", "all"),
                       help="which workloads when none are named; big adds "
                            "the clock and planetary fixtures, mesh is the "
                            "organic meshes by size")
        p.add_argument("--history", metavar="PATH",
                       help="the history file (default ~/.local/state/"
                            "lobole/bench/<host-key>.jsonl)")
        p.add_argument("--no-scope", action="store_true",
                       help="no systemd scope (a machine without a user "
                            "manager): no memory cap and no memory peak")
        p.add_argument("--cpus", metavar="LIST",
                       help="pin to these CPUs (default: the P-cores)")
        p.add_argument("--scale", default="full", choices=("full", "small"),
                       help="small: the cut-down workloads the harness's own "
                            "smoke runs; never compared with full")
        p.add_argument("--repeats", type=int, default=None,
                       help="repeats per workload (default: its row's)")

    p = sub.add_parser("list", help="the workloads")
    common(p)
    p = sub.add_parser("run", help="run workloads and record them")
    common(p)
    p.add_argument("workloads", nargs="*")
    p.add_argument("--dry-run", action="store_true",
                   help="the plan, the fixtures to build, the expected time")
    p.add_argument("--busy-ok", action="store_true",
                   help="run on a busy machine; the record is marked busy")
    p.add_argument("--profile", action="store_true",
                   help="one workload's setup and one body under "
                        "tools/profile.sh --app")
    p = sub.add_parser("compare", help="the latest record against a baseline")
    common(p)
    p.add_argument("workloads", nargs="*")
    p.add_argument("--against", default="last",
                   help="last (default), a git revision, or a history file")
    p.add_argument("--new", metavar="FILE",
                   help="compare this file's last record instead of the "
                        "history's latest")
    p.add_argument("--ab", nargs=2, metavar=("BIN_A", "BIN_B"),
                   help="run two binaries alternately, ABAB, and compare")
    p.add_argument("--json", metavar="PATH", help="--ab: write the result")
    p = sub.add_parser("fixtures", help="build fixtures, time nothing")
    common(p)
    p.add_argument("names", nargs="*")
    args = ap.parse_args(argv)
    if args.repeats is not None and args.repeats < 1:
        ap.error("--repeats must be at least 1")
    try:
        return {"list": cmd_list, "run": cmd_run, "compare": cmd_compare,
                "fixtures": cmd_fixtures}[args.cmd](args)
    except Refusal as e:
        print(f"bench: refused: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
