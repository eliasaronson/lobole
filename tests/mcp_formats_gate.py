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

It also holds every text an agent reads to the method tables
(tests/agent_prose.py: the prompt, the instructions, the format texts, the
tools' descriptions, the help() reference, the describe rows), and lints
every schema describe publishes and, where the SDK is installed, every
tool's input schema for portability.

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
        # What `@mcp.tool(description=...)` announced, by function name:
        # the descriptions the prose scan reads (the schemas need the SDK).
        announced = {}

        def __init__(self, *a, **k):
            pass

        def tool(self, *a, description=None, **k):
            def register(f):
                _MCPServer.announced[f.__name__] = description
                return f
            return register

        def __getattr__(self, name):
            return lambda *a, **k: (lambda f: f)

    mcpserver.MCPServer = _MCPServer
    mcpserver.Image = object
    # ...and `from mcp.server.mcpserver.exceptions import ToolError, ...`,
    # which needs the stand-in to be a package.
    mcpserver.__path__ = []
    exceptions = types.ModuleType("mcp.server.mcpserver.exceptions")
    exceptions.ToolError = type("ToolError", (Exception,), {})
    exceptions.UnexpectedToolError = type("UnexpectedToolError",
                                          (Exception,), {})
    mcpserver.exceptions = exceptions
    server = types.ModuleType("mcp.server")
    server.mcpserver = mcpserver
    mcp = types.ModuleType("mcp")
    mcp.server = server
    sys.modules.update({"mcp": mcp, "mcp.server": server,
                        "mcp.server.mcpserver": mcpserver,
                        "mcp.server.mcpserver.exceptions": exceptions})
    import lobole.mcp
    return lobole.mcp


def announced_tools(m):
    """{name: (description, input schema or None)} of every tool the server
    announces: from the SDK's own list where it is installed, else from the
    stand-in's record (no schemas)."""
    announced = getattr(type(m.mcp), "announced", None)
    if announced is not None:
        return {n: (d, None) for n, d in announced.items()}
    import anyio  # the SDK's own dependency
    return {t.name: (t.description, t.input_schema)
            for t in anyio.run(m.mcp.list_tools)}


def schema_problems(schema, where):
    """A `type` that is an array, or an `enum` beside one: what some model
    vendors refuse a whole tool list over. Alternatives are written `anyOf`
    (docs/scripting/mcp.md#portable-schemas)."""
    out = []
    if isinstance(schema, dict):
        t = schema.get("type")
        if isinstance(t, list):
            out.append(f"{where}: \"type\": {t} (write anyOf)"
                       + (" beside an enum" if "enum" in schema else ""))
        for k, v in schema.items():
            out += schema_problems(v, f"{where}/{k}")
    elif isinstance(schema, list):
        for i, v in enumerate(schema):
            out += schema_problems(v, f"{where}[{i}]")
    return out


def source_files():
    """The app's and the package's source, whose quoted literals count as
    names that exist (tests/agent_prose.py)."""
    import subprocess
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    listed = subprocess.run(
        ["git", "-C", root, "ls-files", "src/*.cpp", "src/*.h",
         "python/lobole/*.py"], capture_output=True, text=True)
    if listed.returncode != 0 or not listed.stdout.strip():
        # Not a checkout (a source tarball): every file under the two trees.
        import glob
        return [f for pat in ("src/**/*.cpp", "src/**/*.h",
                              "python/lobole/*.py")
                for f in glob.glob(os.path.join(root, pat), recursive=True)]
    return [os.path.join(root, f) for f in listed.stdout.split()]


def prose_problems(m, desc, tools):
    """Every agent-facing text checked, and a planted name caught in each
    kind of text: a gate that cannot see a plant is blind."""
    import agent_prose
    problems = []
    names = set(tools) | set(m._EXPOSED)
    literals = agent_prose.source_literals(source_files())
    texts = agent_prose.agent_texts(
        m, desc, {n: d for n, (d, _) in tools.items()})
    topics = set(m.DOC_TOPICS) | {"examples"} | {
        e["name"] for e in m.EXAMPLES}
    for name, unknown in agent_prose.scan(texts, desc, names, literals,
                                          topics).items():
        problems.append(f"{name} names what nothing registers: "
                        f"{', '.join(unknown)}")
    # Every docs/ path a text cites is one help() serves, its anchor a
    # heading there (docs/scripting/mcp.md#served-documents).
    for name, text in texts.items():
        bad = agent_prose.unserved_paths(name, text, m)
        if bad:
            problems.append(f"{name} cites what help() does not serve: "
                            f"{', '.join(bad)}")
    # Planted, built from pieces so the [docs] convention test does not read
    # them as this file's own citations.
    missing = "docs/" + "nowhere.md"
    no_heading = "docs/inspect/review-checklist.md#" + "no-such-heading"
    if agent_prose.unserved_paths(
            "WORKFLOW", f"see {missing} and {no_heading}", m) != sorted(
                [no_heading, missing]):
        problems.append("the served-document check does not see a planted "
                        "path and anchor")
    clash = topics & ({r["name"] for r in desc["methods"]} | names)
    if clash:
        problems.append(f"a help() topic is also a method or a tool: {clash}")
    # One of each kind of text, with a planted call, a planted word and a
    # planted key of a real method: each must come back.
    plant = (" Call rpc('make_it_so') on the frobnicated_widget, then "
             "rpc('add_requirement', {name, colour_key}).")
    want = ["add_requirement.colour_key", "frobnicated_widget", "make_it_so"]
    kinds = {}
    for name in texts:
        kinds.setdefault(name.split(":", 1)[0], name)
    for name in kinds.values():
        planted = {name: texts[name] + plant}
        got = agent_prose.scan(planted, desc, names, literals,
                               topics).get(name, [])
        missing = [w for w in want if w not in got]
        if missing:
            problems.append(f"the prose scan is blind in {name}: a planted "
                            f"{', '.join(missing)} was not caught")
    return problems, len(texts)


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
            key = entry.split(":", 1)[0].split("^", 1)[0].rstrip("?")
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
    # Every text an agent reads names only what exists.
    tools = announced_tools(m)
    found, text_count = prose_problems(m, desc, tools)
    problems += found
    # The schemas stay portable: the app's own (test_rpc_contract.cpp lints
    # them too) and the tools' signatures as the SDK writes them.
    for row in desc["methods"]:
        for key in ("request_schema", "result_schema"):
            if key in row:
                problems += schema_problems(row[key], f"{row['name']}.{key}")
    schemas = {n: s for n, (_, s) in tools.items() if s is not None}
    for name, schema in sorted(schemas.items()):
        problems += schema_problems(schema, f"tool {name}")
    if not schema_problems({"properties": {"x": {"type": ["string",
                                                          "null"]}}}, "p"):
        problems.append("the schema lint does not see a planted type array")
    if problems:
        print("mcp formats gate: python/lobole/mcp.py has drifted from the "
              "app:")
        for p in problems:
            print("  " + p)
        return 1
    print(f"mcp formats gate: {len(desc['feature_types'])} feature types, "
          f"{len(desc['sketch_entity_types'])} entity types and "
          f"{len(app_slugs)} material slugs all documented; {text_count} "
          f"agent-facing texts name only what exists; "
          f"{len(schemas) if schemas else 'no (the mcp SDK is not installed)'}"
          f" tool schemas and every describe schema portable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
