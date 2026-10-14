#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Every claim in pyproject.toml resolves to something real.

The install (`uv tool install .`, `uvx --from git+…`) happens on machines
where none of this repo's tests ever run, so a claim that has rotted — a
module renamed out from under `py-modules`, a `main()` renamed out from under
a console script — surfaces as a broken command line on a user's machine
instead of a red test here. The py_compile gates prove the files parse; this
one proves the packaging metadata still points AT them.

It also proves the listed modules are CLOSED under import: a module that
imports a sibling from tools/ which is not in `py-modules` works here, where
tools/ is on the path, and raises ModuleNotFoundError once installed — the
way `help()` on a typed method did when `designer_contracts` was left out.
Function-level imports count; that one was inside a function.

The `mcp` SDK is not on the stock-Ubuntu CI image, so an entry point whose
module needs it is checked as far as importability allows there and fully in
the nix shell; the module file existing is asserted unconditionally.
"""
import ast
import importlib
import pathlib
import sys
import tomllib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    data = tomllib.loads((ROOT / "pyproject.toml").read_text())
    pkgdir = ROOT / data["tool"]["setuptools"]["package-dir"][""]
    failures = []

    for mod in data["tool"]["setuptools"]["py-modules"]:
        if not (pkgdir / f"{mod}.py").is_file():
            failures.append(f"py-module {mod!r} has no file {pkgdir}/{mod}.py")

    # Every import of a tools/ sibling, at any depth, is a module that must
    # ship too.
    listed = set(data["tool"]["setuptools"]["py-modules"])
    siblings = {p.stem for p in pkgdir.glob("*.py")}
    for mod in sorted(listed):
        path = pkgdir / f"{mod}.py"
        if not path.is_file():
            continue
        for node in ast.walk(ast.parse(path.read_text(), str(path))):
            if isinstance(node, ast.Import):
                names = [a.name for a in node.names]
            elif isinstance(node, ast.ImportFrom) and node.level == 0:
                names = [node.module]
            else:
                continue
            for name in names:
                top = name.split(".")[0]
                if top in siblings and top not in listed:
                    failures.append(
                        f"{mod}.py:{node.lineno} imports {top!r}, which is "
                        "not in py-modules — it would be missing once "
                        "installed")

    sys.path.insert(0, str(pkgdir))
    for script, target in data["project"]["scripts"].items():
        mod_name, _, func_name = target.partition(":")
        try:
            mod = importlib.import_module(mod_name)
        except ModuleNotFoundError as e:
            if e.name == mod_name:
                failures.append(f"script {script!r}: no module {mod_name!r}")
            else:
                print(f"note: {script}: dependency {e.name!r} not installed "
                      "here; entry point checked as far as import allows")
            continue
        func = getattr(mod, func_name, None)
        if not callable(func):
            failures.append(
                f"script {script!r}: {target!r} is not a callable")

    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    print("pyproject entry points and modules all resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
