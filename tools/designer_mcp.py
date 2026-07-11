#!/usr/bin/env python3
"""MCP server for Designer — drive a running instance over its JSON-RPC remote.

Design: docs/scripting/s1-mcp.md. Start the app with `designer --remote 8437`,
then register this server with an MCP client, e.g.:

    claude mcp add designer -- python3 tools/designer_mcp.py --port 8437

The port also comes from $DESIGNER_PORT (flag wins). Requires the official
MCP Python SDK (`mcp` on PyPI, `python3Packages.mcp` on nixpkgs).
"""
import argparse
import os
import sys
import tempfile

from mcp.server.fastmcp import FastMCP, Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from designer_remote import Designer  # noqa: E402

# ---------------------------------------------------------------------------
# Format reference, embedded into tool descriptions. Source of truth:
# src/core/Serialization.cpp (FeatureFromJson / EntityToJson) and
# src/remote/RemoteServer.cpp (add_constraint). Transcribed for OCCT-side v12.
# ---------------------------------------------------------------------------

AXIS_REF = """An axis reference is {"kind": "x"|"y"|"z"} (global axes) or
{"kind": "sketch_line", "sketch_id": <feature id>, "entity_id": <line id>}."""

FEATURE_FORMATS = f"""Feature JSON formats (field: value type; ? = optional):

- Box: {{"type": "Box", "name": str, "position": [x,y,z],
  "width": num, "height": num, "depth": num}}
- Sphere: {{"type": "Sphere", "name": str, "center": [x,y,z], "radius": num}}
- Cylinder: {{"type": "Cylinder", "name": str, "center": [x,y,z],
  "direction": [x,y,z], "radius": num, "height": num}}
- Sketch: {{"type": "Sketch", "name": str,
  "plane": {{"origin": [x,y,z], "normal": [x,y,z], "uAxis": [x,y,z]}},
  "plane_ref"?: <DatumPlane feature id — tracks the datum instead of baking>,
  "entities": [<entity>, ...]}} . Entity formats (2D UV coords on the plane):
    {{"type": "point", "p": [u,v]}}
    {{"type": "line", "a": [u,v], "b": [u,v]}}
    {{"type": "circle", "center": [u,v], "radius": num}}
    {{"type": "arc", "center": [u,v], "radius": num,
      "start_angle": rad, "end_angle": rad}}
    {{"type": "ellipse", "center": [u,v], "r_major": num, "r_minor": num,
      "rotation": rad}}
  Any entity may set "construction": true (reference geometry, not profiled).
  Do NOT send entity ids — the app assigns them; add_feature returns
  "entity_ids" in list order. Use those ids for constraints and ops.
- Extrude: {{"type": "Extrude", "name": str, "sketch_id": <feature id>,
  "distance": num, "distance_expr"?: str, "reversed"?: bool,
  "mode": "new_body"|"add"|"cut",
  "end_mode"?: "symmetric" (centered on the plane) | "two_sided"
  (+ "distance2"/"distance2_expr" backward) | "up_to_face"
  (+ "end_target": {{"feature_id", "element"}} — a planar face from
  list_elements; follows upstream edits),
  "thin"?: {{"thickness": num, "thickness_expr"?: str,
  "side": "out"|"in"|"both"}} — walls every loop instead of filling}}.
  Sketches may hold SEVERAL disjoint profiles (e.g. four bolt-hole circles):
  nested loops become holes, islands become solids again.
- Revolve: {{"type": "Revolve", "name": str, "sketch_id": <feature id>,
  "axis": <axis ref>, "angle_deg"?: num (default 360), "angle_expr"?: str,
  "symmetric"?: bool, "reversed"?: bool, "mode": "new_body"|"add"|"cut"}}
- Boolean: {{"type": "Boolean", "name": str, "target_id": <feature id>,
  "tool_id": <feature id>, "op": "union"|"subtract"|"intersect"}}
- Pattern: {{"type": "Pattern", "name": str,
  "kind": "mirror"|"linear"|"circular", "source_id": <feature id>, ...}}
  mirror: "parent_id"?: <DatumPlane id> or "base_plane": "xy"|"xz"|"yz"
  linear: "axis": <axis ref>, "count": int, "count_expr"?: str,
          "spacing": num, "spacing_expr"?: str
  circular: "axis": <axis ref>, "count": int, "count_expr"?: str,
            "angle_deg": num, "angle_expr"?: str
- DatumPlane: {{"type": "DatumPlane", "name": str,
  "kind": "offset"|"angled"|"through_points", ...}}
  offset/angled parent: "parent_id"?: <DatumPlane id> or
  "base_plane": "xy"|"xz"|"yz" (xy = Front, xz = Top, yz = Right)
  offset: "offset": num, "offset_expr"?: str, "flip"?: bool
  angled: "axis": <axis ref>, "angle_deg": num, "angle_expr"?: str
  through_points: "points": [[x,y,z],[x,y,z],[x,y,z]]
- DatumAxis: {{"type": "DatumAxis", "name": str, "kind":
  "two_points"|"sketch_line"|"global_x"|"global_y"|"global_z", ...}}
  two_points: "points": [[x,y,z],[x,y,z]]
  sketch_line: "sketch_id": <feature id>, "entity_id": <line id>
- Fillet / Chamfer (3D, on B-Rep edges): {{"type": "Fillet"|"Chamfer",
  "name": str, "radius": num, "radius_expr"?: str,
  "edges": [{{"feature_id": <the body's created_by feature id>,
  "element": <stable edge name>}}, ...]}} — all edges must belong to one
  body. Discover edge names + created_by with the list_elements tool.
- Import (STEP): {{"type": "Import", "name": str, "path": str}}

{AXIS_REF}

Every *_expr field is an expression over document variables (see
set_variable), e.g. "width / 2 + 1"; the literal twin is used when absent.
Angles in *_deg fields and expressions are degrees."""

