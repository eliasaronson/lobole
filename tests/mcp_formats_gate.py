#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""Gate: the MCP server's hand-written format prose matches the app.

`describe` is the app-side truth for feature types, sketch entity types and
material slugs; `python/lobole/mcp.py` carries prose copies of all three
(FEATURE_FORMATS, MATERIAL_SLUGS) because an agent with only the MCP surface
reads the prose, not the RPC. mcp_smoke has always cross-checked them — but
mcp_smoke opens a (hidden) window, so it is `gui`-labelled and off the
default path, and the Fastener feature shipped on 2026-09-01 without a row
and stayed red for two days without anyone seeing it.

This is the same check against a HEADLESS app, so it runs in `ctest -LE gui`
and in CI. Needs no display, no GPU and no mcp SDK: it imports only the
constants, with the SDK import stubbed.

Usage: mcp_formats_gate.py <path-to-designer-binary>
"""
import os
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch_app  # noqa: E402


def load_mcp_constants():
    """Import lobole.mcp for its constants, with or without the mcp SDK."""
    try:
        import lobole.mcp
        return lobole.mcp
    except ImportError:
        pass
    # No SDK: satisfy `from mcp.server.mcpserver import MCPServer, Image`
    # with inert stand-ins. Only module-level constants are read below.
    mcpserver = types.ModuleType("mcp.server.mcpserver")

    class _MCPServer:
        def __init__(self, *a, **k):
            pass

        def __getattr__(self, name):
            return lambda *a, **k: (lambda f: f)

    mcpserver.MCPServer = _MCPServer
    mcpserver.Image = object
    server = types.ModuleType("mcp.server")
    server.mcpserver = mcpserver
    mcp = types.ModuleType("mcp")
    mcp.server = server
    sys.modules.update({"mcp": mcp, "mcp.server": server,
                        "mcp.server.mcpserver": mcpserver})
    import lobole.mcp
    return lobole.mcp


def main():
    binary = sys.argv[1]
    state = tempfile.mkdtemp(prefix="lobole-mcp-formats-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, env=env)  # headless
    try:
        from lobole.client import Client
        d = Client.wait_for(app.port)
        try:
            desc = d.call("describe")
        finally:
            d.close()
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except Exception:
            app.kill()

    m = load_mcp_constants()
    problems = []
    for ft in desc["feature_types"]:
        if f'"type": "{ft["type"]}"' not in m.FEATURE_FORMATS and \
           f'"{ft["type"]}"' not in m.FEATURE_FORMATS:
            problems.append(f"feature type {ft['type']} has no row in "
                            "FEATURE_FORMATS")
    for ent in desc["sketch_entity_types"]:
        if f'"{ent["type"]}"' not in m.FEATURE_FORMATS:
            problems.append(f"sketch entity type {ent['type']} is not in "
                            "FEATURE_FORMATS")
    # Every field a type's contract takes is named in the prose an agent
    # reads (docs/scripting/rpc.md#typed-feature-specs): an `x_expr` twin by
    # its `x`, which the prose says once; the keys a spec only carries back
    # from get_state (a sketch's reference images, a template instance's
    # copy of its definition) are not authored, so not documented here.
    carried_back = {"images", "derived_ops", "next_image_id", "definition",
                    "revision", "source_name", "color", "snapshot"}
    common = {"type", "name", "id", "material", "appearance",
              "face_appearance", "suppressed"}
    for ft in desc["feature_types"]:
        for entry in ft["contract"].split(", "):
            key = entry.split(":", 1)[0].rstrip("?")
            base = key[:-len("_expr")] if key.endswith("_expr") else key
            if key in common or key in carried_back:
                continue
            # `angle_expr` is the twin of `angle_deg`
            if f'"{base}"' not in m.FEATURE_FORMATS and \
                    f'"{base}_deg"' not in m.FEATURE_FORMATS:
                problems.append(f"{ft['type']}.{key} is in its contract and "
                                "not in FEATURE_FORMATS")
    prose = {s.strip() for s in m.MATERIAL_SLUGS.split(",")}
    app_slugs = set(desc["material_slugs"])
    if prose != app_slugs:
        problems.append("MATERIAL_SLUGS drifted from describe.material_slugs: "
                        f"prose-only {sorted(prose - app_slugs)}, "
                        f"app-only {sorted(app_slugs - prose)}")
    # The tools exposed in front of an RPC take exactly its keys: a wrapper
    # that kept a hand-copied subset of them refused what the app accepts
    # (delete_feature had no feature_id) and one that added its own name
    # sent the app a key it then refused. execute_script, help, instances
    # and rpc are this server's own.
    import inspect
    rows = {r["name"]: r for r in desc["methods"]}
    for name in sorted(m._EXPOSED):
        if name not in rows:
            continue
        takes = {q.name for q in
                 inspect.signature(getattr(m, name)).parameters.values()}
        row = rows[name]
        keys = set() if row.get("params_free_form") else set(row["params"])
        # `path` on screenshot and render_still is the tool's own: it picks
        # a temporary file when none is given, and hands back the image.
        if name in ("screenshot", "render_still"):
            keys.add("path")
        # An older spelling the app still accepts, which a tool has no
        # reason to offer beside the current one.
        keys -= {"delete_feature": {"id"}}.get(name, set())
        if takes != keys:
            problems.append(
                f"tool {name}: takes {sorted(takes - keys)} the app does "
                f"not, and lacks {sorted(keys - takes)} the app takes")
    for name in m._REFERENCE:
        if name != "ui_action" and name not in rows:
            problems.append(f"help prose for {name}, which the app does not "
                            "serve")
    if problems:
        print("mcp formats gate: python/lobole/mcp.py has drifted from the "
              "app:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"mcp formats gate: {len(desc['feature_types'])} feature types, "
          f"{len(desc['sketch_entity_types'])} entity types and "
          f"{len(app_slugs)} material slugs all documented")
    return 0


if __name__ == "__main__":
    sys.exit(main())
