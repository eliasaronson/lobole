#!/usr/bin/env python3
"""Integration test: model a dimensioned bracket through the MCP server.

Exercises the MCP layer itself (stdio transport, tool schemas, image
content), not just the RPC underneath: designer --remote <- designer_mcp.py
<- the SDK's stdio client.

Usage: mcp_smoke.py <path-to-designer-binary>
Exits 77 (ctest SKIP_RETURN_CODE) if the mcp SDK is unavailable.
"""
import asyncio
import base64
import json
import math
import os
import socket
import subprocess
import tempfile
import sys

try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print("mcp_smoke: python mcp SDK not installed, skipping")
    sys.exit(77)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from smoke_app import launch_app  # noqa: E402

TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "tools")




def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def result(r):
    """Unwrap a CallToolResult into plain data."""
    if r.isError:
        raise RuntimeError(r.content[0].text if r.content else "tool error")
    sc = r.structuredContent
    if isinstance(sc, dict):
        return sc.get("result", sc)
    if r.content and r.content[0].type == "text":
        try:
            return json.loads(r.content[0].text)
        except (ValueError, TypeError):
            return r.content[0].text
    return r.content


async def run(port):
    server = StdioServerParameters(
        command=sys.executable,
        args=[os.path.join(TOOLS_DIR, "designer_mcp.py"), "--port",
              str(port)])
    async with stdio_client(server) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            tools = (await session.list_tools()).tools
            names = {t.name for t in tools}
            # The surface is deliberately SMALL — a couple of dozen
            # conversational tools plus rpc/help, not one tool per RPC (the
            # 118-tool version cost ~8.5k tokens of descriptions before a
            # client asked anything). Both bounds matter: too many means the
            # diet regressed, too few means something failed to register.
            expect(15 <= len(tools) <= 40, f"tool count ({len(tools)})")
            expect({"rpc", "help", "describe", "get_state", "add_feature"}
                   <= names, f"core tools present ({sorted(names)})")

            # Everything else is still reachable, and still documented.
            index = result(await session.call_tool("help", {}))
            # help()'s list of exposed tools is kept by hand; every tool the
            # server announces must be on it, or help('<tool>') answers "no
            # such method" about a tool the client can see.
            expect(names <= set(index["exposed_as_tools"]),
                   "help() lists every announced tool "
                   f"({sorted(names - set(index['exposed_as_tools']))})")
            ex = result(await session.call_tool("help",
                                                {"method": "execute_script"}))
            expect("tool" in ex["doc"], "help('execute_script') answers")
            expect("add_mate" in index["methods"], "help indexes add_mate")
            doc = result(await session.call_tool("help", {"method":
                                                          "add_mate"}))
            expect(len(doc["doc"]) > 200 and "type" in doc["arguments"],
                   "help('add_mate') carries the prose and the signature")
            try:
                result(await session.call_tool("help", {"method": "add_mat"}))
                raise RuntimeError("a typo should not resolve")
            except RuntimeError as e:
                expect("add_mate" in str(e), f"typo suggests a match ({e})")
            prompts = (await session.list_prompts()).prompts
            expect(any(p.name == "modeling-workflow" for p in prompts),
                   "workflow prompt registered")

            async def call(tool, /, **args):
                return result(await session.call_tool(tool, args))

            # Drift gate: every feature type the app accepts must be
            # documented in FEATURE_FORMATS (the standing rule from
            # docs/scripting/mcp.md#format-documentation, enforced). `describe` is the app-side
            # truth; the module constant is the MCP-side prose.
            import designer_mcp
            desc = await call("describe")
            # Narrowed describe: its RPC parameter is named `method`, which
            # collided with _call's own first argument the first time an
            # agent used it (positional-only `/` is the fix). Pin it.
            one = await call("describe", method="add_feature")
            expect(len(one["methods"]) == 1
                   and one["methods"][0]["name"] == "add_feature",
                   f"describe narrows to one method ({one['methods'][:2]})")
            for ft in desc["feature_types"]:
                expect(ft["type"] in designer_mcp.FEATURE_FORMATS,
                       f"feature type {ft['type']} missing from "
                       "FEATURE_FORMATS")
            for ent in desc["sketch_entity_types"]:
                expect(f'"{ent["type"]}"' in designer_mcp.FEATURE_FORMATS,
                       f"entity type {ent['type']} missing from "
                       "FEATURE_FORMATS")
            # Same drift gate for the material-slug prose: the hand-written
            # MATERIAL_SLUGS literal must list exactly the app's slugs.
            prose = {s.strip() for s in
                     designer_mcp.MATERIAL_SLUGS.split(",")}
            expect(prose == set(desc["material_slugs"]),
                   "MATERIAL_SLUGS drifted from describe.material_slugs: "
                   f"prose-only {prose - set(desc['material_slugs'])}, "
                   f"app-only {set(desc['material_slugs']) - prose}")

            # The bracket, parameters-first (the workflow the prompt teaches)
            await call("new_project")
            v = await call("set_variable", name="width", expression="40",
                           comment="plate width")
            expect(abs(v["value"] - 40.0) < 1e-9, "set_variable evaluates")
            await call("set_variable", name="height", expression="width / 2")
            await call("set_variable", name="thickness", expression="5")
            await call("set_variable", name="corner_r", expression="4")

            # Rectangle from four free lines (unconstrained, deliberately off
            # their final positions — the constraints must do the work)
            sk = await call("add_feature", feature={
                "type": "Sketch", "name": "Base",
                "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                          "uAxis": [1, 0, 0]},
                "entities": [
                    {"type": "line", "a": [1, 1], "b": [30, 2]},      # bottom
                    {"type": "line", "a": [30, 2], "b": [31, 15]},    # right
                    {"type": "line", "a": [31, 15], "b": [2, 16]},    # top
                    {"type": "line", "a": [2, 16], "b": [1, 1]},      # left
                ]})
            sk_id, (bot, right, top, left) = sk["id"], sk["entity_ids"]

            # Through rpc(), the door every non-exposed RPC uses now — and
            # the wrapper behind it still shapes the arguments.
            async def constrain(kind, refs, **kw):
                await call("rpc", method="add_constraint",
                           params=dict(sketch_id=sk_id, kind=kind, refs=refs,
                                       **kw))

            await constrain("coincident", [[bot, 1], [right, 0]])
            await constrain("coincident", [[right, 1], [top, 0]])
            await constrain("coincident", [[top, 1], [left, 0]])
            await constrain("coincident", [[left, 1], [bot, 0]])
            await constrain("horizontal", [[bot, 0]])
            await constrain("horizontal", [[top, 0]])
            await constrain("vertical", [[right, 0]])
            await constrain("vertical", [[left, 0]])
            await constrain("coincident", [[bot, 0], [0xFFFFFF00, 0]])
            await constrain("distance", [[bot, 0], [bot, 1]], expr="width")
            await constrain("distance", [[right, 0], [right, 1]],
                            expr="height")

            # The solve happened app-side: verify the rectangle really is
            # width x height at the origin before building on it.
            state = await call("get_state")
            ents = state["features"][0]["entities"]
            xs = [p for e in ents for p in (e["a"][0], e["b"][0])]
            ys = [p for e in ents for p in (e["a"][1], e["b"][1])]
            expect(abs(max(xs) - min(xs) - 40.0) < 1e-4, "solved width 40")
            expect(abs(max(ys) - min(ys) - 20.0) < 1e-4, "solved height 20")

            pad = await call("add_feature", feature={
                "type": "Extrude", "name": "Pad", "sketch_id": sk_id,
                "distance": 1.0, "distance_expr": "thickness",
                "mode": "new_body"})
            state = await call("get_state")
            expect(abs(state["bodies"][0]["volume"] - 40 * 20 * 5) < 1e-3,
                   "pad volume 4000")

            # Round one corner; the volume drop pins the arc really extruded
            await call("rpc", method="fillet",
                       params=dict(sketch_id=sk_id, line1=top, line2=left,
                                   radius=4.0))
            state = await call("get_state")
            expected = 40 * 20 * 5 - (16 - math.pi * 4) * 5
            expect(abs(state["bodies"][0]["volume"] - expected) < 1e-2,
                   f"filleted volume {expected:.2f}, "
                   f"got {state['bodies'][0]['volume']:.2f}")

            # Variables drive the model end-to-end
            await call("set_variable", name="thickness", expression="8")
            state = await call("get_state")
            expect(abs(state["bodies"][0]["volume"] - expected * 8 / 5) < 1e-2,
                   "thickness edit rebuilds the pad")
            await call("undo")

            # The agent's eyes: screenshot returns inline image content
            shot = await session.call_tool("screenshot", {"view": "iso"})
            expect(not shot.isError, "screenshot tool ok")
            imgs = [c for c in shot.content if c.type == "image"]
            expect(len(imgs) == 1, "screenshot returns one image block")
            png = base64.b64decode(imgs[0].data)
            expect(png[:8] == b"\x89PNG\r\n\x1a\n" and len(png) > 1000,
                   "screenshot is a real PNG")

            # Errors surface as tool errors, not hangs or crashes
            bad = await session.call_tool("view", {"name": "sideways"})
            expect(bad.isError, "bad view name reports a tool error")
            # Refused by view's contract, naming the views it takes.
            expect("'name' must be one of: front" in bad.content[0].text,
                   f"error is passed on: {bad.content[0].text}")

            # modify_feature closes the loop on the pad
            state = await call("get_state")
            pad_json = state["features"][1]
            pad_json["reversed"] = True
            await call("modify_feature", index=1, feature=pad_json)
            expect(abs((await call("get_state"))["bodies"][0]["volume"]
                       - expected) < 1e-2, "reversed pad keeps its volume")

            # execute_script (R11): the loop-and-computed-args path for
            # MCP-only agents — `d` arrives pre-bound to the attached
            # instance, prints come back, and the work LANDS in the model.
            # A Box stands `on` something and is stored as a sketch plus an
            # extrude (docs/modeling/README.md#primitives): two features each.
            before = len((await call("get_state"))["features"])
            run_out = await call("execute_script", script=(
                "for k in range(3):\n"
                "    r = d.request('add_feature', {'feature': {\n"
                "        'type': 'Box', 'name': f'grid {k}', 'on': 'Top',\n"
                "        'centered': False, 'offset': [50 + 12 * k, 0],\n"
                "        'width': 8, 'depth': 8, 'height': 4 + k}})\n"
                "    assert r['regen']['ok']\n"
                "print('made', 3)\n"))
            expect(run_out["exit"] == 0, f"script exit 0 ({run_out})")
            expect("made 3" in run_out["stdout"], f"stdout ({run_out})")
            after = (await call("get_state"))["features"]
            expect(len(after) == before + 6,
                   f"the scripted boxes landed ({before}->{len(after)})")
            grid = [b for b in (await call("get_state"))["bodies"]
                    if b["name"].startswith("grid ")]
            expect(sorted(round(b["volume"], 6) for b in grid)
                   == [256.0, 320.0, 384.0],
                   f"8 x 8 x (4 + k) each: {grid}")

    print("mcp_smoke: all assertions passed")


def main():
    binary = sys.argv[1]
    # A scratch state dir: the app announces itself there (instance
    # discovery), and a test must never write into the developer's real
    # ~/.local/state/designer or see the instances they have open.
    state = tempfile.mkdtemp(prefix="designer-mcp-smoke-")
    # CONFIG too: set_settings persists immediately, so a run that touched
    # preferences would edit the developer's real settings.json.
    env = dict(os.environ, XDG_STATE_HOME=state,
               XDG_CONFIG_HOME=os.path.join(state, "config"))
    app = launch_app(binary, extra_args=["--hidden"], env=env,
                     headless=False)
    try:
        # The MCP server fails fast when the app is unreachable (that is the
        # behavior an agent should see), so wait for startup here instead.
        sys.path.insert(0, TOOLS_DIR)
        from designer_remote import Designer
        Designer.wait_for(app.port).close()

        # Discovery: the running app announced itself, so a client can find
        # it without being told the port (the failure this replaced was
        # "Designer is not reachable" while the app was running happily).
        os.environ["XDG_STATE_HOME"] = state
        found = Designer.instances()
        expect([i for i in found if i["port"] == app.port],
               f"the running instance announced itself ({found})")

        asyncio.run(run(app.port))
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