# Datum references usable anywhere a constraint ref names an entity.
DATUM_ORIGIN = 0xFFFFFF00  # sketch origin point (0,0)
DATUM_AXIS_X = 0xFFFFFF01  # sketch X axis (the +U direction)
DATUM_AXIS_Y = 0xFFFFFF02  # sketch Y axis (the +V direction)

CONSTRAINT_FORMATS = f"""Constraint kinds: coincident, horizontal, vertical,
parallel, perpendicular, equal, distance, radius, angle, tangent,
point_on_line, symmetric, midpoint, fixed, concentric, collinear,
distance_x, distance_y, diameter.

refs is a list of [entity_id, which] pairs. `which` picks a point on the
entity: line 0=a 1=b; circle 0=center; arc 0=center 1=start 2=end. For
whole-entity kinds (parallel, equal, tangent, ...) `which` is ignored — pass 0.
symmetric takes a third ref: the symmetry line.

Datum references (usable as entity_id): {DATUM_ORIGIN} = sketch origin,
{DATUM_AXIS_X} = X axis, {DATUM_AXIS_Y} = Y axis. E.g. anchor a corner:
{{"kind": "coincident", "refs": [[<line id>, 0], [{DATUM_ORIGIN}, 0]]}}.

Dimensional kinds (distance, radius, angle, distance_x, distance_y, diameter)
take value (angle in degrees) or expr (expression over document variables —
prefer it: "width" beats 40)."""

MATERIAL_SLUGS = ("aluminum, steel, stainless_steel, brass, bronze, copper, "
                  "chrome, gold, silver, titanium, plastic_white, "
                  "plastic_black, rubber, glass, concrete")

WORKFLOW = """You are modeling in Designer, a parametric B-Rep CAD app
(sketch → constrain → extrude/revolve, feature history, expressions).

The loop that works — follow it every time:
1. Restate the brief as named dimensions, then set_variable for each driving
   dimension (design intent in the name: "flange_width", not "w"). Prefer
   expressions over literals everywhere a *_expr or expr field exists.
2. Sketch on a plane (or a DatumPlane feature). Draw entities, then constrain
   until the geometry cannot wiggle: anchor to the sketch origin/axes datum
   refs, use distance/radius constraints driven by your variables.
3. Extrude/Revolve referencing the sketch by the id add_feature returned.
   Never guess ids — read them from tool results or get_state.
4. VERIFY after every mutating step: get_state (feature list, body volumes,
   bounding boxes, per-feature "error" fields — sanity-check against the
   brief), mass_properties for the real oracle (CoM, mass, inertia), then
   screenshot (view="iso" default; front/top/right for dimensional checks).
   Do not batch-build blind.
5. For 3D fillets/chamfers: list_elements to get stable edge names, then
   add_feature with type Fillet/Chamfer referencing them.
6. Iterate. A wrong step is one undo away — prefer undo over compensating
   edits; checkpoint_create before risky multi-step changes and
   checkpoint_restore instead of reconstructing. A feature that errors no
   longer blanks the model (regeneration continues past it) — read the
   feature's "error", fix or suppress_feature it. set_rollback lets you
   inspect intermediate history states. When done: save_project, and
   render_still for a presentation shot.
"""

# ---------------------------------------------------------------------------
# Connection
# ---------------------------------------------------------------------------

