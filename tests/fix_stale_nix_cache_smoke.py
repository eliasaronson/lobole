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
or a worktree's. The script runs its cmake through tools/nix-run, which
sources the cached environment rather than entering a nix shell. Its
branches are driven:

  1. the toolchain moved (the cache names another compiler): the cache is
     thrown away and the tree configured fresh -- from the scratch source;
  2. a cache entry names a collected store path: it is unset and the tree
     reconfigured -- from the scratch source, which cmake requires;
  3. a package's config directory is cached as not found (a configure ran
     after a GC took the package): no store path is named, and it was
     called clean. It is unset and the tree reconfigured;
  4. a cache entry names ANOTHER VERSION of a package the environment
     resolves, still in the store (Catch2_DIR at 3.16.0 under 3.16.1
     headers: an undefined Catch::AutoReg at link). Nothing was collected,
     so asking the store called it clean. --check says so and changes
     nothing; the plain run unsets it.

Usage: fix_stale_nix_cache_smoke.py
"""
import os
import re
import shutil
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
        os.makedirs(src)
        with open(os.path.join(src, "CMakeLists.txt"), "w") as f:
            f.write("cmake_minimum_required(VERSION 3.20)\n"
                    "project(scratch CXX)\n")
        env = dict(os.environ)
        # The compiler the script calls current: shell.nix's, through
        # tools/nix-run, where there is nix. Not this process's PATH, which
        # may be another nix shell's (an agent's session is one) and would
        # make every case read as a moved toolchain.
        cxx = shutil.which("g++")
        if shutil.which("nix-instantiate"):
            r = subprocess.run([os.path.join(HERE, "..", "tools", "nix-run"),
                                "command -v g++"], capture_output=True,
                               text=True)
            if r.returncode == 0 and r.stdout.strip():
                cxx = r.stdout.strip().splitlines()[-1]

        def configure():
            shutil.rmtree(build, ignore_errors=True)
            # The compiler the script will call current, so only what a case
            # changes in the cache is stale.
            subprocess.run(["cmake", "-S", src, "-B", build, "-G", "Ninja",
                            "-DCMAKE_BUILD_TYPE=Release",
                            "-DCMAKE_CXX_COMPILER=" + cxx],
                           check=True, capture_output=True, env=env)
            expect(entry(build, "CMAKE_HOME_DIRECTORY") == src,
                   f"the scratch tree is configured from {src}")

        def run(*flags, extra=None):
            r = subprocess.run(["bash", SCRIPT, *flags, build],
                               env=dict(env, **(extra or {})),
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

        # 3. A package cached as not found.
        configure()
        rewrite(build, "Gone_DIR", "Gone_DIR-NOTFOUND", "PATH")
        code, said = run()
        expect(code == 0 and "reconfigured" in said,
               f"the not-found package was called clean ({code}): {said}")
        expect(entry(build, "Gone_DIR") is None,
               "the not-found package is still in the cache")

        # 4. Another version of a package the environment resolves. Any
        # store path that exists will do as the "old" one: this python's,
        # which the scratch cache does not otherwise name. The environment
        # is told it resolves version 99 of the same package now.
        real = os.path.realpath(sys.executable)
        parts = real.split("/")
        m = (re.match(r"[a-z0-9]{32}-(.+?)-\d", parts[3])
             if real.startswith("/nix/store/") else None)
        if m is None:
            print("fix_stale_nix_cache_smoke: no python in /nix/store; "
                  "case 4 skipped")
        else:
            old = "/nix/store/" + parts[3]
            current = os.path.join(work, "current")
            with open(current, "w") as f:
                f.write("/nix/store/%s-%s\n" % (
                    "z" * 32, re.sub(r"-\d[\w.]*", "-99.0", parts[3][33:], count=1)))
            pin = {"LOBOLE_FSNC_CURRENT": current}
            configure()
            rewrite(build, "Old_DIR", old + "/lib", "PATH")
            code, said = run("--check", extra=pin)
            expect(code == 1 and "another version" in said,
                   f"--check called another version clean ({code}): {said}")
            expect(entry(build, "Old_DIR") == old + "/lib",
                   "--check changed the cache")
            code, said = run(extra=pin)
            expect(code == 0 and "reconfigured" in said,
                   f"another version was not cleared ({code}): {said}")
            expect(entry(build, "Old_DIR") is None,
                   "the other version is still in the cache")
            code, said = run("--check", extra=pin)
            expect(code == 0, f"--check after the fix ({code}): {said}")
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("fix_stale_nix_cache_smoke: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
