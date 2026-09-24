#!/usr/bin/env python3
"""Which interactive tools are tested on which TAB KINDS.

WHY
---
A feature tested only on a part tab can be dead on an assembly or drawing tab
while every test passes: handles, picks and RPC gates each have a per-tab-kind
path. For each interactive tool, this asks whether a test exercises it on
every tab kind it claims to support.

HOW, AND WHAT TO DISTRUST
-------------------------
The test list comes from the binary itself (`designer --gui-test-list`), so a
renamed or deleted test cannot silently linger here. Everything else is a
SOURCE SCAN against the marker tables below, which is a heuristic: a test is
credited to a tool when its body mentions one of that tool's markers, and to
a tab kind when its body mentions one of that kind's. Both tables are
hand-written and reviewable, which is the point — a generated matrix nobody
believes is worse than none.

So: a cell with a test name is good evidence. An EMPTY cell is a question,
not a verdict — the tool may genuinely not support that tab kind, or the
marker table may be missing a spelling. `supports` records the intended
answer per tool so the two can be compared.

Usage:  coverage_matrix.py [path-to-designer] [--out FILE]

Prints the matrix as markdown (or writes FILE). It is not committed: a copy
in the tree goes stale with the next GUI test, so run it when you want the
answer.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GUITESTS = os.path.join(ROOT, "src", "app", "guitests")

# A tab kind is identified by what a test does to get one. Tests reach an
# assembly three different ways (the action, the tab-bar + menu, the panel),
# so each kind lists every spelling rather than assuming one.
TAB_KINDS = {
    "part": [],  # the default: every test starts on one
    "assembly": ["AddAssembly(", "s_App->assembly", "###Occurrences",
                 "Assembly##addmenu", "AssemblyDocById"],
    "drawing": ["s_App->drawing", "Drawing##addmenu", "###DrawingPanel",
                "DrawingDocById"],
}
# NOT a tab kind, though it looks like one at first: CAM is a tool SESSION on
# a part tab (and Simulation is a mode on one). Listing it here made every CAM
# test count as "not on a part tab", which reported the CAM tool as untested
# on the only tab kind it runs on — a matrix confidently saying the opposite
# of the truth, which is exactly the failure this file exists to avoid.

# The INTERACTIVE tools — the ones driven by clicking or dragging in the 3D
# viewport, which is the half the RPC-driven smokes structurally cannot
# reach. `supports` is the intended answer, written by hand from the code:
# which tab kinds this tool is meant to work on at all.
TOOLS = [
    ("Measure",           ["###tb_measure", "s_App->measure", "MeasureOverlay"],
     ["part", "assembly"]),
    ("Section",           ["###tb_section", "sectionToolOpen", "ClearSections",
                           "AddSection("],
     ["part", "assembly"]),
    ("Mate",              ["mateTool", "###tb_mate", "AddMate("],
     ["assembly"]),
    ("Occurrence gizmo",  ["HandleGizmoInput", "gizmo", "PushOccurrenceLive"],
     ["assembly"]),
    ("Preview handles",   ["handleDrag"],
     ["part"]),
    ("Sketch dimensions", ["dimTool", "SketchUVToScreen", "label_drag"],
     ["part"]),
    ("Sketch drawing",    ["EnterSketchOnPlane", "sketchEdit"],
     ["part"]),
    ("CAM tool",          ["camToolOpen", "camDeriveOp", "###tb_job_pocket"],
     ["part"]),
    ("Simulation loads",  ["simulation", "StudyPanel", "###tb_simulation"],
     ["part"]),
    ("Feature tree",      ["###Model/", "FeatureTreePanel"],
     ["part", "assembly", "drawing"]),
    ("Tab bar",           ["##PartTabs"],
     ["part", "assembly", "drawing"]),
]

# Cells REVIEWED by hand, with the judgement recorded so they stop
# re-flagging. This is the point of the exercise: the count heuristic below
# over-flags, and tuning its threshold until it agrees with a manual reading
# would be fitting the tool to the answer. Review once, write down why, and
# what is left standing is the real question.
REVIEWED = {
    ("Measure", "assembly"):
        "targeted, not sparse: `whole_bodies_and_parts` asserts the OVERLAY "
        "on an assembly tab, which is the exact thing measure-v2 found "
        "missing there, and `assembly_distance` pins occurrence-qualified "
        "picks. The part side's extra tests are about part-only concerns "
        "(corner points, seeds-from-selection, copy-to-variable).",
    ("Section", "assembly"):
        "targeted: `offset_arrow_drags_on_an_assembly` IS the regression "
        "test for the bug this whole matrix exists because of, and "
        "`assembly_cut_is_capped_and_centered` covers the caps. Two tests "
        "aimed at the two known failures beat five aimed at nothing.",
}

# The name may be split over adjacent literals ("long_name_" "continued"),
# which C++ joins; group 2 holds them all and is joined below.
REGISTER = re.compile(
    r'IM_REGISTER_TEST\(engine,\s*"([a-z0-9_]+)"\s*,\s*((?:"[a-z0-9_]+"\s*)+)\)')
# The same, with the NAME computed rather than literal (a table-driven family).
LOOP_REGISTER = re.compile(
    r'IM_REGISTER_TEST\(engine,\s*"([a-z0-9_]+)"\s*,\s*(?!")[A-Za-z_][\w.>-]*\s*\)')


def test_bodies():
    """{'<cat>/<name>': body} — each registration up to the next one."""
    out = {}
    for fn in sorted(os.listdir(GUITESTS)):
        if not fn.endswith(".cpp"):
            continue
        with open(os.path.join(GUITESTS, fn)) as f:
            src = f.read()
        marks = [(m.start(), m.group(1),
                  "".join(re.findall(r'"([a-z0-9_]+)"', m.group(2))))
                 for m in REGISTER.finditer(src)]
        # A family registered from a table has no literal name to scan (the
        # U10 `defaults/*` gate is one per creation tool). Credit the loop's
        # body to every member, from the binary's list — otherwise 24 real
        # tests read as missing and the disagreement note cries wolf.
        for m in LOOP_REGISTER.finditer(src):
            pos = m.start()
            nxt = [p for p, _, _ in marks if p > pos]
            end = min(nxt) if nxt else len(src)
            marks.append((pos, m.group(1), "*"))
            out[f"{m.group(1)}/*"] = src[pos:end]
        for i, (pos, cat, name) in enumerate(marks):
            end = marks[i + 1][0] if i + 1 < len(marks) else len(src)
            out[f"{cat}/{name}"] = src[pos:end]
    return out


def listed_tests(binary):
    """The binary's own list, so a stale name cannot survive here."""
    if not binary or not os.path.exists(binary):
        return None
    try:
        r = subprocess.run([binary, "--gui-test-list"], capture_output=True,
                           text=True, timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    return {ln.strip() for ln in r.stdout.splitlines() if "/" in ln}


def classify(body):
    kinds = {k for k, marks in TAB_KINDS.items()
             if marks and any(m in body for m in marks)}
    return kinds or {"part"}


def main():
    args = sys.argv[1:]
    out_path = None
    if "--out" in args:
        i = args.index("--out")
        out_path = args[i + 1]
        del args[i:i + 2]
    binary = args[0] if args else os.path.join(ROOT, "build", "designer")
    bodies = test_bodies()
    listed = listed_tests(binary)
    note = ""
    if listed is None:
        note = ("> Generated WITHOUT the binary, so the test list is the "
                "source scan's and a deleted test could still appear. Re-run "
                "with a built `designer` to cross-check.\n\n")
    else:
        # Expand a scanned family ("defaults/*") to the members the binary
        # actually registered, so both sides are comparable.
        for fam in [k for k in list(bodies) if k.endswith("/*")]:
            prefix = fam[:-1]
            members = [n for n in listed if n.startswith(prefix)]
            for n in members:
                bodies.setdefault(n, bodies[fam])
            del bodies[fam]
        missing = sorted(set(bodies) - listed)
        extra = sorted(listed - set(bodies))
        if missing or extra:
            note = ("> Source scan and `--gui-test-list` disagree — "
                    f"{len(missing)} scanned-not-listed, {len(extra)} "
                    "listed-not-scanned. The scan's registration regex has "
                    "probably drifted.\n\n")
        bodies = {k: v for k, v in bodies.items() if k in listed}

    rows = []
    for tool, marks, supports in TOOLS:
        hits = {k: [] for k in TAB_KINDS}
        for name, body in sorted(bodies.items()):
            if not any(m in body for m in marks):
                continue
            for k in classify(body):
                hits[k].append(name)
        rows.append((tool, supports, hits))

    kinds = list(TAB_KINDS)
    lines = ["# Interactive tools x tab kinds — coverage matrix", "",
             "> Generated by `tools/coverage_matrix.py`.", "",
             note.rstrip(), "",
             __doc__.split("HOW, AND WHAT TO DISTRUST")[0]
             .split("WHY\n---\n")[1].strip(), "",
             "## The matrix", "",
             "A cell shows how many tests exercise that tool on that tab "
             "kind. **`GAP`** means the tool is meant to work there and "
             "nothing tests it. `-` means it is not meant to.", "",
             "| Tool | " + " | ".join(k for k in kinds) + " |",
             "|---|" + "---|" * len(kinds)]
    gaps, thin = [], []
    for tool, supports, hits in rows:
        # THIN is the finding that matters here, and a zero-only check misses
        # it entirely. Every historical bug of this shape had SOME assembly
        # coverage — the section arrow had an assembly test when it broke on
        # assembly tabs the second time. What it did not have was coverage
        # proportionate to the part side, so the one test it had happened to
        # miss the path that was dead.
        best = max((len(hits[k]) for k in supports), default=0)
        for k in kinds:
            n = len(hits[k])
            if k in supports and n and best >= 5 and n <= max(2, best // 5) \
                    and (tool, k) not in REVIEWED:
                thin.append((tool, k, n, best))
        cells = []
        for k in kinds:
            n = len(hits[k])
            if k not in supports:
                cells.append("—" if n == 0 else f"({n})")
            elif n == 0:
                cells.append("**GAP**")
                gaps.append((tool, k))
            elif (tool, k) in REVIEWED:
                cells.append(f"{n} (reviewed)")
            elif any(t[0] == tool and t[1] == k for t in thin):
                cells.append(f"{n} _(thin)_")
            else:
                cells.append(str(n))
        lines.append(f"| {tool} | " + " | ".join(cells) + " |")

    lines += ["", "A number in brackets is coverage on a tab kind the tool "
              "is not listed as supporting — usually a test that touches it "
              "incidentally, occasionally a sign the `supports` column is "
              "wrong. `(reviewed)` is a cell read by hand and found adequate "
              "despite a low count; the reasons are below.", ""]
    if REVIEWED:
        lines += ["### Reviewed cells", "",
                  "A count cannot tell TARGETED coverage from sparse "
                  "coverage. These were read and judged fine:", ""]
        for (tool, k), why in sorted(REVIEWED.items()):
            lines.append(f"- **{tool}** on **{k}** — {why}")
        lines.append("")

    lines += ["## Gaps and thin cells", ""]
    if gaps:
        lines.append("**Untested where it is meant to work:**")
        lines.append("")
        for tool, k in gaps:
            lines.append(f"- **{tool}** on a **{k}** tab.")
        lines.append("")
    else:
        lines.append("No supported tab kind is completely untested.")
        lines.append("")
    if thin:
        lines += ["**Thin** — covered, but far less than the same tool's "
                  "best-covered tab kind. This is the shape every historical "
                  "bug here actually had: some coverage, not enough of it.",
                  ""]
        for tool, k, n, best in thin:
            lines.append(f"- **{tool}** on **{k}**: {n} test(s) against "
                         f"{best} on its best-covered kind.")
        lines.append("")

    lines += ["## Per-tool detail", ""]
    for tool, supports, hits in rows:
        lines.append(f"### {tool}")
        lines.append("")
        lines.append(f"Supports: {', '.join(supports)}.")
        lines.append("")
        for k in kinds:
            if not hits[k]:
                continue
            lines.append(f"- **{k}**: " + ", ".join(
                f"`{n}`" for n in hits[k][:12])
                + (f" (+{len(hits[k]) - 12} more)" if len(hits[k]) > 12 else ""))
        lines.append("")

    text = "\n".join(lines).rstrip() + "\n"
    if out_path:
        with open(out_path, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    # The summary goes to stderr, so the markdown on stdout stays clean.
    print(f"{len(bodies)} tests, {len(gaps)} gap(s), {len(thin)} thin",
          file=sys.stderr)
    for tool, k in gaps:
        print(f"  GAP:  {tool} on a {k} tab", file=sys.stderr)
    for tool, k, n, best in thin:
        print(f"  THIN: {tool} on {k} — {n} vs {best}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
