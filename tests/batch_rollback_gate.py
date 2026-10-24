#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: a method whose effect a batch's rollback cannot undo is kept out of
a batch (docs/scripting/rpc.md#batch).

A failed `batch` rolls back the undo histories it marked before its first
call (`MarkEdits`/`RollBackEdits`). Two kinds of call are beyond that, and a
batch that failed after one left it in place while answering "the document
is as it was":

  - a call that creates, deletes or switches a TAB: the marked tab's history
    is stashed where the rollback does not unwind it, and the new tab stays;
  - an edit no history records: a tab's name or folder, the requirements,
    the section cuts (all in project.json, with no undo), a document setting
    set outside a command (units, tube rules, the rollback bar), a motion
    pose written into the assembly, the git config.

Each such method carries `NotInBatch` in its MethodDoc row, or -- where only
some of its parameters do it (a CAM verb given another `job`, a drawing verb
given another `drawing`, a component claimed on another part, a part-scope
rename, a motion run told to `leave_at` a pose) -- is named in the
dispatcher's per-call check, `BatchRefusalFor`.

This reads the handlers for what they CALL: each method's branch, and every
function in src/ui and src/remote it reaches with the AppState, is searched
for the operations above. So a new handler that switches a tab, or calls a
helper that does, fails here until its row says so. The list of operations
is the one thing kept by hand, and it is short.

Every run first plants an unflagged handler that switches a tab and checks
that the audit names it: a gate nothing fails is not a gate.

Needs no app, no GPU and no display -- it reads the source.
"""
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

from method_doc_scan import method_docs  # noqa: E402
from rpc_params_scan import COND_RE, _scopes, _strip  # noqa: E402

REMOTE = os.path.join(ROOT, "src", "remote")
UI = os.path.join(ROOT, "src", "ui")

# What a rollback cannot undo, as the source spells it. A function whose body
# matches one, or calls (with the AppState) a function that does, is beyond
# the rollback.
SINKS = [
    ("switches or creates a tab",
     re.compile(r'\b(?:SetActivePart|StashActive)\s*\(|'
                r'\b(?:state|s)\s*\.\s*parts\s*\.\s*'
                r'(?:push_back|emplace_back|insert|erase)\b')),
    ("writes project.json, which has no undo (tab names and folders, the "
     "requirements)",
     re.compile(r'\bprojectMetaDirty\s*=\s*true|'
                r'\b(?:state|s)\s*\.\s*tabFolders\s*(?:\.\s*(?:push_back|'
                r'emplace_back|erase|insert|clear)\b|=[^=])|'
                r'std::erase\s*\(\s*(?:state|s)\s*\.\s*tabFolders|'
                r'\.folder\s*(?:=[^=]|\.clear\s*\()')),
    ("edits the section cuts, which are saved in project.json with no "
     "undo",
     re.compile(r'\b(?:state|s)\s*\.\s*sections\s*\.\s*(?:push_back|'
                r'emplace_back|erase|insert|clear)\b|'
                r'std::erase(?:_if)?\s*\(\s*(?:state|s)\s*\.\s*sections\b|'
                r'\bFindSection\s*\(')),
    ("poses the assembly at a motion study's sample with no undo snapshot",
     re.compile(r'\b(?:ApplyMotionPose|ApplyMotionSample|ApplyMotionValue)'
                r'\s*\(')),
    ("sets saved document state outside a command",
     re.compile(r'(?:\.|->)\s*(?:SetUnits|SetTube|SetCommitGeometry|SetAsset|'
                r'SetRollback)\s*\(')),
    ("writes the git repository or its config",
     re.compile(r'\bGitVault::(?:AbortMerge|AddRemote|Clone|CommitAll|'
                r'CommitMerge|CreateBranch|Fetch|Init|Merge|Push|RemoveRemote|'
                r'RestoreTo|SetPreferredIdentity|SwitchBranch|Tag)\s*\(')),
]

# The tab a call is GIVEN: these switch to the job or drawing their `job` /
# `drawing` parameter names. They are not sinks of their own: the batch
# check refuses such a parameter when it names another tab, for every row of
# the module that declares it (BatchRefusalFor). So they may appear only in
# that module, and only in a method whose row declares the key.
PARAM_SWITCHERS = {
    "SelectJob": ("RemoteCam.cpp", "job"),
    "SelectDrawing": ("RemoteDrawings.cpp", "drawing"),
}

# Where the path to a sink is not taken, said once with the reason. The
# call graph is by name and blind to branches, so a function that CAN reach
# a sink on a path its callers here never take is cut, not the method.
CUTS = {
    "ApplyProjectVarSnapshot": "applies an entry of the project history, "
                               "which the rollback unwinds",
    "RefreshExpressions": "measures expression requirements only, and an "
                          "expression visits no tab (MeasureOn)",
    "RollBackEdits": "is the rollback",
    "EnsureParts": "makes the first tab of an empty project, which a "
                   "rollback has no reason to remove",
}

# A method whose sink is not what it looks like, with the reason.
EXEMPT = {
    "batch": "runs its calls through HandleSemantic, which this gate "
             "audits call by call",
    "add_reference_image": "stores the image's bytes beside the sketch edit "
                           "it commits as a command; rolled back, the bytes "
                           "are an orphan the next save prunes",
}

DEF_RE = re.compile(r'^[A-Za-z][^\n;{}]*?\b([A-Za-z_]\w*)\s*\([^;{}]*\)\s*'
                    r'(?:const\s*)?(?:->[^{;]*)?\{', re.M)
# A call that hands the function the AppState: the functions that can reach
# the tabs and the project. Following only these keeps a name shared by an
# unrelated helper (Table, Row, Apply) from joining the graph.
CALL_RE = re.compile(r'\b([A-Za-z_]\w*)\s*\(\s*\*?state\b')
KEYWORDS = {"if", "for", "while", "switch", "return", "catch", "sizeof"}


def body_end(text, brace):
    depth = 0
    for i in range(brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return i
    return len(text)


def functions(sources):
    """[(name, file, start, end)] of every function defined at column 0."""
    out = []
    for path, bare in sources.items():
        for m in DEF_RE.finditer(bare):
            if m.group(1) in KEYWORDS:
                continue
            out.append((m.group(1), path, m.end() - 1,
                        body_end(bare, m.end() - 1)))
    return out


def sink_in(text):
    for what, rx in SINKS:
        if rx.search(text):
            return what
    return None


def tainted_functions(sources, funcs):
    """{name: why} for every function that reaches a sink."""
    bodies = {}
    for name, path, start, end in funcs:
        text = sources[path][start:end]
        # A dispatcher is audited method by method, not as a whole: that
        # one of its branches switches tabs says nothing of the others.
        if name not in CUTS and name not in PARAM_SWITCHERS and \
                not COND_RE.search(text):
            bodies.setdefault(name, []).append(text)
    why = {}
    for name, texts in bodies.items():
        for text in texts:
            if (what := sink_in(text)):
                why[name] = what
    changed = True
    while changed:
        changed = False
        for name, texts in bodies.items():
            if name in why:
                continue
            for text in texts:
                for callee in CALL_RE.findall(text):
                    if callee in why and callee != name:
                        why[name] = f"calls {callee}, which {why[callee]}"
                        changed = True
                        break
                if name in why:
                    break
    return why


def refusal_body(sources):
    """The text of BatchRefusalFor, the dispatcher's per-call check."""
    for path, bare in sources.items():
        if os.path.basename(path) != "RemoteDispatch.cpp":
            continue
        m = re.search(r'\bBatchRefusalFor\s*\([^;{}]*\)\s*\{', bare)
        if m:
            return bare[m.end() - 1:body_end(bare, m.end() - 1)]
    return None


def audit(sources, rows):
    """Problems, as lines. `sources` is {path: text}, `rows` method_docs."""
    bare = {p: _strip(t) for p, t in sources.items()}
    funcs = functions(bare)
    why = tainted_functions(bare, funcs)
    problems = []

    refusal = refusal_body(bare)
    if refusal is None:
        problems.append("RemoteDispatch.cpp has no BatchRefusalFor -- the "
                        "per-call check this gate reads moved")
        refusal = ""
    by_params = set(re.findall(r'"([a-z0-9_]+)"', refusal))
    for helper in ("CamMethodDocs", "DrawingMethodDocs"):
        if helper not in refusal:
            problems.append(f"BatchRefusalFor no longer reads {helper}(): "
                            f"the verbs given another tab are not refused")

    def covered(method):
        flags = rows.get(method, {}).get("flags", set())
        return "NotInBatch" in flags or method in by_params or \
            method in EXEMPT

    for path in sorted(p for p in bare if os.path.dirname(p) == REMOTE and
                       os.path.basename(p).startswith("Remote")):
        text = bare[path]
        scopes = _scopes(text)
        # The handlers a batch's call runs through (HandleSemantic and the
        # domain Handle*Rpc it hands on to). The parking that runs before a
        # single call is never reached from a batch.
        dispatchers = [(s, e) for n, f, s, e in funcs
                       if f == path and n.startswith("Handle") and
                       COND_RE.search(text[s:e])]

        def methods_at(pos):
            best = None
            for names, start, end in scopes:
                if start <= pos <= end and (best is None or
                                            start > best[1]):
                    best = (names, start)
            return best[0] if best else None

        hits = []
        for what, rx in SINKS:
            for m in rx.finditer(text):
                hits.append((m.start(), what, None))
        direct = {pos for pos, _w, _s in hits}
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(\s*\*?state\b', text):
            callee = m.group(1)
            if m.start() in direct:
                continue  # the sink itself, already reported
            if callee in PARAM_SWITCHERS:
                hits.append((m.start(), None, callee))
            elif callee in why and callee not in CUTS:
                hits.append((m.start(), f"calls {callee}, which {why[callee]}",
                             None))
        for pos, what, switcher in hits:
            names = methods_at(pos)
            line = text.count("\n", 0, pos) + 1
            where = f"{os.path.relpath(path, ROOT)}:{line}"
            if names is None:
                # Not inside any method's branch: a helper (followed through
                # the call graph) or a dispatcher's preamble, which runs for
                # every method after it.
                if not any(s <= pos <= e for s, e in dispatchers):
                    continue
                if switcher and os.path.basename(path) == \
                        PARAM_SWITCHERS[switcher][0]:
                    continue
                problems.append(
                    f"{where}: a dispatcher's preamble "
                    f"{what or 'calls ' + switcher}, so every method after "
                    f"it can -- move it into the methods that need it")
                continue
            for method in names:
                if switcher:
                    module, key = PARAM_SWITCHERS[switcher]
                    row = rows.get(method)
                    ok = (row is not None and
                          os.path.basename(row["file"]) == module and
                          isinstance(row["params"], list) and
                          key in row["params"])
                    if not ok and not covered(method):
                        problems.append(
                            f"{where}: {method} calls {switcher}, which "
                            f"switches to the tab its `{key}` names, but the "
                            f"batch check only knows the `{key}` of "
                            f"{module}'s rows -- flag it NotInBatch")
                    continue
                if not covered(method):
                    problems.append(
                        f"{where}: {method} {what}, which a batch's rollback "
                        f"cannot undo -- give its row NotInBatch, or refuse "
                        f"the parameters that do it in BatchRefusalFor "
                        f"(docs/scripting/rpc.md#batch)")
    for method in sorted(by_params):
        if method in rows and "NotInBatch" in rows[method]["flags"]:
            problems.append(f"{method} is NotInBatch AND named in "
                            f"BatchRefusalFor: one of the two is stale")
    return sorted(set(problems))


def load():
    paths = sorted(glob.glob(os.path.join(REMOTE, "*.cpp")) +
                   glob.glob(os.path.join(UI, "*.cpp")))
    sources = {}
    for p in paths:
        with open(p) as f:
            sources[p] = f.read()
    rows = method_docs(sorted(glob.glob(os.path.join(REMOTE, "Remote*.cpp"))))
    return sources, rows


def planted(sources, rows):
    """The same tree with one more method: an unflagged handler that
    switches tabs through a helper, the shape a real one would take."""
    path = os.path.join(REMOTE, "RemoteProject.cpp")
    sources = dict(sources)
    sources[path] = sources[path] + (
        '\nvoid PlantedHop(ui::AppState& state) {\n'
        '  ui::SetActivePart(state, 0);\n}\n'
        'std::optional<json> PlantedRpc(ui::AppState& state,\n'
        '                               const std::string& m) {\n'
        '  if (m == "planted_tab_hop") {\n'
        '    PlantedHop(state);\n'
        '    return json::object();\n'
        '  }\n'
        '  return std::nullopt;\n}\n')
    rows = dict(rows)
    rows["planted_tab_hop"] = {"flags": set(), "params": [], "file": path}
    return sources, rows


def main():
    sources, rows = load()
    control = audit(*planted(sources, rows))
    if not any("planted_tab_hop" in p for p in control):
        print("batch_rollback_gate: the planted tab switch was NOT found -- "
              "the audit is blind")
        return 1
    problems = audit(sources, rows)
    if problems:
        print(f"batch_rollback_gate: {len(problems)} problem(s):")
        for p in problems:
            print("  " + p)
        return 1
    flagged = sorted(m for m, r in rows.items() if "NotInBatch" in r["flags"])
    print(f"batch_rollback_gate: ok ({len(flagged)} methods NotInBatch, the "
          f"planted tab switch found)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
