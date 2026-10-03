#!/usr/bin/env python3
"""Gate: every RPC method's `describe` parameter list matches its handler.

The vocabulary a method accepts is decided by which keys its handler READS —
that is literally how `RpcParams` defines it. But that knowledge only exists
at runtime, after the handler has run, so it is written down: the MethodDoc
`params` field, which `describe` hands an agent and the dispatcher refuses
unknown keys against BEFORE the handler runs
(docs/scripting/rpc.md#a-refused-call-changes-nothing).

This gate makes the written-down copy honest, in BOTH directions:

  - a key the handler reads with no row  -> the dispatcher REFUSES a key the
                                            handler takes, and an agent
                                            cannot discover it either
  - a key in the row nobody reads        -> an agent sends it and the strict
                                            unknown-key check REJECTS the call
  - a branch that hands its params to a HELPER with no annotation -> the row
    describes a fraction of the method and the gate cannot tell

The second direction is the one worth having. A `describe` that advertises a
parameter the dispatcher then refuses is worse than saying nothing, and it is
the same failure shape as the `measure` row that promised angle/radius for a
year while the handler implemented distance alone.

Needs no app, no GPU and no display — it reads the source.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from rpc_params_scan import scan_files  # noqa: E402
from method_doc_scan import method_docs  # noqa: E402
from rpc_contract import ContractSyntaxError, parse_column  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")

# Keys the DISPATCHER consumes before the handler runs, so no branch reads
# them: `expect_revision` is the revision guard
# (docs/scripting/rpc.md#the-revision-guard). A row lists one to accept it,
# and only a typed row may, with its type.
DISPATCHER_KEYS = {"expect_revision": "revision"}


def contract_problems(name, column, info):
    """A typed row (docs/scripting/rpc.md#typed-contracts) against its
    handler: every key typed (the parser refuses a half-typed row), a `?`
    exactly where the handler can do without the key, every literal of an
    enum one the branch actually compares against."""
    try:
        params = parse_column(column)
    except ContractSyntaxError as e:
        return [f"{name}: the typed row does not parse: {e}"]
    if params is None:
        return [f"{name}: lists {k} without a type — only a typed row "
                f"carries a dispatcher key" for k in DISPATCHER_KEYS
                if k in column.replace(" ", "").split(",")]
    out = []
    for p in params:
        k = p["name"]
        if k in DISPATCHER_KEYS:
            if p["type"] != DISPATCHER_KEYS[k] or p["required"]:
                out.append(f"{name}: {k} is `{k}?:{DISPATCHER_KEYS[k]}`")
            continue
        if k in info["annotated_keys"] and k not in info["optional"]:
            continue  # read where the scanner cannot see how
        optional = k in info["optional"]
        if p["required"] and optional:
            out.append(f"{name}: '{k}' is required in the row, but the "
                       f"handler reads it with value()/contains(), so a call "
                       f"may leave it out — mark it `{k}?`")
        if not p["required"] and not optional and k in info["keys"]:
            out.append(f"{name}: '{k}' is optional in the row, but the "
                       f"handler only reads it with at(), so a call without "
                       f"it is refused inside the handler — drop the `?`")
        for a in p["alts"]:
            if a["kind"] == "literal" and a["text"] not in info["literals"]:
                out.append(f"{name}: '{k}' accepts '{a['text']}', which the "
                           f"branch never compares against — a stale "
                           f"literal (a vocabulary the parser owns is "
                           f"`@name`)")
    return out


def main():
    paths = sorted(os.path.join(REMOTE, f) for f in os.listdir(REMOTE)
                   if f.startswith("Remote") and f.endswith(".cpp"))
    scanned = scan_files(paths)
    docs = method_docs(paths)
    declared = {}
    for k, v in docs.items():
        ps = v["params"]
        if isinstance(ps, list):
            ps = [x for x in ps if x not in DISPATCHER_KEYS]
        declared[k] = ps

    problems = []
    typed = 0
    for name, info in sorted(scanned.items()):
        column = docs.get(name, {}).get("column")
        if column is None or info["opaque"]:
            continue
        found = contract_problems(name, column, info)
        problems += found
        try:
            typed += bool(parse_column(column))
        except ContractSyntaxError:
            pass
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
        elif info["handoff"] and not info["annotated"] and not info["opaque"]:
            # The row may even MATCH here: it matches what the scanner can
            # see, and the helper is what it cannot. Saying so is the point.
            problems.append(
                f"{name}: hands its params to "
                f"{', '.join(info['handoff'])} — the scanner cannot follow "
                f"that, so the row describes only the keys read in the "
                f"branch. Declare what the helper reads with "
                f"`// rpc-params: a, b, c` (or `// rpc-params: none`)")
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
          f"{len(declared)} describe rows, {typed} typed")
    if problems:
        print(f"\n{len(problems)} mismatch(es):")
        for pr in problems:
            print("  " + pr)
        print("\nFix the row beside the branch, or — when the keys are read "
              "somewhere\nthe scanner cannot see (a lambda taking the key as "
              "an argument, a helper) —\nadd a `// rpc-params: a, b, c` "
              "comment inside the branch. `// rpc-params: none` says the "
              "helper reads none of\nthem; `// rpc-params: opaque` is for a "
              "genuinely free-form params object (p.raw()).")
        return 1
    print("every describe row matches the keys its handler reads")
    return 0


if __name__ == "__main__":
    sys.exit(main())
