#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The lobole wheel, built and imported the way a user's install sees it.

The install (`uv tool install .`, `uvx --from git+…`) happens on machines
where none of this repo's tests ever run, and everywhere in the repo the
package is imported from the source tree (shell.nix and ctest put python/ on
PYTHONPATH). So a module left out of the package, a file the package needs
that is not shipped, or a console script naming a `main` that moved works
here and breaks on the user's machine. `designer_contracts` (now
`lobole.contracts`) was left out that way, and `help()` on every typed
method raised ModuleNotFoundError once installed.

So this builds the sdist from a COPY of the tree (setuptools writes build/
and *.egg-info next to its input, and build/ is ours), the wheel from that
sdist (what pip does with a PyPI release), unpacks it, and with the source
tree off sys.path:

- the wheel carries VERSION's version (the sdist must ship the file);
- every module in python/lobole/ is in the wheel, and `py.typed` with it;
- every module imports from the wheel alone;
- every console script names a callable, and `lobole-remote` runs.

The `mcp` SDK is not on every machine that runs this; `lobole.mcp` is then
checked as far as importability allows. setuptools is a dev tool like mypy:
without it this skips, and CI's build-and-test job installs it so it runs.
"""
import configparser
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SKIP = 77


def main():
    if importlib.util.find_spec("setuptools") is None:
        print("packaging_smoke: SKIP — no setuptools for this interpreter "
              f"({sys.executable}). nix-shell has it; apt: python3-setuptools")
        return SKIP
    failures = []
    with tempfile.TemporaryDirectory(prefix="lobole-wheel-") as tmp:
        tmp = pathlib.Path(tmp)
        src = tmp / "src"
        src.mkdir()
        for name in ("pyproject.toml", "VERSION"):
            shutil.copy(ROOT / name, src)
        shutil.copytree(ROOT / "python", src / "python",
                        ignore=shutil.ignore_patterns("__pycache__"))
        dist = tmp / "dist"

        def build(kind, cwd):
            r = subprocess.run(
                [sys.executable, "-c",
                 "import sys; from setuptools import build_meta as b; "
                 f"print(b.build_{kind}(sys.argv[1]))", str(dist)],
                cwd=cwd, capture_output=True, text=True)
            if r.returncode != 0:
                print(r.stdout + r.stderr)
                return None
            return dist / r.stdout.strip().splitlines()[-1]

        sdist = build("sdist", src)
        if sdist is None:
            print("FAIL: the sdist does not build")
            return 1
        unpacked = tmp / "sdist"
        shutil.unpack_archive(sdist, unpacked)
        wheel = build("wheel", next(unpacked.iterdir()))
        if wheel is None:
            print("FAIL: the wheel does not build from the sdist")
            return 1
        version = (ROOT / "VERSION").read_text().strip()
        if f"-{version}-" not in wheel.name:
            failures.append(f"the wheel is {wheel.name}, not version "
                            f"{version} from VERSION")
        site = tmp / "site"
        with zipfile.ZipFile(wheel) as z:
            names = set(z.namelist())
            z.extractall(site)

        modules = sorted(p.stem for p in (ROOT / "python/lobole").glob("*.py"))
        for mod in modules:
            if f"lobole/{mod}.py" not in names:
                failures.append(f"lobole/{mod}.py is not in the wheel")
        if "lobole/py.typed" not in names:
            failures.append("lobole/py.typed is not in the wheel: a type "
                            "checker would ignore the installed package")

        # -I: no PYTHONPATH, no user site, no script directory. Only the
        # unpacked wheel (and the interpreter's own site-packages, for mcp).
        def run(code):
            return subprocess.run([sys.executable, "-I", "-c",
                                   f"import sys; sys.path.insert(0, {str(site)!r}); "
                                   + code], capture_output=True, text=True)

        for mod in modules:
            r = run(f"import lobole.{mod}")
            if r.returncode != 0:
                if mod == "mcp" and "No module named 'mcp'" in r.stderr:
                    print("note: lobole.mcp: the mcp SDK is not installed "
                          "here; checked as far as import allows")
                    continue
                failures.append(f"lobole.{mod} does not import from the "
                                f"wheel:\n{r.stderr.strip()}")
            elif "/lobole-wheel-" not in run(
                    f"import lobole.{mod} as m; print(m.__file__)").stdout:
                failures.append(f"lobole.{mod} imported from outside the "
                                "wheel; the check proves nothing")

        info = next(site.glob("*.dist-info"))
        eps = configparser.ConfigParser()
        eps.read(info / "entry_points.txt")
        scripts = dict(eps["console_scripts"]) if eps.has_section(
            "console_scripts") else {}
        if not scripts:
            failures.append("the wheel declares no console scripts")
        for script, target in scripts.items():
            mod, _, func = target.partition(":")
            r = run(f"import {mod} as m; assert callable(m.{func})")
            if r.returncode != 0 and "No module named 'mcp'" not in r.stderr:
                failures.append(f"script {script!r} -> {target!r}: "
                                f"{r.stderr.strip()}")
        r = run("from lobole.client import main; sys.argv = ['lobole-remote']; "
                "sys.exit(main())")
        if "<port>" not in r.stdout + r.stderr:
            failures.append("lobole-remote with no arguments does not print "
                            f"its usage: {r.stdout}{r.stderr}")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    print(f"lobole {version} wheel, built from its sdist: {len(modules)} modules ship, import alone, and "
          f"{len(scripts)} console scripts resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
