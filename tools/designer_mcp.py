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
# src/core/io/SerializeFeatures.cpp (FeatureFromJson / EntityFromJson) and
# RemoteModeling.cpp (ConstraintKindTable). The describe RPC serves the
# authoritative type lists; mcp_smoke cross-checks this text against it, so
# a new feature type that isn't documented here fails the suite.
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
    {{"type": "ellipse_arc", "center": [u,v], "r_major": num, "r_minor": num,
      "rotation": rad, "start_param": rad, "end_param": rad}}
    {{"type": "spline", "poles": [[u,v], ...], "degree"?: int (default 3)}}
    {{"type": "rect", "c1": [u,v], "c2": [u,v]}} — a convenience that
      expands to FOUR "line" entities (+ corner coincident/H/V constraints);
      the 4 ids land at the END of entity_ids, after every non-rect
      entity's, in rect order. Corner-fillet the expanded lines like any
      other adjacent pair.
  Any entity may set "construction": true (reference geometry, not profiled).
  Entity coordinates can be EXPRESSION-DRIVEN via "<field>_expr" keys
  evaluated against document variables on every regeneration: line "ax_expr"/
  "ay_expr"/"bx_expr"/"by_expr", circle+arc "cx_expr"/"cy_expr"/"r_expr",
  point "x_expr"/"y_expr" (e.g. {{"type": "circle", "center": [0, 0],
  "radius": 5, "r_expr": "bore / 2"}}). Driven coordinates are pinned during
  the sketch solve (shown violet, like linked copies) — don't also dimension
  them. Often a distance/radius constraint with "expr" is the better tool;
  entity exprs shine for direct analytic placement (lobe centers, cam
  profiles).
  Do NOT send entity ids — the app assigns them; add_feature returns
  "entity_ids" in list order (except rect expansions — see above). Use
  those ids for constraints and ops.
- Extrude: {{"type": "Extrude", "name": str, "sketch_id": <feature id>,
  "distance": num, "distance_expr"?: str, "reversed"?: bool,
  "mode": "new_body"|"add"|"cut",
  "end_mode"?: "symmetric" (centered on the plane) | "two_sided"
  (+ "distance2"/"distance2_expr" backward) | "up_to_face"
  (+ "end_target": {{"feature_id", "element"}} — a planar face from
  list_elements; follows upstream edits) | "through_all" (cut only:
  sweeps past every body, distance ignored),
  "thin"?: {{"thickness": num, "thickness_expr"?: str,
  "side": "out"|"in"|"both"}} — walls every loop instead of filling}}.
  Extrudes sweep along the sketch plane's +normal; "reversed": true flips
  them. So a cut whose sketch sits ON a face wants "reversed" (into the
  material) — no need to park the plane behind the part and overshoot;
  "through_all" + "reversed" is the usual drill-through-from-a-face combo.
  Sketches may hold SEVERAL disjoint profiles (e.g. four bolt-hole circles):
  nested loops become holes, islands become solids again.
  "regions"?: [[entity ids]] — extrude only the listed regions, each named
  by its OUTER loop's boundary entity ids (holes inside a chosen region cut
  automatically). Absent = every region. Also on Revolve.
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
- Draft (taper faces by an angle for mould release): {{"type": "Draft",
  "name": str, "body_id": <the body's created_by feature id>,
  "angle_deg": num, "angle_expr"?: str, "neutral_plane": "xy"|"xz"|"yz"
  (the pull direction is this plane's normal), "faces": [{{"feature_id":
  <same body>, "element": <side-face name from list_elements>}}, ...]}} —
  faces perpendicular to the pull can't be tapered (pick side faces).
- Hole (drill holes at a sketch's Point entities, cut into the bodies below
  along the sketch plane's -normal): {{"type": "Hole", "name": str,
  "sketch_id": <a sketch whose points are the hole centers>,
  "hole_kind": "simple"|"counterbore"|"countersink", "diameter": num,
  "diameter_expr"?: str, "depth": num, "depth_expr"?: str, "through"?: bool,
  counterbore: "cb_diameter"/"cb_depth", countersink: "cs_diameter"/
  "cs_depth"}} — far cleaner than a construction circle + cut-extrude per
  bolt hole; put the point sketch on the face you drill from.
- Sweep (sweep a closed profile sketch along an open path):
  {{"type": "Sweep", "name": str, "profile_id": <sketch feature id>,
  "path_id": <sketch OR Curve3D feature id>, "guide_id"?: <sketch/Curve3D —
  a guide curve shaping the section along the way>,
  "frame"?: "corrected_frenet" (default) | "frenet" | "fixed",
  "mode": "new_body"|"add"|"cut"}} — put the profile at the path's start, on
  a plane perpendicular to the path (e.g. profile on XY, path on XZ/YZ from
  the origin). Path = one open chain of lines/arcs (not closed).
- Loft (skin a solid through 2+ profile sketches): {{"type": "Loft",
  "name": str, "sketch_ids": [<feature id>, <feature id>, ...],
  "path_id"?: <spine curve>, "guide_id"?: <guide curve>,
  "mode": "new_body"|"add"|"cut"}} — each sketch's outer profile is a
  section, in order; put the sketches on datum planes at different heights.
- Curve3D (a 3D curve for sweep/pipe/loft paths): {{"type": "Curve3D",
  "name": str, "kind": "fit_spline"|"control_poly"|"helix", ...}}
  fit_spline/control_poly: "points": [[x,y,z], ...], "degree"?: int,
  "closed"?: bool
  helix: "origin": [x,y,z], "axis": [x,y,z], "radius": num, "pitch": num,
  "turns": num (each with "_expr" twins), "left_handed"?: bool
- Pipe (tube along a path, no profile sketch needed): {{"type": "Pipe",
  "name": str, "path_id": <sketch/Curve3D id>, "outer_diameter": num,
  "wall_thickness": num (0 = solid rod), "outer_diameter_expr"?/
  "wall_thickness_expr"?: str, "mode": "new_body"|"add"|"cut"}}
