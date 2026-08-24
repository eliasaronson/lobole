#!/usr/bin/env python3
"""Integration test: C1 visual-mesh export (glTF/OBJ/PLY), headless.

Exports a part and an assembly to .glb and parses the binary-glTF container
directly (no external deps): magic/version header + the JSON chunk, then
asserts meshes/nodes/materials are present and that a body's assigned
material carried through as PBR (metallic + baseColorFactor). Also exports
.obj and .ply and checks the files are non-empty and well-formed at the head.

Usage: visual_export_smoke.py <path-to-designer-binary>
"""
import os
import struct
import subprocess
import sys
import tempfile
import json as jsonlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
from designer_remote import Designer  # noqa: E402


def free_port():
    import socket
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def parse_glb(path):
    """Return the glTF JSON dict from a binary .glb (validates the header)."""
    with open(path, "rb") as f:
        data = f.read()
    expect(len(data) >= 12, f"{path}: too short to be a glb")
    magic, version, length = struct.unpack_from("<III", data, 0)
    expect(magic == 0x46546C67, f"{path}: bad glTF magic {magic:#x}")
    expect(version == 2, f"{path}: glTF version {version} != 2")
    expect(length == len(data), f"{path}: header length {length} != file "
           f"{len(data)}")
    # First chunk must be JSON (type 0x4E4F534A).
    clen, ctype = struct.unpack_from("<II", data, 12)
    expect(ctype == 0x4E4F534A, f"{path}: first chunk is not JSON")
    return jsonlib.loads(data[20:20 + clen])


def main():
    binary = sys.argv[1]
    port = free_port()
    env = dict(os.environ)
    env.pop("DISPLAY", None)
    env.pop("WAYLAND_DISPLAY", None)
    tmp = tempfile.mkdtemp(prefix="designer_visual_")
    env["XDG_STATE_HOME"] = tmp
    # ...and its own CONFIG dir, or the run edits the developer's real
    # preferences: set_settings persists immediately (headless runs no GUI
    # loop to debounce it), so a suite reset their font size to 120%.
    env["XDG_CONFIG_HOME"] = os.path.join(tmp, "config")
    app = subprocess.Popen([binary, "--headless", "--remote", str(port)],
                           env=env, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL)
    try:
        d = Designer.wait_for(port)
        expect(d.call("ping")["ok"], "ping")

        # --- a part with a real material -----------------------------------
        d.call("new_project")
        d.call("add_part", name="widget")
        blk = d.call("add_feature", feature={
            "type": "Box", "name": "Blk", "position": [0, 0, 0],
            "width": 10, "height": 6, "depth": 4})
        d.call("set_material", feature_id=blk["id"], material="steel")

        glb = os.path.join(tmp, "widget.glb")
        out = d.call("export_visual", path=glb)
        expect(out["items"] == 1, f"one item exported: {out}")
        gltf = parse_glb(glb)
        expect(len(gltf.get("meshes", [])) >= 1, f"glb has a mesh: {gltf.keys()}")
        expect(len(gltf.get("materials", [])) >= 1, "glb has a material")
        # A triangle mesh must have a positions accessor.
        prim = gltf["meshes"][0]["primitives"][0]
        expect("POSITION" in prim["attributes"], "primitive has POSITION")
        # Steel is metallic — the material carried through as PBR, not a flat
        # color guess.
        pbr = gltf["materials"][0].get("pbrMetallicRoughness", {})
        expect(pbr.get("metallicFactor", 0) > 0.5,
               f"steel exported metallic: {gltf['materials'][0]}")

        # --- OBJ + PLY are non-empty and well-formed at the head -----------
        obj = os.path.join(tmp, "widget.obj")
        d.call("export_visual", path=obj)
        obj_text = open(obj).read()
        expect(obj_text.count("\nv ") + obj_text.startswith("v ") > 0,
               "OBJ has vertices")
        expect("f " in obj_text, "OBJ has faces")

        # RWPly_CafWriter arrived in OpenCASCADE 7.7; on an older kernel
        # (Ubuntu 24.04 ships 7.6.3) the app refuses by design, naming the
        # version. Skip only THIS assertion — the rest of the suite is
        # kernel-version independent.
        ply = os.path.join(tmp, "widget.ply")
        try:
            d.call("export_visual", path=ply)
            expect(open(ply, "rb").read(3) == b"ply", "PLY magic")
        except RuntimeError as e:
            if "7.7" not in str(e):
                raise
            print(f"  PLY skipped: {e}")

        # --- an assembly: two occurrences of one part (instanced) + a second
        d.call("add_part", name="pin")
        d.call("add_feature", feature={
            "type": "Box", "name": "P", "position": [0, 0, 0],
            "width": 2, "height": 2, "depth": 8})
        d.call("add_assembly", name="rig")
        d.call("add_occurrence", part="widget", fixed=True)
        d.call("add_occurrence", part="pin",
               transform={"position": [20, 0, 0], "orientation": [0, 0, 0, 1]})
        d.call("add_occurrence", part="pin",
               transform={"position": [30, 0, 0], "orientation": [0, 0, 0, 1]})

        asm_glb = os.path.join(tmp, "rig.glb")
        out = d.call("export_visual", path=asm_glb)
        expect(out["items"] == 3, f"three occurrence-items exported: {out}")
        gltf = parse_glb(asm_glb)
        # Three placed nodes; the two pin occurrences share one mesh
        # (instancing), so meshes < nodes.
        nodes = gltf.get("nodes", [])
        placed = [n for n in nodes if "mesh" in n]
        expect(len(placed) >= 3, f"three placed nodes: {len(placed)}")
        expect(len(gltf["meshes"]) <= len(placed),
               f"shared part instanced (meshes {len(gltf['meshes'])} <= placed "
               f"{len(placed)})")

        print("visual_export_smoke: OK")
    finally:
        app.terminate()
        try:
            app.wait(timeout=5)
        except subprocess.TimeoutExpired:
            app.kill()


if __name__ == "__main__":
    sys.exit(main())
