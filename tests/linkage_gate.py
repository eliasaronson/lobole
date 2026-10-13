#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The LGPL-family dependencies are linked DYNAMICALLY, and nothing arrives undocumented.

Why this is a test and not a comment in CMakeLists.txt:

LGPL-2.1 section 6 permits combining an LGPL library with a work of our own and
distributing the combination under terms of OUR choosing -- but only if the
recipient can relink against a modified version of the library. A separate
shared object satisfies that for free: ship `libplanegcs.so`, anyone may swap
it. A static link does not; we would have to distribute object files so the
recipient could relink, which means exposing the application's object code.

So `add_library(planegcs SHARED ...)` is not a build preference. It is the
reason this project's licence stays a choice, written in CMake instead of in
English, and THIRD_PARTY.md states it as a promise to anyone receiving a build.

One word turns SHARED into STATIC. The build still succeeds, every test still
passes, every render is identical -- and the promise is silently void, in
binaries already shipped. That is the exact failure shape this codebase gates
against everywhere else, so it is gated here.

Following the rule in CLAUDE.md -- a test checks the OUTCOME, not the formula --
this does not grep CMakeLists.txt for the word SHARED. It reads DT_NEEDED out
of the linked binary and asserts what was actually produced.

Usage: linkage_gate.py <path-to-designer-binary> NAME=ON|OFF ...