mcp = FastMCP(
    "designer",
    instructions="Tools for driving a running Designer CAD instance. "
    "Read the modeling-workflow prompt before modeling. Semantic tools are "
    "the interface; ui_action is only for driving/testing the GUI itself.",
)

_client = None
_port = int(os.environ.get("DESIGNER_PORT", "8437"))


def _call(method, **params):
    """Call the app, reconnecting once across a dropped connection."""
    global _client
    for attempt in (1, 2):
        if _client is None:
            try:
                # Generous timeout: render_still runs synchronously in-app.
                _client = Designer(_port, timeout=600.0)
            except OSError as e:
                raise RuntimeError(
                    f"Designer is not reachable on port {_port}. Start it "
                    f"with: designer --remote {_port}  (or set "
                    f"DESIGNER_PORT / --port to where it listens)") from e
        try:
            return _client.call(method, **params)
        except (ConnectionError, BrokenPipeError, OSError):
            _client = None  # stale socket (app restarted?) — retry fresh
            if attempt == 2:
                raise


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------

@mcp.tool(description="Full document state: features (with ids and per-type "
          "fields), bodies (name, volume, bbox_min/bbox_max, material, and "
          "the render 'mesh' id select_face needs — mesh ids change on every "
          "regeneration, always read them fresh), current selection, camera, "
          "undo/redo availability. Call after every mutating step to verify.")
def get_state() -> dict:
    return _call("get_state")


@mcp.tool(description="List document variables: name, expression, evaluated "
          "value, comment, and error if the expression fails to evaluate.")
def list_variables() -> dict:
    return _call("list_variables")


@mcp.tool(description="Mass properties per body (and a document total): "
          "volume, surface_area, center_of_mass, inertia matrix (about the "
          "CoM), density and mass_g (grams, assuming model units are mm; "
          "null when the body has no material assigned). The geometry "
          "oracle — verify against the brief with this, not just volume. "
          "Optional body index restricts to one body.")
def mass_properties(body: int = -1) -> dict:
    return _call("mass_properties", **({"body": body} if body >= 0 else {}))


@mcp.tool(description="Stable element names (topological naming) for each "
          "body's faces and edges, plus the body's created_by feature id. "
          "This is how Fillet/Chamfer edge refs are built: edges: "
          "[{feature_id: <created_by>, element: <edge name>}]. Names survive "
          "upstream edits; re-query after changing the feature history. "
          "Optional body index restricts to one body.")
def list_elements(body: int = -1) -> dict:
    return _call("list_elements", **({"body": body} if body >= 0 else {}))


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

@mcp.tool(description="Append a feature to the history and regenerate. "
          "Returns {index, id, entity_ids?} — reference the feature by this "
          "id (e.g. Extrude.sketch_id), and sketch entities by entity_ids "
          "(in the order you sent them). One undo step.\n\n" + FEATURE_FORMATS)
def add_feature(feature: dict) -> dict:
    return _call("add_feature", feature=feature)


@mcp.tool(description="Replace the feature at a history index with new "
          "feature JSON (same formats as add_feature; keep the same type and "
          "id). Regenerates. Get indices from get_state.")
def modify_feature(index: int, feature: dict) -> dict:
    return _call("modify_feature", index=index, feature=feature)


@mcp.tool(description="Delete the feature at a history index. Later features "
          "referencing it will fail to regenerate — check get_state after.")
def delete_feature(index: int) -> dict:
    return _call("delete_feature", index=index)


@mcp.tool(description="Assign a render material to a feature (by feature id)."
          " Slugs: " + MATERIAL_SLUGS + ". Empty string restores the default.")
def set_material(feature_id: int, material: str) -> dict:
    return _call("set_material", feature_id=feature_id, material=material)


# ---------------------------------------------------------------------------
# Sketch
# ---------------------------------------------------------------------------

@mcp.tool(description="Add a constraint to a sketch (by feature id), solve, "
          "and regenerate. The solve may move geometry — read positions back "
          "via get_state, never assume.\n\n" + CONSTRAINT_FORMATS)
def add_constraint(sketch_id: int, kind: str, refs: list,
                   value: float = 0.0, expr: str = "") -> dict:
    params = {"sketch_id": sketch_id, "kind": kind, "refs": refs,
              "value": value}
    if expr:
        params["expr"] = expr
    return _call("add_constraint", **params)


@mcp.tool(description="Delete a sketch entity by id; constraints referencing "
          "it are dropped (count returned as dropped_constraints).")
def delete_entity(sketch_id: int, entity_id: int) -> dict:
    return _call("delete_entity", sketch_id=sketch_id, entity_id=entity_id)


