#!/usr/bin/env python3
"""Gate: the MCP server's hand-written format prose matches the app.

`describe` is the app-side truth for feature types, sketch entity types and
material slugs; `tools/designer_mcp.py` carries prose copies of all three
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "tools"))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch_app  # noqa: E402


def load_mcp_constants():
    """Import designer_mcp for its constants, with or without the mcp SDK."""
    try:
        import designer_mcp  # noqa: F401
        return designer_mcp
    except ImportError:
        pass
    # No SDK: satisfy `from mcp.server.fastmcp import FastMCP, Image` with
    # inert stand-ins. Only module-level constants are read below.
    fastmcp = types.ModuleType("mcp.server.fastmcp")

    class _FastMCP:
        def __init__(self, *a, **k):
            pass

        def __getattr__(self, name):
            return lambda *a, **k: (lambda f: f)

    fastmcp.FastMCP = _FastMCP
    fastmcp.Image = object
    server = types.ModuleType("mcp.server")
    server.fastmcp = fastmcp
    mcp = types.ModuleType("mcp")
    mcp.server = server
    sys.modules.update({"mcp": mcp, "mcp.server": server,
                        "mcp.server.fastmcp": fastmcp})
    import designer_mcp
    return designer_mcp


def main():
    binary = sys.argv[1]
    state = tempfile.mkdtemp(prefix="designer-mcp-formats-")
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, env=env)  # headless
    try:
        from designer_remote import Designer
        d = Designer.wait_for(app.port)
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
    prose = {s.strip() for s in m.MATERIAL_SLUGS.split(",")}
    app_slugs = set(desc["material_slugs"])
    if prose != app_slugs:
        problems.append("MATERIAL_SLUGS drifted from describe.material_slugs: "
                        f"prose-only {sorted(prose - app_slugs)}, "
                        f"app-only {sorted(app_slugs - prose)}")
    if problems:
        print("mcp formats gate: tools/designer_mcp.py has drifted from the "
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