Each NAME=ON|OFF is a CMake result for one optional component (HAVE_NETGEN,
HAVE_CAM_ADAPTIVE, ...), passed by tests/CMakeLists.txt from the build's own
configuration. A component the build was configured WITHOUT is not demanded
(Ubuntu's apt has no Netgen, and that build has no FEM meshing at all); one it
was configured WITH must be linked dynamically. The gate never guesses: a
component whose flag is not passed is a usage error, so a new optional
dependency cannot be skipped by forgetting to say how it was built.
Exits 77 (skip) on a non-ELF binary; the Windows import table is not read yet.
"""

import pathlib
import re
import struct
import sys

# Every entry here is a promise THIRD_PARTY.md makes to the people who receive
# this software. The test asserts BOTH halves: that the document still claims a
# shared library, and that the binary actually has one. Changing the linkage of
# anything below is a licensing decision, so it has to be a visible edit here
# rather than one word in a build file.
#
#   "heading" matches the start of a '### ' heading under
#   '## LGPL-family libraries' (headings carry a trailing subtitle).
#   "required" sonames must each appear in DT_NEEDED.
#   "family" is the prefix the whole component's libraries share, so the
#   undocumented-dependency check below recognises every one of them.
#   "built_if" names the CMake result that says the build has the component;
#   tests/CMakeLists.txt passes it. Absent means the build cannot be
#   configured without it.
LGPL_SHARED = [
    {"heading": "PlaneGCS",
     "required": ["libplanegcs.so"],
     "family": "libplanegcs",
     "built_if": "HAVE_PLANEGCS"},
    {"heading": "libarea Adaptive",
     "required": ["libdesigner_adaptive.so"],
     "family": "libdesigner_adaptive",
     "built_if": "HAVE_CAM_ADAPTIVE"},
    {"heading": "Netgen",
     "required": ["libnglib.so", "libngcore.so"],
     "family": "libng",
     "built_if": "HAVE_NETGEN"},
    # OCCT ships ~30 libTK*; these three are load-bearing and always present.
    {"heading": "OpenCASCADE Technology",
     "required": ["libTKernel.so", "libTKBRep.so", "libTKMath.so"],
     "family": "libTK",
     "built_if": "OPENCASCADE_FOUND"},
]

# The C runtime, the toolchain, and the display/IPC stack the OS provides.
# These carry no distribution obligation we must document.
SYSTEM_SONAMES = {
    "libc", "libm", "libdl", "libutil", "libpthread", "librt", "libresolv",
    "libstdc++", "libgcc_s", "libatomic",
    "libX11", "libxcb", "libdbus-1",
    # The sanitizer runtimes of a --asan/--tsan build, which is never shipped.
    "libasan", "libubsan", "libtsan", "liblsan",
}

# Linked, not vendored, and each has a row in THIRD_PARTY.md's
# "Header-only and linked dependencies" table. A NEW soname that is not here
# and not a system library fails this test: add the row, then add it here.
DOCUMENTED_SONAMES = {
    "libglfw",     # GLFW 3.4, zlib
    "libspdlog",   # spdlog 1.17, MIT
    "libfmt",      # fmt, MIT
    "libvterm",    # libvterm 0.3, MIT (optional, -DENABLE_TERMINAL=OFF)
    "libvulkan",   # Vulkan Loader, Apache-2.0
}


def dt_needed(path: pathlib.Path) -> list[str]:
    """DT_NEEDED entries of an ELF64 file, without a binutils dependency."""
    blob = path.read_bytes()
    if blob[:4] != b"\x7fELF":
        return []
    if blob[4] != 2:  # ELFCLASS64
        raise SystemExit(f"{path}: not ELF64; this gate only reads ELF64")
    end = "<" if blob[5] == 1 else ">"

    e_shoff, = struct.unpack_from(end + "Q", blob, 0x28)
    e_shentsize, e_shnum = struct.unpack_from(end + "HH", blob, 0x3A)

    sections: list[tuple[int, int]] = []
    dynamic: tuple[int, int] | None = None
    dynstr_idx = -1
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from(end + "I", blob, off + 4)
        sh_offset, sh_size = struct.unpack_from(end + "QQ", blob, off + 0x18)
        sh_link, = struct.unpack_from(end + "I", blob, off + 0x28)
        sections.append((sh_offset, sh_size))
        if sh_type == 6:  # SHT_DYNAMIC
            dynamic, dynstr_idx = (sh_offset, sh_size), sh_link
    if dynamic is None or not 0 <= dynstr_idx < len(sections):
        return []

    str_off, str_size = sections[dynstr_idx]
    strtab = blob[str_off:str_off + str_size]

    needed: list[str] = []
    off, size = dynamic
    for pos in range(off, off + size, 16):
        d_tag, d_val = struct.unpack_from(end + "QQ", blob, pos)
        if d_tag == 0:  # DT_NULL
            break
        if d_tag == 1:  # DT_NEEDED
            needed.append(strtab[d_val:strtab.index(b"\0", d_val)].decode())
    return needed


def linkage_claims(repo: pathlib.Path) -> dict[str, str]:
    """Each '### heading' -> its '**Linkage:**' text, inside the LGPL section."""
    text = (repo / "THIRD_PARTY.md").read_text(encoding="utf-8")
    start = text.index("## LGPL-family libraries")
    section = text[start:text.index("\n## ", start + 1)]

    claims: dict[str, str] = {}
    heading = None
    for para in re.split(r"\n(?=### |- \*\*)", section):
        if para.startswith("### "):
            heading = para.split("\n", 1)[0][4:].strip()
        elif para.startswith("- **Linkage:**") and heading:
            claims[heading] = " ".join(para.split())
    return claims


def claim_for(claims: dict[str, str], heading: str) -> str | None:
    """Headings carry a subtitle ('PlaneGCS - constraint solver')."""
    for full, claim in claims.items():
        if full.startswith(heading):
            return claim
    return None


def configured(args: list[str]) -> dict[str, bool]:
    """NAME=VALUE pairs as CMake wrote them, read with CMake's truthiness."""
    flags: dict[str, bool] = {}
    for a in args:
        name, sep, value = a.partition("=")
        if not sep:
            raise SystemExit(f"linkage_gate: expected NAME=ON|OFF, got {a!r}")
        flags[name] = value.upper() not in {
            "", "0", "OFF", "FALSE", "NO", "N", "IGNORE", "NOTFOUND"
        } and not value.upper().endswith("-NOTFOUND")
    return flags


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    binary = pathlib.Path(sys.argv[1]).resolve()
    flags = configured(sys.argv[2:])
    unsaid = sorted({c["built_if"] for c in LGPL_SHARED} - flags.keys())
    if unsaid:
        print(f"linkage_gate: not told how the build configured {unsaid}; "
              "tests/CMakeLists.txt passes each component's CMake result")
        return 2
    repo = pathlib.Path(__file__).resolve().parent.parent

    needed = dt_needed(binary)
    if not needed:
        print(f"SKIP: {binary} has no ELF DT_NEEDED "
              "(Windows PE imports are not read by this gate yet)")
        return 77

    claims = linkage_claims(repo)
    components = [c for c in LGPL_SHARED if flags[c["built_if"]]]
    left_out = [c["heading"] for c in LGPL_SHARED if not flags[c["built_if"]]]
    failures: list[str] = []

    # 1. Every LGPL-family dependency is a shared object, in the document and
    #    in the binary.
    for comp in components:
        heading = comp["heading"]
        claim = claim_for(claims, heading)
        if claim is None:
            failures.append(
                f"THIRD_PARTY.md has no '### {heading}' with a **Linkage:** "
                "line under\n"
                "    '## LGPL-family libraries'. The document and this gate "
                "have to name the\n"
                "    same components.")
        elif "shared librar" not in claim.lower():
            failures.append(
                f"THIRD_PARTY.md no longer claims a shared library for "
                f"{heading}:\n"
                f"      {claim}\n"
                "    Changing this is a licensing decision, not an edit.")

        for soname in comp["required"]:
            if not any(n.startswith(soname) for n in needed):
                failures.append(
                    f"{heading}: {binary.name} does not link {soname} "
                    "dynamically.\n"
                    "    If this became a static link, the LGPL-2.1 s6 relink "
                    "right is gone and\n"
                    "    this project can no longer be licensed on our own "
                    "terms. See THIRD_PARTY.md.")

    # 2. Nothing new is linked without a THIRD_PARTY.md row.
    families = tuple(c["family"] for c in LGPL_SHARED)
    known = SYSTEM_SONAMES | DOCUMENTED_SONAMES
    for soname in sorted(set(needed)):
        if soname.split(".so")[0] in known or soname.startswith(families):
            continue
        failures.append(
            f"{soname} is linked but undocumented. Add a row to "
            "THIRD_PARTY.md\n"
            "    (component, purpose, licence) and list it in "
            "DOCUMENTED_SONAMES here.\n"
            "    A dependency whose licence nobody recorded is how a "
            "distribution\n"
            "    obligation arrives unnoticed.")

    if failures:
        print(f"Linkage gate FAILED for {binary}\n")
        for f in failures:
            print(f"  - {f}\n")
        return 1

    print(f"linkage ok: {sum(len(c['required']) for c in components)} "
          f"LGPL-family sonames linked dynamically, "
          f"{len(set(needed))} dependencies all documented"
          f"{'' if not left_out else ' (built without ' + ', '.join(left_out) + ')'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
