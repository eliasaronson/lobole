#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Random designs against headless instances (docs/modeling/README.md#random-designs).

    tools/fuzz.py run [--from 0] [--count 1000] [--jobs N] [--timeout 90]
                      [--out scratch/fuzz/<stamp>/] [--binary build/lobole]
    tools/fuzz.py one <seed> [--design] [--minimise] [--asan] [--keep]
                      [--out CASE.jsonl]
    tools/fuzz.py replay <case.jsonl>
    tools/fuzz.py case RUN/<seed>.json [--out CASE.jsonl]
    tools/fuzz.py self-test

`run` writes one JSON file per seed and a report.md (the outcome table, the
gists by count, the slowest seeds). Each worker owns one headless instance
and runs its seeds one after another, `new_project` between them; the
instance is replaced every 50 seeds and after any failure, and a seed that
fails is run again in a fresh instance before it is reported -- clean there,
it is `leak`, and the seed before it is named. `--jobs` defaults from free
memory (tools/build-jobs), never from the core count. A run of 1000 seeds is
longer than an agent's 600 s tool cap: start it in the background. The
workers are waited on by pid, and an instance is killed only by its pid.

`one --minimise` cuts a failing design down to the steps its failure needs
and writes the case as a --record session with a header line, which
tests/fuzz_corpus_smoke.py replays and python/lobole/replay.py plays by
hand. `replay` runs a case with the checks applied. `case` writes a run's
seed file as a case, cut at its failing step: for a failure already fixed,
which a minimiser has nothing left to reproduce.
"""
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "python"))  # the lobole package

import fuzz_design as fd  # noqa: E402

REPLACE_EVERY = 50


def default_binary():
    return os.path.join(ROOT, "build", "lobole")


def default_jobs():
    try:
        out = subprocess.run([os.path.join(ROOT, "tools", "build-jobs"),
                              "1200"], capture_output=True, text=True,
                             check=True).stdout.strip()
        return max(1, int(out))
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 1


class Instance:
    """One headless app for a worker: smoke_app's isolation (scratch XDG
    dirs, LOBOLE_EXIT_WITH_PARENT), plus a scratch path to save into and
    a per-call deadline."""

    def __init__(self, binary, timeout, record=None, env=None):
        from smoke_app import launch_app
        from lobole.client import Client
        args = ["--record", record] if record else []
        self.app = launch_app(binary, extra_args=args, env=env)
        self.d = Client.wait_for(self.app.port, strict=True)
        self.d.scratch_path = os.path.join(self.app.tmp, "fuzz.dsgn")
        self.timeout = timeout
        self.deadline = None
        outer = self

        class Timed:
            """d.request with the seed's remaining time as its timeout."""
            scratch_path = self.d.scratch_path

            def request(self, method, params=None):
                left = outer.timeout if outer.deadline is None else \
                    max(1.0, outer.deadline - time.time())
                return outer.d.request(method, params or {}, timeout=left)

        self.timed = Timed()

    def stop(self):
        try:
            self.d.close()
        except Exception:  # noqa: BLE001
            pass
        self.app.terminate()
        try:
            self.app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.app.kill()  # by its pid, which is the Popen's
            self.app.wait(timeout=10)

    def log_tail(self, n=30):
        return "\n".join(self.app.log().splitlines()[-n:])

    def crash_file(self):
        """The crash handler's own report of this instance, if it crashed:
        <state>/lobole/sessions/<started>-<pid>.crash, written as it died
        (docs/architecture.md#crashes-and-stalls). Its text, or ''."""
        import glob
        for path in glob.glob(os.path.join(self.app.tmp, "state", "lobole",
                                           "sessions", "*.crash")):
            try:
                with open(path) as f:
                    return f.read()
            except OSError:
                pass
        return ""


