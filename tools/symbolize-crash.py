#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Name the raw frames of a crash report.

    tools/symbolize-crash.py REPORT BINARY [LIBRARY ...]

REPORT is a crash report's report.txt (or a session's .crash file): its raw
frames are `#N <module>+0x<offset>` lines, written by the crash handler before
anything that could hang (docs/architecture.md#crashes-and-stalls). BINARY is
the UNSTRIPPED executable of the same build — the report's `Build:` line says
which commit — and any LIBRARY named the same way gets its frames named too.

Each frame whose module has the basename of a binary given is handed to
`addr2line -f -C -i`: a release build names the function from its symbol
table, a build with -g adds file:line. A Windows `.exe`/`.dll` takes the
mingw addr2line (`x86_64-w64-mingw32-addr2line` on PATH, or $ADDR2LINE),
and its offsets are RVAs, so the image base is read from the PE header.
A build without debug info is named from its symbol table, through `nm`
where addr2line has nothing (a PE's addr2line does not read COFF symbols).
Frames of modules not given are printed as they were.
"""
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys

FRAME = re.compile(r"^(\s*#(\d+)\s+)(.+)\+0x([0-9a-fA-F]+)\s*$")


def is_pe(path):
    with open(path, "rb") as f:
        return f.read(2) == b"MZ"


def pe_image_base(path):
    with open(path, "rb") as f:
        data = f.read(4096)
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError(f"{path}: not a PE image")
    opt = pe + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:  # PE32+
        return struct.unpack_from("<Q", data, opt + 24)[0]
    return struct.unpack_from("<I", data, opt + 28)[0]


def addr2line_for(binary):
    if os.environ.get("ADDR2LINE"):
        return os.environ["ADDR2LINE"]
    if is_pe(binary):
        tool = shutil.which("x86_64-w64-mingw32-addr2line")
        if tool:
            return tool
        sys.exit("symbolize-crash: a Windows binary needs the mingw addr2line "
                 "(x86_64-w64-mingw32-addr2line on PATH, or $ADDR2LINE)")
    tool = shutil.which("addr2line")
    if not tool:
        sys.exit("symbolize-crash: no addr2line on PATH")
    return tool


def nm_for(binary):
    """The nm beside the addr2line in use: a build with no debug info has
    only its symbol table, and a PE's addr2line will not read that."""
    if os.environ.get("NM"):
        return os.environ["NM"]
    tool = addr2line_for(binary)
    return os.path.join(os.path.dirname(tool),
                        os.path.basename(tool).replace("addr2line", "nm"))


def symbol_table(binary):
    """Sorted [(address, name)] of the functions in the symbol table."""
    out = subprocess.run([nm_for(binary), "-C", "--defined-only", binary],
                         capture_output=True, text=True).stdout
    table = []
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) == 3 and parts[1] in "TtWw" and parts[0]:
            try:
                table.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    table.sort()
    return table


def nearest(table, address):
    """The function an address lies in: the last one starting at or before
    it (a symbol table has no sizes on PE)."""
    i = bisect.bisect_right(table, (address, "\uffff")) - 1
    return table[i][1] if i >= 0 else "??"


def symbolize(binary, offsets):
    """{offset: "function at file:line"} for the given file offsets/RVAs."""
    base = pe_image_base(binary) if is_pe(binary) else 0
    # A return address points just past its call: one back stays inside the
    # calling function when the call was its last instruction.
    addrs = [hex(base + max(o - 1, 0) if adj else base + o)
             for o, adj in offsets]
    out = subprocess.run([addr2line_for(binary), "-f", "-C", "-i", "-a", "-e",
                          binary, *addrs],
                         capture_output=True, text=True, check=True).stdout
    names = {}
    table = None
    # -a prints each address, then function/location pairs (several when
    # inlined) until the next address.
    blocks = re.split(r"^0x[0-9a-f]+\n", out, flags=re.M)[1:]
    for (offset, _), addr, block in zip(offsets, addrs, blocks):
        lines = block.splitlines()
        pairs = []
        for fn, loc in zip(lines[0::2], lines[1::2]):
            where = "" if loc.startswith("??") else f" at {loc}"
            pairs.append(fn + where)
        name = ", inlined in ".join(pairs) if pairs else "??"
        if name.startswith("??"):
            # No debug info, and a PE's addr2line does not fall back to the
            # symbol table the way an ELF one does: ask nm.
            if table is None:
                table = symbol_table(binary)
            name = nearest(table, int(addr, 16))
        names[offset] = name
    return names


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    report, binaries = sys.argv[1], sys.argv[2:]
    with open(report, encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    by_name = {os.path.basename(b).lower(): b for b in binaries}

    wanted = {}  # binary -> [(offset, adjust)]
    for line in lines:
        m = FRAME.match(line)
        if not m:
            continue
        module = re.split(r"[\\/]", m.group(3))[-1].lower()
        if module in by_name:
            wanted.setdefault(by_name[module], []).append(
                (int(m.group(4), 16), m.group(2) != "0"))
    names = {b: symbolize(b, offs) for b, offs in wanted.items()}

    for line in lines:
        m = FRAME.match(line)
        if m:
            module = re.split(r"[\\/]", m.group(3))[-1].lower()
            binary = by_name.get(module)
            if binary:
                name = names[binary].get(int(m.group(4), 16), "??")
                print(f"{m.group(1)}{name}  ({os.path.basename(m.group(3))}"
                      f"+0x{m.group(4)})")
                continue
        print(line)
    if not wanted:
        print("symbolize-crash: no frame belongs to "
              + ", ".join(os.path.basename(b) for b in binaries),
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
