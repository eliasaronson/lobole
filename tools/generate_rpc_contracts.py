#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Generate the typed SDK stubs from the typed MethodDoc rows.

The rows in src/remote/Remote*.cpp are the one source
(docs/scripting/rpc.md#typed-contracts). This writes python/lobole/rpc.py:
a TypedDict of each contracted method's parameters and an `RpcSurface`
class with one typed method per contracted RPC, which `lobole.sdk.App`
inherits for a type checker only. Checked in; `--check` (ctest
`rpc_contracts_generated`) fails when it is stale. Needs no running app.
"""
import argparse
import glob
import keyword
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from method_doc_scan import method_docs  # noqa: E402
from method_doc_scan import literal  # noqa: E402
from rpc_contract import (parse_column, parse_result_column,  # noqa: E402
                          request_schema, result_schema)

OUT = ROOT / "python/lobole/rpc.py"


def contracts():
    """{method: (column, params)} for every typed or parameterless row."""
    paths = sorted(glob.glob(str(ROOT / "src/remote/Remote*.cpp")))
    out = {}
    for name, row in sorted(method_docs(paths).items()):
        params = parse_column(row["column"] or "")
        if params is not None:
            out[name] = (row["column"], params)
    return out


def _header_literal(name):
    """A `const char* name = "..." "...";` or a `std::array` of literals in
    core/io/Serialization.h, joined or listed."""
    text = (ROOT / "src/core/io/Serialization.h").read_text()
    m = re.search(name + r"\s*=\s*\{?(.*?)\}?;", text, re.S)
    if not m:
        raise SystemExit(f"no {name} in Serialization.h")
    return m.group(1)


def feature_contracts():
    """{type: whole contract column} from kFeatureTypes' rows
    (docs/scripting/rpc.md#typed-feature-specs), each with its `type` and
    the keys every spec may carry, as the server composes it."""
    common = literal(_header_literal("kFeatureCommonContract"))
    src = (ROOT / "src/core/io/SerializeFeatures.cpp").read_text()
    table = src[src.index("kFeatureTypes[] = {"):]
    table = table[:table.index("\n};")]
    out = {}
    for m in re.finditer(r'\{\s*"([A-Za-z0-9]+)",\s*(?:\d+|192\d*|197\d*),((?:\s*"(?:[^"\\]|\\.)*")+)'
                         r'\s*,((?:\s*"(?:[^"\\]|\\.)*")+)\s*\}', table):
        out[m.group(1)] = (f"type:'{m.group(1)}', {common}, "
                           + literal(m.group(3)))
    return out


def results():
    """{method: parsed returns column} for every row whose `returns` is
    typed (docs/scripting/rpc.md#typed-results)."""
    paths = sorted(glob.glob(str(ROOT / "src/remote/Remote*.cpp")))
    out = {}
    for name, row in sorted(method_docs(paths).items()):
        parsed = parse_result_column(row["returns"] or "")
        if parsed is not None:
            out[name] = parsed
    return out


def _result_classes(cls, parsed, out):
    """The TypedDicts for one object literal, nested ones first. The
    functional form, because a key may be a Python keyword (`from`)."""
    fields = []
    for e in parsed["entries"]:
        types = []
        for a in e["alts"]:
            if a["kind"] == "object_lit":
                sub = cls + title(e["name"])
                _result_classes(sub, a, out)
                t = sub
                if a.get("array"):
                    t = f"list[{t}]"
                for _ in a.get("outer") or []:
                    t = f"list[{t}]"
            elif a["kind"] == "literal":
                t = f"Literal[{a['text']!r}]"
            else:
                t = _result_alt_type(a)
            if t not in types:
                types.append(t)
        t = " | ".join(types)
        fields.append(f"    {e['name']!r}: "
                      + (t if e["required"] else f"NotRequired[{t}]") + ",")
    out.append("")
    out.append(f"{cls} = TypedDict({cls!r}, {{")
    out += fields
    out.append("})")


def _result_alt_type(alt):
    t = {"int": "int", "number": "float", "bool": "bool", "string": "str",
         "object": "dict[str, Any]", "null": "None", "any": "Any",
         "revision": "Revision", "vocab": "str"}[alt["kind"]]
    if alt.get("array"):
        t = f"list[{t}]"
    for _ in alt.get("outer") or []:
        t = f"list[{t}]"
    return t


def _alt_type(alt):
    k = alt["kind"]
    t = {"int": "int", "number": "float", "bool": "bool", "string": "str",
         "object": "Mapping[str, Any]", "null": "None", "any": "Any",
         "revision": "Revision", "vocab": "str"}.get(k)
    if alt.get("array"):
        return f"Sequence[{t}]"
    return t


def py_type(param):
    literals = [a["text"] for a in param["alts"] if a["kind"] == "literal"]
    types = []
    if literals:
        types.append("Literal[" + ", ".join(repr(x) for x in literals) + "]")
    for a in param["alts"]:
        if a["kind"] != "literal":
            t = _alt_type(a)
            if t not in types:
                types.append(t)
    return " | ".join(types)


def title(method):
    return "".join(w.title() for w in method.split("_"))


def api_version():
    text = (ROOT / "src/remote/ApiVersion.h").read_text()
    m = re.search(r"kApiVersion\s*=\s*(\d+)\s*;", text)
    if not m:
        raise SystemExit("no kApiVersion in src/remote/ApiVersion.h")
    return int(m.group(1))


def generate():
    rows = contracts()
    lines = [
        "# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors",
        "# SPDX-License-Identifier: AGPL-3.0-or-later",
        "",
        "# Generated by tools/generate_rpc_contracts.py from the typed",
        "# MethodDoc rows (docs/scripting/rpc.md#typed-contracts). Do not edit.",
        "#",
        "# Annotations are deferred: a params dict names FeatureSpec, which is",
        "# defined after it, and only Python 3.14 defers them by itself.",
        "from __future__ import annotations",
        "",
        "from collections.abc import Mapping, Sequence",
        "from typing import Any, Literal, NotRequired, TypedDict, Unpack",
        "",
        "# The RPC surface these were generated against (src/remote/ApiVersion.h);",
        "# lobole.sdk.App warns when the app reports another.",
        f"API_VERSION = {api_version()}",
        "",
        "",
        "class Revision(TypedDict):",
        '    """A part revision: geometry_snapshot, get_state and every guarded',
        '    write return one; `expect_revision` names it."""',
        "    session: str",
        "    part_id: int",
        "    instance: int",
        "    version: int",
        "",
        "",
        "# The typed rows as written: the server parses the same text.",
        "COLUMNS: dict[str, str] = {",
    ]
    for name, (column, _) in rows.items():
        lines.append(f"    {name!r}: {column!r},")
    lines += ["}", ""]
    for name, (_, params) in rows.items():
        if not params:
            continue
        lines += ["", f"class {title(name)}Params(TypedDict):"]
        for p in params:
            t = py_type(p)
            # A spec typed by its feature type, or any mapping: generators
            # build specs as plain dicts, and only a spec annotated as one
            # (`ExtrudeSpec`) is checked field by field.
            if name in ("add_feature", "modify_feature") and \
                    p["name"] == "feature":
                t = "FeatureSpec | Mapping[str, Any]"
            # A key that is a Python keyword (`async`, `from`) is passed with
            # a trailing underscore, which the SDK strips (sdk.md).
            key = p["name"] + ("_" if keyword.iskeyword(p["name"]) else "")
            lines.append(f"    {key}: "
                         + (t if p["required"] else f"NotRequired[{t}]"))
    features = feature_contracts()
    lines += ["", "", "# Each feature type's spec, as add_feature and modify_feature take it",
              "# (docs/scripting/rpc.md#typed-feature-specs): its schema, the keys",
              "# get_state stamps around a spec and the old primitive keys the parser",
              "# refuses by name (neither is held to a schema), and its type.",
              "FEATURE_SCHEMAS: dict[str, dict[str, Any]] = {"]
    for name, column in features.items():
        lines.append(f"    {name!r}: {request_schema(parse_column(column), None)!r},")
    def keys(name):
        return repr(tuple(re.findall(r'"([a-z_]+)"', _header_literal(name))))
    lines += ["}",
              "FEATURE_ENVELOPE_KEYS: tuple[str, ...] = "
              + keys("kFeatureEnvelopeKeys"),
              "PRIMITIVE_RETIRED_KEYS: tuple[str, ...] = "
              + keys("kPrimitiveRetiredKeys"),
              ""]
    for name, column in features.items():
        fields = []
        for prm in parse_column(column):
            t = py_type(prm)
            fields.append(f"    {prm['name']!r}: "
                          + (t if prm["required"] else f"NotRequired[{t}]") + ",")
        lines += ["", f"{name}Spec = TypedDict({name + 'Spec'!r}, {{"] + fields + ["})"]
    lines += ["", "FeatureSpec = " + " | ".join(f"{n}Spec" for n in features), ""]
    typed_results = results()
    lines += ["", "", "# Each typed `returns` column, as the JSON Schema an answer is held to",
              "# (a strict lobole.client validates against these), and as types.",
              "RESULT_SCHEMAS: dict[str, dict[str, Any]] = {"]
    for name, parsed in typed_results.items():
        lines.append(f"    {name!r}: {result_schema(parsed, None)!r},")
    lines += ["}", ""]
    for name, parsed in typed_results.items():
        _result_classes(f"{title(name)}Result", parsed, lines)
    lines += [
        "",
        "",
        "class RpcSurface:",
        '    """The contracted RPCs, typed. `lobole.sdk.App` inherits this for a',
        "    type checker only: at runtime every method is the generated",
        '    `describe` surface, validated against the same contract."""',
    ]
    for name in sorted(set(rows) | set(typed_results)):
        args = "self, _timeout: float | None = None"
        if name not in rows:
            args += ", **params: Any"
        elif rows[name][1]:
            args += f", **params: Unpack[{title(name)}Params]"
        ret = (f"{title(name)}Result" if name in typed_results
               else "dict[str, Any]")
        lines += ["", f"    def {name}({args}) -> {ret}:",
                  "        raise NotImplementedError"]
    return {OUT: "\n".join(lines) + "\n"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    stale = []
    for path, text in generate().items():
        if path.exists() and path.read_text() == text:
            continue
        if args.check:
            stale.append(os.path.relpath(path, ROOT))
        else:
            path.write_text(text)
    if stale:
        parser.exit(1, "Regenerate the RPC stubs (python3 "
                       "tools/generate_rpc_contracts.py): "
                       + ", ".join(stale) + "\n")


if __name__ == "__main__":
    main()