def run_one(inst, seed):
    """One seed on `inst`; (outcome, design, seconds)."""
    t0 = time.time()
    inst.deadline = t0 + inst.timeout
    try:
        outcome, design = fd.generate_and_run(inst.timed, seed)
    except (ConnectionError, OSError) as e:
        cls = "hang" if "timed out" in str(e) else "crash"
        outcome = {"class": cls, "message": f"{e}\n{inst.log_tail()}",
                   "gist": fd.gist(cls, str(e)), "step": None, "limits": []}
        design = []
    finally:
        inst.deadline = None
    if outcome["class"] in ("crash", "hang"):
        outcome["log"] = inst.log_tail(60)
        # Give a dying instance a moment to finish writing its report.
        try:  # the Popen's own wait: AppProcess.wait removes the scratch
            inst.app._proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass
        outcome["crash_report"] = inst.crash_file()
    return outcome, design, time.time() - t0


def worker(binary, seeds, out, timeout):
    """Run `seeds` on one instance, replacing it every REPLACE_EVERY seeds
    and after any failure; a failing seed is rerun fresh."""
    inst = None
    used = 0
    previous = None
    try:
        for seed in seeds:
            if inst is None or used >= REPLACE_EVERY:
                if inst:
                    inst.stop()
                inst = Instance(binary, timeout)
                used = 0
            outcome, design, secs = run_one(inst, seed)
            used += 1
            if outcome["class"] not in ("clean",):
                # Fresh, and once more: clean there is state the previous
                # seed left behind.
                inst.stop()
                inst = Instance(binary, timeout)
                again, design2, secs2 = run_one(inst, seed)
                inst.stop()
                inst = None
                if again["class"] == "clean" and used > 1:
                    outcome = {"class": "leak", "step": outcome["step"],
                               "message": f"failed after seed {previous} "
                                          f"({outcome['class']}: "
                                          f"{outcome['message']}), clean in "
                                          f"a fresh instance",
                               "gist": fd.gist("leak", outcome["gist"]),
                               "limits": outcome["limits"],
                               "after_seed": previous}
                else:
                    outcome, design, secs = again, design2, secs2
            report = outcome.pop("crash_report", "")
            if report:
                crash = os.path.join(out, f"{seed}.crash")
                with open(crash, "w") as f:
                    f.write(report)
                outcome["crash_report"] = crash
            record = {"seed": seed, "generator": fd.GENERATOR_VERSION,
                      "seconds": round(secs, 3), **outcome,
                      "design": design}
            with open(os.path.join(out, f"{seed}.json"), "w") as f:
                json.dump(record, f, indent=1)
            previous = seed
    finally:
        if inst:
            inst.stop()


def report(out, seeds, started):
    rows = []
    for s in seeds:
        try:
            with open(os.path.join(out, f"{s}.json")) as f:
                rows.append(json.load(f))
        except (OSError, ValueError):
            rows.append({"seed": s, "class": "missing", "gist": "",
                         "seconds": 0, "limits": []})
    by_class = {}
    for r in rows:
        by_class.setdefault(r["class"], []).append(r)
    gists = {}
    for r in rows:
        if r["class"] not in ("clean",):
            gists.setdefault(r["gist"] or r["class"], []).append(r["seed"])
        for g in r.get("limits", []):
            gists.setdefault(g, []).append(r["seed"])
    lines = [f"# Fuzz run: {len(rows)} seeds",
             "", f"Generator version {fd.GENERATOR_VERSION}; "
             f"{time.time() - started:.0f} s wall.", "",
             "| outcome | seeds | a finding? |", "|---|---|---|"]
    for cls in sorted(by_class, key=lambda c: -len(by_class[c])):
        finding = ("yes" if cls in fd.FINDINGS else
                   "the generator's" if cls == "generator" else "no")
        lines.append(f"| `{cls}` | {len(by_class[cls])} | {finding} |")
    lines += ["", "## By gist", "", "| count | gist | seeds |",
              "|---|---|---|"]
    for g, ss in sorted(gists.items(), key=lambda kv: -len(kv[1])):
        lines.append(f"| {len(ss)} | {g.replace('|', '/')} | "
                     f"{', '.join(map(str, ss[:8]))}"
                     f"{' ...' if len(ss) > 8 else ''} |")
    lines += ["", "## Slowest", "", "| seed | s | outcome |", "|---|---|---|"]
    for r in sorted(rows, key=lambda r: -r.get("seconds", 0))[:10]:
        lines.append(f"| {r['seed']} | {r.get('seconds', 0)} | "
                     f"{r['class']} |")
    text = "\n".join(lines) + "\n"
    with open(os.path.join(out, "report.md"), "w") as f:
        f.write(text)
    return text, by_class


