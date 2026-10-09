#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""The part gate has to agree with the handlers it gates.

`kNoPartDoc` (RemoteServer.cpp) decides which RPCs refuse on an assembly or
drawing tab. It is a LIST kept apart from the handlers, so widening a handler
to cover the whole project never widened its gate — a 2026-08-23 dogfooding
round found six RPCs refusing for no reason, four of which carried comments
saying they had ALREADY been fixed to be project-wide. Nobody had looked at
the list since.

So: every gated method must either touch the part document in its own handler
body, or say here why it is gated anyway. A new project-level RPC that gets
silently gated fails this test until someone writes the reason down — which is
the point, because writing it down is where you notice there isn't one.

Usage: rpc_gate_smoke.py <path-to-designer-binary>
"""
import os
import re
import socket
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402
from smoke_app import launch_app  # noqa: E402

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")

# A gated method whose handler shows no part dependency, and the reason it is
# gated anyway. Every entry is a claim someone has to stand behind; an empty
# reason is not accepted.
GATED_ON_PURPOSE = {
    # The study verbs used to sit here ("studies belong to a part document").
    # A study now lives on a part OR an assembly
    # (docs/simulation/README.md#assembly-studies), and the handler resolves
    # the tab itself.
    # A pending (preview) feature only ever exists on a part tab.
    "accept_pending": "a pending feature only exists on a part tab",
    "cancel_pending": "a pending feature only exists on a part tab",
    # Its source is the active part's body unless `part` names another; the
    # handler reads the part document through its helper.
    "split_for_printing": "it cuts the active part's body by default",
    # The literals lint reads the active part unless `part` names another,
    # and extract_variables edits the active part's features; both reach the
    # document through their helpers (RemoteModeling.cpp).
    "check_literals": "it lints the active part by default",
    "extract_variables": "it edits the active part's features",
    # A template is saved from the active part's features
    # (docs/modeling/templates.md#saving-one); the handler reaches the part
    # through state.document in its helper (ui::SaveTemplate).
    "save_template": "it saves the active part's features",
    # orient_to_face sat here ("assembly-tab behaviour unverified") until
    # --offscreen provided a render surface to verify on; it is ungated now
    # and tests/offscreen_smoke.py pins its assembly-tab behaviour.
}

# What a handler body looks like when it depends on the active part: `doc`
# itself, or one of the locals the dispatch function derives from it before
# the if-chain (`bodies`, `sk`, a Feature*).
PART_DEPENDENCY = re.compile(
    r"\b(doc|bodies|sk|sketch|RequirePartDoc|feature|Feature)\b")


def handler_bodies():
    """Every `if (m == "name")` block in the remote modules, by method name."""
    out = {}
    for name in sorted(os.listdir(os.path.join(SRC, "remote"))):
        if not name.startswith("Remote") or not name.endswith(".cpp"):
            continue
        src = open(os.path.join(SRC, "remote", name)).read()
        for mo in re.finditer(
                r'if \((?:m == "[a-z_0-9]+"(?:\s*\|\|\s*)?)+\) \{', src):
            methods = re.findall(r'"([a-z_0-9]+)"', mo.group(0))
            start = mo.end() - 1
            depth = 0
            end = start
            for j in range(start, len(src)):
                if src[j] == "{":
                    depth += 1
                elif src[j] == "}":
                    depth -= 1
                    if depth == 0:
                        end = j
                        break
            # Every block that names the method, not the last one: a shared
            # handler with a small branch of its own for one method
            # (export_stl's tolerance) is still that method's handler.
            for m in methods:
                out[m] = out.get(m, "") + src[start:end]
    return out




def main():
    binary = sys.argv[1]
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_gate_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = launch_app(binary, env=env)
    problems = []
    try:
        d = Designer.wait_for(app.port)
        gated = sorted(m["name"] for m in d.call("describe")["methods"]
                       if m.get("requires_part_doc"))
        bodies = handler_bodies()

        for m in gated:
            if m not in bodies:
                problems.append(
                    f"{m}: gated, but no `if (m == \"{m}\")` handler was found "
                    f"— the extractor above and the dispatch have drifted")
                continue
            if PART_DEPENDENCY.search(bodies[m]):
                continue  # it really does work on the active part
            reason = GATED_ON_PURPOSE.get(m)
            if not reason:
                problems.append(
                    f"{m}: refuses on an assembly/drawing tab, but its handler "
                    f"never touches the part document. Either remove it from "
                    f"kNoPartDoc's complement (add it TO kNoPartDoc in "
                    f"RemoteServer.cpp) or add a reason to GATED_ON_PURPOSE "
                    f"here saying why a part tab is required.")

        # The other direction: a reason for a method that is no longer gated
        # is a leftover, and leftovers are how the list stopped being read.
        for m, reason in GATED_ON_PURPOSE.items():
            if m not in gated:
                problems.append(
                    f"{m}: listed in GATED_ON_PURPOSE ({reason!r}) but it is "
                    f"not gated any more — drop the entry")

        if problems:
            print("rpc_gate_smoke: FAILED")
            for p in problems:
                print("  - " + p)
            return 1
        print(f"rpc_gate_smoke: {len(gated)} gated methods, "
              f"{len(GATED_ON_PURPOSE)} of them by written reason — all agree")
        return 0
    finally:
        app.terminate()
        app.wait(timeout=10)


if __name__ == "__main__":
    sys.exit(main())
