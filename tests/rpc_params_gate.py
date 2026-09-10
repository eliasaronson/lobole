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
from method_doc_scan import table_rows  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")


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
          f"{len(declared)} describe rows")
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