@mcp.tool(description="Mirror sketch entities across a line entity. sources: "
          "entity ids; axis: the mirror line's entity id (construction lines "
          "work). link=true keeps mirrors following their sources. Returns "
          "generated entity ids.")
def mirror(sketch_id: int, sources: list, axis: int,
           link: bool = True) -> dict:
    return _call("mirror", sketch_id=sketch_id, sources=sources, axis=axis,
                 link=link)


@mcp.tool(description="Fillet the corner where two sketch lines meet with an "
          "arc of the given radius. Returns the arc's entity id (arc_id).")
def fillet(sketch_id: int, line1: int, line2: int, radius: float) -> dict:
    return _call("fillet", sketch_id=sketch_id, line1=line1, line2=line2,
                 radius=radius)


@mcp.tool(description="Repeat sketch entities in a linear or circular "
          "pattern. sources: entity ids; count includes the originals. "
          "Linear: step=[du,dv] spacing. Circular: circular=true, "
          "center=[u,v], angle = total sweep in degrees. link=true keeps "
          "copies following their sources. Returns generated entity ids.")
def pattern(sketch_id: int, sources: list, count: int,
            circular: bool = False, step: list | None = None,
            center: list | None = None, angle: float = 0.0,
            link: bool = True) -> dict:
    params = {"sketch_id": sketch_id, "sources": sources, "count": count,
              "circular": circular, "link": link}
    if circular:
        params["center"] = center or [0, 0]
        params["angle"] = angle
    else:
        params["step"] = step or [1, 0]
    return _call("pattern", **params)


@mcp.tool(description="Offset a connected chain of sketch entities by a "
          "distance (sign picks the side). chain: entity ids in order. "
          "join_arc=true rounds convex corners. Returns generated entity ids.")
def offset(sketch_id: int, chain: list, distance: float,
           join_arc: bool = True) -> dict:
    return _call("offset", sketch_id=sketch_id, chain=chain,
                 distance=distance, join_arc=join_arc)


# ---------------------------------------------------------------------------
# Variables
# ---------------------------------------------------------------------------

@mcp.tool(description="Create or redefine a document variable. expression is "
          "a string over numbers and other variables ('40', 'width / 2'). "
          "Redefining regenerates every expression-driven feature; one undo "
          "step. Returns the evaluated value (and error if it fails). Name "
          "every driving dimension of the design — parameters first.")
def set_variable(name: str, expression: str, comment: str = "") -> dict:
    params = {"name": name, "expression": expression}
    if comment:
        params["comment"] = comment
    return _call("set_variable", **params)


@mcp.tool(description="Rename a variable and rewrite every expression that "
          "references it.")
def rename_variable(name: str, new_name: str) -> dict:
    return _call("rename_variable", name=name, new_name=new_name)


@mcp.tool(description="Delete a variable. Refused (with the referencing "
          "fields listed) while anything still uses it.")
def delete_variable(name: str) -> dict:
    return _call("delete_variable", name=name)


# ---------------------------------------------------------------------------
# History & project
# ---------------------------------------------------------------------------

@mcp.tool(description="Suppress or unsuppress a feature (by feature id): "
          "suppressed features are skipped by regeneration but keep their "
          "parameters; anything referencing their outputs errors like a "
          "deleted reference. One undoable edit.")
def suppress_feature(feature_id: int, suppressed: bool) -> dict:
    return _call("suppress_feature", feature_id=feature_id,
                 suppressed=suppressed)


@mcp.tool(description="Move the rollback bar: regeneration executes only "
          "features [0, index); -1 = end. Features after the bar keep their "
          "params but contribute nothing — use it to inspect intermediate "
          "history states. Not undoable (a view, not an edit); "
          "get_state.rollback shows the position.")
def set_rollback(index: int) -> dict:
    return _call("set_rollback", index=index)


@mcp.tool(description="Undo the last document edit. Prefer this over "
          "compensating edits when a step went wrong.")
def undo() -> dict:
    return _call("undo")


@mcp.tool(description="Redo the last undone edit.")
def redo() -> dict:
    return _call("redo")


@mcp.tool(description="Replace the document with an empty one. Not undoable "
          "— save first if the current model matters.")
def new_project() -> dict:
    return _call("new_project")


@mcp.tool(description="Load a Designer project (.json) from a path, "
          "replacing the current document.")
def open_project(path: str) -> dict:
    return _call("open_project", path=path)


@mcp.tool(description="Save the document as Designer project JSON.")
def save_project(path: str) -> dict:
    return _call("save_project", path=path)


@mcp.tool(description="Import a STEP file's bodies as an Import feature.")
def import_step(path: str) -> dict:
    return _call("import_step", path=path)


