#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: flake.nix pins each submodule at the commit the repository records.

The flake takes the four submodules as `flake = false` inputs, so `nix build`
needs no `?submodules=1` (docs/packaging/README.md#the-nix-flake). That is a
second copy of each pin, and a submodule bumped without its input builds the
Nix package, the AppImage, the container and the Windows release from the
old code while every other build uses the new.

For each `[submodule]` in .gitmodules, the flake input with the same GitHub
owner/repo must name exactly the commit `git ls-tree HEAD <path>` records.
"""
import configparser
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    if not (ROOT / ".git").exists():
        print("flake_submodules_gate: SKIP — not a git checkout")
        return 0
    flake = (ROOT / "flake.nix").read_text()
    pins = {f"{o}/{r}".lower(): sha for o, r, sha in re.findall(
        r'url\s*=\s*"github:([^/"]+)/([^/"]+)/([0-9a-f]{40})"', flake)}

    mods = configparser.ConfigParser()
    mods.read(ROOT / ".gitmodules")
    bad = []
    for section in mods.sections():
        path, url = mods[section]["path"], mods[section]["url"]
        m = re.search(r"github\.com[/:]([^/]+)/([^/]+?)(?:\.git)?$", url)
        if not m:
            bad.append(f"{path}: {url} is not on GitHub; pin it in flake.nix by hand")
            continue
        repo = f"{m.group(1)}/{m.group(2)}".lower()
        tree = subprocess.run(["git", "-C", str(ROOT), "ls-tree", "HEAD", path],
                              capture_output=True, text=True, check=True).stdout.split()
        recorded = tree[2] if len(tree) >= 3 else None
        pinned = pins.get(repo)
        if pinned is None:
            bad.append(f"{path}: flake.nix has no input for github:{repo}")
        elif pinned != recorded:
            bad.append(f"{path}: flake.nix pins {pinned}, the repository "
                       f"records {recorded or 'nothing'}")
    if bad:
        print("FAIL: flake.nix and the submodules disagree:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"flake_submodules_gate: {len(mods.sections())} submodules pinned alike")
    return 0


if __name__ == "__main__":
    sys.exit(main())
