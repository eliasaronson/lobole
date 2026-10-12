#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
"""A format bump takes its number when it lands (docs/versioning/format.md).

A branch writes `vNEXT` (and `vNEXT2`, `vNEXT3` for further bumps on the same
branch) wherever the new version number would go, and raises
`kVersionPending` in src/core/io/SerializationInternal.h. The merge numbers
it, so two branches built in parallel never both take "the next number".

  tools/format-version.py check            consistent on any branch
  tools/format-version.py check --landed   ...and nothing pending (main)
  tools/format-version.py land --base COMMIT [--label NAME]
      number the pending bumps carried by COMMIT..HEAD and commit the result
      (tools/merge-branch.sh runs it after the cherry-pick)

`land` replaces placeholders only on lines the branch ADDED since COMMIT, so
a document that describes the rule (this file, format.md's policy, CLAUDE.md)
keeps its placeholders; `check` exempts those same files.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = "src/core/io/SerializationInternal.h"
FORMAT_MD = "docs/versioning/format.md"
GOLDEN_DIRS = ["tests/golden", "tests/golden/assembly", "tests/golden/job",
               "tests/golden/drawing"]
# The files that DESCRIBE the rule, and so spell the placeholder as text:
# `land` leaves them alone and `check` does not scan them.
DESCRIBES = {"tools/format-version.py", "tools/merge-branch.sh", "CLAUDE.md",
             "docs/versioning/README.md"}
# `check` also skips the header (the placeholders' definitions), format.md
# (its pending rows are checked as rows; its policy describes the rule) and
# plans, which describe work before it lands.
EXEMPT = DESCRIBES | {HEADER, FORMAT_MD}
EXEMPT_DIRS = ("docs/plans/",)
POLICY_HEADING = "## Migration policy"

# kVersionNEXT2 -> 176; vNEXT2 -> v176. The bare word NEXT is never touched.
TOKEN = re.compile(r"\b(kVersion|v)NEXT([1-9]?)\b")
LANDED = re.compile(r"^(inline constexpr int kVersionLanded = )(\d+);", re.M)
PENDING = re.compile(r"^(inline constexpr int kVersionPending = )(\d+);", re.M)
PENDING_HEADING = "## Pending changes"
ROW = re.compile(r"^\|\s*([^|]*?)\s*\|")


def k_of(digit: str) -> int:
    return int(digit) if digit else 1


def git(*args: str, check: bool = True) -> str:
    return subprocess.run(["git", "-C", str(ROOT), *args], check=check,
                          capture_output=True, text=True).stdout


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def header_numbers() -> tuple[int, int]:
    text = read(HEADER)
    landed, pending = LANDED.search(text), PENDING.search(text)
    if not landed or not pending:
        sys.exit(f"format-version: {HEADER} has no kVersionLanded / "
                 "kVersionPending line")
    if "kVersion = kVersionLanded + kVersionPending;" not in text:
        sys.exit(f"format-version: {HEADER}: kVersion must be "
                 "kVersionLanded + kVersionPending")
    return int(landed.group(2)), int(pending.group(2))


def split_format_md(text: str) -> tuple[list[str], list[str]]:
    """The history table's first cells, and the pending table's."""
    history: list[str] = []
    pending: list[str] = []
    section = None
    for line in text.splitlines():
        if line.startswith("## "):
            section = ("history" if line.strip() == "## Version history" else
                       "pending" if line.strip() == PENDING_HEADING else None)
            continue
        m = ROW.match(line)
        if not m or section is None:
            continue
        cell = m.group(1)
        if cell in ("v", "") or set(cell) <= {"-", ":"}:
            continue  # the header row and the rule under it
        (history if section == "history" else pending).append(cell)
    return history, pending


def golden_files() -> list[Path]:
    out = []
    for d in GOLDEN_DIRS:
        for p in sorted((ROOT / d).glob("*.json")):
            if not p.name.endswith(".expect.json"):
                out.append(p)
    return out


def check(landed_only: bool) -> int:
    errors: list[str] = []
    landed, pending = header_numbers()
    if landed_only and pending:
        errors.append(f"kVersionPending is {pending} on a landed tree: the "
                      "branch was cherry-picked without tools/merge-branch.sh "
                      "numbering it (tools/format-version.py land)")

    history, rows = split_format_md(read(FORMAT_MD))
    numbers = [int(c) for c in history if c.isdigit()]
    if not numbers or max(numbers) != landed:
        errors.append(f"{FORMAT_MD}: the version history reaches "
                      f"v{max(numbers, default=0)}, kVersionLanded is "
                      f"{landed}")
    want = ["vNEXT"] + [f"vNEXT{k}" for k in range(2, pending + 1)]
    want = want[:pending]
    if rows != want:
        errors.append(f"{FORMAT_MD}: the pending table's rows are {rows}, "
                      f"kVersionPending {pending} wants {want}")

    for p in golden_files():
        m = re.match(r"v(\d+|NEXT[1-9]?)-", p.name)
        rel = p.relative_to(ROOT)
        if not m:
            errors.append(f"{rel}: a golden is named v<N>-… or vNEXT-…")
            continue
        try:
            version = json.loads(p.read_text(encoding="utf-8")).get("version")
        except (ValueError, OSError) as e:
            errors.append(f"{rel}: {e}")
            continue
        tag = m.group(1)
        if tag.isdigit():
            expect = int(tag)
        else:
            k = k_of(tag[4:])
            if k > pending:
                errors.append(f"{rel}: vNEXT{tag[4:]} with kVersionPending "
                              f"{pending}")
                continue
            expect = landed + k
        if version != expect:
            errors.append(f"{rel}: \"version\": {version}, its name says "
                          f"{expect}")

    # Placeholders anywhere else: none beyond what is pending.
    try:
        hits = git("grep", "-n", "-I", "-E", r"(kVersion|v)NEXT[1-9]?\b",
                   check=False)
        names = git("ls-files")
    except FileNotFoundError:
        print("format-version: no git; the placeholder scan is skipped")
        return 77
    for line in (hits + "".join(n + ":0:" + n + "\n"
                                for n in names.splitlines()
                                if "NEXT" in n)).splitlines():
        path, _, rest = line.partition(":")
        _, _, text = rest.partition(":")
        if path in EXEMPT or path.startswith(EXEMPT_DIRS):
            continue
        for m in TOKEN.finditer(text):
            if k_of(m.group(2)) > pending:
                errors.append(f"{path}: {m.group(0)} with kVersionPending "
                              f"{pending}")

    for e in errors:
        print(f"format-version: {e}", file=sys.stderr)
    if not errors:
        state = (f"v{landed}" if not pending else
                 f"v{landed} + {pending} pending (vNEXT..)")
        print(f"format-version: consistent, {state}")
    return 1 if errors else 0


def added_lines(base: str) -> dict[str, set[int]]:
    """Per file changed since BASE, the line numbers (1-based, in HEAD) the
    branch added."""
    out: dict[str, set[int]] = {}
    current = None
    for line in git("diff", "-U0", "--no-renames", base, "HEAD").splitlines():
        if line.startswith("+++ "):
            current = None if line == "+++ /dev/null" else line[6:]
            if current is not None:
                out.setdefault(current, set())
        elif line.startswith("@@") and current is not None:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            start, count = int(m.group(1)), int(m.group(2) or "1")
            out[current].update(range(start, start + count))
    return out


def land(base: str, label: str) -> int:
    landed, pending = header_numbers()
    if pending == 0:
        print("format-version: nothing pending")
        return 0
    if git("status", "--porcelain", "--untracked-files=no").strip():
        sys.exit("format-version: the tree has uncommitted changes")
    number = {k: landed + k for k in range(1, pending + 1)}

    def sub(text: str) -> str:
        def one(m: re.Match[str]) -> str:
            k = k_of(m.group(2))
            if k not in number:
                sys.exit(f"format-version: {m.group(0)} but only {pending} "
                         "pending")
            n = number[k]
            return str(n) if m.group(1) == "kVersion" else f"v{n}"
        return TOKEN.sub(one, text)

    # 1. The lines the branch added, in every file it touched.
    for rel, lines in added_lines(base).items():
        path = ROOT / rel
        if rel in DESCRIBES or not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        parts = text.split("\n")
        if rel == FORMAT_MD:  # its policy section describes the rule
            section = None
            for i, line in enumerate(parts, 1):
                if line.startswith("## "):
                    section = line.strip()
                if section == POLICY_HEADING:
                    lines.discard(i)
        changed = False
        for i in lines:
            if 0 < i <= len(parts) and TOKEN.search(parts[i - 1]):
                if rel == HEADER and "constexpr int kVersionNEXT" in \
                        parts[i - 1]:
                    continue  # the placeholders' own definitions
                if rel == FORMAT_MD and parts[i - 1].startswith("| vNEXT"):
                    continue  # a pending row: step 3 moves it
                parts[i - 1] = sub(parts[i - 1])
                changed = True
        if changed:
            path.write_text("\n".join(parts), encoding="utf-8")

    # 2. Paths: a golden's name, and its "version" field.
    for rel in git("diff", "--name-only", "--diff-filter=A", base,
                   "HEAD").splitlines():
        if not TOKEN.search(rel):
            continue
        new = sub(rel)
        git("mv", rel, new)
        m = re.match(r"v(\d+)-", Path(new).name)
        if (str(Path(new).parent) in GOLDEN_DIRS and m
                and not new.endswith(".expect.json")):
            path = ROOT / new
            text = path.read_text(encoding="utf-8")
            old = json.loads(text).get("version")
            found = re.findall(rf'"version":\s*{old}\b', text)
            if len(found) != 1:
                sys.exit(f"format-version: {new}: cannot tell the top-level "
                         "\"version\" apart")
            text = re.sub(rf'("version":\s*){old}\b',
                          lambda mm: mm.group(1) + m.group(1), text)
            path.write_text(text, encoding="utf-8")

    # 3. format.md: the pending rows move into the history, numbered.
    lines = read(FORMAT_MD).split("\n")
    rows: list[str] = []
    section = None
    keep: list[str] = []
    for line in lines:
        if line.startswith("## "):
            section = line.strip()
        if section == PENDING_HEADING and ROW.match(line) and \
                ROW.match(line).group(1).startswith("vNEXT"):
            rows.append(line)
            continue
        keep.append(line)
    if len(rows) != pending:
        sys.exit(f"format-version: {pending} pending, but {len(rows)} pending "
                 f"row(s) in {FORMAT_MD}")
    rows.sort(key=lambda r: k_of(ROW.match(r).group(1)[5:]))
    numbered = [re.sub(r"^\|\s*vNEXT[1-9]?\s*\|",
                       f"| {number[k_of(ROW.match(r).group(1)[5:])]} |",
                       sub_rest(r, sub)) for r in rows]
    last = max(i for i, line in enumerate(keep)
               if (m := ROW.match(line)) and m.group(1) == str(landed))
    keep[last + 1:last + 1] = numbered
    (ROOT / FORMAT_MD).write_text("\n".join(keep), encoding="utf-8")

    # 4. The header: main's number moves, nothing is pending.
    text = read(HEADER)
    text = LANDED.sub(lambda m: f"{m.group(1)}{landed + pending};", text)
    text = PENDING.sub(lambda m: f"{m.group(1)}0;", text)
    (ROOT / HEADER).write_text(text, encoding="utf-8")

    git("add", "-u")  # the tree was clean when this began
    first, last_n = landed + 1, landed + pending
    span = f"v{first}" if pending == 1 else f"v{first}-v{last_n}"
    what = f"{label}'s format change" + ("s" if pending > 1 else "")
    git("commit", "-q", "-m", f"chore(format): number {what} {span}")
    print(f"format-version: numbered {span}")
    return 0


def sub_rest(row: str, sub) -> str:
    """A pending row's text after its first cell, placeholders numbered."""
    m = re.match(r"^(\|\s*vNEXT[1-9]?\s*\|)(.*)$", row)
    return m.group(1) + sub(m.group(2))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    c = sp.add_parser("check")
    c.add_argument("--landed", action="store_true")
    la = sp.add_parser("land")
    la.add_argument("--base", required=True)
    la.add_argument("--label", default="the branch")
    a = ap.parse_args()
    if a.cmd == "check":
        return check(a.landed)
    return land(a.base, a.label)


if __name__ == "__main__":
    sys.exit(main())
