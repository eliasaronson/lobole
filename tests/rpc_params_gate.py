#!/usr/bin/env python3
"""Gate: every RPC method's `describe` parameter list matches its handler.

The vocabulary a method accepts is decided by which keys its handler READS —
that is literally how `RpcParams` defines it, and how the dispatcher's
unknown-key rejection works (RemoteServer.cpp: "we learn a method's vocabulary
by which keys it reads"). But that knowledge only exists at runtime, after the
handler has run, so `describe` cannot harvest it; the MethodDoc `params` field
has to be written down.

This gate makes the written-down copy honest, in BOTH directions:

  - a key the handler reads with no row  -> an agent cannot discover it
  - a key in the row nobody reads        -> an agent sends it and the strict
                                            unknown-key check REJECTS the call

The second direction is the one worth having. A `describe` that advertises a
parameter the dispatcher then refuses is worse than saying nothing, and it is
the same failure shape as the `measure` row that promised angle/radius for a
year while the handler implemented distance alone.

Needs no app, no GPU and no display — it reads the source.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from rpc_params_scan import scan_files  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")
ROW_RE = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,')


def table_rows(paths):
    """{method: [declared keys] | "*" | None} from the MethodDoc tables."""
    out = {}
    for path in paths:
        with open(path) as f:
            src = f.read()
        for blk in re.finditer(r'MethodDoc\s+\w+\[\]\s*=\s*\{(.*?)\n\};',
                               src, re.S):
            body = blk.group(1)
            depth, start, rows = 0, None, []
            for i, c in enumerate(body):
                if c == '{':
                    if depth == 0:
                        start = i
                    depth += 1
                elif c == '}':
                    depth -= 1
                    if depth == 0:
                        rows.append(body[start:i + 1])
            for row in rows:
                nm = ROW_RE.match(row)
                if not nm:
                    continue
                fields = _split_fields(row)
                if len(fields) < 3:
                    out[nm.group(1)] = None      # row has no params field
                    continue
                # A field is one or more ADJACENT string literals, which C++
                # concatenates — and clang-format splits a long params list
                # across lines exactly that way, so joining is required, not
                # cosmetic. Reading only the last literal silently truncated
                # four rows here (pattern, query_geometry, set_configuration,
                # set_study) into a prefix that still looked like a valid list.
                ps = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', fields[2]))
                out[nm.group(1)] = "*" if ps == "*" else \
                    sorted(k.strip() for k in ps.split(",") if k.strip())
    return out


def _split_fields(row):
    """A MethodDoc row's top-level comma-separated initialisers.

    Splitting on bare commas is wrong twice over: the doc strings are full of
    them, and a nested brace initialiser would hide more. So walk the row
    tracking string state and brace depth."""
    inner = row[1:-1] if row.startswith("{") else row
    fields, depth, buf, i, n = [], 0, [], 0, len(inner)
    while i < n:
        c = inner[i]
        if c == '"':
            j = i + 1
            while j < n and inner[j] != '"':
                j += 2 if inner[j] == '\\' else 1
            buf.append(inner[i:j + 1])
            i = j + 1
            continue
        if c in "{[(":
            depth += 1
        elif c in "}])":
            depth -= 1
        elif c == "," and depth == 0:
            fields.append("".join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    fields.append("".join(buf))
    return fields


def main():
    paths = sorted(os.path.join(REMOTE, f) for f in os.listdir(REMOTE)
                   if f.startswith("Remote") and f.endswith(".cpp"))
    scanned = scan_files(paths)
    declared = table_rows(paths)

    problems = []
    for name, info in sorted(scanned.items()):
        if name not in declared:
            problems.append(
                f"{name}: handler branch exists but no MethodDoc row "
                f"(it would vanish from `describe` entirely)")
            continue
        want = "*" if info["opaque"] else info["keys"]
        got = declared[name]
        if got is None:
            problems.append(f"{name}: MethodDoc row has no params initialiser")
        elif got != want:
            w = "*" if want == "*" else (", ".join(want) or "(none)")
            g = "*" if got == "*" else (", ".join(got) or "(none)")
            extra = ""
            if want != "*" and got != "*":
                add = sorted(set(want) - set(got))
                rm = sorted(set(got) - set(want))
                if add:
                    extra += f"  [+{', '.join(add)}]"
                if rm:
                    extra += f"  [-{', '.join(rm)}]"
            problems.append(
                f"{name}: handler reads [{w}], row says [{g}]{extra}")
    for name in sorted(set(declared) - set(scanned)):
        problems.append(
            f"{name}: MethodDoc row with no handler branch "
            f"(describe advertises a method the dispatcher does not answer)")

    print(f"rpc params gate: {len(scanned)} handler branches, "
          f"{len(declared)} describe rows")
    if problems:
        print(f"\n{len(problems)} mismatch(es):")
        for pr in problems:
            print("  " + pr)
        print("\nFix the row beside the branch, or — when the keys are read "
              "somewhere\nthe scanner cannot see (a lambda taking the key as "
              "an argument, a helper) —\nadd a `// rpc-params: a, b, c` "
              "comment inside the branch. `// rpc-params: opaque`\nis for a "
              "genuinely free-form params object (p.raw()).")
        return 1
    print("every describe row matches the keys its handler reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
