#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Every authored source file carries an SPDX header.

The top-level LICENSE is what makes the licence valid, so this is not about
correctness. It is about the two places a missing per-file notice actually
costs something: distribution packagers (Debian, Fedora, Flathub) check for
SPDX identifiers and file bugs when they are absent, and a file with no notice
is one whose licence a person copying a function out of it has to guess.

Why this and not `reuse lint`: REUSE requires LICENSES/ to hold files named by
SPDX identifier. Ours is a curated distribution bundle named by component
(GLFW.txt, OpenCASCADE-exception.txt), shipped by CMake, cross-referenced by
filename from THIRD_PARTY.md and carrying the LGPL written offer. Renaming it
to satisfy a linter would mean re-verifying a compliance story to gain a tool.

What is deliberately NOT checked, because the header would be false or
pointless: deps/ and src/thirdparty/ are other people's code and carry their
own upstream headers; benchmarks/results/ are committed records of runs;
assets/ is third-party data covered by THIRD_PARTY.md; documentation and
golden files are not copied out a function at a time.

Usage: spdx_gate.py            (from anywhere in the checkout)
"""

import pathlib
import subprocess
import sys

COPYRIGHT = "SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors"
LICENSE = "SPDX-License-Identifier: AGPL-3.0-or-later"

SLASH = {".cpp", ".h", ".hpp", ".inc", ".comp", ".frag", ".vert", ".rgen",
         ".rchit", ".rahit", ".rmiss", ".rint", ".rcall", ".glsl", ".tesc",
         ".tese", ".geom"}
HASH = {".py", ".sh", ".cmake"}

EXCLUDED_PREFIXES = ("deps/", "src/thirdparty/", "benchmarks/results/",
                     "assets/")
# Generated at build time by tools/icons/build_icon_font.py; the generator
# emits whatever header it is told to, so the checked-in copy is not ours to
# annotate by hand.
EXCLUDED_FILES = {
    "deps/fonts/IconsDesigner.h",
    # Third-party data living inside src/: the original Hershey Roman Simplex
    # glyph records, public domain with required attribution
    # (LICENSES/Hershey.txt, and the fonts table in THIRD_PARTY.md). Stamping
    # our copyright and AGPL on it would be a false claim over someone else's
    # public-domain dataset -- which the first run of this sweep did, and this
    # entry is why it cannot happen again.
    "src/core/sketch/HersheyRomanSimplex.inc",
}

# The header must sit within the first few lines: after a shebang, after an
# editor modeline, but before anything substantive.
SCAN_LINES = 6


def comment_prefix(path: pathlib.Path) -> str | None:
    if path.suffix in SLASH:
        return "//"
    if path.suffix in HASH or path.name == "CMakeLists.txt":
        return "#"
    return None


def tracked_sources() -> list[pathlib.Path]:
    repo = pathlib.Path(__file__).resolve().parent.parent
    out = subprocess.run(["git", "-C", str(repo), "ls-files"],
                         capture_output=True, text=True, check=True).stdout
    files = []
    for rel in out.split("\n"):
        if not rel or rel.startswith(EXCLUDED_PREFIXES) or rel in EXCLUDED_FILES:
            continue
        p = repo / rel
        if comment_prefix(p) is None or not p.is_file():
            continue
        files.append(p)
    return files


def main() -> int:
    repo = pathlib.Path(__file__).resolve().parent.parent
    missing: list[str] = []
    wrong: list[str] = []

    for p in tracked_sources():
        try:
            head = "\n".join(p.read_text(encoding="utf-8").split("\n")[:SCAN_LINES])
        except UnicodeDecodeError:
            continue
        rel = p.relative_to(repo).as_posix()
        if COPYRIGHT not in head or LICENSE not in head:
            missing.append(rel)
        elif "SPDX-License-Identifier:" in head and LICENSE not in head:
            wrong.append(rel)

    if missing or wrong:
        print("SPDX gate FAILED\n")
        if missing:
            print(f"  {len(missing)} file(s) have no SPDX header in the first "
                  f"{SCAN_LINES} lines.")
            print(f"    Add, after any shebang:\n"
                  f"      {COPYRIGHT}\n      {LICENSE}")
            for rel in missing[:15]:
                print(f"      {rel}")
            if len(missing) > 15:
                print(f"      ... and {len(missing) - 15} more")
        for rel in wrong:
            print(f"  {rel}: carries a DIFFERENT SPDX licence identifier.")
        return 1

    print(f"spdx ok: {len(tracked_sources())} authored source files carry the "
          "header")
    return 0


if __name__ == "__main__":
    sys.exit(main())