@mcp.tool(description="Snapshot the current document as a named checkpoint "
          "(stored beside the project — save_project first). Checkpoint "
          "before risky multi-step edits; restore beats reconstructing.")
def checkpoint_create(name: str) -> dict:
    return _call("checkpoint_create", name=name)


@mcp.tool(description="List the project's checkpoints (name + modified "
          "unix time, newest first).")
def checkpoint_list() -> dict:
    return _call("checkpoint_list")


@mcp.tool(description="Replace the current document with a checkpoint's "
          "state (same project path; the restored state is unsaved until "
          "you save_project).")
def checkpoint_restore(name: str) -> dict:
    return _call("checkpoint_restore", name=name)


# ---------------------------------------------------------------------------
# View & verify
# ---------------------------------------------------------------------------

@mcp.tool(description="Capture the viewport as a PNG and return the image — "
          "look at it. Optional view (front|back|left|right|top|bottom|iso) "
          "orients + fits first; iso for shape checks, axis views for "
          "dimensional ones. Optional path also keeps the file on disk.")
def screenshot(view: str = "", path: str = "") -> Image:
    if view:
        _call("view", name=view)
    out = path or os.path.join(tempfile.gettempdir(), "designer_mcp_shot.png")
    _call("screenshot", path=out)
    return Image(path=out)


@mcp.tool(description="Snap the camera to a standard view "
          "(front|back|left|right|top|bottom|iso); fit=true (default) frames "
          "the whole model. Instant — safe to screenshot right after.")
def view(name: str, fit: bool = True) -> dict:
    return _call("view", name=name, fit=fit)


@mcp.tool(description="Frame the whole model at the current orientation.")
def fit_view() -> dict:
    return _call("fit_view")


@mcp.tool(description="Select a B-Rep face by render mesh id + face index "
          "(mesh ids from get_state bodies[].mesh — refetch after every "
          "regeneration). Returns the face normal and center. Follow with "
          "orient_to_face to look straight at it.")
def select_face(mesh: int, face: int) -> dict:
    return _call("select_face", mesh=mesh, face=face)


@mcp.tool(description="Orient the camera to look squarely at the selected "
          "face (select_face first).")
def orient_to_face() -> dict:
    return _call("orient_to_face")


@mcp.tool(description="Offline path-traced render (soft shadows, materials, "
          "environment lighting) returned as an image. Frame with view/"
          "fit_view first. The app freezes for the duration — samples=64 is "
          "a fast preview, 512+ presentation quality. ground adds a shadow-"
          "catcher plane. Requires a ray-tracing GPU (errors otherwise). "
          "Optional path also keeps the PNG.")
def render_still(width: int = 640, height: int = 480, samples: int = 64,
                 seed: int = 1, ground: bool = True, path: str = "") -> Image:
    out = path or os.path.join(tempfile.gettempdir(), "designer_mcp_still.png")
    r = _call("render_still", width=width, height=height, samples=samples,
              seed=seed, ground=ground, path=out)
    if not r.get("ok"):
        raise RuntimeError("render failed: " + str(r.get("error", "unknown")))
    return Image(path=out)


# ---------------------------------------------------------------------------
# Escape hatch: raw UI events (for driving/testing the Designer GUI itself)
# ---------------------------------------------------------------------------

_UI_ACTIONS = ("menu_click", "item_click", "item_input", "viewport_click",
               "viewport_drag", "key_press", "scroll")


@mcp.tool(description="Raw GUI event, for driving or testing the Designer "
          "GUI itself — for modeling, use the semantic tools instead. "
          "action + params: menu_click {path: 'Edit/Undo'}; item_click "
          "{ref}; item_input {ref, value}; viewport_click {x, y (0-1 window "
          "fractions), button?: 0/1/2, ctrl?}; viewport_drag {x0, y0, x1, "
          "y1, button?: 2=MMB orbit, ctrl?: pan, shift?: zoom}; key_press "
          "{chord: 'Ctrl+Z'}; scroll {x, y, amount}.")
def ui_action(action: str, params: dict | None = None) -> dict:
    if action not in _UI_ACTIONS:
        raise ValueError(f"unknown action {action!r}; one of {_UI_ACTIONS}")
    return _call(action, **(params or {}))


@mcp.prompt(name="modeling-workflow",
            description="How to model a part in Designer with these tools")
def modeling_workflow() -> str:
    return WORKFLOW


def main():
    global _port
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=_port,
                        help="port of the running `designer --remote <port>`")
    args = parser.parse_args()
    _port = args.port
    mcp.run()  # stdio


if __name__ == "__main__":
    main()
