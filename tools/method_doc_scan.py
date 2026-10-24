#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Parse the MethodDoc tables in src/remote/Remote*.cpp.

The counterpart to rpc_params_scan.py, which reads what the HANDLERS touch.
This reads what the tables DECLARE — the name, the prose, and the parameter
vocabulary `describe` hands an agent.

It lives on its own because two gates need it and each asks a different
question of the same rows: rpc_params_gate compares the declared keys against
the scanned ones, and rpc_units_gate asks whether a dimensional key says what
unit it is in.
"""
import re

ROW_RE = re.compile(r'\{\s*"([a-z0-9_]+)"\s*,')
TABLE_RE = re.compile(r'MethodDoc\s+\w+\[\]\s*=\s*\{(.*?)\n\};', re.S)


def split_fields(row):
    """A MethodDoc row's top-level comma-separated initialisers.

    Splitting on bare commas is wrong twice over: the doc strings are full of
    them, and a nested brace initialiser would hide more. So walk the row
    tracking string state and brace depth.
    """
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


def literal(field):
    """One or more ADJACENT string literals, joined as C++ concatenates them.

    clang-format splits a long params list across lines exactly that way, so
    joining is required rather than cosmetic: reading only the last literal
    silently truncated four rows (pattern, query_geometry, set_configuration,
    set_study) into a prefix that still looked like a valid list.
    """
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', field))


def method_docs(paths):
    """{method: {"doc", "params", "flags", "returns", "since", "file"}}.

    `returns` is the fifth field where a row carries one, else "" — the
    response contract, which only the read family is required to state
    (tests/rpc_returns_gate.py). `flags` is the fourth as written, the
    names it ORs together (`remote::` dropped), so `"NotInBatch" in flags`
    asks what the dispatcher asks (tests/batch_rollback_gate.py).
    """
    out = {}
    for path in paths:
        with open(path) as f:
            src = f.read()
        for blk in TABLE_RE.finditer(src):
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
                fields = split_fields(row)
                doc = literal(fields[1]) if len(fields) > 1 else ""
                rets = literal(fields[4]) if len(fields) > 4 else ""
                flags = (set(re.findall(r'(?:remote::)?([A-Za-z]+)',
                                        fields[3]))
                         if len(fields) > 3 else set())
                # The sixth field, MethodDoc::since; 1 when the row omits it.
                since = (int(fields[5].strip()) if len(fields) > 5
                         and fields[5].strip().isdigit() else 1)
                if len(fields) < 3:
                    out[nm.group(1)] = {"doc": doc, "params": None,
                                        "flags": flags,
                                        "returns": rets, "since": since,
                                        "file": path}
                    continue
                ps = literal(fields[2])
                out[nm.group(1)] = {
                    "doc": doc,
                    "params": "*" if ps == "*" else sorted(param_names(ps)),
                    # The column as written: a typed one is the method's
                    # contract (rpc_contract.parse_column).
                    "column": ps,
                    "flags": flags,
                    "returns": rets,
                    "since": since,
                    "file": path,
                }
    return out


def param_names(column):
    """The plain key names of a params column, typed or not — `a?:int` is
    `a` (src/remote/RpcContract.h, ParamNames)."""
    from rpc_contract import split_top
    names = []
    for entry in split_top(column, ","):
        name = entry.split(":", 1)[0].strip().rstrip("?").strip()
        if name:
            names.append(name)
    return names


def table_rows(paths):
    """{method: [declared keys] | "*" | None} — the params alone."""
    return {k: v["params"] for k, v in method_docs(paths).items()}
