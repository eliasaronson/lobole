#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""tools/fix-stale-nix-cache.sh reconfigures a tree from ITS OWN source.

It ran `cmake -S .` for every build dir it found, so a worktree's build/,
reconfigured by the script run from main, came back configured from main's
source -- and a tree with stale cache entries failed outright, since cmake
refuses a cache made from another source. It now reads each tree's
CMAKE_HOME_DIRECTORY before it touches the cache and passes that as -S.

A scratch project and a scratch build dir, nothing else: never main's build/
or a worktree's. `nix-shell` is stubbed to run its --run command in the
environment this test already has, so the test neither enters nor nests a
nix shell. Both of the script's branches are driven:

  1. the toolchain moved (the cache names another compiler): the cache is
     thrown away and the tree configured fresh -- from the scratch source;
  2. a cache entry names a collected store path: it is unset and the tree
     reconfigured -- from the scratch source, which cmake requires.

Usage: fix_stale_nix_cache_smoke.py
"""
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "tools", "fix-stale-nix-cache.sh")
GONE = "/nix/store/" + "0" * 32


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def cache(build):
    with open(os.path.join(build, "CMakeCache.txt")) as f:
        return f.read()


def entry(build, name):
    m = re.search(rf"^{name}:[A-Z]*=(.*)$", cache(build), re.M)
    return m.group(1) if m else None


def rewrite(build, name, value, kind="FILEPATH"):
    text = cache(build)
    line = f"{name}:{kind}={value}"
    if re.search(rf"^{name}:", text, re.M):
        text = re.sub(rf"^{name}:[A-Z]*=.*$", line, text, flags=re.M)
    else:
        text += line + "\n"
    with open(os.path.join(build, "CMakeCache.txt"), "w") as f:
        f.write(text)


def main():
    if any(shutil.which(t) is None for t in ("cmake", "ninja", "g++")):
        print("fix_stale_nix_cache_smoke: no cmake/ninja/g++ here; skipped")
        return 77
    work = tempfile.mkdtemp(prefix="lobole_fsnc_")
    try:
        src = os.path.join(work, "src")
        build = os.path.join(work, "build")
        stub = os.path.join(work, "bin")
        os.makedirs(src)
        os.makedirs(stub)
        with open(os.path.join(src, "CMakeLists.txt"), "w") as f:
            f.write("cmake_minimum_required(VERSION 3.20)\n"
                    "project(scratch CXX)\n")
        # `nix-shell shell.nix --run CMD` -> CMD, here.
        nix_shell = os.path.join(stub, "nix-shell")
        with open(nix_shell, "w") as f:
            f.write('#!/usr/bin/env bash\n'
                    'while [ $# -gt 0 ] && [ "$1" != --run ]; do shift; done\n'
                    'exec bash -c "$2"\n')
        os.chmod(nix_shell, os.stat(nix_shell).st_mode | stat.S_IXUSR)
        env = dict(os.environ, PATH=stub + os.pathsep + os.environ["PATH"])

        def configure():
            shutil.rmtree(build, ignore_errors=True)
            # The compiler the script will call current, so only what a case
            # changes in the cache is stale.
            subprocess.run(["cmake", "-S", src, "-B", build, "-G", "Ninja",
                            "-DCMAKE_BUILD_TYPE=Release",
                            "-DCMAKE_CXX_COMPILER=" + shutil.which("g++")],
                           check=True, capture_output=True, env=env)
            expect(entry(build, "CMAKE_HOME_DIRECTORY") == src,
                   f"the scratch tree is configured from {src}")

        def run():
            r = subprocess.run(["bash", SCRIPT, build], env=env,
                               capture_output=True, text=True, timeout=300)
            return r.returncode, r.stdout + r.stderr

        # 1. The toolchain moved.
        configure()
        rewrite(build, "CMAKE_CXX_COMPILER", GONE + "-gcc-1/bin/g++")
        code, said = run()
        expect(code == 0 and "toolchain moved" in said,
               f"the moved toolchain was not reconfigured ({code}): {said}")
        expect(entry(build, "CMAKE_HOME_DIRECTORY") == src,
               f"reconfigured from {entry(build, 'CMAKE_HOME_DIRECTORY')}, "
               f"not its own source {src}: {said}")
        expect(entry(build, "CMAKE_BUILD_TYPE") == "Release",
               "the build type was not carried over")

        # 2. A cache entry names a collected store path.
        configure()
        rewrite(build, "STALE_INCLUDE_DIR", GONE + "-gone/include", "PATH")
        code, said = run()
        expect(code == 0 and "reconfigured" in said,
               f"the stale entry was not cleared ({code}): {said}")
        expect(entry(build, "STALE_INCLUDE_DIR") is None,
               "the stale entry is still in the cache")
        expect(entry(build, "CMAKE_HOME_DIRECTORY") == src,
               f"reconfigured from {entry(build, 'CMAKE_HOME_DIRECTORY')}, "
               f"not its own source {src}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("fix_stale_nix_cache_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
