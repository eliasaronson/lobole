# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The typed MethodDoc `params` column, read in Python.

The twin of src/remote/RpcContract.{h,cpp}
(docs/scripting/rpc.md#typed-contracts): the generator types the SDK from it
(tools/generate_rpc_contracts.py) and the gates check rows with it, all
without a running app. tests/rpc_contracts_smoke.py holds this reading of
every row equal to the server's `request_schema`.

    "feature_id?:int>=0, feature:object, type?:@mate_types, mode?:'a'|'b'"
"""
import re

BASES = ("int", "number", "bool", "string", "object", "null", "any",
         "revision")


class ContractSyntaxError(ValueError):
    pass


def split_top(s, sep):
    """Split outside 'literals' (which may hold the separator)."""
    out, cur, quoted = [], [], False
    for c in s:
        if c == "'":
            quoted = not quoted
        if c == sep and not quoted:
            out.append("".join(cur))
            cur = []
            continue
        cur.append(c)
    out.append("".join(cur))
    return out


_ALT_RE = re.compile(r"^([a-z]+)((?:(?:>=|<=|>|<)-?[0-9.eE+-]+)*)(\[(\d*)\])?$")
_RANGE_RE = re.compile(r"(>=|<=|>|<)(-?[0-9.eE+-]+)")


def _alt(key, a):
    if not a:
        raise ContractSyntaxError(f"contract key '{key}': an empty type")
    if a[0] == "'":
        if len(a) < 2 or a[-1] != "'":
            raise ContractSyntaxError(
                f"contract key '{key}': unterminated literal {a}")
        return {"kind": "literal", "text": a[1:-1]}
    if a[0] == "@":
        if not re.fullmatch(r"[a-z_]+", a[1:]):
            raise ContractSyntaxError(
                f"contract key '{key}': vocabulary '{a[1:]}' is not a "
                f"describe key")
        return {"kind": "vocab", "text": a[1:]}
    m = _ALT_RE.match(a)
    if not m or m.group(1) not in BASES:
        raise ContractSyntaxError(f"contract key '{key}': bad type '{a}'")
    alt = {"kind": m.group(1), "array": m.group(3) is not None,
           "length": int(m.group(4)) if m.group(4) else None}
    for op, num in _RANGE_RE.findall(m.group(2)):
        if alt["kind"] not in ("int", "number"):
            raise ContractSyntaxError(
                f"contract key '{key}': a range applies to int or number")
        v = float(num)
        if op.startswith(">"):
            alt["min"], alt["min_exclusive"] = v, op == ">"
        else:
            alt["max"], alt["max_exclusive"] = v, op == "<"
    return alt


def parse_column(column):
    """[{name, required, type, alts}] for a typed column, [] for "" (a method
    that takes nothing), None for an untyped one or "*"."""
    column = column.strip()
    if column == "":
        return []
    if column == "*":
        return None
    entries = [e.strip() for e in split_top(column, ",")]
    if not any(":" in e for e in entries):
        return None
    params, seen = [], set()
    for e in entries:
        name, sep, typ = e.partition(":")
        name = name.strip()
        required = not name.endswith("?")
        name = name.rstrip("?").strip()
        if not name:
            raise ContractSyntaxError(f"contract entry '{e}' names no key")
        if not sep:
            raise ContractSyntaxError(
                f"contract key '{name}': untyped in a typed row")
        if name in seen:
            raise ContractSyntaxError(f"contract key '{name}': listed twice")
        seen.add(name)
        typ = typ.strip()
        params.append({"name": name, "required": required, "type": typ,
                       "alts": [_alt(name, a.strip())
                                for a in split_top(typ, "|")]})
    return params


def revision_schema():
    ident = {"type": "integer", "minimum": 0, "maximum": 9007199254740991.0}
    return {"type": "object",
            "properties": {"session": {"type": "string", "minLength": 1},
                           "part_id": ident, "instance": ident,
                           "version": ident},
            "required": ["session", "part_id", "instance", "version"],
            "additionalProperties": False}


def _alt_schema(alt, vocab):
    k = alt["kind"]
    if k == "vocab":
        if vocab is None or alt["text"] not in vocab:
            raise ContractSyntaxError(f"no vocabulary '{alt['text']}'")
        s = {"type": "string", "enum": list(vocab[alt["text"]])}
    elif k == "literal":
        s = {"type": "string", "enum": [alt["text"]]}
    elif k == "revision":
        s = revision_schema()
    elif k == "any":
        s = {}
    else:
        s = {"type": {"int": "integer", "bool": "boolean"}.get(k, k)}
    if "min" in alt:
        s["exclusiveMinimum" if alt["min_exclusive"] else "minimum"] = alt["min"]
    if "max" in alt:
        s["exclusiveMaximum" if alt["max_exclusive"] else "maximum"] = alt["max"]
    if not alt.get("array"):
        return s
    arr = {"type": "array", "items": s}
    if alt["length"] is not None:
        arr["minItems"] = arr["maxItems"] = alt["length"]
    return arr


def request_schema(params, vocab):
    """The JSON Schema the server publishes for this contract."""
    props, required = {}, []
    for p in params:
        alts, literals = [], []
        for a in p["alts"]:
            if a["kind"] == "literal":
                literals.append(a["text"])
                continue
            if literals:
                alts.append({"type": "string", "enum": literals})
                literals = []
            alts.append(_alt_schema(a, vocab))
        if literals:
            alts.append({"type": "string", "enum": literals})
        props[p["name"]] = alts[0] if len(alts) == 1 else {"anyOf": alts}
        if p["required"]:
            required.append(p["name"])
    return {"type": "object", "properties": props, "required": required,
            "additionalProperties": False}