- Rib (thicken an open sketch chain into a web up to the body):
  {{"type": "Rib", "name": str, "sketch_id": <open-profile sketch id>,
  "thickness": num, "thickness_expr"?: str, "flip"?: bool,
  "body_id"?: <target body's created_by id>}}
- Thread (cosmetic or modeled thread on a cylindrical face):
  {{"type": "Thread", "name": str, "face": {{"feature_id", "element"}},
  "designation"?: str (e.g. "M8x1.25"), "pitch": num, "length"?: num,
  "length_expr"?: str, "modeled"?: bool (true cuts real helical threads)}}
- Split (cut a body in two, keeping chosen sides): {{"type": "Split",
  "name": str, "body_id": <created_by id>, "knife"?: "plane" (default) |
  "face", plane: "datum_id"?: <DatumPlane id> or "base_plane":
  "xy"|"xz"|"yz" + "offset"/"offset_expr", face: "face": {{"feature_id",
  "element"}}, "keep"?: which sides}}
- Refine (heal/simplify a body's B-Rep — merge split faces):
  {{"type": "Refine", "name": str, "body_id": <created_by id>}}
- Shell (hollow a body into a wall): {{"type": "Shell", "name": str,
  "body_id": <the body's created_by feature id>, "thickness": num,
  "thickness_expr"?: str, "open_faces": [{{"feature_id": <same body>,
  "element": <stable face name from list_elements>}}, ...]}} — at least one
  open face is required (the faces removed to hollow through).
- Import (STEP): {{"type": "Import", "name": str, "path": str}}

Call the `describe` tool for the authoritative method/type/enum lists.

{AXIS_REF}

Every *_expr field is an expression over PROJECT variables (project-wide by
default — see set_variable; shared across every part), e.g. "width / 2 + 1".
A variable that does not resolve is a FEATURE ERROR (visible in get_state), not
a silent fallback to the literal twin — define the variable before referencing
it. Angles in *_deg fields and expressions are degrees."""

# Datum references usable anywhere a constraint ref names an entity.
DATUM_ORIGIN = 0xFFFFFF00  # sketch origin point (0,0)
DATUM_AXIS_X = 0xFFFFFF01  # sketch X axis (the +U direction)
DATUM_AXIS_Y = 0xFFFFFF02  # sketch Y axis (the +V direction)

CONSTRAINT_FORMATS = f"""Constraint kinds: coincident, horizontal, vertical,
parallel, perpendicular, equal, distance, radius, angle, tangent,
point_on_line, symmetric, midpoint, fixed, concentric, collinear,
distance_x, distance_y, diameter, distance_pl.

refs is a list of [entity_id, which] pairs. `which` picks a point on the
entity: line 0=a 1=b; circle 0=center; arc 0=center 1=start 2=end. For
whole-entity kinds (parallel, equal, tangent, ...) `which` is ignored — pass 0.
symmetric takes a third ref: the symmetry line.

Datum references (usable as entity_id): {DATUM_ORIGIN} = sketch origin,
{DATUM_AXIS_X} = X axis, {DATUM_AXIS_Y} = Y axis. E.g. anchor a corner:
{{"kind": "coincident", "refs": [[<line id>, 0], [{DATUM_ORIGIN}, 0]]}}.

Dimensional kinds (distance, radius, angle, distance_x, distance_y, diameter,
distance_pl) take value (angle in degrees) or expr (expression over document
variables — prefer it: "width" beats 40). distance_pl is the perpendicular
point-to-line distance: refs = [[point entity, which], [line entity, 0]] —
also the parallel-lines distance (use an endpoint of the first line)."""

MATERIAL_SLUGS = ("aluminum, steel, stainless_steel, brass, bronze, copper, "
                  "chrome, gold, silver, titanium, plastic_white, "
                  "plastic_black, rubber, glass, concrete, wood_oak, "
                  "wood_pine, wood_walnut, plywood_birch, plywood_pine, mdf")

WORKFLOW = """You are modeling in Designer, a parametric B-Rep CAD app
(sketch → constrain → extrude/revolve, feature history, expressions).

The loop that works — follow it every time:
1. Restate the brief as named dimensions, then set_variable for each driving
   dimension (design intent in the name: "flange_width", not "w"). Variables
   are PROJECT-WIDE by default — one 'bore' drives every part of an assembly;
   use scope='part' only for a dimension private to one document. Prefer
   expressions over literals everywhere a *_expr or expr field exists.
2. Sketch on a plane (or a DatumPlane feature). Draw entities, then constrain
   until the geometry cannot wiggle: anchor to the sketch origin/axes datum
   refs, use distance/radius constraints driven by your variables.
3. Extrude/Revolve referencing the sketch by the id add_feature returned.
   Never guess ids — read them from tool results or get_state. Unsure of a
   feature's JSON dialect? describe lists feature_types[].params (field
   names, axis conventions); unknown fields ERROR instead of vanishing, so
   trust the error text — it names the accepted vocabulary.
4. VERIFY after every mutating step — the order is EXECUTE, MEASURE, then
   RENDER. Measure first: every mutating call already returns a `digest`
   (per-body volume/bbox/solid-count plus a delta: volume moved, bodies
   added/removed) — read it before anything else; a cut whose delta is ~0 or
   a body whose `solids` jumped to 2 is wrong NOW, not three features later.
   Check `regen.warnings` (a warning = built but likely did nothing, e.g. a
   cut whose profile missed every body). mass_properties is the oracle the
   render cannot fake (CoM, mass, inertia). THEN look: render_sheet gives
   four views + the kernel numbers burned into one PNG (screenshot for
   close-ups). Do not batch-build blind, and do not trust pixels for
   dimensions — cross-check them against the footer numbers.
4b. ASSEMBLIES: after placing occurrences, gate on contact_report — the
   whole structure must be ONE component (anything in its own component is
   floating in mid-air), zero clashes, zero near_misses (a 0.1-1 mm gap is
   an arithmetic slip). Assert every bolted joint `"touching"` and every
   designed clearance by number via interference {a, b} — state and
   distance, never just "no clash". Reproducing a reference or guarding a
   regression? compare_geometry {golden_path} gives IoU/precision/recall
   with plain-language verdicts, and missing_path/extra_path export the
   difference as solids.
   The full pre-delivery list: docs/inspect/review-checklist.md (one RPC
   per box, plus a symptom->cause->fix table for the classic traps).
5. For 3D fillets/chamfers: list_elements to get stable edge names, then
   add_feature with type Fillet/Chamfer referencing them. To FIND geometry
   (or assert it): query_geometry takes declarative predicates —
   geometry_type/normal/axis/radius±tol/area/near_point — and an
   expected_count that turns the query into a one-call assertion
   ("exactly 4 Ø6 bores with axis Z" -> ok true/false).
6. Iterate. A wrong step is one undo away — prefer undo over compensating
   edits; checkpoint_create before risky multi-step changes and
   checkpoint_restore instead of reconstructing. A feature that errors no
   longer blanks the model (regeneration continues past it) — read the
   feature's "error", fix or suppress_feature it. set_rollback lets you
   inspect intermediate history states. When done: save_project, and
   render_still for a presentation shot.

No display session of your own? screenshot needs a rendering instance:
launch `tools/headless-preview.sh <scratch-port>` (real renderer offscreen
under Xvfb) and point DESIGNER_PORT / --port at it — it drives and
screenshots exactly like a windowed instance. Leave port 8799 for the human's
dev-watch (the build they review).

RESEARCH REAL DIMENSIONS FIRST. When the part interfaces with a real product
(bearings, inserts, screws, connectors, rails), web-search the datasheet
numbers BEFORE sketching — bore/OD/width for bearings, thread pitch and
tap/clearance drill for screws, pin pitch for connectors. Never guess a real
dimension; put each researched number in a named variable with a comment
citing the source.

MANUFACTURING (FDM defaults — apply unless told otherwise):
- Walls >= 1.2 mm; any freestanding feature >= 0.4 mm (one nozzle width).
- Holes mating with hardware: add 0.2-0.3 mm diametral clearance (printed
  holes shrink); press-fits get 0.0-0.1 mm.
- Note the intended print orientation in your final answer (which face on the
  bed, where supports land, strength direction — layer bonds are the weak
  axis).
- Close with a one-line print recipe: material / layer height / walls /
  infill + one clause of why (e.g. "PETG, 0.2 mm, 4 walls, 40% gyroid —
  outdoor bracket under sustained load").

DELIVERY CADENCE. Do not disappear into a long build: model the base shape,
show the user (screenshot/render) before detailing; add features, show again;
then finish (chamfers/fillets) and deliver. Close every job with the exported
STL path, a parameter table (variable, value, what it drives) so the user can
see every knob, and an explicit offer to adjust any of them.
"""

# ---------------------------------------------------------------------------
# Connection
# ---------------------------------------------------------------------------

mcp = FastMCP(
    "designer",
    instructions="Tools for driving a running Designer CAD instance. Read the "
    "modeling-workflow prompt before modeling. The listed tools are the "
    "conversational surface; EVERY other RPC (assemblies, drawings, studies, "
    "configurations, sketch constraints, exports, raw GUI actions) is reached "
    "with rpc('method', {...}) and documented by help('method'). For building "
    "a part with loops or computed arguments, a Python script against "
    "tools/designer_remote.py beats a tool call per step.",
)

_client = None
# An EXPLICIT port (flag or env) is an instruction, not a guess: it pins the
# instance and never falls back to discovery, so a client told to drive one
# app cannot silently start driving another.
_pinned_port = os.environ.get("DESIGNER_PORT")
_port = int(_pinned_port) if _pinned_port else None
_attached = None  # what ping said about the instance we are talking to

DEFAULT_PORT = 8437


def _running_instances():
    try:
        return Designer.instances()
    except Exception:
        return []  # discovery is a convenience, never a failure mode


def _resolve_port():
    """Which app to talk to — found, not assumed.

    The old behaviour was to assume 8437 and report "not reachable" when the
    app was running on any other port; this repo's own .mcp.json hardcoded
    8799 to work around it. Instances announce themselves now
    (src/remote/InstanceRegistry.h), so: a pinned port wins, one running
    instance is obvious, several are ambiguous and get asked about rather
    than guessed between.
    """
    if _port is not None:
        return _port
    live = _running_instances()
    if len(live) == 1:
        return int(live[0]["port"])
    if not live:
        raise RuntimeError(
            "No running Designer found. Start one with:  designer --remote "
            f"{DEFAULT_PORT}   (or `designer --headless --remote "
            f"{DEFAULT_PORT}` for no window — modeling and measurement work, "
            "screenshots do not). If it is already running, it is on a build "
            "older than instance discovery: pass --port/DESIGNER_PORT.")
    listed = ", ".join(
        f"{i['port']} ({i.get('mode', '?')})" for i in live)
    raise RuntimeError(
        f"{len(live)} Designer instances are running: {listed}. Which one? "
        "Set DESIGNER_PORT (or --port) — attaching to the wrong window would "
        "answer questions about a document you are not looking at.")


def _connect():
    global _client, _attached
    port = _resolve_port()
    try:
        # Generous timeout: render_still runs synchronously in-app.
        _client = Designer(port, timeout=600.0)
    except OSError as e:
        raise RuntimeError(
            f"Designer announced port {port} but is not answering there "
            f"({e}). If it just exited, start it again with: designer "
            f"--remote {port}") from e
    try:
        _attached = _client.call("ping")
    except Exception:
        _attached = {"port": port}  # older build: no enriched ping


def attached_to():
    """One line naming the instance in use — surfaced by the tools that
    matter, so nobody discovers halfway through that the agent was looking at
    a different window than they were."""
    if _attached is None:
        return None
    where = f"port {_attached.get('port', '?')}"
    mode = _attached.get("mode")
    project = _attached.get("project") or "unsaved project"
    if mode is None:
        return f"Designer on {where}"
    dirty = " (unsaved changes)" if _attached.get("dirty") else ""
    return f"Designer on {where}: {project}{dirty}, {mode}"


def _call(method, **params):
    """Call the app, reconnecting once across a dropped connection."""
    global _client
    for attempt in (1, 2):
        if _client is None:
            _connect()
        try:
            return _client.call(method, **params)
        except (ConnectionError, BrokenPipeError, OSError):
            _client = None  # stale socket (app restarted?) — retry fresh
            if attempt == 2:
                raise


# ---------------------------------------------------------------------------
# Inspect
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The exposed surface, and the reference behind it
#
# Every RPC the app serves used to be its own MCP tool: 118 of them, ~34k
# characters of descriptions that every client loaded before asking a single
# question. Most were thin wrappers around one `_call`, and the ones an
# assistant reaches for in a conversation are a couple of dozen — the rest are
# authoring calls that a script does better (a loop with logic between the
# steps, which a tool call per step cannot express).
#
# So the tool list is now the conversational surface plus two meta tools:
#
#   rpc(method, params)  — call ANY of the app's RPCs, including every one
#                          that used to be a tool. Where a wrapper exists it
#                          still runs, so the argument massaging and the
#                          friendly defaults are unchanged.
#   help(method)         — the prose that used to sit in that tool's
#                          description, on demand instead of always.
#
# Nothing was deleted: @_reference keeps the wrapper and its documentation,
# it just does not spend context announcing itself.
# ---------------------------------------------------------------------------

_REFERENCE = {}  # method -> {"doc": str, "fn": callable}


def _reference(description):
    """Keep a wrapper and its prose, without registering it as an MCP tool."""
    def wrap(fn):
        _REFERENCE[fn.__name__] = {"doc": description, "fn": fn}
        return fn
    return wrap


# The tools this server announces, for help()'s index. Kept next to the
# meta tools rather than derived from the FastMCP registry, which is private.
_EXPOSED = {
    "describe", "get_state", "check_geometry", "measure", "mass_properties",
    "list_elements", "list_variables", "set_variable", "screenshot",
    "render_still", "view", "set_camera", "set_section", "set_render_options",
    "new_project", "open_project", "save_project", "export_stl", "export_step",
    "add_feature", "modify_feature", "delete_feature", "undo", "rpc", "help",
    "instances",
}

@mcp.tool(description="API introspection — call this FIRST to learn the "
          "surface. Top-level keys: methods (name + one-line doc + "
          "requires_part_doc), feature_types (type + since_version), "
          "sketch_entity_types, constraint_kinds, extrude_modes, "
          "extrude_end_modes, axis_kinds, material_slugs, "
          "document_format_version. The authoritative companion to the "
          "format prose in this server's tool descriptions.")
def describe() -> dict:
    out = _call("describe")
    # Which instance answered. Everything below describes an app; this says
    # WHICH app, so a session cannot quietly be about the wrong window.
    out["attached"] = attached_to()
    return out


@mcp.tool(description="Full document state: features (with ids and per-type "
          "fields, plus 'error' if regeneration failed and 'warning' for a "
          "valid-but-likely-unintended no-op such as a cut that removed no "
          "material), bodies (name, volume, bbox_min/bbox_max, material, and "
          "the render 'mesh' id select_face needs — mesh ids change on every "
          "regeneration, always read them fresh), current selection, camera, "
          "undo/redo availability. Call after every mutating step to verify.")
def get_state() -> dict:
    return _call("get_state")


@mcp.tool(description="List variables: name, expression, evaluated value, "
          "scope ('project' = shared by every part, or 'part' = the active "
          "document's local scope, which shadows a same-named project var), "
          "comment, and error if the expression fails to evaluate. Project "
          "variables come first.")
def list_variables() -> dict:
    return _call("list_variables")


@mcp.tool(description="Toggle the viewport SECTION VIEW — a clipping plane "
          "that cuts the model open so you can see inside (screenshot to "
          "view it). enabled=False clears it. Plane by base_plane+offset or "
          "origin+normal. Works on part AND assembly tabs; on assemblies the "
          "clip applies per occurrence in world space but there are no "
          "filled cut caps, so the section reads hollow. (This is the "
          "visual cut; use `section` for the cross-section AREA.)")
def set_section(enabled: bool = True, base_plane: str = "xy",
                offset: float = 0.0, origin: list | None = None,
                normal: list | None = None) -> dict:
    if not enabled:
        return _call("set_section", enabled=False)
    if origin is not None and normal is not None:
        return _call("set_section", enabled=True, origin=origin, normal=normal)
    return _call("set_section", enabled=True, base_plane=base_plane,
                 offset=offset)


@_reference(description="Cross-section area where a plane cuts through all "
          "bodies (inspection). Plane: base_plane 'xy'|'xz'|'yz' + offset, "
          "or explicit origin+normal. Returns {area}. Use to check wall "
          "thickness / material at a cut.")
def section(base_plane: str = "xy", offset: float = 0.0,
            origin: list | None = None, normal: list | None = None) -> dict:
    if origin is not None and normal is not None:
        return _call("section", origin=origin, normal=normal)
    return _call("section", base_plane=base_plane, offset=offset)


@_reference(description="Interference (clash) check: returns {clash: bool, "
          "volume} — the volume of the overlap. On a PART tab a/b are body "
          "indices. On an active ASSEMBLY tab a/b are occurrence ids (int) "
          "or occurrence names (str); each occurrence's part bodies are "
          "placed by its solved transform and checked in world space — or "
          "OMIT a/b to sweep EVERY visible occurrence pair (returns {clash, "
          "pairs: [{a, b, clash, volume}]}). Run it after mating — clash "
          "checks matter most in assemblies.")
def interference(a: int | str | None = None, b: int | str | None = None) -> dict:
    if a is None and b is None:
        return _call("interference")
    return _call("interference", a=a, b=b)


@mcp.tool(description="Mass properties per body (and a document total): "
          "volume, surface_area, center_of_mass, inertia matrix (about the "
          "CoM), density and mass_g (grams, assuming model units are mm; "
          "null when the body has no material assigned). The geometry "
          "oracle — verify against the brief with this, not just volume. "
          "Optional body index restricts to one body. On an ASSEMBLY tab "
          "(no body arg) it aggregates every visible, non-suppressed "
          "occurrence in WORLD space: `occurrences` lists per-part rows "
          "(volume, world center_of_mass, mass_g) and `total` is the combined "
          "mass/volume/world CoM.")
def mass_properties(body: int = -1) -> dict:
    return _call("mass_properties", **({"body": body} if body >= 0 else {}))


@mcp.tool(description="Did the model build? Returns {ok, valid, body_count, "
          "feature_errors, bodies:[{body, name, valid, bad_faces, bad_edges, "
          "bad_wires, reasons}]}. `ok` is the probe you want: every existing "
          "body is a valid B-Rep AND no feature errored. `valid` is only the "
          "B-Rep check (OCC BRepCheck_Analyzer) — vacuously true over zero "
          "bodies, so read it with body_count. Read-only — run it on imported "
          "STEP or after booleans. Optional body index restricts to one body.")
def check_geometry(body: int = -1) -> dict:
    return _call("check_geometry", **({"body": body} if body >= 0 else {}))


@_reference(description="Project external geometry into a sketch (FreeCAD's "
          "ShapeBinder): reference a FACE or EDGE of a body earlier in the "
          "history, projected onto the sketch plane as LOCKED construction "
          "geometry that tracks the source when it moves. Returns "
          "{external_index, gen_ids}. `element` is the stable element name "
          "(from list_elements). Use it to align new geometry to existing "
          "edges/holes. params: sketch_id, feature_id (the body's producer), "
          "element, kind ('face' or 'edge').")
def project_geometry(sketch_id: int, feature_id: int, element: str,
                     kind: str = "face") -> dict:
    return _call("project_geometry", sketch_id=sketch_id, feature_id=feature_id,
                 element=element, kind=kind)


@mcp.tool(description="Measure geometry over stable element names (from "
          "list_elements). With `a` only: that element's own measure — a "
          "face's area or an edge's length. With `a` and `b`: the minimum "
          "distance between the two elements (faces or edges). Each ref is "
          "{feature_id, element}. Optional `part` (slot name or stable id) "
          "measures against that part WITHOUT switching the active tab — "
          "works from an assembly tab too. Verify dimensions with this "
          "instead of trusting your inputs.")
def measure(a: dict, b: dict | None = None,
            part: str | int | None = None) -> dict:
    params = {"a": a}
    if b is not None:
        params["b"] = b
    if part is not None:
        params["part"] = part
    return _call("measure", **params)


@mcp.tool(description="Stable element names (topological naming) for each "
          "body's faces and edges, plus the body's created_by feature id. "
          "Every face carries kind ('plane'|'cylinder'|'cone'|'sphere'|"
          "'other'), area, `centroid` [x,y,z], and outward `normal` [x,y,z] "
          "— find 'the back face' or 'the +Y pocket floors' by geometry "
          "directly, NO select_face probe loop needed; every edge carries "
          "kind ('line'|'circle'|'arc'|'other'), length, `midpoint`, and "
          "radius when circular — identify bore rims / "
          "seams directly instead of measure-probing names. This is how "
          "Fillet/Chamfer edge refs are built: edges: "
          "[{feature_id: <created_by>, element: <edge name>}]. Names survive "
          "upstream edits; re-query after changing the feature history. "
          "Optional body index restricts to one body; optional `part` (slot "
          "name or stable id) reads another part WITHOUT switching the "
          "active tab — works from an assembly tab too.")
def list_elements(body: int = -1, part: str | int | None = None) -> dict:
    params = {}
    if body >= 0:
        params["body"] = body
    if part is not None:
        params["part"] = part
    return _call("list_elements", **params)


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

@mcp.tool(description="Append a feature to the history and regenerate. "
          "Returns {index, id, entity_ids?, regen} — reference the feature by "
          "this id (e.g. Extrude.sketch_id), and sketch entities by "
          "entity_ids (in the order you sent them; each rect expands to 4 "
          "line ids appended at the end). One undo step. ALWAYS "
          "check regen: {ok, failed:[{index,name,error}], warnings:[{index,"
          "name,warning}]} — ok false means the edit broke the model (fix the "
          "first non-collateral failure); a warning means it built but likely "
          "did nothing intended (e.g. a cut whose profile missed every "
          "body).\n\n" + FEATURE_FORMATS)
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


@_reference(description="Assign a render material. Target ONE of: feature_id "
          "(the body's created_by from get_state/list_elements) or body (an "
          "index into get_state's bodies — add/cut keep the original creator, "
          "so this recolors everything fused into that body). Slugs: "
          + MATERIAL_SLUGS + ". Empty string restores the default.")
def set_material(material: str, feature_id: int = -1, body: int = -1) -> dict:
    args = {"feature_id": feature_id} if feature_id >= 0 else \
        ({"body": body} if body >= 0 else {})
    return _call("set_material", material=material, **args)


# ---------------------------------------------------------------------------
# Sketch
# ---------------------------------------------------------------------------

@_reference(description="Add a constraint to a sketch (by feature id), solve, "
          "and regenerate. The solve may move geometry — read positions back "
          "via get_state, never assume.\n\n" + CONSTRAINT_FORMATS)
def add_constraint(sketch_id: int, kind: str, refs: list,
                   value: float = 0.0, expr: str = "") -> dict:
    params = {"sketch_id": sketch_id, "kind": kind, "refs": refs,
              "value": value}
    if expr:
        params["expr"] = expr
    return _call("add_constraint", **params)


@_reference(description="Delete a sketch entity by id; constraints referencing "
          "it are dropped (count returned as dropped_constraints).")
def delete_entity(sketch_id: int, entity_id: int) -> dict:
    return _call("delete_entity", sketch_id=sketch_id, entity_id=entity_id)


@_reference(description="Mirror sketch entities across a line entity. sources: "
          "entity ids; axis: the mirror line's entity id (construction lines "
          "work). link=true keeps mirrors following their sources. Returns "
          "generated entity ids.")
def mirror(sketch_id: int, sources: list, axis: int,
           link: bool = True) -> dict:
    return _call("mirror", sketch_id=sketch_id, sources=sources, axis=axis,
                 link=link)


@_reference(description="Fillet the corner where two sketch lines meet with an "
          "arc of the given radius. Returns the arc's entity id (arc_id).")
def fillet(sketch_id: int, line1: int, line2: int, radius: float) -> dict:
    return _call("fillet", sketch_id=sketch_id, line1=line1, line2=line2,
                 radius=radius)


@_reference(description="Repeat sketch entities in a linear or circular "
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


@_reference(description="Offset a connected chain of sketch entities by a "
          "distance (sign picks the side). chain: entity ids in order. "
          "join_arc=true rounds convex corners. Returns generated entity ids.")
def offset(sketch_id: int, chain: list, distance: float,
           join_arc: bool = True) -> dict:
    return _call("offset", sketch_id=sketch_id, chain=chain,
                 distance=distance, join_arc=join_arc)


# ---------------------------------------------------------------------------
# Reference images (sketch underlays)
# ---------------------------------------------------------------------------

@_reference(description="Import a raster image (PNG/JPEG/BMP) into a sketch (by "
          "feature id) as a reference underlay to trace geometry over. path is "
          "read by the running app (must be reachable by it). The image is "
          "centered on 'at' (default plane origin [0,0]) at a default scale; "
          "recalibrate to a real dimension with calibrate_reference_image. "
          "Returns {image_id, width_px, height_px, asset}.")
def add_reference_image(sketch_id: int, path: str, at: list | None = None,
                        units_per_pixel: float | None = None,
                        opacity: float = 0.5) -> dict:
    params = {"sketch_id": sketch_id, "path": path, "opacity": opacity}
    if at is not None:
        params["at"] = at
    if units_per_pixel is not None:
        params["units_per_pixel"] = units_per_pixel
    return _call("add_reference_image", **params)


@_reference(description="Scale a sketch reference image so its two picked "
          "plane-UV points p1,p2 are 'distance' plane units apart (p1 held "
          "fixed). This is how you match an image's printed dimension: click "
          "the two ends of a dimension line, pass its real length. "
          "align='horizontal'|'vertical' also rotates the image so the p1->p2 "
          "line lands on that axis; 'none' (default) keeps the orientation.")
def calibrate_reference_image(sketch_id: int, image_id: int, p1: list,
                              p2: list, distance: float,
                              align: str = "none") -> dict:
    return _call("calibrate_reference_image", sketch_id=sketch_id,
                 image_id=image_id, p1=p1, p2=p2, distance=distance,
                 align=align)


@_reference(description="Edit a sketch reference image (by image_id): pass any "
          "of opacity (0..1), hidden (bool), rotation (radians), "
          "units_per_pixel, or origin [u,v] (plane-UV of the image's (0,0) "
          "pixel corner). Only the fields you pass change.")
def set_reference_image(sketch_id: int, image_id: int,
                        opacity: float | None = None,
                        hidden: bool | None = None,
                        rotation: float | None = None,
                        units_per_pixel: float | None = None,
                        origin: list | None = None) -> dict:
    params = {"sketch_id": sketch_id, "image_id": image_id}
    if opacity is not None:
        params["opacity"] = opacity
    if hidden is not None:
        params["hidden"] = hidden
    if rotation is not None:
        params["rotation"] = rotation
    if units_per_pixel is not None:
        params["units_per_pixel"] = units_per_pixel
    if origin is not None:
        params["origin"] = origin
    return _call("set_reference_image", **params)


@_reference(description="Remove a sketch reference image by image_id.")
def delete_reference_image(sketch_id: int, image_id: int) -> dict:
    return _call("delete_reference_image", sketch_id=sketch_id,
                 image_id=image_id)


# ---------------------------------------------------------------------------
# Variables
# ---------------------------------------------------------------------------

@mcp.tool(description="Create or redefine a variable. expression is a string "
          "over numbers and other variables ('40', 'width / 2'). scope defaults "
          "to 'project' — a PROJECT-WIDE variable shared by every part, so one "
          "'bore' drives axle + wheel + fork together; pass scope='part' to "
          "write the active document's local scope instead (it shadows a "
          "same-named project var). Redefining regenerates every "
          "expression-driven feature in every part; one undo step. Returns the "
          "evaluated value + scope (and error if it fails). Name every driving "
          "dimension of the design — parameters first.")
def set_variable(name: str, expression: str, comment: str = "",
                 scope: str = "project") -> dict:
    params = {"name": name, "expression": expression, "scope": scope}
    if comment:
        params["comment"] = comment
    return _call("set_variable", **params)


@_reference(description="Rename a variable and rewrite every expression that "
          "references it (a project variable's references span all parts). "
          "scope auto-detects (project first, then the active part's local) "
          "when omitted.")
def rename_variable(name: str, new_name: str, scope: str = "") -> dict:
    params = {"name": name, "new_name": new_name}
    if scope:
        params["scope"] = scope
    return _call("rename_variable", **params)


@_reference(description="Delete a variable. Refused (with the referencing fields "
          "listed) while anything still uses it — a project variable checks "
          "every part. scope auto-detects (project first, then the active "
          "part's local) when omitted.")
def delete_variable(name: str, scope: str = "") -> dict:
    params = {"name": name}
    if scope:
        params["scope"] = scope
    return _call("delete_variable", **params)


# ---------------------------------------------------------------------------
# Configurations (parametric variants of the active part OR assembly)
# ---------------------------------------------------------------------------

@_reference(description="Add a configuration to the active part or assembly — a "
          "named variant. On a part it overrides variables / suppresses "
          "features; on an assembly it suppresses occurrences. Empty until you "
          "set_configuration it. Returns its id.")
def add_configuration(name: str = "") -> dict:
    return _call("add_configuration", **({"name": name} if name else {}))


@_reference(description="Remove a configuration by id (falls back to base if it "
          "was active).")
def remove_configuration(id: int) -> dict:
    return _call("remove_configuration", id=id)


@_reference(description="List the active part's configurations with each one's "
          "parameter overrides and suppressed feature ids, and which is active "
          "(0 = base, the model as authored).")
def list_configurations() -> dict:
    return _call("list_configurations")


@_reference(description="Edit a configuration (by id). On a PART: "
          "parameter_overrides = a list of {name, expression} replacing the "
          "config's variable overrides (e.g. {'name':'depth','expression':"
          "'12'}); suppressed_features = feature ids to suppress. On an "
          "ASSEMBLY: suppressed_occurrences = occurrence ids to hide. name "
          "renames. Each list replaces. Read list_configurations, adjust, send "
          "back. One undoable edit.")
def set_configuration(id: int, name: str = "",
                      parameter_overrides: list | None = None,
                      suppressed_features: list | None = None,
                      suppressed_occurrences: list | None = None) -> dict:
    params = {"id": id}
    if name:
        params["name"] = name
    if parameter_overrides is not None:
        params["parameter_overrides"] = parameter_overrides
    if suppressed_features is not None:
        params["suppressed_features"] = suppressed_features
    if suppressed_occurrences is not None:
        params["suppressed_occurrences"] = suppressed_occurrences
    return _call("set_configuration", **params)


@_reference(description="Switch the active configuration of the part (0 = base). "
          "Regenerates against it — subsequent geometry, export, drawings and "
          "FEM all reflect the active configuration. Returns the resulting body "
          "count and volumes.")
def set_active_configuration(id: int) -> dict:
    return _call("set_active_configuration", id=id)


# ---------------------------------------------------------------------------
# History & project
# ---------------------------------------------------------------------------

@_reference(description="Suppress or unsuppress a feature (by feature id): "
          "suppressed features are skipped by regeneration but keep their "
          "parameters; anything referencing their outputs errors like a "
          "deleted reference. One undoable edit.")
def suppress_feature(feature_id: int, suppressed: bool) -> dict:
    return _call("suppress_feature", feature_id=feature_id,
                 suppressed=suppressed)


@_reference(description="Move the rollback bar: regeneration executes only "
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


@_reference(description="Redo the last undone edit.")
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


@mcp.tool(description="Save the document. A .dsgn path (the default "
          "format) is a project DIRECTORY, git-backed: every save commits "
          "with `message`. A .json path is the single-file interchange "
          "form.")
def save_project(path: str, message: str = "save") -> dict:
    return _call("save_project", path=path, message=message)


@mcp.tool(description="Export to STEP (one shape per body). Whole document, "
          "or one body by index. STEP moves the exact B-Rep to other CAD.")
def export_step(path: str, body: int = -1) -> dict:
    return _call("export_step", path=path,
                 **({"body": body} if body >= 0 else {}))


@mcp.tool(description="Export to binary STL (re-meshed at `deflection`, "
          "smaller = finer). Whole document fuses to one solid, or one body "
          "by index. STL is how a part gets 3D-printed. The reply reports "
          "the source shape's watertightness ({watertight, shells, "
          "free_edges}); a non-watertight source still exports, but flag it "
          "before anyone slices the file.")
def export_stl(path: str, body: int = -1, deflection: float = 0.1) -> dict:
    return _call("export_stl", path=path, deflection=deflection,
                 **({"body": body} if body >= 0 else {}))


@_reference(description="Export to native OCC BREP text (exact, no "
          "tessellation) — useful for kernel bug reports.")
def export_brep(path: str, body: int = -1) -> dict:
    return _call("export_brep", path=path,
                 **({"body": body} if body >= 0 else {}))


@_reference(description="Export the tessellated mesh to glTF (.glb/.gltf), OBJ, "
          "or PLY, chosen by the path extension — for viewers (Blender, web, "
          "engines), not CAD round-trips. Materials carry through (glTF is "
          "PBR). Works on a part OR the active assembly (all occurrences at "
          "their solved placements). `deflection` sets mesh fineness.")
def export_visual(path: str, deflection: float = 0.1) -> dict:
    return _call("export_visual", path=path, deflection=deflection)


@_reference(description="List the project's tabs (parts and assemblies) with "
          "each one's type and stable id, and which is active. Modeling tools "
          "act on the active part; assembly tools on the active assembly.")
def list_parts() -> dict:
    return _call("list_parts")


@_reference(description="Add a new empty part to the project and switch to it "
          "(subsequent modeling goes into it). Returns its index.")
def add_part(name: str = "") -> dict:
    return _call("add_part", **({"name": name} if name else {}))


@_reference(description="Switch the active part by index (from list_parts). "
          "The document tools then act on that part.")
def set_active_part(index: int) -> dict:
    return _call("set_active_part", index=index)


@_reference(description="Delete a part by index (never the last one).")
def delete_part(index: int) -> dict:
    return _call("delete_part", index=index)


@_reference(description="Rename an existing part/assembly/drawing tab so the "
          "default 'Part 1' isn't permanent (it feeds BOM + add_occurrence). "
          "`part` is a name, stable id, or slot index (from list_parts). "
          "Occurrences reference parts by stable id, so the rename is safe.")
def rename_part(part, new_name: str) -> dict:
    return _call("rename_part", part=part, new_name=new_name)


# ---------------------------------------------------------------------------
# Assemblies (A2): an assembly tab holds occurrences — placed instances of
# parts. list_parts reports each tab's type ("part"/"assembly") and stable id.
# ---------------------------------------------------------------------------

@_reference(description="Add a new empty ASSEMBLY tab to the project and switch "
          "to it. An assembly places copies (occurrences) of parts by "
          "transform. Part-editing tools do not act on an assembly tab — use "
          "add_occurrence. Returns its index and stable id.")
def add_assembly(name: str = "") -> dict:
    return _call("add_assembly", **({"name": name} if name else {}))


@_reference(description="Place a part OR ANOTHER ASSEMBLY into the ACTIVE assembly "
          "as an occurrence (A5 sub-assemblies). part = the name of either, "
          "resolved to a stable id, so renames are safe; parts are matched "
          "first. transform = {position:[x,y,z], orientation:[x,y,z,w] "
          "quaternion}; omitted = identity. fixed=true grounds it as the "
          "assembly anchor; visible=false hides it. A sub-assembly is one "
          "rigid unit: it contributes its whole subtree to the BOM, mass and "
          "clash checks, and a reference that would close a cycle is refused. "
          "Returns {id, part_id, ref_kind}.")
def add_occurrence(part: str, name: str = "", transform: dict | None = None,
                   fixed: bool = False, visible: bool = True) -> dict:
    params = {"part": part, "fixed": fixed, "visible": visible}
    if name:
        params["name"] = name
    if transform is not None:
        params["transform"] = transform
    return _call("add_occurrence", **params)


@_reference(description="Remove an occurrence from the active assembly by id.")
def remove_occurrence(id: int) -> dict:
    return _call("remove_occurrence", id=id)


@_reference(description="Reposition an occurrence. transform = {position:[x,y,z], "
          "orientation:[x,y,z,w] quaternion}. Undoable. Returns the occurrence.")
def set_occurrence_transform(id: int, transform: dict) -> dict:
    return _call("set_occurrence_transform", id=id, transform=transform)


@_reference(description="Show or hide an occurrence (hidden ones are not "
          "rendered or pickable). Undoable.")
def set_occurrence_visible(id: int, visible: bool) -> dict:
    return _call("set_occurrence_visible", id=id, visible=visible)


@_reference(description="Ground an occurrence (fixed=true) or free it. A fixed "
          "occurrence is the assembly's immovable anchor. Undoable.")
def set_occurrence_fixed(id: int, fixed: bool) -> dict:
    return _call("set_occurrence_fixed", id=id, fixed=fixed)


@_reference(description="Rename an occurrence (its display label; the referenced "
          "part is unchanged). Undoable.")
def rename_occurrence(id: int, name: str) -> dict:
    return _call("rename_occurrence", id=id, name=name)


@_reference(description="List the active assembly's occurrences: id, name, "
          "part_id, transform, visible, fixed.")
def list_occurrences() -> dict:
    return _call("list_occurrences")


@_reference(description="Add a MATE between two occurrences of the active "
          "assembly (A3). type is 'fastened' (rigid), 'revolute' (rotate "
          "about Z), 'slider' (slide along Z), 'cylindrical' (rotate AND "
          "slide), 'planar' (slide in-plane via dx/dy + spin), or 'ball' "
          "(free 3-axis rotation). Each connector `a`/`b` is {occurrence:<id>, "
          "origin:true} for the part origin, or {occurrence:<id>, face:"
          "{feature_id, element}} for a named face (see list_elements): "
          "planar/cylindrical faces give a full frame; a CONE anchors at its "
          "apex, a SPHERE at its centre (natural ball-mate anchors). Beware "
          "coaxial faces sharing a centroid (a head and its cross-hole) — "
          "pick by area. The reply's errored_mates is the GLOBAL count after "
          "re-solve; `errored` lists which mates failed and why. `b`'s "
          "occurrence is placed relative to `a`. flipped opposes the two Z "
          "axes (default true, so touching faces mate); rotation/offset/dx/dy "
          "and ball_rotation ([x,y,z,w]) set the DOF pose. offset slides `b` "
          "along +Z of `a`'s connector frame (a's outward face normal) — for "
          "revolute it's the axial seat depth (slide up/down the shaft), for "
          "slider/cylindrical the DOF, for planar the plane gap; POSITIVE = "
          "along a's normal. Solves immediately.")
def add_mate(type: str, a: dict, b: dict, name: str = "",
             flipped: bool = True, rotation: float = 0.0, offset: float = 0.0,
             dx: float = 0.0, dy: float = 0.0,
             ball_rotation: list | None = None) -> dict:
    params = {"type": type, "a": a, "b": b, "flipped": flipped,
              "rotation": rotation, "offset": offset}
    if dx:
        params["dx"] = dx
    if dy:
        params["dy"] = dy
    if ball_rotation is not None:
        params["ball_rotation"] = ball_rotation
    if name:
        params["name"] = name
    return _call("add_mate", **params)


@_reference(description="Adjust a mate (by id) and re-solve — how you MOVE a "
          "mechanism numerically. rotation (revolute/cylindrical angle, "
          "radians), offset (slide along +Z of a's connector normal: "
          "slider/cylindrical DOF, revolute axial seat depth, planar gap; "
          "POSITIVE = along a's outward normal), dx/dy (planar in-plane "
          "slide), ball_rotation ([x,y,z,w] for a ball), flipped, or name. "
          "e.g. set a revolute's rotation to swing a hinge, or its offset to "
          "seat the part along the shaft.")
def set_mate(id: int, rotation: float | None = None, offset: float | None = None,
             dx: float | None = None, dy: float | None = None,
             ball_rotation: list | None = None,
             flipped: bool | None = None, name: str | None = None) -> dict:
    params: dict = {"id": id}
    for k, v in (("rotation", rotation), ("offset", offset), ("dx", dx),
                 ("dy", dy), ("ball_rotation", ball_rotation),
                 ("flipped", flipped), ("name", name)):
        if v is not None:
            params[k] = v
    return _call("set_mate", **params)


@_reference(description="Remove a mate (by id) and re-solve. Occurrences it "
          "positioned keep their last solved pose.")
def remove_mate(id: int) -> dict:
    return _call("remove_mate", id=id)


@_reference(description="List the active assembly's mates: id, name, type, the "
          "two connectors, DOF pose, and a per-mate 'error' when a connector "
          "ref is dangling or the mate is over-constrained.")
def list_mates() -> dict:
    return _call("list_mates")


@_reference(description="Re-solve the active assembly's mates (place every "
          "mated, non-fixed occurrence by frame alignment). Returns the "
          "count of errored mates and each mate's status. Solves run "
          "automatically after mate/occurrence edits; call this to inspect.")
def solve_assembly() -> dict:
    return _call("solve_assembly")


@_reference(description="Add a named exploded view to the active assembly "
          "(A4). Returns its id. Views are persistent; apply one with "
          "set_explode.")
def add_exploded_view(name: str = "Exploded") -> dict:
    return _call("add_exploded_view", name=name)


@_reference(description="Remove an exploded view by id.")
def remove_exploded_view(id: int) -> dict:
    return _call("remove_exploded_view", id=id)


@_reference(description="List the active assembly's exploded views (id, name, "
          "per-occurrence offsets) plus the applied view id and factor.")
def list_exploded_views() -> dict:
    return _call("list_exploded_views")


@_reference(description="Set one occurrence's world-space offset in an "
          "exploded view (creates the step if missing). offset is [x,y,z] "
          "at factor 1.")
def set_explode_step(view: int, occurrence: int, offset: list) -> dict:
    return _call("set_explode_step", view=view, occurrence=occurrence,
                 offset=offset)


@_reference(description="Seed an exploded view radially: every occurrence "
          "pushed away from the assembly centroid, scaled by assembly size. "
          "A starting point to refine with set_explode_step. Omit `view` (or "
          "pass 0) to auto-create a fresh exploded view; the returned `view` "
          "is its id.")
def auto_explode(view: int = 0) -> dict:
    return _call("auto_explode", **({"view": view} if view else {}))


@_reference(description="Apply an exploded view to the display (transient, "
          "never saved): view id (0 = none) and/or factor 0..1. The solved "
          "transforms are untouched — screenshot an exploded assembly with "
          "set_explode + screenshot.")
def set_explode(view: int = -1, factor: float = -1.0) -> dict:
    args = {}
    if view >= 0:
        args["view"] = view
    if factor >= 0:
        args["factor"] = factor
    return _call("set_explode", **args)


@_reference(description="Bill of materials for the active assembly (A4): one "
          "row per referenced part with quantity, material, unit/total mass "
          "(grams; null when a body has no material density) and a mass "
          "total. Derived fresh from the document on every call.")
def bom() -> dict:
    return _call("bom")


@_reference(description="Write the active assembly's bill of materials to a "
          "CSV file at `path` (same rows as `bom`).")
def export_bom(path: str) -> dict:
    return _call("export_bom", path=path)


@_reference(description="Import a STEP file's bodies as an Import feature.")
def import_step(path: str) -> dict:
    return _call("import_step", path=path)


@_reference(description="Save the WHOLE project and name this state a "
          "version (save_project to a .dsgn project first). Name a version "
          "before risky multi-step edits; restoring beats reconstructing.")
def checkpoint_create(name: str) -> dict:
    return _call("checkpoint_create", name=name)


@_reference(description="List the project's named versions (name + unix "
          "time, newest first).")
def checkpoint_list() -> dict:
    return _call("checkpoint_list")


@_reference(description="Put the WHOLE project back to a named version and "
          "reopen it. The restore is itself recorded, so the state you left "
          "stays in history_log.")
def checkpoint_restore(name: str) -> dict:
    return _call("checkpoint_restore", name=name)


@_reference(description="History of a directory (.dsgn) project: every save "
          "is a version, named ones are tagged. Returns {log: [{hash, "
          "subject, unix, named}]}, newest first.")
def history_log(limit: int = 20) -> dict:
    return _call("history_log", limit=limit)


@_reference(description="Put the WHOLE project back to a version (hash, tag "
          "like 'cp/v1', or any git rev) and reopen it. History stays linear "
          "— the restore is recorded on top, so nothing is lost.")
def history_restore(commit: str) -> dict:
    return _call("history_restore", commit=commit)


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


@_reference(description="Frame the whole model at the current orientation.")
def fit_view() -> dict:
    return _call("fit_view")


@_reference(description="Read the user's preferences: GPU device, font size, "
          "shadows/SSAO/shadow map, autosave interval. Also returns `path` "
          "(the settings file) and `devices` (what gpu_device may be set to). "
          "These are per-USER and persist; a project's own settings do not "
          "live here.")
def get_settings() -> dict:
    return _call("get_settings")


@_reference(description="Change user preferences, e.g. "
          "{\"ssao\": \"full\", \"ui_font_scale\": 1.2}. Validates first "
          "— a bad value changes nothing and names itself — then applies and "
          "writes the file. The viewport GPU needs a restart; everything else "
          "is immediate.")
def set_settings(**values) -> dict:
    return _call("set_settings", **values)


@mcp.tool(description="Viewport render-quality options; all fields optional, "
          "returns the resulting state (call with no arguments to just "
          "read). shadows: key-light shadow map on/off. ssao: 'off'|'half'|"
          "'full' — ambient occlusion off, half-res (default) or full-res. "
          "shadow_map: 1024|2048 texels. Useful for A/B-ing renders via "
          "screenshot. Needs a GUI instance (errors under --headless).")
def set_render_options(shadows: bool | None = None, ssao: str | None = None,
                       shadow_map: int | None = None) -> dict:
    params = {}
    if shadows is not None:
        params["shadows"] = shadows
    if ssao is not None:
        params["ssao"] = ssao
    if shadow_map is not None:
        params["shadow_map"] = shadow_map
    return _call("set_render_options", **params)


@_reference(description="Select a B-Rep face. Preferred: by STABLE element name "
          "(list_elements), e.g. element='2/g4' — survives regeneration. "
          "Legacy: by render mesh id + face index (get_state bodies[].mesh; "
          "ids change every regeneration). Returns the face normal and "
          "center. Follow with orient_to_face to look straight at it.")
def select_face(mesh: int = -1, face: int = -1, element: str = "") -> dict:
    if element:
        return _call("select_face", element=element)
    return _call("select_face", mesh=mesh, face=face)


@mcp.tool(description="Direct camera control (view/fit only snap to standard "
          "views): optional target [x,y,z], direction [x,y,z] (unit offset "
          "from target toward the camera), up [x,y,z], and distance (mm, "
          "zoom). Each field optional, applied in that order. Returns the "
          "resulting position/target/distance.")
def set_camera(target: list | None = None, direction: list | None = None,
               up: list | None = None, distance: float | None = None) -> dict:
    params = {}
    if target is not None:
        params["target"] = target
    if direction is not None:
        params["direction"] = direction
    if up is not None:
        params["up"] = up
    if distance is not None:
        params["distance"] = distance
    return _call("set_camera", **params)


@_reference(description="Rebuild the default panel layout (Model left, "
          "Properties right, 3D center) — recovery for a wedged or "
          "misdocked panel arrangement, without restarting the app.")
def reset_layout() -> dict:
    return _call("reset_layout")


@_reference(description="Orient the camera to look squarely at the selected "
          "face (select_face first).")
def orient_to_face() -> dict:
    return _call("orient_to_face")


@mcp.tool(description="Offline path-traced render (soft shadows, materials, "
          "environment lighting) returned as an image. Works on part AND "
          "assembly tabs (occurrences render at their solved placements). "
          "Frame with view/fit_view first. The app freezes for the duration "
          "— samples=64 is a fast preview, 512+ presentation quality. ground "
          "adds a shadow-catcher plane. Requires a ray-tracing GPU (errors "
          "otherwise). Optional path also keeps the PNG.")
def render_still(width: int = 640, height: int = 480, samples: int = 64,
                 seed: int = 1, ground: bool = True, path: str = "") -> Image:
    out = path or os.path.join(tempfile.gettempdir(), "designer_mcp_still.png")
    r = _call("render_still", width=width, height=height, samples=samples,
              seed=seed, ground=ground, path=out)
    if not r.get("ok"):
        raise RuntimeError("render failed: " + str(r.get("error", "unknown")))
    return Image(path=out)


# ---------------------------------------------------------------------------
# Simulation (FEM stress analysis)
# ---------------------------------------------------------------------------
# A Study is a document-level linear-static analysis: fix faces, load faces,
# solve for von Mises stress + displacement. Faces are {feature_id, element}
# refs from list_elements (feature_id = the body's created_by). Assign a real
# material first (set_material, e.g. "steel") — a study on the default material
# has no stiffness data and errors. Magnitudes are N (force) / MPa (pressure);
# mesh sizes are mm (0 = auto). Results are derived and go stale on any edit;
# re-run explicitly. The whole loop is headless: no GUI needed.


@_reference(description="Create a FEM study on the active part. kind = "
          "'static' (default) | 'modal' | 'topology'. Returns {id, name, "
          "kind}. A study fixes/loads faces and solves for stress.")
def add_study(name: str = "", kind: str = "") -> dict:
    params = {}
    if name:
        params["name"] = name
    if kind:
        params["kind"] = kind
    return _call("add_study", **params)


@_reference(description="Delete a study (by id) and its cached results.")
def remove_study(study: int) -> dict:
    return _call("remove_study", study=study)


@_reference(description="List studies: id, name, evaluated mesh_size, loads, "
          "refinement count, has_results, and stale (results outdated by an "
          "edit).")
def list_studies() -> dict:
    return _call("list_studies")


@_reference(description="Set a study's name, global mesh size, analysis type, or "
          "loads. mesh_size (mm; 0 = auto) may be a number or an expression "
          "string. type is 'static' (stress), 'modal' (natural frequencies), or "
          "'topology' (generative shape). num_modes sets how many modes a modal "
          "study computes. gravity ([x,y,z] m/s^2, e.g. [0,0,-9.81]) applies "
          "self-weight to a static study (needs a material density). For a "
          "topology study: volume_fraction (0..1 target kept, number or "
          "expression), filter_radius (mm minimum member size; 0 = auto), and "
          "max_iterations.")
def set_study(study: int, name: str | None = None, mesh_size=None,
              type: str | None = None, num_modes: int | None = None,
              gravity: list | None = None, volume_fraction=None,
              filter_radius=None, max_iterations: int | None = None) -> dict:
    params: dict = {"study": study}
    for k, v in (("name", name), ("mesh_size", mesh_size), ("type", type),
                 ("num_modes", num_modes), ("gravity", gravity),
                 ("volume_fraction", volume_fraction),
                 ("filter_radius", filter_radius),
                 ("max_iterations", max_iterations)):
        if v is not None:
            params[k] = v
    return _call("set_study", **params)


@_reference(description="Add a KEEP FACE to a topology study (frozen solid, so a "
          "bolt boss or bearing seat survives the optimization). face is "
          "{feature_id, element}.")
def add_keep_face(study: int, face: dict) -> dict:
    return _call("add_keep_face", study=study, face=face)


@_reference(description="Remove a keep face ({feature_id, element}) from a "
          "topology study.")
def remove_keep_face(study: int, face: dict) -> dict:
    return _call("remove_keep_face", study=study, face=face)


@_reference(description="Add a per-face mesh refinement (smaller elements on one "
          "face). face is {feature_id, element}; size is mm (number or "
          "expression).")
def add_refinement(study: int, face: dict, size) -> dict:
    return _call("add_refinement", study=study, face=face, size=size)


@_reference(description="Remove the refinement on a face ({feature_id, element}).")
def remove_refinement(study: int, face: dict) -> dict:
    return _call("remove_refinement", study=study, face=face)


@_reference(description="Add a boundary condition to a study. type is 'fixed' "
          "(zero displacement), 'force' (a total vector in N over the face), "
          "or 'pressure' (MPa along the inward normal). face is {feature_id, "
          "element}. magnitude is N/MPa (number or expression); direction is "
          "[x,y,z] for a force. Returns {id} of the new load. Every loaded "
          "body needs at least one fixed face.")
def add_load(study: int, type: str, face: dict, magnitude=None,
             direction: list | None = None) -> dict:
    params: dict = {"study": study, "type": type, "face": face}
    if magnitude is not None:
        params["magnitude"] = magnitude
    if direction is not None:
        params["direction"] = direction
    return _call("add_load", **params)


@_reference(description="Remove a load (by id) from a study.")
def remove_load(study: int, load: int) -> dict:
    return _call("remove_load", study=study, load=load)


@_reference(description="Mesh a study's bodies without solving (a quick check). "
          "Returns per-body node/element counts and worst element quality.")
def mesh_study(study: int) -> dict:
    return _call("mesh_study", study=study)


@_reference(description="Mesh + solve a study now (explicit; never automatic). "
          "A static study returns nodes, elements, max_von_mises_mpa {value, "
          "at, body}, max_displacement_mm, factor_of_safety (vs yield), and "
          "stale:false. A modal study returns per-body frequencies_hz (the "
          "natural frequencies). A TOPOLOGY study runs ASYNC (minutes on a "
          "worker thread): it returns {started:true} immediately — poll "
          "study_status until running:false, then read study_results. "
          "async_run=True runs a static/modal study the same way — use it "
          "for big meshes (a synchronous solve can outlive the client "
          "timeout; note a running static solve cannot be cancelled). "
          "Errors name the offending face/material/support.")
def run_study(study: int, async_run: bool = False) -> dict:
    if async_run:
        return _call("run_study", study=study, **{"async": True})
    return _call("run_study", study=study)


@_reference(description="Poll an async topology run. While running, returns "
          "{running:true, iteration, max_iterations, compliance, "
          "volume_fraction}. Once done, {running:false, has_results, converged, "
          "cancelled, iterations, stale}.")
def study_status(study: int) -> dict:
    return _call("study_status", study=study)


@_reference(description="Cancel an in-progress topology run (keeps the best "
          "shape found so far as a 'cancelled' result). Returns {cancelled}.")
def cancel_study(study: int) -> dict:
    return _call("cancel_study", study=study)


@_reference(description="Read a study's cached results (never solves). Same "
          "summary as run_study plus the current stale flag. For topology: "
          "iterations, converged, volume_fraction, compliance.")
def study_results(study: int) -> dict:
    return _call("study_results", study=study)


@_reference(description="Export a topology study's optimized shape (the "
          "iso-surface, Taubin-smoothed) to a binary STL for 3D printing. "
          "threshold (0..1, 0 = auto volume-matched) sets how much material to "
          "keep; smooth is the number of smoothing passes (default 3). The "
          "reply reports {triangles, boundary_edges, watertight} — flag a "
          "non-watertight mesh before anyone slices it. Note: the organic STL "
          "cannot be re-imported as a B-Rep; view results via "
          "set_result_display field='shape'.")
def export_topo_stl(study: int, path: str, threshold: float = 0.0,
                    smooth: int = 3) -> dict:
    return _call("export_topo_stl", study=study, path=path,
                 threshold=threshold, smooth=smooth)


@_reference(description="Toggle the topology SHAPE GUIDE: a translucent overlay "
          "of a study's optimized shape in the normal modeling workspace, to "
          "re-model clean geometry against. on=false hides it. The opaque "
          "body can hide the guide from most angles — to LOOK at a result, "
          "set_result_display field='shape' is the reliable view.")
def set_topo_ghost(study: int, on: bool = True) -> dict:
    return _call("set_topo_ghost", study=study, on=on)


@_reference(description="Show a study's result overlay in the viewport (for a "
          "screenshot). field is 'von_mises', 'displacement', or 'none' for a "
          "static/modal study, or 'shape' (grey optimized iso-surface) / "
          "'density' (colormap) for a topology study. deform_scale exaggerates "
          "the deformed shape (0 = undeformed). For a modal study, mode selects "
          "which mode shape (0-based). For a topology 'shape' view, threshold "
          "(0..1, 0 = auto) sets the iso-surface level. Enters simulation mode; "
          "display-only, like set_explode.")
def set_result_display(study: int | None = None, field: str = "von_mises",
                       deform_scale: float | None = None,
                       mode: int | None = None,
                       threshold: float | None = None) -> dict:
    params: dict = {"field": field}
    if study is not None:
        params["study"] = study
    if deform_scale is not None:
        params["deform_scale"] = deform_scale
    if mode is not None:
        params["mode"] = mode
    if threshold is not None:
        params["threshold"] = threshold
    return _call("set_result_display", **params)


# ---------------------------------------------------------------------------
# Drawings (workstream 13): a drawing tab holds a 2D sheet with projected views
# of a part or assembly. list_parts reports drawing tabs with type "drawing".
# ---------------------------------------------------------------------------

@_reference(description="Add a new DRAWING tab (a 2D sheet) and switch to it. "
          "Optional sheet = {size: 'a4'|'a3'|'a2'|'a1'|'a0'|'letter', "
          "landscape: bool, title_block: {project, document, author, date}}. "
          "Views and dimensions are added in later tools. Returns its index, "
          "stable id, and sheet size.")
def create_drawing(name: str = "", sheet: dict | None = None) -> dict:
    params = {}
    if name:
        params["name"] = name
    if sheet is not None:
        params["sheet"] = sheet
    return _call("create_drawing", **params)


@_reference(description="List the project's drawing tabs with each one's stable "
          "id, index, and view/dimension counts.")
def list_drawings() -> dict:
    return _call("list_drawings")


@_reference(description="Add a projected VIEW of a part to the ACTIVE drawing. "
          "source = the part's name. orientation = 'front'|'back'|'left'|"
          "'right'|'top'|'bottom'|'iso' (default front). scale = drawing:model "
          "ratio, e.g. 0.5 for 1:2 (default 1.0). position = [x,y] sheet mm "
          "(default sheet centre). base_view = a view id to fold this one from "
          "(D2b). The view is added but NOT projected — call update_drawing. "
          "Returns the new view id.")
def add_drawing_view(source: str, orientation: str = "", scale: float = 1.0,
                     position: list | None = None, name: str = "",
                     base_view: int = 0) -> dict:
    params = {"source": source, "scale": scale}
    if orientation:
        params["orientation"] = orientation
    if position is not None:
        params["position"] = position
    if name:
        params["name"] = name
    if base_view:
        params["base_view"] = base_view
    return _call("add_drawing_view", **params)


@_reference(description="Remove a view (by id) from the active drawing.")
def remove_drawing_view(id: int) -> dict:
    return _call("remove_drawing_view", id=id)


@_reference(description="Edit a view in place: position [x,y] sheet mm, scale, "
          "name, show_hidden, base_view (0 clears folding). Re-projects. A "
          "folded child's shared axis follows its base — the reply's "
          "effective_position/folded_axes say when a requested coordinate "
          "was overridden.")
def set_drawing_view(id: int, position: list | None = None, scale: float = 0,
                     name: str = "", show_hidden: int = -1,
                     base_view: int = -1) -> dict:
    params = {"id": id}
    if position is not None:
        params["position"] = position
    if scale > 0:
        params["scale"] = scale
    if name:
        params["name"] = name
    if show_hidden >= 0:
        params["show_hidden"] = bool(show_hidden)
    if base_view >= 0:
        params["base_view"] = base_view
    return _call("set_drawing_view", **params)


@_reference(description="Edit the active drawing's sheet after creation: size "
          "('a4'..'a0'|'letter', case-insensitive — unknown sizes ERROR), "
          "landscape, title_block fields. Views keep their positions; run "
          "check_drawing to see what no longer fits.")
def set_drawing_sheet(size: str = "", landscape: int = -1,
                      title_block: dict | None = None) -> dict:
    params = {}
    if size:
        params["size"] = size
    if landscape >= 0:
        params["landscape"] = bool(landscape)
    if title_block is not None:
        params["title_block"] = title_block
    return _call("set_drawing_sheet", **params)


@_reference(description="Validate the active drawing's sheet layout — the "
          "checks a drafter does by eye: every view inside the border, no "
          "view/view or view/title-block overlaps, dimensions on the paper, "
          "nothing dangling or stale. Returns {ok, issues:[{kind, detail, "
          "view?, other_view?, dim?}]}. update=False judges the cached "
          "projection instead of re-projecting. Run before export_drawing "
          "(whose reply echoes the same issues).")
def check_drawing(update: bool = True) -> dict:
    return _call("check_drawing", update=update)


@_reference(description="Re-project every view of the active drawing (the "
          "explicit hidden-line-removal pass — expensive, never automatic). "
          "Returns per-view curve counts (visible/hidden/silhouettes) and any "
          "projection error. Run after adding views or editing the source part.")
def update_drawing() -> dict:
    return _call("update_drawing")


@_reference(description="List the active drawing's views with their orientation, "
          "scale, position, projected curve count, and whether each is stale "
          "(source changed since last projection).")
def list_drawing_views() -> dict:
    return _call("list_drawing_views")


@_reference(description="Export the active drawing to a vector file (path ending "
          ".svg or .pdf). Re-projects first so the file reflects the current "
          "geometry, then writes the sheet — border, title block, every view's "
          "visible (solid)/hidden (dashed) edges, and each resolved dimension. "
          "Returns the path, view count, and the check_drawing issue list "
          "(export never blocks on issues — read them).")
def export_drawing(path: str) -> dict:
    return _call("export_drawing", path=path)


@_reference(description="List a projected view's dimensionable geometry: each "
          "distinct source edge / silhouette with the `ref` to pass to "
          "add_drawing_dim, its type (line/circle/curve), sheet-mm endpoints "
          "or circle centre+radius, `visible`, `closed` (a full circle = a "
          "bore rim; false = an arc such as a corner fillet — do NOT put a "
          "diameter dim on those), and `source_feature` {type,name} (part "
          "sources). Call update_drawing first.")
def list_drawing_geometry(view: int) -> dict:
    return _call("list_drawing_geometry", view=view)


@_reference(description="Add a driven dimension to a view of the active drawing. "
          "kind = 'distance'|'distance_x'|'distance_y'|'radius'|'diameter'|"
          "'angle'|'hole_callout'|'pattern_callout'. refs = a list of {edge OR "
          "sil_face: {feature_id, element} (from list_drawing_geometry), "
          "occurrence?: source_id, end?: 0 whole / 1|2 endpoint}: two refs for "
          "distance/angle, one for radius/diameter, ONE closed circle of a "
          "drilled hole for hole_callout ('Ø4.5 THRU', 'Ø4.5 CSK Ø9.5 × 90°' "
          "— text derived from the Hole feature; part sources only), 2+ "
          "equal-radius circles for pattern_callout ('3× Ø4.5', '+ PITCH p' "
          "when collinear and evenly spaced). offset = [x,y] label offset in "
          "sheet mm; OMIT it to auto-place the dim clear of the view. The "
          "value is MEASURED from the projected geometry (read-only). Returns "
          "the id, measured value/text, and the stored offset.")
def add_drawing_dim(view: int, kind: str, refs: list,
                    offset: list | None = None) -> dict:
    params = {"view": view, "kind": kind, "refs": refs}
    if offset is not None:
        params["offset"] = offset
    return _call("add_drawing_dim", **params)


@_reference(description="Remove a dimension (by id) from the active drawing.")
def remove_drawing_dim(id: int) -> dict:
    return _call("remove_drawing_dim", id=id)


@_reference(description="List the active drawing's dimensions with each one's "
          "kind, measured value + formatted text, and whether it resolved "
          "(false = a ref dangles after a source edit).")
def list_drawing_dims() -> dict:
    return _call("list_drawing_dims")


# ---------------------------------------------------------------------------
# Escape hatch: raw UI events (for driving/testing the Designer GUI itself)
# ---------------------------------------------------------------------------

_UI_ACTIONS = ("menu_click", "item_click", "item_double_click", "item_input",
               "item_hover", "item_combo", "ui_items", "viewport_click",
               "viewport_drag", "key_press", "scroll")


@_reference(description="Raw GUI event, for driving or testing the Designer "
          "GUI itself — for modeling, use the semantic tools instead. "
          "A `ref` is a test-engine path: '//WindowName/**/###stable_id' "
          "(** = any depth; ### ids are stable across labels/icons); "
          "ui_items lists what is actually there. "
          "Widgets with hidden labels (combos) have no path — "
          "address them by the {id} ui_items reports. "
          "action + params: menu_click {path: 'Edit/Undo'}; item_click "
          "{ref|id, button?: 1 = right-click}; item_double_click {ref|id} "
          "(e.g. a tree sketch → edit mode); item_input {ref|id, value}; "
          "item_combo {ref|id, value: 'Countersink'}; item_hover {ref|id}; "
          "ui_items {ref?, depth?} → {items: [{label, id, window, "
          "disabled, x, y}]}; viewport_click {x, y (0-1 fractions OF THE 3D "
          "VIEWPORT), button?: 0/1/2, ctrl?}; viewport_drag {x0, y0, x1, "
          "y1, button?: 2=MMB orbit, ctrl?: pan, shift?: zoom}; key_press "
          "{chord: 'Ctrl+Z'}; scroll {x, y, amount}.")
def ui_action(action: str, params: dict | None = None) -> dict:
    if action not in _UI_ACTIONS:
        raise ValueError(f"unknown action {action!r}; one of {_UI_ACTIONS}")
    return _call(action, **(params or {}))




# ---------------------------------------------------------------------------
# Meta: reach everything, and read the manual for it
# ---------------------------------------------------------------------------

@mcp.tool(description="Call any Designer RPC by name — the whole API, not "
          "just the tools listed here. Assemblies, drawings, studies, "
          "configurations, sketch constraints, patterns, checkpoints, "
          "exports and the GUI-driving actions are all reached this way: "
          "rpc('add_mate', {...}). Use describe() for the method list and "
          "help('add_mate') for its arguments and gotchas. Prefer this over "
          "guessing; a wrong method name returns the list of close matches.")
def rpc(method: str, params: dict | None = None) -> dict:
    entry = _REFERENCE.get(method)
    if entry is not None:
        # Route through the wrapper so its defaults and argument shaping
        # still apply — rpc() is a smaller door to the same room.
        return entry["fn"](**(params or {}))
    known = sorted(_REFERENCE) + sorted(_EXPOSED)
    if method not in known:
        import difflib
        near = difflib.get_close_matches(method, known, n=5)
        hint = f" Did you mean: {', '.join(near)}?" if near else ""
        # Not a hard error: the app serves RPCs this file has never heard of
        # (it is the older half of the pair). Try it, and only complain if
        # the app disagrees.
        try:
            return _call(method, **(params or {}))
        except Exception as e:
            raise RuntimeError(f"{e}{hint}") from e
    return _call(method, **(params or {}))


@mcp.tool(description="Which Designer instances are running, and which one "
          "these tools are talking to. Instances announce themselves when "
          "they open a remote port, so no port has to be guessed. Use this "
          "when a call reports several instances (set DESIGNER_PORT to "
          "choose) or nothing reachable.")
def instances() -> dict:
    live = _running_instances()
    return {"attached": attached_to(),
            "instances": live,
            "hint": ("Start one with `designer --remote 8437`."
                     if not live else
                     "Pin one with DESIGNER_PORT=<port> if several are up.")}


@mcp.tool(description="Documentation for one RPC: what it does, its "
          "arguments, and the traps. Call with no argument for the index of "
          "everything reachable through rpc(). This is the prose that used "
          "to be preloaded as 118 tool descriptions — same words, fetched "
          "when they matter.")
def help(method: str = "") -> dict:
    if not method:
        return {"methods": sorted(_REFERENCE),
                "exposed_as_tools": sorted(_EXPOSED),
                "hint": "help('add_mate') for one; describe() for the "
                        "app's own authoritative type tables."}
    entry = _REFERENCE.get(method)
    if entry is None:
        if method in _EXPOSED:
            return {"method": method,
                    "doc": "Exposed directly as the MCP tool of the same "
                           "name — see its tool description."}
        import difflib
        near = difflib.get_close_matches(method, sorted(_REFERENCE), n=5)
        raise ValueError(f"no such method {method!r}" +
                         (f"; close: {', '.join(near)}" if near else ""))
    import inspect
    return {"method": method,
            "doc": entry["doc"],
            "arguments": str(inspect.signature(entry["fn"]))}

@mcp.prompt(name="modeling-workflow",
            description="How to model a part in Designer with these tools")
def modeling_workflow() -> str:
    return WORKFLOW


def main():
    global _port
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=None,
                        help="pin a port; omit to attach to the running "
                             "instance that announced itself")
    args = parser.parse_args()
    if args.port is not None:
        _port = args.port
    mcp.run()  # stdio


if __name__ == "__main__":
    main()