def cmd_run(args):
    seeds = list(range(args.start, args.start + args.count))
    out = args.out or os.path.join(ROOT, "scratch", "fuzz",
                                   time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    jobs = max(1, min(args.jobs or default_jobs(), len(seeds)))
    started = time.time()
    procs = []
    for j in range(jobs):
        mine = seeds[j::jobs]
        if not mine:
            continue
        procs.append(subprocess.Popen(
            [sys.executable, os.path.abspath(__file__), "_worker",
             "--binary", args.binary, "--out", out, "--timeout",
             str(args.timeout), "--seeds", ",".join(map(str, mine))]))
    print(f"fuzz: {len(seeds)} seeds on {len(procs)} workers "
          f"(pids {' '.join(str(p.pid) for p in procs)}) into {out}",
          flush=True)
    try:
        for p in procs:
            p.wait()  # by pid
    except KeyboardInterrupt:
        for p in procs:
            p.send_signal(signal.SIGTERM)
        raise
    text, by_class = report(out, seeds, started)
    print(text)
    bad = sum(len(v) for k, v in by_class.items()
              if k in fd.FINDINGS or k in ("generator", "missing"))
    return 1 if bad else 0


def record_case(binary, design, outcome, seed, path, timeout):
    """The minimised design replayed into a fresh instance with --record:
    a header line, then the literal session."""
    tmp = tempfile.mkdtemp(prefix="fuzz_record_")
    rec = os.path.join(tmp, "session.jsonl")
    inst = Instance(binary, timeout, record=rec)
    try:
        fd.run_design(inst.timed, design)
    finally:
        inst.stop()
    header = {"fuzz": {"seed": seed, "class": outcome["class"],
                       "gist": outcome["gist"],
                       "generator": fd.GENERATOR_VERSION,
                       "design": design}}
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w") as f:
        f.write(json.dumps(header) + "\n")
        try:
            with open(rec) as r:
                f.write(r.read())
        except OSError:
            pass
    shutil.rmtree(tmp, ignore_errors=True)


def cmd_one(args):
    binary = args.binary
    env = None
    if args.asan:
        binary = os.path.join(ROOT, "build-asan", "lobole")
        if not os.access(binary, os.X_OK):
            print(f"fuzz: no sanitized app at {binary}; build it with "
                  f"cmake -B build-asan -G Ninja -DENABLE_SANITIZERS=ON && "
                  f"ninja -C build-asan designer", file=sys.stderr)
            return 2
        env = dict(os.environ, ASAN_OPTIONS="halt_on_error=0",
                   UBSAN_OPTIONS="halt_on_error=0")
    inst = Instance(binary, args.timeout, env=env)
    try:
        outcome, design, secs = run_one(inst, args.seed)
        print(json.dumps({"seed": args.seed, "seconds": round(secs, 2),
                          **{k: v for k, v in outcome.items()
                             if k not in ("log", "crash_report")}}, indent=1))
        if outcome.get("log"):
            print(outcome["log"])
        if outcome.get("crash_report"):
            print("--- the crash handler's report ---\n" +
                  outcome["crash_report"])
        if args.design:
            for step in design:
                print(f"step {step['k']}: {step['op']}"
                      f"{' (limit)' if step.get('limit') else ''}")
                for c in step["calls"]:
                    print("   ", json.dumps(c["params"]))
        if args.keep:
            print(f"fuzz: the instance is still up: port {inst.app.port}, "
                  f"pid {inst.app.pid}; it exits with this process "
                  f"(press Enter to stop it)")
            try:
                input()
            except EOFError:
                pass
    finally:
        inst.stop()
    if not args.minimise or outcome["class"] == "clean":
        return 0 if outcome["class"] in ("clean", "limit") else 1
    want = (outcome["class"], outcome["gist"])
    fresh_each = outcome["class"] in ("crash", "hang")
    holder = {"inst": None}

    def reproduces(cand):
        if fresh_each or holder["inst"] is None:
            if holder["inst"]:
                holder["inst"].stop()
            holder["inst"] = Instance(binary, args.timeout, env=env)
        inst = holder["inst"]
        inst.deadline = time.time() + args.timeout
        try:
            got = fd.run_design(inst.timed, cand)
        except (ConnectionError, OSError) as e:
            cls = "hang" if "timed out" in str(e) else "crash"
            got = {"class": cls, "gist": fd.gist(cls, str(e))}
            inst.stop()
            holder["inst"] = None
        finally:
            inst.deadline = None
        return (got["class"], got["gist"]) == want

    small = fd.minimise(design, outcome, reproduces)
    if holder["inst"]:
        holder["inst"].stop()
    path = args.out or os.path.join(
        ROOT, "scratch", "fuzz", "cases",
        f"{outcome['class']}-{args.seed}.jsonl")
    record_case(binary, small, outcome, args.seed, path, args.timeout)
    print(f"fuzz: minimised to {len(small)} of {len(design)} steps "
          f"({', '.join(s['op'] for s in small)}): {path}")
    return 1


def cmd_case(args):
    """A run's seed file as a case: its design up to the step that failed
    (all of it for an end-of-design failure), recorded like a minimised one.
    For a failure already fixed, where `one --minimise` has nothing left to
    reproduce."""
    with open(args.seed_json) as f:
        rec = json.load(f)
    design = rec["design"]
    if rec.get("step") is not None:
        design = [s for s in design if s["k"] <= rec["step"]]
    path = args.out or os.path.join(
        ROOT, "tests", "fuzz", "cases", f"{rec['class']}-{rec['seed']}.jsonl")
    record_case(args.binary, design, rec, rec["seed"], path, args.timeout)
    print(f"fuzz: {len(design)} steps of seed {rec['seed']} "
          f"({rec['class']}: {rec['gist']}): {path}")
    return 0


def replay_case(d, path):
    """A case file's design, run with the checks. Returns the outcome."""
    with open(path) as f:
        header = json.loads(f.readline())["fuzz"]
    return fd.run_design(d, header["design"]), header


def cmd_replay(args):
    inst = Instance(args.binary, args.timeout)
    try:
        inst.deadline = time.time() + args.timeout
        outcome, header = replay_case(inst.timed, args.case)
    finally:
        inst.stop()
    print(f"{args.case}: recorded {header['class']} ({header['gist']}); "
          f"now {outcome['class']} {outcome['message']}")
    return 0 if outcome["class"] == "clean" else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawTextHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--binary", default=default_binary())
    common.add_argument("--timeout", type=float, default=90.0,
                        help="seconds a seed may take before it is a hang")
    r = sub.add_parser("run", parents=[common])
    r.add_argument("--from", dest="start", type=int, default=0)
    r.add_argument("--count", type=int, default=1000)
    r.add_argument("--jobs", type=int, default=0)
    r.add_argument("--out", default="")
    o = sub.add_parser("one", parents=[common])
    o.add_argument("seed", type=int)
    o.add_argument("--minimise", action="store_true")
    o.add_argument("--asan", action="store_true")
    o.add_argument("--keep", action="store_true")
    o.add_argument("--design", action="store_true",
                   help="print the design's steps and their calls")
    o.add_argument("--out", default="")
    p = sub.add_parser("replay", parents=[common])
    p.add_argument("case")
    c = sub.add_parser("case", parents=[common])
    c.add_argument("seed_json", help="a run's <seed>.json")
    c.add_argument("--out", default="")
    sub.add_parser("self-test")
    w = sub.add_parser("_worker", parents=[common])
    w.add_argument("--seeds", required=True)
    w.add_argument("--out", required=True)
    args = ap.parse_args()
    args.binary = os.path.abspath(getattr(args, "binary", default_binary()))
    if args.cmd == "run":
        return cmd_run(args)
    if args.cmd == "one":
        return cmd_one(args)
    if args.cmd == "replay":
        return cmd_replay(args)
    if args.cmd == "case":
        return cmd_case(args)
    if args.cmd == "self-test":
        return fd.self_test()
    worker(args.binary, [int(s) for s in args.seeds.split(",")], args.out,
           args.timeout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
