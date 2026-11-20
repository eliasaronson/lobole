#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""What a merge has to run, from the paths it changes.

  merge_plan.py classify --mode M [--no-skip] CHANGED
      prints the decision, one `key=value` line per suite at the end
  merge_plan.py select --build BUILD CHANGED OUT
      writes the headless tests a subset plan runs, one exact name a line
      (ctest --tests-from-file), and prints how many and why

CHANGED is a file of repo-relative paths, one a line. M is `fast` (a plain
merge: tools/test-fast.sh), `full` (the headless suite and the GUI suite,
and the heavy suites whose ground the merge touches) or `release` (every
suite, whatever changed). --no-skip runs the mode's every suite, whatever
changed.

The map is CONSERVATIVE: a path it does not know is `code`, and code runs
everything the mode runs. It narrows only where the skipped tests cannot
observe the change (docs/ui/testing.md#what-a-merge-runs):

  docs      docs/**, *.md     the [docs] and other convention cases, the
                              format gates, and the tests that read a page
                              as input (DOC_READERS). Nothing else does.
  tests     a test source     the cases that file declares (Catch2's own
                              listing says which file each case is in), a
                              smoke script's own ctest entries; a helper
                              no test names directly is all of them
  python    python/lobole/**  every python test (they all drive the app
                              through it), and the heavy suites
  tools     tools/**          the tests that name the tool; the GUI suite
                              when it is the GUI runner's; code when the
                              build reads it (tools/icons)
  code      anything else     everything the mode runs

The heavy suites in a `full` merge, each run only when a path is in its
ground (and always by `release`):

  generators       code and python/lobole/** and generators/** (a big
                   generator drives nearly every subsystem, CAM, drawings
                   and studies included); not a GUI test's own source
  action_matrix    code outside CAM, FEM, drawings, the renderers and the
                   GUI tests (it sends modeling actions to a headless
                   app), python/lobole/**, its corpus (tests/action_matrix,
                   tests/golden, generators/**), tools/action_matrix.py
  reader_fuzz_all  the readers and serializers (src/core/io/**), the
                   goldens and reader cases, the mutators (tests/mutate),
                   tests/test_reader_fuzz.cpp, tools/reader-fuzz.py, and
                   the kernel (shell.nix, nix/**)
  fem_reference    the FEM code (src/core/fem/**, src/femgpu/**), the
                   material library (src/cad/CADMaterials*), the mesher's
                   kernel (shell.nix, nix/**), the cases
                   (tests/fem_reference/**), tools/fem_reference.py, the
                   simulation RPC (src/remote/RemoteSimulation*) and the
                   page it writes; only its own ground, as reader_fuzz_all
"""

import fnmatch
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The tools that run the suites. Running the suites a merge runs exercises
# them; the GUI runner's own need the GUI suite.
GUI_RUNNER = {"gui-tests-xvfb.sh", "gui_shard.py", "gui_shard_record.py",
              "xvfb-exec.sh", "xvfb-run-path"}

DOC_NAMED_TESTS = ["format_version_gate", "generator_table_gate",
                   "The version history covers every kVersion"]
# The pages a test reads as input, beyond the conventions' scan of all of
# them. Pages are cited in test comments everywhere; a citation is not a
# read, so this is a list, not a grep.
DOC_READERS = {"docs/modeling/reference-parts.md": ["reference_parts"]}

MATRIX_NOT = ["src/core/cam/*", "src/core/fem/*", "src/femgpu/*",
              "src/core/drawing/*", "src/render/*", "src/shaders/*",
              "src/app/guitests/*", "src/remote/RemoteCam*",
              "src/remote/RemoteDrawing*", "src/ui/Cam*", "src/ui/Drawing*"]
MATRIX_ALSO = ["python/lobole/*", "tests/action_matrix*", "tests/golden/*",
               "generators/*", "tools/action_matrix.py",
               "tests/action_matrix_smoke.py"]
READER_GROUND = ["src/core/io/*", "tests/golden/*", "tests/reader_cases/*",
                 "tests/mutate/*", "tests/test_reader_fuzz.cpp",
                 "tests/test_reader_cases.cpp", "tools/reader-fuzz.py",
                 "shell.nix", "nix/*"]
FEM_REFERENCE_GROUND = ["src/core/fem/*", "src/femgpu/*", "src/cad/CADMaterials*",
                        "shell.nix", "nix/*", "tests/fem_reference/*",
                        "tools/fem_reference.py",
                        "src/remote/RemoteSimulation*",
                        "docs/simulation/reference-cases.md"]
GENERATORS_NOT = ["src/app/guitests/*"]
GENERATORS_ALSO = ["python/lobole/*", "generators/*", "tests/generator_runs.py",
                   "tools/generator_tests.py"]


def match(path, patterns):
    return any(fnmatch.fnmatch(path, p) for p in patterns)


def kind(path):
    if path.startswith("docs/") or path.endswith(".md"):
        return "docs"
    if path.startswith("python/lobole/"):
        return "python"
    if path.startswith("tools/"):
        # The build reads these (the icon font); a test cannot see the
        # rest except by naming them.
        if path.startswith("tools/icons/"):
            return "code"
        return "tools"
    if path.startswith("tests/"):
        if path == "tests/CMakeLists.txt" or not re.search(
                r"\.(cpp|py)$", path) or "/" in path[len("tests/"):]:
            return "code"  # registration, data, goldens, helpers in a dir
        return "tests"
    return "code"


def read_changed(path):
    with open(path) as f:
        return [ln.strip() for ln in f if ln.strip()]


def classify(mode, no_skip, changed):
    kinds = {}
    for p in changed:
        kinds.setdefault(kind(p), []).append(p)
    out = {}
    why = []
    code = no_skip or "code" in kinds or mode == "release"
    narrow = not code and bool(kinds)
    if not changed:
        why.append("no changed paths: runs what the mode runs")
        code = True
        narrow = False
    for k in ("docs", "tests", "python", "tools", "code"):
        if k in kinds:
            ps = kinds[k]
            more = " ..." if len(ps) > 4 else ""
            why.append("%-7s %d: %s%s" % (k, len(ps), " ".join(ps[:4]), more))
    if mode == "release":
        why.append("--release: every suite, whatever changed")
    elif no_skip:
        why.append("--no-skip: every suite the mode runs")

    docs_only = set(kinds) == {"docs"} and not code
    out["build"] = "if-stale" if docs_only else "yes"
    if code:
        out["headless"] = "all"
    elif narrow:
        out["headless"] = "subset"
    else:
        out["headless"] = "all"

    gui_runner = any(os.path.basename(p) in GUI_RUNNER
                     for p in kinds.get("tools", []))
    if mode == "fast":
        out["gui"] = "no"
    else:
        out["gui"] = "yes" if (code or gui_runner) else "no"

    def heavy(name, ground, exclude=()):
        if mode == "release":
            return "yes", "--release"
        if mode != "full":
            return "no", "not a --full merge"
        if no_skip:
            return "yes", "--no-skip"
        hit = [p for p in changed
               if (kind(p) == "code" and not match(p, exclude)) or
               match(p, ground)]
        if not changed:
            return "yes", "no changed paths to judge by"
        if hit:
            return "yes", "touches its ground: " + " ".join(hit[:3]) + (
                " ..." if len(hit) > 3 else "")
        return "no", "nothing it covers changed"

    for name, ground, exclude in (
            ("generators", GENERATORS_ALSO, GENERATORS_NOT),
            ("action_matrix", MATRIX_ALSO, MATRIX_NOT),
            ("reader_fuzz_all", READER_GROUND, None),
            ("fem_reference", FEM_REFERENCE_GROUND, None)):
        if name in ("reader_fuzz_all", "fem_reference"):
            # Only its own ground: a code change elsewhere is what the
            # sample in the headless suite is for.
            v, r = heavy(name, ground, exclude=["*"])
        else:
            v, r = heavy(name, ground, exclude)
        out[name] = v
        why.append("%-15s %-3s %s" % (name, v, r))
    if out["headless"] == "subset":
        why.append("headless        subset: only what the changed paths "
                   "can reach (tools/merge_plan.py select)")
    elif docs_only:
        pass
    return out, why


def ctest_tests(build):
    r = subprocess.run(["ctest", "-N", "--show-only=json-v1"], cwd=build,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("merge_plan: ctest -N failed in %s: %s" % (build, r.stderr))
    tests = []
    for t in json.loads(r.stdout)["tests"]:
        props = {p["name"]: p["value"] for p in t.get("properties", [])}
        tests.append({"name": t["name"], "command": t.get("command", []),
                      "labels": props.get("LABELS", [])})
    return tests


def catch_files(build):
    """Catch2 case name -> the source file it is declared in."""
    exe = os.path.join(build, "tests", "designer_tests")
    if not os.path.exists(exe):
        return None
    r = subprocess.run([exe, "--list-tests", "--reporter", "json",
                        "--verbosity", "high"], capture_output=True,
                       text=True, cwd=os.path.dirname(exe))
    try:
        listing = json.loads(r.stdout)["listings"]["tests"]
    except (ValueError, KeyError):
        return None
    out = {}
    for t in listing:
        loc = t.get("source-location", {}).get("filename", "")
        # __FILE__ as the compiler was given it: relative to the build
        # directory (the launcher's CCACHE_BASEDIR makes paths relative).
        out[t["name"]] = os.path.normpath(os.path.join(build, loc))
    return out


def mentions(name, files):
    """The test sources under tests/ that name `name` (a tool, a page)."""
    hits = []
    for f in files:
        try:
            with open(f, errors="replace") as fh:
                if name in fh.read():
                    hits.append(f)
        except OSError:
            continue
    return hits


def select(build, changed, out_path):
    tests = ctest_tests(build)
    names = set()
    why = []
    tests_dir = os.path.join(ROOT, "tests")
    py_sources = [os.path.join(tests_dir, f) for f in os.listdir(tests_dir)
                  if f.endswith(".py")]
    cmake = os.path.join(tests_dir, "CMakeLists.txt")

    def by_script(script):
        return [t["name"] for t in tests
                if any(os.path.normpath(c) == os.path.normpath(script)
                       for c in t["command"])]

    def named_in_cmake(text):
        """ctest tests whose add_test block in tests/CMakeLists.txt names
        `text` (a tool passed as the command, a page given as input)."""
        found = []
        src = open(cmake).read()
        for m in re.finditer(r"add_test\(NAME (\S+)(.*?)\)\n", src, re.S):
            if text in m.group(2):
                found.append(m.group(1))
        return found

    every_python = [t["name"] for t in tests
                    if any(c.endswith(".py") for c in t["command"])]

    def by_scripts(paths):
        """Their ctest entries; a script with none is a helper the others
        import (smoke_app.py), so every python test."""
        got = []
        for f in paths:
            own = by_script(f)
            got += own if own else every_python
        return got
    files = None
    for p in changed:
        k = kind(p)
        full = os.path.join(ROOT, p)
        base = os.path.basename(p)
        if k == "docs":
            got = [t["name"] for t in tests if "convention" in t["labels"]]
            got += [n for n in DOC_NAMED_TESTS
                    if any(t["name"] == n for t in tests)]
            got += [n for n in DOC_READERS.get(p, [])
                    if any(t["name"] == n for t in tests)]
            names.update(got)
            why.append("%s: %d (conventions, format gates, its readers)"
                       % (p, len(got)))
        elif k == "tests" and p.endswith(".py"):
            got = by_script(full)
            if not got:
                # A helper the scripts import (smoke_app.py, host_load.py).
                got = every_python
                why.append("%s: a helper, so every python test (%d)"
                           % (p, len(got)))
            else:
                why.append("%s: its own %d" % (p, len(got)))
            names.update(got)
        elif k == "tests":
            if files is None:
                files = catch_files(build)
            if files is None:
                return None, why + ["%s: no Catch2 listing; run all" % p]
            got = [n for n, f in files.items()
                   if os.path.normpath(f) == os.path.normpath(full)]
            ctest_names = {t["name"] for t in tests}
            got = [n for n in got if n in ctest_names]
            if not got:
                return None, why + [
                    "%s: declares no case ctest runs (a helper); run all" % p]
            names.update(got)
            why.append("%s: its %d case(s)" % (p, len(got)))
        elif k == "python":
            names.update(every_python)
            why.append("%s: every python test (%d)" % (p, len(every_python)))
        elif k == "tools":
            stem = os.path.splitext(base)[0]
            got = by_scripts(set(mentions(base, py_sources) + (
                mentions(stem, py_sources) if len(stem) > 5 else [])))
            got += named_in_cmake(base)
            got = sorted(set(got))
            names.update(got)
            why.append("%s: %d test(s) name it" % (p, len(got)))
        else:
            return None, why + ["%s: code; run all" % p]
    # The conventions always: they scan the tree for what a change forgot.
    names.update(t["name"] for t in tests if "convention" in t["labels"])
    with open(out_path, "w") as f:
        for n in sorted(names):
            f.write(n + "\n")
    return len(names), why


def main():
    a = sys.argv[1:]
    if a[:1] == ["classify"]:
        mode, no_skip, rest = "fast", False, []
        i = 1
        while i < len(a):
            if a[i] == "--mode":
                mode = a[i + 1]
                i += 2
                continue
            if a[i] == "--no-skip":
                no_skip = True
            else:
                rest.append(a[i])
            i += 1
        if mode not in ("fast", "full", "release") or len(rest) != 1:
            sys.exit(__doc__)
        out, why = classify(mode, no_skip, read_changed(rest[0]))
        for w in why:
            print("merge-plan: " + w)
        for k, v in out.items():
            print("%s=%s" % (k, v))
        return 0
    if a[:1] == ["select"] and len(a) == 5 and a[1] == "--build":
        n, why = select(os.path.abspath(a[2]), read_changed(a[3]), a[4])
        for w in why:
            print("merge-plan: " + w)
        if n is None:
            print("merge-plan: the subset cannot be named; the whole suite runs")
            return 3
        print("merge-plan: %d headless test(s) in the subset" % n)
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
