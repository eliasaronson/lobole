#!/usr/bin/env python3
"""The direct-emission arm's whole world: one tool that takes a document.

This exists to ask the question vcad's own benchmark answers against them, and
which ours has never asked: **is the RPC surface helping, or is it a tax?**

Their mecheval runs, same model, same tiers (claude-opus-4-7):

    tier   direct (emit the .vcad JSON)   through the MCP tool loop
    a1              88%                            82%
    a2             100%                            72%
    a3              56%                            56%
    a4              82%                            42%

Emitting the document format in one shot beat driving their API. Our `full`,
`rpc-only` and `script-first` arms all go through the API, so no arm we have
can see this. This one hands the agent the document format and nothing else.

Deliberately NOT part of tools/designer_mcp.py: this is a measuring
instrument, not a capability we ship. A product tool that swallows a whole
document would be a second way to author, and the benchmark would then be
measuring a surface that exists only because the benchmark wanted it.

Usage (the harness does this):
    document_mcp.py --port <port>
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "tools"))
from mcp.server.fastmcp import FastMCP  # noqa: E402
from designer_remote import Designer  # noqa: E402

mcp = FastMCP("designer-document")
_PORT = None


def _client():
    return Designer(port=_PORT)


@mcp.tool(description=(
    "Replace the open document with the one you supply, and report what the "
    "kernel made of it. `document` is the COMPLETE project as JSON, in "
    "Designer's own save format — the same thing save_project writes. Call "
    "document_format() first to see that format: it returns the current "
    "format version and a worked example of a part with a sketch, an extrude "
    "and a hole.\n\n"
    "The response is the regeneration result: every feature that failed, with "
    "its error, plus the resulting bodies and their volumes and bounding "
    "boxes. Nothing is accepted silently — a feature type or a field name the "
    "format does not have is REFUSED and named, so a response with no errors "
    "means the kernel built exactly what you described."))
def write_document(document: str) -> dict:
    try:
        doc = json.loads(document) if isinstance(document, str) else document
    except json.JSONDecodeError as e:
        return {"ok": False,
                "error": f"that is not valid JSON: {e}. Send the whole "
                         f"document as one JSON object."}
    d = _client()
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "part.json")
        with open(path, "w") as f:
            json.dump(doc, f)
        try:
            d.call("open_project", path=path)
        except Exception as e:
            return {"ok": False, "error": str(e)}
    out = {"ok": True}
    try:
        state = d.call("get_state")
        out["bodies"] = [{"name": b.get("name"), "volume": b.get("volume"),
                          "bbox_min": b.get("bbox_min"),
                          "bbox_max": b.get("bbox_max")}
                         for p in state.get("parts", [])
                         for b in p.get("bodies", [])] or [
                             {"name": b.get("name"), "volume": b.get("volume")}
                             for b in state.get("bodies", [])]
        errs = [f for f in state.get("features", []) if f.get("error")]
        if errs:
            out["ok"] = False
            out["feature_errors"] = [{"name": f.get("name"),
                                      "error": f.get("error")} for f in errs]
    except Exception as e:
        out["ok"] = False
        out["error"] = str(e)
    return out


@mcp.tool(description=(
    "The document format: its current version number, and a worked example "
    "of a complete part — a sketch, an extrude that names it, and a second "
    "sketch cut through. Read this before your first write_document."))
def document_format() -> dict:
    d = _client()
    version = d.call("describe").get("document_format_version")
    # Built by the app itself rather than transcribed here, so the example can
    # never drift from what the loader accepts.
    d.call("new_project")
    sid = d.call("add_feature", feature={
        "type": "Sketch", "name": "Base",
        "plane": {"origin": [0, 0, 0], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "rect", "c1": [0, 0], "c2": [50, 30]}]})["id"]
    d.call("add_feature", feature={"type": "Extrude", "name": "Body",
                                   "sketch_id": sid, "distance": 10,
                                   "mode": "new_body"})
    hid = d.call("add_feature", feature={
        "type": "Sketch", "name": "Hole",
        "plane": {"origin": [0, 0, 10], "normal": [0, 0, 1],
                  "uAxis": [1, 0, 0]},
        "entities": [{"type": "circle", "center": [25, 15], "radius": 4}]})["id"]
    d.call("add_feature", feature={"type": "Extrude", "name": "Bore",
                                   "sketch_id": hid, "distance": 10,
                                   "mode": "cut", "reversed": True})
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "example.json")
        d.call("save_project", path=path)
        example = json.load(open(path))
    d.call("new_project")
    return {"document_format_version": version, "example": example}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, required=True)
    args = ap.parse_args()
    global _PORT
    _PORT = args.port
    mcp.run()


if __name__ == "__main__":
    main()
