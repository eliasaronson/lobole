#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Turn gcov data into the one question worth asking: what has NO test at all.

Called by tools/coverage.sh. Reports per-directory line coverage for our own
source, then the actionable part — functions in src/core with zero executed
lines. Those are behaviours nothing has ever run, which is where a silent bug
survives indefinitely.

Deliberately not a percentage-chasing tool. src/render and src/ui score low by
design (they need a GPU and are covered by the GUI suite instead); a number
going up is not the goal, a named uncovered function is.
"""
import json
import os
import subprocess
import sys
import zlib
from collections import defaultdict


def gcov_json(build, root):
    """Run gcov over every .gcda and yield its JSON, one object per file.

    `gcov --json-format --stdout` writes PLAIN concatenated JSON objects (the
    .gz only applies when it writes files), so this decodes a stream with
    raw_decode rather than trying to gunzip it.
    """
    gcdas = []
    for dirpath, _, names in os.walk(build):
        gcdas += [os.path.join(dirpath, n) for n in names if n.endswith(".gcda")]
    if not gcdas:
        sys.exit("no .gcda files — run the instrumented suite first")
    print(f"  {len(gcdas)} instrumented objects", file=sys.stderr)

    dec = json.JSONDecoder()
    for i in range(0, len(gcdas), 100):
        r = subprocess.run(["gcov", "--json-format", "--stdout"] + gcdas[i:i + 100],
                           cwd=build, capture_output=True, text=True, check=False)
        text, pos = r.stdout, 0
        while pos < len(text):
            while pos < len(text) and text[pos] in " \t\r\n":
                pos += 1
            if pos >= len(text):
                break
            try:
                obj, pos = dec.raw_decode(text, pos)
            except json.JSONDecodeError:
                break
            yield obj


def main():
    build, root = sys.argv[1], sys.argv[2]
    per_dir = defaultdict(lambda: [0, 0])          # dir -> [covered, total]
    dead = []                                       # (file, function, line)
    seen_fn = {}

    for doc in gcov_json(build, root):
        for f in doc.get("files", []):
            path = f.get("file", "")
            # OUR src/, resolved against the repo root — Eigen ships its own
            # src/Core, src/SparseLU etc. and a bare "src/" substring test
            # counts 60k lines of dependency as if it were ours.
            absolute = path if os.path.isabs(path) else os.path.join(build, path)
            absolute = os.path.normpath(absolute)
            prefix = os.path.join(os.path.normpath(root), "src") + os.sep
            if not absolute.startswith(prefix):
                continue
            rel = os.path.relpath(absolute, os.path.normpath(root))
            if "thirdparty" in rel or "/deps/" in rel:
                continue
            top = "/".join(rel.split("/")[:2])
            for ln in f.get("lines", []):
                per_dir[top][1] += 1
                if ln.get("count", 0) > 0:
                    per_dir[top][0] += 1
            for fn in f.get("functions", []):
                key = (rel, fn.get("name", "?"))
                prev = seen_fn.get(key, 0)
                seen_fn[key] = max(prev, fn.get("execution_count", 0))

    print("\n=== line coverage of our own source, by directory ===")
    for d in sorted(per_dir, key=lambda k: -(per_dir[k][1])):
        cov, tot = per_dir[d]
        if tot == 0:
            continue
        print(f"  {d:22s} {cov:6d}/{tot:6d}  {100.0 * cov / tot:5.1f}%")

    for (rel, name), count in seen_fn.items():
        if count == 0 and rel.startswith("src/core"):
            dead.append((rel, name))

    print(f"\n=== src/core functions the headless suite never calls "
          f"({len(dead)}) ===")
    by_file = defaultdict(list)
    for rel, name in dead:
        by_file[rel].append(name)
    # Demangle so the output names something a human recognises.
    names = sorted({n for _, n in dead})
    demangled = dict(zip(names, names))
    try:
        r = subprocess.run(["c++filt"], input="\n".join(names), text=True,
                           capture_output=True, check=False)
        if r.returncode == 0:
            demangled = dict(zip(names, r.stdout.splitlines()))
    except FileNotFoundError:
        pass
    for rel in sorted(by_file, key=lambda k: -len(by_file[k]))[:25]:
        print(f"\n  {rel}  ({len(by_file[rel])})")
        for n in sorted(by_file[rel])[:8]:
            print(f"      {demangled.get(n, n)[:110]}")


if __name__ == "__main__":
    main()
