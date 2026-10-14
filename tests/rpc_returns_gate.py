#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: every read-only RPC says what it RETURNS, and says it truthfully.

`rpc_params_gate` keeps the PARAMETER contract honest, and it is why a wrong
key is always refused by name. The RESULT contract had nowhere to live: a
caller that guesses the shape wrong does not get a refusal, it gets a
KeyError three calls later, somewhere else entirely.

Three that cost real time while building a chassis generator, all in one
afternoon:

  mass_properties   the row says "per body and total", so the total reads as
                    r["mass_g"]; it is r["total"]["mass_g"]
  list_elements     nests under `bodies` even when ONE body was asked for
  list_parts        returns parts, assemblies AND drawings, so iterating the
                    parts needs a `type` filter

So `MethodDoc` has a `returns` column, and it is REQUIRED for the family an
agent parses rather than merely calls: list_*, get_* and check_*. Everything
else may carry one and mostly will not — a mutating verb's answer is its
`regen` summary and its prose says so.

The truthfulness half is deliberately ONE-DIRECTIONAL. A key the row names
must appear in the handler's branch; keys the handler emits without a row are
NOT flagged, because a response nests (`bodies[]{name, volume}`) and a
scanner reading `{"name",` cannot tell a top-level key from a key three
levels down. Naming a key that does not exist is the failure worth catching:
it is the same shape as `describe` advertising a parameter the dispatcher
then refuses.

Needs no app, no GPU and no display — it reads the source.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from method_doc_scan import method_docs  # noqa: E402
from rpc_contract import parse_result_column  # noqa: E402
from rpc_params_scan import scan_files  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")

# Keys a SHARED response helper attaches, which therefore appear in no
# branch: AttachReceipt puts `receipt` on every oracle's answer, and the
# mutating verbs' `regen` arrives the same way.
HELPER_KEYS = {"receipt", "regen"}

# A branch whose response is BUILT somewhere else says so, the way a branch
# whose params are read somewhere else says `// rpc-params:`. The named file
# is read for its string literals and they join the pool.
FROM_RE = re.compile(r"//\s*rpc-returns-from:\s*([^\n]+)")
LITERAL_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# The read family. `checkpoint_*` starts with "check" and is not one of them:
# it is a verb that moves the project's history about.
PREFIXES = ("list_", "get_", "check_")

# The readers named for their NOUN rather than a verb, which no prefix rule
# can catch. `mass_properties` is the one that started this: its row reads
# "per body and total", which makes the total look like r["mass_g"] when it
# is r["total"]["mass_g"].
#
# Still to do, and deliberately not guessed at: interference, measure,
# query_geometry, stack_up and compare_geometry are reads too, and their
# shapes want reading from the handler rather than inferring from the prose
# — writing a contract that has not been verified is worse than none.
NOUN_READERS = ("mass_properties", "contact_report", "tube_schedule")


def required(name):
    if name in NOUN_READERS:
        return True
    return name.startswith(PREFIXES) and not name.startswith("checkpoint_")


# A key as written in `returns`: `name`, `name?`, `name{a,b}`, `name[]{a,b}`.
KEY_RE = re.compile(r"([A-Za-z0-9_]+)")
NESTED_RE = re.compile(r"\{[^{}]*\}")


def declared_keys(spec):
    """The TOP-LEVEL keys of a `returns` spec — the braces dropped.

    Only the top level is checked against the handler, because only the top
    level is built THERE. A nested row usually comes from a serializer in
    core/io (OccurrenceToJson, MateToJson, FeatureToJson) which this scanner
    never reads, so checking nested names would fail every row that documents
    its rows properly — punishing exactly the rows worth having.
    """
    # A typed column (docs/scripting/rpc.md#typed-results) names its keys
    # outright; its words are types too, which are not keys.
    typed = parse_result_column(spec)
    if typed is not None:
        return sorted(e["name"] for e in typed["entries"])
    flat = spec
    while True:
        stripped = NESTED_RE.sub(" ", flat)
        if stripped == flat:
            break
        flat = stripped
    return sorted(set(KEY_RE.findall(flat)))


def main():
    paths = sorted(os.path.join(REMOTE, f) for f in os.listdir(REMOTE)
                   if f.startswith("Remote") and f.endswith(".cpp"))
    docs = method_docs(paths)
    branches = scan_files(paths)

    # What each handler branch could plausibly emit: every string literal
    # that is a JSON key in it. Deliberately generous (see the docstring).
    emitted = {}
    for name, info in branches.items():
        emitted[name] = set(info.get("literals", ())) | HELPER_KEYS

    # `// rpc-returns-from: <path>` anywhere in the remote layer lends that
    # file's literals to the method named on the preceding branch line.
    for path in paths:
        src = open(path).read()
        for mm in FROM_RE.finditer(src):
            rel = mm.group(1).strip()
            head = src.rfind('m == "', 0, mm.start())
            if head < 0:
                continue
            owner = src[head + 6:src.find('"', head + 6)]
            try:
                with open(os.path.join(ROOT, rel)) as f:
                    emitted.setdefault(owner, set()).update(
                        LITERAL_RE.findall(f.read()))
            except OSError:
                print(f"  (rpc-returns-from names {rel}, which is not a file)")

    problems = []
    checked = 0
    for name in sorted(docs):
        if not required(name):
            continue
        checked += 1
        spec = docs[name].get("returns") or ""
        if not spec.strip():
            problems.append(
                f"{name}: no `returns` — a caller has to guess the shape, and "
                f"guessing wrong is a KeyError somewhere else, not a refusal")
            continue
        seen = emitted.get(name)
        if not seen:
            continue  # the scanner could not read this branch; presence only
        missing = [k for k in declared_keys(spec) if k not in seen]
        if missing:
            problems.append(
                f"{name}: `returns` names {', '.join(missing)}, which the "
                f"handler never emits — describe would promise a key that is "
                f"not there")

    print(f"rpc returns gate: {checked} read-only methods "
          f"({len(docs)} rows in all)")
    if problems:
        print(f"\n{len(problems)} problem(s):")
        for pr in problems:
            print("  " + pr)
        print("\nAdd the response's top-level keys to the row's fifth field, "
              "in the\nshape `params` uses: `total{mass_g,volume}`, "
              "`bodies[]{name,volume}`,\n`optional?`. A row with flags writes "
              "them both:\n  {\"name\", \"doc\", \"params\", "
              "remote::Deferrable, \"returns\"}\nand a row with none says "
              "`remote::NoFlags` rather than a bare 0.")
        return 1
    print("every read-only method states its response shape")
    return 0


if __name__ == "__main__":
    sys.exit(main())
