#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""A passing suite, stamped with the exact tree and toolchain it passed on.

  suite_stamp.py write BUILD_DIR TREE KEY...    after a suite passed
  suite_stamp.py forget BUILD_DIR TREE KEY...   after it failed
  suite_stamp.py find BUILD_DIR OTHER TREE KEY...
                       exit 0 if BUILD_DIR holds a stamp for this tree and
                       key, made on the toolchain OTHER (a build tree) has
  suite_stamp.py toolchain BUILD_DIR            print the toolchain key
  suite_stamp.py clean REPO                     exit 0 if REPO's tracked
                       files match HEAD and nothing untracked lies where a
                       build or a suite could pick it up

tools/merge-branch.sh lands a branch and runs the suites on main. When the
branch was rebased onto main first, main's tree after the pick IS the tree
the worktree already tested, and running the suites again re-proves the same
result on the same bytes while main's build lock queues every other merge.
So tools/ctest and tools/gui-tests-xvfb.sh stamp a run that passed, and the
merge reuses a stamp only when everything a result depends on matches:

* TREE: `git rev-parse HEAD^{tree}`, taken before the run and again after
  it, with no tracked file modified at either moment (the caller checks; a
  dirty tree is never stamped). Equal trees are equal sources, tests,
  goldens and tools. A format bump that the landing numbers changes the
  tree, so its merge always runs.
* the toolchain: every store path the build tree was configured against
  (CMakeCache.txt), its build type and options, and every store path in
  the environment tools/nix-run gives the suites. A tree on another kernel
  or another Catch2 has another key.
* KEY: what ran -- the ctest arguments less the ones that do not change
  which tests run or how they are judged (-j, --output-on-failure, -V), or
  a name (`gui-suite`).

Stamps live in BUILD_DIR/suite-stamps/, one file per key, overwritten by
the next pass and deleted by a failure of the same key.
"""

import hashlib
import json
import os
import re
import sys

STORE = re.compile(r"/nix/store/[a-z0-9]{32}-[^/;:\"'\s]*")
# Arguments that change neither which tests run nor their verdict.
DROP_ALONE = {"--output-on-failure", "-V", "-VV", "--verbose",
              "--extra-verbose", "-Q", "--quiet", "--progress"}
DROP_WITH_VALUE = {"-j", "--parallel", "--test-dir"}


def normalize(args):
    out = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in DROP_ALONE:
            continue
        if a in DROP_WITH_VALUE:
            skip = True
            continue
        if re.match(r"^(-j\d+|--parallel=\d+|--test-dir=.*)$", a):
            continue
        out.append(a)
    return out


def toolchain(build_dir):
    h = hashlib.sha256()
    cache = os.path.join(build_dir, "CMakeCache.txt")
    paths, options = set(), []
    try:
        with open(cache) as f:
            for line in f:
                paths.update(m.split("/")[3] for m in STORE.findall(line))
                if re.match(r"^(CMAKE_BUILD_TYPE|ENABLE_[A-Z_]+|LOBOLE_[A-Z_]+)"
                            r":[A-Z]+=", line):
                    options.append(line.strip())
    except FileNotFoundError:
        return None
    # The environment the suites run in: tools/nix-run's cache of it, in the
    # checkout the build tree was configured from. The nix-shell derivation
    # itself is named for the checkout's path, so it is left out.
    src = None
    with open(cache) as f:
        for line in f:
            if line.startswith("CMAKE_HOME_DIRECTORY:"):
                src = line.split("=", 1)[1].strip()
    env_dir = os.path.join(src or "", ".nix-env")
    if os.path.isdir(env_dir):
        for name in sorted(os.listdir(env_dir)):
            if not name.endswith(".sh"):
                continue
            with open(os.path.join(env_dir, name)) as f:
                for m in STORE.findall(f.read()):
                    base = m.split("/")[3]
                    if not base.endswith("-nix-shell"):
                        paths.add(base)
    for p in sorted(paths):
        h.update(p.encode() + b"\n")
    for o in sorted(options):
        h.update(o.encode() + b"\n")
    return h.hexdigest()[:20]


# Untracked files here can change what is built or run (a globbed test.json,
# a new source a CMakeLists picks up); elsewhere (docs/plans, scratch/) not.
WATCHED = ("src/", "tests/", "generators/", "python/", "tools/", "nix/",
           "cmake/", "deps/", "assets/", "api/")


def clean(repo):
    import subprocess
    r = subprocess.run(["git", "-C", repo, "status", "--porcelain",
                        "--untracked-files=all"],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return False
    for line in r.stdout.splitlines():
        path = line[3:]
        if not line.startswith("??") or path.startswith(WATCHED):
            return False
    return True


def stamp_path(build_dir, key):
    name = hashlib.sha256("\0".join(key).encode()).hexdigest()[:16]
    return os.path.join(build_dir, "suite-stamps", name + ".json")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cmd, build_dir = sys.argv[1], sys.argv[2]
    if cmd == "toolchain":
        print(toolchain(build_dir) or "")
        return 0
    if cmd == "clean":
        return 0 if clean(build_dir) else 1
    other = None
    if cmd == "find":
        other = sys.argv[3]
        del sys.argv[3]
    if len(sys.argv) < 5:
        sys.exit(__doc__)
    tree, key = sys.argv[3], normalize(sys.argv[4:])
    path = stamp_path(build_dir, key)
    if cmd == "write":
        tc = toolchain(build_dir)
        if not tc or not re.match(r"^[0-9a-f]{40,64}$", tree):
            return 1
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path + ".tmp", "w") as f:
            json.dump({"tree": tree, "toolchain": tc, "key": key}, f)
        os.replace(path + ".tmp", path)
        return 0
    if cmd == "forget":
        try:
            os.remove(path)
        except FileNotFoundError:
            pass
        return 0
    if cmd == "find":
        try:
            with open(path) as f:
                s = json.load(f)
        except (FileNotFoundError, ValueError):
            print("no stamp")
            return 1
        if s.get("key") != key:
            print("no stamp")
            return 1
        if s.get("tree") != tree:
            print("stamped tree %s, not this one" % s.get("tree", "?")[:12])
            return 1
        tc = toolchain(other)
        if s.get("toolchain") != tc:
            print("stamped on another toolchain")
            return 1
        print("tree %s, toolchain %s" % (tree[:12], tc))
        return 0
    sys.exit(__doc__)


if __name__ == "__main__":
    sys.exit(main())
