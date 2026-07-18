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
import sys

try:
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError:
    print("mcp_smoke: python mcp SDK not installed, skipping")
    sys.exit(77)

TOOLS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                         "tools")


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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
            expect(len(tools) >= 25, f"tool count ({len(tools)})")
            prompts = (await session.list_prompts()).prompts
            expect(any(p.name == "modeling-workflow" for p in prompts),
                   "workflow prompt registered")

            async def call(tool, /, **args):
                return result(await session.call_tool(tool, args))

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

            async def constrain(kind, refs, **kw):
                await call("add_constraint", sketch_id=sk_id, kind=kind,
                           refs=refs, **kw)

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
            await call("fillet", sketch_id=sk_id, line1=top, line2=left,
                       radius=4.0)
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
            expect("unknown view" in bad.content[0].text, "error is passed on")

            # modify_feature closes the loop on the pad
            state = await call("get_state")
            pad_json = state["features"][1]
            pad_json["reversed"] = True
            await call("modify_feature", index=1, feature=pad_json)
            expect(abs((await call("get_state"))["bodies"][0]["volume"]
                       - expected) < 1e-2, "reversed pad keeps its volume")

    print("mcp_smoke: all assertions passed")


def main():
    binary = sys.argv[1]
    port = free_port()
    app = subprocess.Popen([binary, "--remote", str(port), "--hidden"],
                           stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        # The MCP server fails fast when the app is unreachable (that is the
        # behavior an agent should see), so wait for startup here instead.
        sys.path.insert(0, TOOLS_DIR)
        from designer_remote import Designer
        Designer.wait_for(port).close()
        asyncio.run(run(port))
    finally:
        app.terminate()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
    return 0


if __name__ == "__main__":
    sys.exit(main())
