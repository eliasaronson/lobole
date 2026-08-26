#!/usr/bin/env python3
"""Every claim in pyproject.toml resolves to something real.

The install (`uv tool install .`, `uvx --from git+…`) happens on machines
where none of this repo's tests ever run, so a claim that has rotted — a
module renamed out from under `py-modules`, a `main()` renamed out from under
a console script — surfaces as a broken command line on a user's machine
instead of a red test here. The py_compile gates prove the files parse; this
one proves the packaging metadata still points AT them.

The `mcp` SDK is not on the stock-Ubuntu CI image, so an entry point whose
module needs it is checked as far as importability allows there and fully in
the nix shell; the module file existing is asserted unconditionally.
"""
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
