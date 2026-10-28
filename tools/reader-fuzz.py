#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Reader fuzzing by hand (docs/versioning/README.md#reader-fuzzing).

    tools/reader-fuzz.py case <id> [--name NAME] [--seed S] [--force]
    tools/reader-fuzz.py run [--seed S] [--family F] [--only TEXT] [--sample]
    tools/reader-fuzz.py families

`case` writes one case of the sweep -- its damaged file, or its damaged
tree -- to tests/reader_cases/<name>/input/ with an expect.json made from
what this build's reader does with it: `refused` with the message's file
and its first words, or `loads` with its counts and what it dropped. Edit
the substrings down to what the regression is about; test_reader_cases.cpp
loads every case directory. A case stores the BYTES, not the id, so a
changed operator never silently retargets an old regression.

`run` reruns the full sweep (every case) with another base seed and prints
the cases that fail: a deliberate audit, since ctest's seed is fixed. It
goes through tools/ctest's lock-free cousin, the test binary itself, one
family at a time (a family is one Catch2 test case).

Both drive build/tests/designer_tests, in the nix environment
(tools/nix-run). Nothing here writes outside tests/reader_cases/ and the
system temp dir.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "build", "tests", "designer_tests")
CASES = os.path.join(ROOT, "tests", "reader_cases")

# The Catch2 names of the families (tests/test_reader_fuzz.cpp), by the
# name a case id starts with.
FAMILIES = {
    "part": "part files",
    "assembly": "assembly files",
    "drawing": "drawing files",
    "job": "job files",
    "part-dir": "part directories",
    "assembly-dir": "assembly directories",
    "drawing-dir": "drawing directories",
    "job-dir": "job directories",
    "project": "projects",
    "settings": "settings",
    "profiles": "machine and print profiles",
    "crash": "crash reports",
    "instances": "instance registry",
    "recovery": "autosave recovery",
    "vault": "git vault and merge",
    "images": "images",
    "meshes": "meshes",
}


def run_tests(args, env):
    """The test binary under tools/nix-run, its output as text."""
    cmd = " ".join(_quote(a) for a in [BINARY] + args)
    full = dict(os.environ)
    full.update(env)
    proc = subprocess.run(
        [os.path.join(ROOT, "tools", "nix-run"), cmd],
        cwd=ROOT, env=full, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def _quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:60]


def cmd_case(a):
    family = a.id.split("/", 1)[0]
    if family not in FAMILIES:
        sys.exit(f"reader-fuzz: no family '{family}' (one of: "
                 f"{', '.join(sorted(FAMILIES))})")
    name = a.name or slug(a.id)
    case_dir = os.path.join(CASES, name)
    if os.path.exists(case_dir) and not a.force:
        sys.exit(f"reader-fuzz: {case_dir} exists (--force replaces it)")
    with tempfile.TemporaryDirectory(prefix="reader-case-") as tmp:
        # The file keeps its own name (a reader names it in a refusal); a
        # tree is written as a directory of the same name.
        file_part = a.id.split(":", 1)[0].split("/")
        base = file_part[1] if family in ("part", "assembly", "drawing",
                                          "job") else "tree.lob"
        out = os.path.join(tmp, base)
        rc, text = run_tests(["Reader fuzz: write one case"], {
            "LOBOLE_READER_FUZZ_CASE": a.id,
            "LOBOLE_READER_FUZZ_OUT": out,
            "LOBOLE_READER_FUZZ_SEED": str(a.seed),
        })
        if not os.path.exists(out):
            sys.exit(f"reader-fuzz: the case was not written:\n{text}")
        expect = {"family": family, "input": base, "id": a.id,
                  "seed": a.seed}
        outcome = parse_outcome(text)
        if outcome.get("check") is not None:
            expect["expect"] = "passes" if not outcome["check"] else "fails"
            if outcome["check"]:
                expect["message"] = [outcome["check"][:80]]
        elif outcome.get("refused") is not None:
            msg = outcome["refused"]
            expect["expect"] = "refused"
            # The file it names, and the start of what it says about it.
            head = msg.split(": ", 1)
            expect["message"] = [head[0]] + ([head[1][:40]]
                                             if len(head) > 1 else [])
        else:
            expect["expect"] = "loads"
            expect["counts"] = outcome.get("counts", {})
            if outcome.get("dropped"):
                expect["dropped"] = outcome["dropped"]
        if os.path.exists(case_dir):
            shutil.rmtree(case_dir)
        os.makedirs(os.path.join(case_dir, "input"))
        dest = os.path.join(case_dir, "input", base)
        if os.path.isdir(out):
            shutil.copytree(out, dest, symlinks=True)
        else:
            shutil.copy(out, dest)
        with open(os.path.join(case_dir, "expect.json"), "w") as f:
            json.dump(expect, f, indent=2, sort_keys=True)
            f.write("\n")
    print(f"wrote {case_dir}: {expect['expect']}")
    print(json.dumps(expect, indent=2, sort_keys=True))


def parse_outcome(text):
    out = {"counts": {}, "dropped": []}
    for line in text.splitlines():
        if line.startswith("check: "):
            out["check"] = line[len("check: "):]
        elif line.startswith("refused: "):
            out["refused"] = line[len("refused: "):]
        elif line.startswith("count "):
            _, what, n = line.split(" ", 2)
            out["counts"][what] = int(n)
        elif line.startswith("dropped "):
            out["dropped"].append(line[len("dropped "):].split(":", 1)[0]
                                  .strip())
    return out


def cmd_run(a):
    families = [a.family] if a.family else list(FAMILIES)
    failed = 0
    for fam in families:
        if fam not in FAMILIES:
            sys.exit(f"reader-fuzz: no family '{fam}'")
        # A sharded family's every case is several test cases: the `*`
        # takes them all (and an unsharded family's one).
        test = (f"Reader fuzz: {FAMILIES[fam]}" if a.sample else
                f"Reader fuzz (every case): {FAMILIES[fam]}*")
        env = {"LOBOLE_READER_FUZZ_SEED": str(a.seed)}
        if a.only:
            env["LOBOLE_READER_FUZZ_ONLY"] = a.only
        rc, text = run_tests([test], env)
        summary = [l for l in text.splitlines()
                   if l.startswith("reader fuzz ")]
        for l in summary[1:]:
            print(l)
        fails = [l.strip()[len("FAILED "):] for l in text.splitlines()
                 if l.strip().startswith("FAILED ")]
        print(summary[0] if summary else
              f"reader fuzz {fam}: no summary (exit {rc}; a crash? see "
              f"build/reader-fuzz-last-case.{fam})")
        for f in fails:
            print("  " + f)
        failed += len(fails) + (0 if summary else 1)
    sys.exit(1 if failed else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("case", help="write one case as a regression")
    c.add_argument("id")
    c.add_argument("--name")
    c.add_argument("--seed", type=int, default=0)
    c.add_argument("--force", action="store_true")
    r = sub.add_parser("run", help="the full sweep with another seed")
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--family")
    r.add_argument("--only")
    r.add_argument("--sample", action="store_true",
                   help="the fixed sample instead of every case")
    sub.add_parser("families", help="the family names a case id starts with")
    a = p.parse_args()
    if a.cmd == "case":
        cmd_case(a)
    elif a.cmd == "run":
        cmd_run(a)
    else:
        for k, v in FAMILIES.items():
            print(f"{k:14} {v}")


if __name__ == "__main__":
    main()
