#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The performance harness works: it launches, runs, records and compares,
and refuses what it should (docs/performance/README.md#its-own-test).

    bench_harness_smoke.py <path-to-designer-binary>

Runs `startup/headless` and `boolean/plate-holes` at --scale small, twice,
into a private history, checks each record against the schema, compares the
second with the first, and has compare refuse a busy baseline and one from
another build type, run refuse an unknown workload, and a missing input
skip naming its path. It asserts no duration: a number from a loaded test
machine says nothing, and a gate that fails on noise gets ignored. The harness's own state (its history, its GPU
cache) goes to a private directory, never the user's.
"""
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BENCH = os.path.join(ROOT, "tools", "bench.py")
sys.path[:0] = [os.path.join(ROOT, "tools"), os.path.join(ROOT, "python"),
                HERE]
import bench  # noqa: E402

WORKLOADS = ["startup/headless", "boolean/plate-holes"]


def expect(ok, what):
    if not ok:
        raise AssertionError(what)


def run(env, *args, ok=(0,)):
    r = subprocess.run([sys.executable, BENCH, *args], env=env, text=True,
                       capture_output=True, timeout=600)
    said = r.stdout + r.stderr
    if r.returncode not in ok:
        raise AssertionError(f"bench {' '.join(args)} exited {r.returncode}"
                             f", not {ok}:\n{said}")
    return r.returncode, said


def check_record(rec, scoped):
    for key, kind in (("schema", int), ("host_key", str), ("host", dict),
                      ("build", dict), ("machine", dict), ("busy", bool),
                      ("busy_reasons", list), ("injected", dict),
                      ("options", dict), ("workloads", dict),
                      ("started", float), ("finished", float)):
        expect(isinstance(rec.get(key), kind),
               f"record[{key!r}] is {rec.get(key)!r}, not a {kind.__name__}")
    expect(rec["schema"] == bench.SCHEMA, f"schema {rec['schema']}")
    for key in ("commit", "dirty", "build_type", "binary", "binary_mtime"):
        expect(key in rec["build"], f"build has no {key}")
    for key in ("cpu", "p_cpus", "e_cpus", "ram_gb", "gpu", "key"):
        expect(key in rec["host"], f"host has no {key}")
    for key in ("load1", "load_limit", "compilers", "other_instances",
                "governors", "ac_power", "load_after"):
        expect(key in rec["machine"], f"machine has no {key}")
    expect(rec["options"]["scale"] == "small", "scale small")
    expect(rec["busy"] == bool(rec["busy_reasons"]),
           "busy exactly when there is a reason")
    expect(sorted(rec["workloads"]) == sorted(WORKLOADS),
           f"workloads {sorted(rec['workloads'])}")
    for name, w in rec["workloads"].items():
        expect(w["status"] == "ok", f"{name}: {w['status']} "
               f"{w.get('reason', '')} {w.get('failures')}")
        expect(len(w["samples_s"]) == 2 and all(
            isinstance(s, float) and s > 0 for s in w["samples_s"]),
            f"{name}: two samples, {w['samples_s']}")
        expect(isinstance(w["median_s"], float)
               and isinstance(w["spread_s"], float),
               f"{name}: median and spread")
        expect(len(w["phase_samples"]) == 2, f"{name}: a phase dict a repeat")
        expect(w["cpus"], f"{name}: the CPU set it ran on")
    expect("launch_to_ping" in rec["workloads"]["startup/headless"]["phases"],
           "startup's phase")
    plate = rec["workloads"]["boolean/plate-holes"]
    expect(plate["phases"].get("executed") == 24,
           f"the small plate executes 24 features: {plate['phases']}")
    if scoped:
        expect(isinstance(plate["memory_peak_bytes"], int)
               and plate["memory_peak_bytes"] > 0,
               f"a scoped run records its peak: {plate['memory_peak_bytes']}")


def main():
    binary = os.path.abspath(sys.argv[1])
    tmp = tempfile.mkdtemp(prefix="designer_bench_smoke_")
    try:
        env = dict(os.environ,
                   XDG_STATE_HOME=os.path.join(tmp, "state"),
                   XDG_CACHE_HOME=os.path.join(tmp, "cache"))
        for k in bench.INJECTED:
            env.pop(k, None)
        history = os.path.join(tmp, "history.jsonl")
        # A run names the GPU from an --offscreen instance's ping, cached a
        # boot; seeding the cache keeps this test headless (no device).
        cache = os.path.join(tmp, "cache", "lobole", "bench")
        os.makedirs(cache)
        with open("/proc/sys/kernel/random/boot_id") as f:
            boot = f.read().strip()
        with open(os.path.join(cache, "gpu.json"), "w") as f:
            json.dump({"boot": boot, "gpu": "", "why": "not probed: the "
                       "harness's smoke"}, f)
        scoped, why = bench.scope_available()
        common = ["--binary", binary, "--history", history,
                  "--scale", "small"]
        if not scoped:
            print(f"no systemd user scope here ({why}): --no-scope")
            common.append("--no-scope")

        # An unknown workload is refused, and `list` is named.
        _, said = run(env, "run", "no/such-workload", *common, ok=(2,))
        expect("bench.py list" in said, f"the refusal names list: {said}")

        for _ in range(2):
            _, said = run(env, "run", *WORKLOADS, *common, "--repeats", "2",
                          "--busy-ok")
            print(said)
        with open(history) as f:
            records = [json.loads(line) for line in f if line.strip()]
        expect(len(records) == 2, f"two records, not {len(records)}")
        for rec in records:
            check_record(rec, scoped)

        # compare, against a copy of the first record that is not busy (a
        # test machine is busy; whether compare calls it a change is noise).
        def baseline(name, **change):
            rec = copy.deepcopy(records[0])
            for k, v in change.items():
                if k == "build_type":
                    rec["build"]["build_type"] = v
                else:
                    rec[k] = v
            path = os.path.join(tmp, name)
            with open(path, "w") as f:
                f.write(json.dumps(rec) + "\n")
            return path

        quiet = baseline("quiet.jsonl", busy=False, busy_reasons=[])
        code, said = run(env, "compare", *common, "--against", quiet,
                         ok=(0, 1))
        for name in WORKLOADS:
            expect(name in said and "->" in said,
                   f"compare lists {name}: {said}")
        print(said)

        # A busy record is never a baseline.
        busy = baseline("busy.jsonl", busy=True,
                        busy_reasons=["a planted reason"])
        _, said = run(env, "compare", *common, "--against", busy, ok=(2,))
        expect("BUSY" in said and "never a baseline" in said,
               f"a busy baseline is refused: {said}")

        # Nor is a record of another build type.
        other = baseline("debug.jsonl", busy=False, busy_reasons=[],
                         build_type="Debug")
        _, said = run(env, "compare", *common, "--against", other, ok=(2,))
        expect("build type" in said, f"another build type is refused: "
               f"{said}")

        # A missing input is a skip naming its path, never a substitute.
        gone = os.path.join(tmp, "no-such.step")
        row = bench.W.Workload("step/import/gone", "", "headless",
                               lambda ctx: None, inputs=(gone,))
        why = bench.Runner(binary, False, None, "small", 1).skip_reason(row)
        expect(why == f"input missing: {gone}",
               f"a missing input is skipped naming it: {why!r}")

        # Nothing of the user's: the history went where it was told.
        expect(not os.path.exists(os.path.join(tmp, "state", "lobole",
                                               "bench")),
               "nothing was written to the default history")
        print("bench_harness_smoke: ok")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
