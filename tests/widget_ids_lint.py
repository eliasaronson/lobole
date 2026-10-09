#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Two widgets in one window must not answer to one ImGui id.

ImGui derives a widget's id from the part of its label after "###", so
    ImGui::BeginCombo("###cam_post", ...)     // the post dialect
    ImGui::Button("Save G-code###cam_post")   // writes the file
are the SAME widget as far as ImGui is concerned. It shipped that way: the
combo simply stopped existing whenever the button was drawn, and the user got
"Programmer error: 2 visible items with conflicting ID!" over a real job.

Nothing caught it and nothing could have. ImGui notices only while the mouse
is over one of the pair — its test is "how many items claimed the HOVERED id
last frame" — and the test engine gathers items BY id, so a collision shows up
in a listing as one entry rather than two. A scan of the panel's widgets came
back clean while the bug was live.

So the check is on the source, where both call sites are plainly visible. It
runs in a second and needs no display.

What counts as one site: a widget whose label is chosen by a conditional
    ImGui::Button(ghosted ? "hide###ghost" : "show###ghost")
is ONE call and ONE widget — two spellings of a label, not two widgets. Those
are grouped by the call expression they sit in, so they do not report.
"""
import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Ids that legitimately appear at more than one call site. Each needs a reason
# and, ideally, an eventual fix — an entry here is a promise that the two can
# never be submitted in the same frame.
ALLOWED = {
    # (empty — the one entry, CamTool.cpp's cam_tool_done, went back to a
    # single site when the window became per-verb on 2026-09-02)
}

# Strings, comments and the call structure are all we need.
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/', re.S)


def call_site(text, at):
    """The offset of the '(' of the call expression containing `at`.

    Walking back over balanced parens finds the enclosing call, which is what
    makes a ternary of two labels count once: both literals sit inside the
    same ImGui::Button( ... ).
    """
    depth = 0
    i = at
    while i > 0:
        i -= 1
        c = text[i]
        if c == ')':
            depth += 1
        elif c == '(':
            if depth == 0:
                return i
            depth -= 1
        elif c == ';' or c == '{' or c == '}':
            break
    return at  # not inside a call: its own site


def main():
    # Only the code that DRAWS widgets. GuiTests.cpp is full of "###tb_box"
    # too, but those are refs it clicks, not widgets it creates, and a test
    # addressing one button from thirty places is the point of an id.
    sources = []
    for dirpath, _, names in os.walk(os.path.join(ROOT, "src", "ui")):
        sources += [os.path.join(dirpath, n) for n in sorted(names)
                    if n.endswith(".cpp")]
    sources.append(os.path.join(ROOT, "src", "app", "main.cpp"))

    bad = []
    for path in sources:
        rel = os.path.relpath(path, ROOT)
        raw = open(path, encoding="utf-8").read()
        # Blank out comments so a mention in prose is not a call site.
        text = COMMENT.sub(lambda m: " " * len(m.group(0)), raw)
        sites = {}
        for m in STRING.finditer(text):
            for ident in re.findall(r'###([A-Za-z0-9_]+)', m.group(0)):
                sites.setdefault(ident, set()).add(call_site(text, m.start()))
        for ident, offsets in sorted(sites.items()):
            if len(offsets) < 2 or (rel, ident) in ALLOWED:
                continue
            lines = sorted(text.count("\n", 0, o) + 1 for o in offsets)
            bad.append(f"{rel}: ###{ident} is used by {len(offsets)} "
                       f"widgets, at lines {lines}")

    if bad:
        print("Widgets sharing an ImGui id (one window, one id, two widgets):")
        for b in bad:
            print("  " + b)
        print("\nGive each widget its own ###id, or — if the two can never be "
              "submitted in the same frame — add it to ALLOWED with the "
              "reason.")
        return 1
    print("widget ids: no two widgets share an id")
    return 0


if __name__ == "__main__":
    sys.exit(main())
