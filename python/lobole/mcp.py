#!/usr/bin/env python3
# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

"""MCP server for Loböle — drive a running instance over its JSON-RPC remote.

Design: docs/scripting/mcp.md. Register this server with an MCP client:

    claude mcp add lobole -- lobole-mcp      # installed (uv tool install .)
    claude mcp add lobole -- env PYTHONPATH=python python3 -m lobole.mcp
                                             # from a checkout

No port needed: running instances announce themselves and the server attaches
(several running → it asks; pin with --port or $LOBOLE_PORT, flag wins).
With NOTHING running it autostarts `lobole --offscreen --remote 0` — a real
renderer, no window — and kills it on exit; disable with --no-autostart or
LOBOLE_AUTOSTART=0. Requires the official MCP Python SDK (`mcp` on PyPI,
`python3Packages.mcp` on nixpkgs).
"""
import argparse
import os
import sys
import tempfile

# The wheel depends on the SDK, but the repo's own type check also runs where
# it is absent (CI's apt mypy): ignored there, harmless where it is installed.
from mcp.server.mcpserver import MCPServer, Image  # type: ignore[import-not-found,unused-ignore]
from mcp.server.mcpserver.exceptions import (  # type: ignore[import-not-found,unused-ignore]
    ToolError, UnexpectedToolError)

from lobole.client import Client  # noqa: E402

# ---------------------------------------------------------------------------
# Format reference, embedded into tool descriptions. Source of truth:
# src/core/io/SerializeFeatures.cpp (FeatureFromJson / EntityFromJson) and
# RemoteModeling.cpp (ConstraintKindTable). The describe RPC serves the
# authoritative type lists; mcp_formats_gate cross-checks this text against it, so
# a new feature type that isn't documented here fails the suite.
# ---------------------------------------------------------------------------

AXIS_REF = """An axis reference is {"kind": "x"|"y"|"z"} (world axes through
the world origin), {"kind": "sketch_line", "sketch_id": <feature id>,
"entity_id": <line id>}, {"kind": "datum_axis", "datum_axis_id": <id>}, or
{"kind": "custom", "origin": [x,y,z], "direction": [x,y,z]} (mm; any axis,
written out: the one for a turned feature off the origin)."""

FEATURE_FORMATS = f"""Feature JSON formats (field: value type; ? = optional):

A dimension "x" below takes an "x_expr"?: str twin -- an expression over
the document's variables that drives it on every regeneration (the literal
is then only its last value). describe's feature_types carry each type's
typed `contract`: every field, its type and its enum values.

- Box / Cylinder / Sphere — a primitive is for a fixture, a stand-in or a
  first rough block, and is always placed ON something; it is stored as a
  dimensioned Sketch on that thing plus an Extrude (Sphere: a Revolve), one
  undo step, and follows it. "on" is "Top" | "Front" | "Right", a planar
  face {{"feature_id": id, "element": name}}, or {{"feature_id": id}} of a
  DatumPlane, a Sketch or a Frame. No world position: off those, put it on a
  DatumPlane or a Frame placed there. "offset"?: [u, v] mm in that frame
  (a face's frame: origin its centroid). Sizes default from what it is on.
  Returns "ids", the solid's first.
  {{"type": "Box", "name": str, "on": <on>, "width"?: num (u),
    "depth"?: num (v), "height"?: num (along the normal), "offset"?: [u,v],
    "centered"?: bool (default true; false = offset is the low corner),
    "mode"?: "new_body"|"add"|"cut", "reversed"?: bool}}
  {{"type": "Cylinder", "name": str, "on": <on>, "diameter"?: num (or
    "radius"), "height"?: num, "offset"?: [u,v], "mode"?, "reversed"?}}
  {{"type": "Sphere", "name": str, "on": <on>, "diameter"?: num (or
    "radius"), "offset"?: [u,v] (its centre, ON the plane), "mode"?}}
- Sketch: {{"type": "Sketch", "name": str,
  "plane": {{"origin": [x,y,z], "normal": [x,y,z], "uAxis": [x,y,z]}},
  "plane_ref"?: <the id of a DatumPlane, another Sketch or a Frame (its XY
  plane, X as u) — follows it instead of baking>,
  "face_ref"?: {{"feature_id": id, "element": name}} (a planar body face —
  the sketch takes its frame and follows it),
  "entities": [<entity>, ...], "constraints"?: [<constraint>, ...] (as
  add_constraint takes them, solved with the sketch), "external"?:
  [{{"feature_id", "element", "kind": "face"|"edge"}}] (live projected
  construction geometry of another body, as project_geometry adds) or
  {{"kind": "section", "fit"?: "polyline"|"arcs"|"spline", "bodies"?: [ids],
  "tolerance_mm"?}} (where the sketch's plane cuts the bodies -- a scan's
  outline -- as project_section adds)}} .
  Entity formats (2D UV coords on the plane):
    {{"type": "point", "p": [u,v]}}
    {{"type": "line", "a": [u,v], "b": [u,v]}}
    {{"type": "circle", "center": [u,v], "radius": num}}
    {{"type": "arc", "center": [u,v], "radius": num,
      "start_angle": rad, "end_angle": rad}}
    {{"type": "ellipse", "center": [u,v], "r_major": num, "r_minor": num,
      "rotation": rad}} — r_major >= r_minor. Constraint refs: [id, 0] the
      centre, [id, 3]/[id, 4] the major-axis ends, [id, 5]/[id, 6] the minor;
      a radius is a "distance" from [id, 0] to [id, 3] or [id, 5]
    {{"type": "ellipse_arc", "center": [u,v], "r_major": num, "r_minor": num,
      "rotation": rad, "start_param": rad, "end_param": rad}} — [id, 1]/[id, 2]
      its ends, 3..6 its axis ends as for ellipse
    {{"type": "spline", "poles": [[u,v], ...], "degree"?: int (default 3)}}
      — [id, k] is pole k; the first and last poles are its ends
    {{"type": "text", "text": str (one line), "height": CAP height mm,
      "anchor": [u,v], "font"?: "sans" (outline, default) | "hershey"
      (single stroke: open curves for engraving, NO profile), "angle_deg"?:
      deg, "halign"?: left|center|right, "valign"?: baseline|bottom|middle|top,
      "spacing"?: extra mm between characters}} — one entity; its glyphs
      are regenerated, never sent. Extruding it cuts/adds every letter (one
      region = the whole label); its faces are "<feature>/g<id>.<glyph>...".
      An unknown font or a character the font lacks is refused by name.
    {{"type": "rect", "c1": [u,v], "c2": [u,v]}} — a convenience that
      expands to FOUR "line" entities (+ corner coincident/H/V constraints);
      the 4 ids land at the END of entity_ids, after every non-rect
      entity's, in rect order. Corner-fillet the expanded lines like any
      other adjacent pair.
  Any entity may set "construction": true (reference geometry, not profiled).
  Entity coordinates can be EXPRESSION-DRIVEN via "<field>_expr" keys
  evaluated against document variables on every regeneration: line "ax_expr"/
  "ay_expr"/"bx_expr"/"by_expr", circle+arc "cx_expr"/"cy_expr"/"r_expr",
  point "x_expr"/"y_expr", text "x_expr"/"y_expr"/"height_expr"/
  "angle_deg_expr"/"spacing_expr" (e.g. {{"type": "circle", "center": [0, 0],
  "radius": 5, "r_expr": "bore / 2"}}). Driven coordinates are pinned during
  the sketch solve (shown violet, like linked copies) — don't also dimension
  them. Often a distance/radius constraint with "expr" is the better tool;
  entity exprs shine for direct analytic placement (lobe centers, cam
  profiles).
  Do NOT send entity ids — the app assigns them; add_feature returns the
  new feature's id as "id", plus "entity_ids" in list order (except rect
  expansions — see above). Use those ids for constraints and ops.
- Extrude: {{"type": "Extrude", "name": str, "sketch_id": <feature id>,
  "distance": num, "distance_expr"?: str, "reversed"?: bool,
  "mode": "new_body"|"add"|"cut",
  "end_mode"?: "symmetric" (centered on the plane) | "two_sided"
  (+ "distance2"/"distance2_expr" backward) | "up_to_face"
  (+ "end_target": {{"feature_id", "element"}} — a planar face from
  list_elements; follows upstream edits) | "through_all" (cut only:
  sweeps past every body, distance ignored),
  "thin"?: {{"thickness": num, "thickness_expr"?: str,
  "side": "out"|"in"|"both"}} — walls every loop instead of filling,
  "twist_deg"?: num, "twist_expr"?: str — degrees the profile turns about
  the normal through the SKETCH ORIGIN over the length, right-handed about
  the direction of travel (a helical gear: tan(helix)/r_pitch radians per
  mm times the length); blind or symmetric only, never thin,
  "herringbone"?: bool — climb the first half at that rate and back,
  "taper_deg"?: num, "taper_expr"?: str — every wall leans IN that many
  degrees over the length (negative: out); blind only, not thin or
  twisted; refused past the angle that closes the profile}}.
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
  A mesh operand (an imported mesh body, a Cradle's result) runs on
  triangles: both must be closed; a B-Rep operand is tessellated at a
  deflection derived from its size (the reply's report says it), and the
  result is a mesh body.
- Cradle: {{"type": "Cradle", "name": str, "target_id": <the block's
  feature id>, "tool_id": <the scan's>, "clearance_mm"?: number (unset = the
  print process's gap a side; refused without a process),
  "direction"?: [x, y, z] (the way the object moves in, default [0, 0, -1]),
  "sweep"?: bool (default true: no undercuts, it lifts out; false keeps
  them, a snap-in grip), "resolution_mm"?: number (default half the
  clearance)}} — a holder shaped to a scan, which may be open. The block
  becomes a mesh body, so do fillets and bosses BEFORE it. Builds off the
  frame; refused past its cost budget naming the resolution or triangle
  count that fits; the reply's report gives the resolution and deviation.
- Pattern: {{"type": "Pattern", "name": str,
  "kind": "mirror"|"linear"|"circular", "source_id": <feature id>, ...}}
  source either CREATES a new body (Box, Revolve, a new-body Extrude, ...) —
  the bodies are replicated — or is a cut or add Extrude or a Hole, whose
  tool is replicated and each copy cut or fused, so a hole patterns as a
  hole.
  mirror: "parent_id"?: <DatumPlane id> or "base_plane": "xy"|"xz"|"yz"
  linear: "axis": <axis ref>, "count": int, "count_expr"?: str,
          "spacing": num, "spacing_expr"?: str
  circular: "axis": <axis ref>, "count": int, "count_expr"?: str,
            "angle_deg": num, "angle_expr"?: str
- DatumPlane: {{"type": "DatumPlane", "name": str,
  "kind": "offset"|"angled"|"through_points", ...}}
  offset/angled parent: "parent_id"?: <DatumPlane id> or
  "base_plane": "xy"|"xz"|"yz" (Z is up: xy = Top, xz = Front, yz = Right)
  offset: "offset": num, "offset_expr"?: str, "flip"?: bool
  angled: "axis": <axis ref>, "angle_deg": num, "angle_expr"?: str
  through_points: "points": [[x,y,z],[x,y,z],[x,y,z]]
- Frame (a NAMED datum frame — origin + axes — that a mate anchors to by
  name and the `frame` query reports; the way to position without
  transcribing a coordinate): {{"type": "Frame", "name": str, "kind":
  "explicit"|"from_element"|"offset", ...}}
  explicit: "origin": [x,y,z], "origin_expr"?: [sx,sy,sz],
            "z_axis"?: [x,y,z], "x_axis"?: [x,y,z]
  from_element: "elements": [{{"feature_id": <body's created_by>,
            "element": <name from list_elements>}}, ...] — ONE planar face
            gives its centroid + outward normal, one cylindrical face or
            circular edge its centre + axis, and SEVERAL circular ones a
            BOLT CIRCLE (centred on the pattern, X toward the first named);
            "flip"?: true negates Z (a face's normal points out of the part)
  offset: "parent_id": <a Frame; 0 = the part origin>, "offset": [x,y,z]
            (+"offset_expr"), "rotation_deg": [rx,ry,rz] (+"rotation_expr")
  Mate to one with add_mate a/b = {{"occurrence": <id|name>,
  "frame": <Frame id or name>}}; then moving the geometry the frame was
  built from moves the mate with it.
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
- Hole (drill holes at a sketch's Point entities and at its circles' centres
  — so external geometry of another body's hole edge places a hole with no
  coordinate copied — cut into the bodies below along the sketch plane's
  -normal): {{"type": "Hole", "name": str,
  "sketch_id": <a sketch whose points/circles are the hole centers>,
  "hole_kind": "simple"|"counterbore"|"countersink", "diameter": num,
  "diameter_expr"?: str, "depth": num, "depth_expr"?: str, "through"?: bool,
  counterbore: "cb_diameter"/"cb_depth", countersink: "cs_diameter"/
  "cs_depth", "fit"?: {{"thread": "M6", "kind": "close"|"normal"|"loose"|
  "tapped", "pitch"?}} (what the hole is FOR: it derives the diameter, and
  a tapped fit threads the bore so a screw in it engages),
  "drill_point_deg"?: a blind floor's point (absent: 118 with a
  fit, else flat), "thread_depth"?: a tapped fit's thread from the mouth
  (absent: the depth less 3 pitches), "printed"?: "teardrop"|
  "teardrop_flat" (a hole across the print direction on a printed part:
  its roof pointed at 45 degrees up the part's print direction, or flat
  at the process's bridge)}} — far cleaner than a construction circle + cut-extrude per
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
  "name": str, "kind": "fit_spline"|"control_poly"|"bent_path"|"helix",
  ...}}
  fit_spline/control_poly: "points": [[x,y,z], ...], "degree"?: int,
  "closed"?: bool
  bent_path (a bent tube's centreline): "points": [[x,y,z], ...] — the
  CORNERS, in order — and "radius": num (+"radius_expr"), the former it is
  bent round, to the centreline. Built line-arc-line, so the bend has a
  radius and an angle a bender can be given (tube_schedule)
  helix: "origin": [x,y,z], "axis": [x,y,z], "radius": num, "pitch": num,
  "turns": num (each with "_expr" twins), "left_handed"?: bool,
  "start_deg"?: num (+"_expr") — where it starts round the axis, right-handed
  from a reference: +X for a +Z axis, -X for -Z, +Z for +X or +Y, -Z for
  -X or -Y
- Pipe (tube along a path, no profile sketch needed): {{"type": "Pipe",
  "name": str, "path_id": <sketch/Curve3D id>, "outer_diameter": num,
  "wall_thickness": num (0 = solid rod), "outer_diameter_expr"?/
  "wall_thickness_expr"?: str, "mode": "new_body"|"add"|"cut"}}
- Cope (cut a frame member to fit what it MEETS, leaving those standing):
  {{"type": "Cope", "name": str, "member_id": <the member's Pipe id>,
  "against": [<ids whose SIDE it lands on — a fishmouth>], "mitre":
  [<ids it meets END TO END — cut on the plane bisecting the two, which is
  what a corner wants: there is no round side to fishmouth against>],
  "gap"?: num (+"gap_expr", mm; a cope opens on the radius, a mitre moves
  its plane back along the member)}}. Which cut a joint wants is TOPOLOGY
  the caller holds: members are drawn LONG past their nodes (a plane through
  the very end of a tube shaves a wedge off it instead of mitring it), and
  once they are, a corner and a tee look alike. Each neighbour cuts
  with its OUTER solid and SURVIVES — a Boolean consumes both operands, so
  coping with one needs a throwaway copy of the neighbour per joint, and
  every throwaway is a tube in the cut list nobody ordered. One Cope per
  member handles all of its joints. A cope that removes nothing is REFUSED:
  the two do not meet, so there is no joint there to cut
- Rib (thicken an open sketch chain into a web up to the body):
  {{"type": "Rib", "name": str, "sketch_id": <open-profile sketch id>,
  "thickness": num, "thickness_expr"?: str, "flip"?: bool,
  "body_id"?: <target body's created_by id>}}
- Worm (a ZA worm cut into a cylindrical blank; its outside is the tip):
  {{"type": "Worm", "name": str, "face": {{"feature_id", "element"}},
  "module"?: num (mm, axial; omitted = tip dia / 12), "starts"?: int,
  "pressure_angle_deg"?: num (20), "hand"?: "right"|"left",
  "backlash"?: num (mm axial, half off each flank), "length"?: num (mm)}}
  — the response's "report" gives the lead angle and self-locking.
- CamProfile (a disc cam, the mechanism, cut from a follower motion):
  {{"type": "CamProfile", "name": str, "face"?: {{"feature_id", "element"}}
  (a shaft's or hub's cylindrical outside: axis, base radius and width) |
  "frame"?: <Frame feature id> (then base_radius_mm and width_mm are
  required), "base_radius_mm"?: num, "width_mm"?: num, "segments"?:
  [{{"to_deg", "rise_mm"|"rise_deg", "law":
  "cycloidal"|"harmonic"|"poly345"|"dwell"|"drop"}}] closing at 360 |
  "lift"?: [[angle_deg, lift]], "follower"?: {{"kind":
  "roller"|"flat"|"knife", "motion": "slide"|"turn", "radius_mm",
  "offset_mm"? | "pivot_mm": [x, y] + "arm_mm"}}, "turn"?: "ccw"|"cw",
  "body"?: "new"|"add"}} — omitted motion = a cycloidal lobe rising half the
  base radius; undercut is refused naming the base radius that clears it;
  "report" gives the pressure angle, radii and fit deviation. A cam relation
  reads it with add_relation type "cam" {{profile: {{part, feature}}}}.
- WormWheel (hobbed from a cylindrical blank; the rim is the face and its
  length the face width): {{"type": "WormWheel", "name": str, "face":
  {{"feature_id", "element"}}, "teeth"?: int (40), "module"?: num (mm;
  omitted = rim dia / (teeth + 3)), "starts"?: int, "worm_diameter"?: num
  (mm, the worm's pitch diameter; omitted = 10 modules),
  "pressure_angle_deg"?: num, "hand"?: "right"|"left"}} — "report" gives
  the centre distance to mount the worm at. Couple the pair with
  add_relation type "worm" {{a: worm mate, b: wheel mate, starts, teeth,
  hand}}, not "gear": its sign depends on the hand and the side.
- Thread (cosmetic or modeled thread on a cylindrical face):
  {{"type": "Thread", "name": str, "face": {{"feature_id", "element"}},
  "designation"?: str (ISO "M8"/"M8x1" or unified "3/8-16 UNC", "#10-32
  UNF", optionally with a class suffix "-6g"/"-2B"; a series the tables
  lack is REFUSED, other text is a custom thread), "pitch": num (mm; a
  custom thread's, required; a table thread's is its row's, omit it or give
  exactly that -- another is REFUSED), "thread_class"?: str, "length"?: num (mm), "length_expr"?: str,
  "modeled"?: bool (true cuts the real 60-degree helical groove, slow;
  a failure keeps the body), "hand"?: "right"|"left",
  "flank_angle_deg"?: num (half the thread angle: 30 is the ISO/UTS
  60-degree profile, the only one a table designation takes; a custom
  thread 15-60), "allow_engagement"?: bool (a mating thread overlapping
  this one is an ENGAGED pair in contact checks, not a clash),
  "minimum_engagement"?: num (mm, the shortest engagement accepted)}}. It is also what a
  CAM job mills: rpc('add_cam_op', {{type: "thread_mill", target: the
  threaded face}}) with a thread_mill tool reads the size, pitch, hand,
  side and class from it (a Hole with a tapped fit carries one too), and
  cam_verify's `threads` judges the result.
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
- Direct edit — change a body's faces where they are, the way an imported
  part (no history) is changed. Each takes faces on ONE body:
  {{"feature_id": <the body's created_by>, "element": <face name from
  list_elements>}}, and a call whose edit cannot build is REFUSED with the
  face named, the document unchanged. grow_faces grows picks to the
  whole hole or boss they belong to.
  DeleteFace: {{"type": "DeleteFace", "name": str, "body_id": <created_by>,
  "faces": [...]}} — removed, the gap healed (a hole fills, a boss levels, a
  fillet gives back its edge).
  OffsetFace: {{"type": "OffsetFace", "name": str, "body_id", "faces":
  [...], "distance_mm": num (+ outward), "distance_mm_expr"?: str}} — a
  face beside a tangent fillet is offset with the fillet or not at all.
  MoveFace: {{"type": "MoveFace", "name": str, "body_id", "faces": [...],
  "translate_mm": [x, y, z], "translate_mm_expr"?: [str, str, str] ("" =
  none), "rotate_deg"?: num, "rotate_deg_expr"?: str, "axis"?: an axis as
  Revolve's}} — the faces of one hole/boss/pocket, moved rigidly.
  ReplaceFace: {{"type": "ReplaceFace", "name": str, "body_id", "face":
  {{...}}, "target": {{"kind": "plane", "base_plane": "xy"|"xz"|"yz"}} |
  {{"kind": "plane", "datum_id": id}} | {{"kind": "sketch", "sketch_id":
  id}} | {{"kind": "face", "face": {{...}}}}}} — the face put onto the
  target, its neighbours extended or trimmed to meet it.
- BaseFlange (the first wall of a SHEET part): {{"type": "BaseFlange",
  "name": str, "sketch_id": <a closed-profile sketch>, "reversed"?: bool,
  "regions"?: [[entity ids]]}} — the profile thickened by the document's
  sheet-metal thickness, so it takes none of its own. Call
  set_sheet_metal_rules (material + gauge) first or it REFUSES: a blank is
  cut from one sheet, so a flange has no thickness to guess.
- EdgeFlange (bend a wall up from a sheet edge): {{"type": "EdgeFlange",
  "name": str, "edges": [{{"feature_id": <the sheet body's created_by>,
  "element": <stable STRAIGHT edge name from list_elements>}}, ...],
  "length": num, "length_expr"?: str, "angle_deg": num (default 90),
  "angle_deg_expr"?: str, "radius"?: num (omitted = the document's bend
  radius), "radius_expr"?: str, "position"?: "material_inside" (default) |
  "material_outside" | "bend_outside", "length_mode"?: "flat" (default) |
  "outside" | "inside", "offset_start"?: num, "offset_end"?: num,
  "relief"?: "none"|"rectangular"|"obround"|"tear"}} — WHICH of a rim's two
  edges you pick decides which way the wall folds, and `length_mode` says
  which dimension `length` states (a drawing dimensions to a mold line, not
  to the flat wall). Each edge records a bend on the body: radius, angle, K
  and where that K came from.
- FlatPattern (the BLANK: a sheet body laid flat): {{"type": "FlatPattern",
  "name": str, "body_id": <the sheet body's created_by>, "fixed_face"?:
  {{"feature_id": ..., "element": ...}}}} — replaces the body's shape in
  place, so author it SUPPRESSED and put its id in a configuration's
  `unsuppressed_features` to switch between folded and flat. It must sit
  AFTER every feature that reshapes the sheet, since it flattens from the
  bends those features recorded; flattening a blank twice is refused.
- Hem (fold a sheet edge right back on itself): {{"type": "Hem", "name":
  str, "edges": [{{"feature_id": ..., "element": <straight panel edge>}}],
  "length": num (the return), "length_expr"?: str, "kind"?: "closed"
  (default) | "open", "gap"?: num (open only; omitted = one thickness),
  "outside"?: bool, "offset_start"?/"offset_end"?: num (+_expr) — insets
  from the ends of the picked edge}} — exempt from the minimum bend radius
  (a hem is crushed). As with EdgeFlange, which of a rim's two edges you
  pick decides which way it folds; and on a pan, inset the hem or it meets
  the wall beside it at the corner and the part flattens from no blank.
- Jog (step a panel up along a line): {{"type": "Jog", "name": str,
  "sketch_id": <a sketch holding ONE line drawn on the panel face>,
  "height": num (face to face), "height_expr"?: str, "angle_deg"?: num
  (0 < a <= 90, default 90), "flip"?: bool (which side is carried)}} — the
  bends run the full panel width; a height under (2R + t)(1 - cos a) is
  refused.
- An EdgeFlange whose edges meet at a corner MITRES the flanges there;
  add "corners": "closed" to carry flanges that only touch at a square
  corner round it and mitre them shut.
- CornerRelief (a relief wherever two bends cross): {{"type":
  "CornerRelief", "name": str, "body_id": <sheet body's created_by>,
  "style"?: "circular" (default) | "rectangular", "size"?: num (radius or
  half-side; omitted = bend radius + thickness)}} — every such corner of
  the body; three bends at one corner, or a non-square corner, is refused.
- NormalCut (a cut through SHEET, square to the metal wherever it lands):
  {{"type": "NormalCut", "name": str, "sketch_id": <closed profile>,
  "body_id"?: <sheet body>, "regions"?: [...]}} — use it instead of an
  Extrude cut on sheet metal: an extrude cut through a bend comes out
  slanted to the metal. Sketch it where it sees only the face to cut.
- Unfold (lay sheet bends flat so the next features cut in the flat):
  {{"type": "Unfold", "name": str, "body_id": <the sheet body's
  created_by>, "fixed_face"?: {{"feature_id": ..., "element": ...}},
  "bends"?: [<ids of the EdgeFlange features whose bends to unfold>]}}
  — omit `bends` for all of them.
- Refold (fold an Unfold's bends back up): {{"type": "Refold", "name": str,
  "unfold_id": <the Unfold's id>}} — through-cuts made in between are
  wrapped onto the bend; a partial-depth cut across a crease is refused.
- Import (STEP, IGES, STL, OBJ, PLY, glTF, BREP): {{"type": "Import",
  "name": str, "path": str, "units"?: "mm"|"cm"|"m"|"in" (what an STL, OBJ,
  PLY or BREP was drawn in; refused on STEP and IGES, which declare their
  own, and on glTF, which is metres),
  "keep_origin"?: bool (default false: the import is centred on its
  bounding box; true keeps the file's coordinates), "up_axis"?:
  "auto"|"none"|"+x"|"-x"|"+y"|"-y"|"-z" (which axis OF THE FILE is up;
  default auto reads the header: a SolidWorks file is +y), "turn_deg"?:
  number (about world Z, after the axis), "representation"?:
  "recognize"|"facets" (what an STL becomes: its holes, flats, cones and
  spheres rebuilt as true faces, or a face per triangle; default the
  user's preference), "min_sides"?: int (with recognize, 5-1024, default
  12: the fewest sides a ring of facets needs to be round), "placement"?:
  {{"translate_mm"?: [x, y, z], "rotate_deg"?: [x, y, z]}} and "scale"?:
  number > 0 (after the axis and the centring: scaled about the origin,
  turned about world X, Y, Z, moved; rpc('align_import', {{feature_id,
  mode: floor|three_points|principal|scale, ...}}) writes them from a
  picked floor, three points, the principal axes or a known distance)}} —
  the reply's
  `up_axis` says what was applied and why, and `recognition` what an STL
  was found to be. A DXF or SVG goes into a sketch with
  rpc('import_sketch', ...).
- Derived (a copy of another part's bodies or sketches, to draw on):
  {{"type": "Derived", "name": str, "source_part": <part slot id from
  list_parts>, "configuration"?: <the source's configuration, id or
  name> (absent = its active one), "bodies"?: [<created_by ids>] and/or
  "body_names"?: [<names, as get_state shows them>] (both absent = all),
  "sketches"?: [<source sketch ids>] and/or "sketch_names"?: [<names>]
  (a copy naming a sketch takes bodies only by name), "follow"?: true
  (default: retaken before any call that reads geometry, reported as
  `links_updated`) | false, "placement"?: {{"position": [x,y,z],
  "orientation": [x,y,z,w]}}, "reference"?: true (a ghost: absent from
  mass, BOM, export, CAM and booleans) | false (an ordinary body of this
  part)}} — a sketch here stands on a copied sketch with "plane_ref":
  <its id>, and rpc('project_geometry', {{sketch_id, feature_id, element:
  "<derived id>/<sketch id>/<entity id>"}}) projects its entities;
  `get_state` says when a copy is stale.
- Template (one instance of a feature template from the project's
  library — a composite saved once, e.g. a bearing seat, placed as ONE
  feature that expands into its features at every regeneration):
  {{"type": "Template", "template": <name from list_templates>,
  "on": <on> (what it stands on, as a primitive's), "parameters":
  {{<name>: value, ...}}, "name"?: str}} — EVERY parameter the template
  declares, none filled in: a length/angle as a number (mm, deg) or an
  expression string over the variables, a count a whole number, a
  string, a reference in the <on> form. Make one with `save_template`
  {{name, feature_ids, literals?: [{{feature_id, path, param}}]}} (the
  variables the features read become parameters; `dry_run` lists the
  literal candidates); the instance's faces are named under ids of its
  own, so a later feature references them like any face. `get_state`
  says `template_changed` when the library moved on —
  `update_template_instances` follows; `unlink_template` makes plain
  features (docs/modeling/templates.md).
- Fastener (a standard screw, nut or washer read from its ISO table — every
  dimension but these comes from the standard): {{"type": "Fastener",
  "name": str, "standard": "iso4762"|"iso14579"|"iso4017"|"iso10642"|
  "iso7380" (screws) | "asme_b18_3" (inch socket head cap screw, "size" a
  UNC designation such as "1/4-20") | "iso4032" (nut) | "iso7089"
  (washer), "size": str
  (a stocked key, e.g. "M6" — an unstocked one is refused naming the
  nearest stocked), "length"?: num (mm under the head, OVERALL for a
  countersunk screw; omit for a nut or a washer), "length_expr"?: str,
  "drive"?: str (omit for the drive the standard is made with)}} — in an
  ASSEMBLY, prefer the `add_fastener`/`fit_fastener` RPCs, which derive all
  of this from the hole you pick (docs/parts/README.md).
- Bearing (an ISO 15 deep-groove ball bearing — d, D, B and the chamfer
  come from the standard): {{"type": "Bearing", "name": str,
  "designation": str (series 618/619/60/62/63, bores 3 to 30 mm: "6200",
  "608", "618/4"; anything else is refused naming what is carried)}} — in
  an ASSEMBLY, prefer `fit_bearing`, which reads the size off the shaft or
  bore you pick and mates it there (docs/parts/README.md#bearings).
- RetainingRing (a DIN 471/472 circlip — thickness and free diameter come
  from the table): {{"type": "RetainingRing", "name": str, "standard":
  "din471" (external, a shaft, sizes 3-56) | "din472" (internal, a bore,
  sizes 8-72), "size": num (the nominal shaft or bore diameter, mm)}} — in
  an ASSEMBLY, `fit_retaining_ring {{groove}}` puts the ring a groove
  takes into it, seated on the groove's faces so it follows the groove
  (`add_retaining_ring` places one where told); `list_retaining_rings`
  answers the table, its groove dimensions included.
- RetainingRingGroove (the groove a ring sits in, cut in a part):
  {{"type": "RetainingRingGroove", "name": str, "face": {{"feature_id",
  "element"}} (a CYLINDRICAL face: a shaft takes din471, a bore din472),
  "from": {{"feature_id", "element"}} (a circular edge at ONE END of that
  face: the side of what the ring holds, e.g. the shoulder a bearing sits
  against), "offset": num (mm from that edge to the groove's near flank —
  the bearing's width, for a bearing between the shoulder and the ring),
  "offset_expr"?: str, "standard"?: str (must be the face's)}} — the
  groove's diameter and width come from the table for the face's
  diameter; it is REFUSED when the face is not a carried size, the groove
  runs off the face, or less than the table's edge distance n is left past
  it (docs/parts/README.md#the-groove).

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
distance_x, distance_y, diameter, distance_pl, curvature, arc_length.

curvature (G2) takes two curves already joined end to end by a coincident,
at least one a spline: tangent there with equal curvature. It is refused for
two lines, a line and an arc, two arcs, a degree-1 spline, and curves that
share no end.

refs is a list of [entity_id, which] pairs. `which` picks a point on the
entity: line 0=a 1=b; circle 0=center; arc 0=center 1=start 2=end;
text 0=anchor 1..4=its box corners BL,BR,TR,TL (dimension a corner to place
the label; the box is the font's ascender/descender and the advance). For
whole-entity kinds (parallel, equal, tangent, ...) `which` is ignored — pass 0.
symmetric takes a third ref: the symmetry line.

Datum references (usable as entity_id): {DATUM_ORIGIN} = sketch origin,
{DATUM_AXIS_X} = X axis, {DATUM_AXIS_Y} = Y axis. E.g. anchor a corner:
{{"kind": "coincident", "refs": [[<line id>, 0], [{DATUM_ORIGIN}, 0]]}}.

Dimensional kinds (distance, radius, angle, distance_x, distance_y, diameter,
distance_pl, arc_length -- the length along a circular arc, r x sweep, refs =
the arc) take value or expr (expression over document variables — prefer
it: "width" beats 40). `value` is MILLIMETRES, except for angle, where it is
RADIANS — pass value_deg for degrees, never both, and more than a full turn
is refused as a degrees value in the radians field. `expr` is different and
always in the document's base units, where angles are DEGREES: "45", "45deg"
and "0.785rad" all mean 45 degrees. distance_pl is the perpendicular
point-to-line distance: refs = [[point entity, which], [line entity, 0]] —
also the parallel-lines distance (use an endpoint of the first line)."""

MATERIAL_SLUGS = ("steel, stainless_steel, brass, bronze, copper, "
                  "chrome, gold, silver, titanium, bimetal_kanthal_94s, "
                  "aluminum_6061_t6, "
                  "aluminum_6082_t6, plastic_white, "
                  "plastic_black, petg, pla, tpu, carbon_fibre, rubber, glass, ruby, concrete, "
                  "epoxy, brick, roof_tile, plaster, roofing_felt, render, "
                  "soil, wood_oak, wood_pine, wood_walnut, wood_maple, "
                  "wood_beech, plywood_birch, plywood_pine, mdf")

WORKFLOW = """You are modeling in Loböle, a parametric B-Rep CAD app
(sketch → constrain → extrude/revolve, feature history, expressions).
The world is Z-up: +Z is height, the XY plane is the Top plane / the floor,
gravity pulls -Z — the usual CAD and G-code convention.

The loop that works — follow it every time:
1. Restate the brief as named dimensions, then set_variable for each driving
   dimension (design intent in the name: "flange_width", not "w"). Variables
   are PROJECT-WIDE by default — one 'bore' drives every part of an assembly;
   use scope='part' only for a dimension private to one document. Prefer
   expressions over literals everywhere a *_expr or expr field exists.
2. Sketch on a plane (or a DatumPlane feature). Draw entities, then constrain
   until the geometry cannot wiggle: anchor to the sketch origin/axes datum
   refs, use distance/radius constraints driven by your variables.
   Model on faces and planes from sketches: a boss is a sketch ON the face
   it stands on (face_ref), so it follows the face when the part changes.
   Box/Cylinder/Sphere are for a fixture, a stand-in or a first rough block,
   and are always placed `on` something (a face, a plane, a Frame) — never at
   a world position.
3. Extrude/Revolve referencing the sketch by the id add_feature returned.
   Never guess ids — read them from tool results or get_state. Unsure of a
   feature's JSON dialect? describe lists feature_types[].params (field
   names, axis conventions); unknown fields ERROR instead of vanishing, so
   trust the error text — it names the accepted vocabulary.
4. VERIFY after every mutating step — the order is EXECUTE, MEASURE, then
   RENDER. Measure first: every call that rebuilds the part answers `regen`
   and a `digest` (per-body volume_mm3/bbox/solid-count plus a delta:
   volume_mm3 moved, bodies added/removed; a batch answers one for the
   whole) — read it before anything else; a cut whose delta is ~0 or
   a body whose `solids` jumped to 2 is wrong NOW, not three features later.
   Check `regen.warnings` (a warning = built but likely did nothing, e.g. a
   cut whose profile missed every body). mass_properties is the oracle the
   render cannot fake (CoM, mass, inertia). THEN look: rpc('render_sheet')
   gives four views + the kernel numbers burned into one PNG (screenshot
   for close-ups). Do not batch-build blind, and do not trust pixels for
   dimensions — cross-check them against the footer numbers.
4b. ASSEMBLIES: after placing occurrences, gate on contact_report — the
   whole structure must be ONE component (anything in its own component is
   floating in mid-air), zero clashes, zero near_misses (a 0.1-1 mm gap is
   an arithmetic slip). Assert every bolted joint `"touching"` and every
   designed clearance by number via interference {a, b} — state and
   distance, never just "no clash". Reproducing a reference or guarding a
   regression? compare_geometry {golden_path} gives IoU/precision/recall
   with plain-language verdicts, and missing_path/extra_path export the
   difference as solids. Add {align: true} when you are checking the SHAPE
   and not where it sits — a re-draw whose volume matches to four figures
   scores IoU 0.333 if the reference is a few hundred mm away.
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
   feature's "error", fix or suppress_feature it. rpc('set_rollback')
   shows an intermediate history state, and its own answer's digest is
   the bodies as of that point. When done: save_project, and
   render_still for a presentation shot.
7. Built a part from literals, or handed one? Run rpc('check_literals'):
   it names each number that equals a variable in scope (bind it with
   rpc('extract_variables', {reuse: true, apply: true})) and each sketch or
   frame placed on a world plane where a face of the part already lies
   (attach it to that face). rpc('extract_variables') without `apply` is a
   dry run proposing a named variable per literal; with apply: true it is
   one undo step and the geometry does not move.
8. STATE THE REQUIREMENTS, AND CHECK THEM BEFORE DELIVERING. What the
   design is FOR -- "deflects at most 2 mm at 3 g", "the camera clears the
   yoke through the tilt", "every bearing seat passes", "under 400 g" -- goes
   into the project with rpc('add_requirement', {name, measure, bound,
   source}), not into your head or a comment: a measure names what to read
   by stable reference (a study's probe or peak, a clearance between two
   occurrences, mass, a section's area, check_fits / check_seat /
   check_fasteners / check_literals, a motion study clean, cam_verify) and
   a bound (at_most, at_least, within, or pass; a value may be an
   expression over the variables). Before you say a design is done, run
   rpc('check_requirements', {async: true}) and poll study_status {job},
   then list_requirements: only `pass` is a pass. `stale` means the model
   changed after the check, `unresolved` that a reference is gone or the
   measure could not be taken, `unchecked` that it was never checked or was
   edited since -- report every one of those as NOT met, with its note.
   Re-checking an unchanged design costs nothing (the evidence and the
   study results are reused by input hash).
8b. A PART THAT CARRIES A LOAD IS NOT DONE WITHOUT ITS STUDIES. Before you
   call it done it gets: a static study of each way it is loaded; a
   buckling study (add_study {type: 'buckling'}) of each load that
   compresses something; and requirements on factor_of_safety (static) and
   buckling_factor (buckling) -- both read as the governing values. A part
   that buckles passes its static study with a comfortable margin, so the
   static study alone says nothing about it. The shapes that buckle:
   struts and posts, ribs and webs, thin walls, tubes, levers and links in
   compression. buckling_factor is the governing factor: the elastic factor
   knocked down for a curved wall's imperfections and met with yield
   (docs/simulation/README.md#buckling); read its warnings (a mode local
   where a load enters, one element through a wall). A curved wall loaded
   across its curve (an arch, a dome, a clip) can SNAP THROUGH, which
   linear buckling does not see: say so, and name the nonlinear study for
   it -- it does not exist yet -- rather than calling the part done on the
   linear factor. A requirement on a study never run is `unchecked`:
   report it as NOT met. A part beside heat (a motor, a hot end, a lamp)
   gets a thermal study (add_study {type: 'thermal'}) and a requirement
   that its peak stays under its material's service temperature, with a
   margin: a printed plastic softens at its heat deflection temperature
   long before a stress fails it.

IMPORTING a file: rpc('import_step', {path}) is the importer for every 3D
format, STL, OBJ, PLY and glTF meshes included, despite its name (an STL's
planes, cylinders, cones and spheres come back as true faces unless
representation is "facets"); a DXF or SVG goes into a sketch with rpc('import_sketch').

"This", "here", "the selected": read get_state's `selection` (and
`selection_items`) BEFORE acting — that is what the user means. When it is
empty, say so and ask; never guess which face or feature was meant.

No display session of your own? Start `lobole --offscreen --remote 0` —
a real renderer with no window or display server: screenshot, render_still,
set_render_options, rpc('render_sheet') and rpc('select_face') all work (the
GUI-driving RPCs, rpc('menu_click'), rpc('key_press') and the rest, do not —
they need a window, e.g. `tools/headless-preview.sh --gui <scratch-port>`
under Xvfb). Never drive the human's instance — a `gui` one in the
registry (`instances`) that you did not start, the build they review; start
your own on a scratch port.

RESEARCH REAL DIMENSIONS FIRST. When the part interfaces with a real product
(bearings, inserts, screws, connectors, rails), web-search the datasheet
numbers BEFORE sketching — bore/OD/width for bearings, thread pitch and
tap/clearance drill for screws, pin pitch for connectors. Never guess a real
dimension; put each researched number in a named variable with a comment
citing the source.

MANUFACTURING (FDM defaults — apply unless told otherwise):
- Walls >= 1.2 mm; any freestanding feature >= 0.4 mm (one nozzle width).
- Holes mating with hardware: add 0.2-0.3 mm diametral clearance (printed
  holes shrink); press-fits get 0.0-0.1 mm. For a fit that matters, declare
  rpc('set_print_process', {process: 'fdm', material: 'petg'}) on the part
  and ask rpc('print_allowance', {kind, size_mm}) or check_fit what to model.
- Note the intended print orientation in your final answer (which face on the
  bed, where supports land, strength direction — layer bonds are the weak
  axis).
- Close with a one-line print recipe: material / layer height / walls /
  infill + one clause of why (e.g. "PETG, 0.2 mm, 4 walls, 40% gyroid —
  outdoor bracket under sustained load"). On a printed part, state it:
  rpc('set_print_settings', {walls, top_layers, bottom_layers, infill_pct,
  infill_pattern}) -- mass_properties then weighs it as printed -- and
  rpc('export_print_job', {path, plates}) writes a 3MF the slicer opens
  with those settings per object.

DELIVERY CADENCE. Do not disappear into a long build: model the base shape,
show the user (screenshot/render) before detailing; add features, show again;
then finish (chamfers/fillets) and deliver. Close every job with the exported
STL path, a parameter table (variable, value, what it drives) so the user can
see every knob, and an explicit offer to adjust any of them.
"""

# ---------------------------------------------------------------------------
# Connection
# ---------------------------------------------------------------------------

class _Server(MCPServer):
    """MCPServer with the two guarantees the tools below are written against.

    ONE CALL AT A TIME. mcp 2 runs each request as its own task and a sync
    tool on a worker thread, so two calls a client sends together would share
    the one app connection (and the autostart) from two threads, interleaving
    their JSON-RPC. The lock keeps calls in the order they started.

    A REFUSAL KEEPS ITS TEXT. mcp 2 passes a tool's own message on only for
    ToolError; anything else is a crash, and the model reads just "Error
    executing tool <name>". Every refusal here arrives as a RuntimeError
    (lobole.errors.Refused and the rest of LoboleError are ones), a ValueError
    or an OSError (the app gone), and its text, naming what IS accepted, is
    the point of refusing, so those keep it. Any other exception is a bug in
    this file and stays a crash, logged with its traceback.
    """

    _lock = None

    async def call_tool(self, name, arguments, context=None):
        if self._lock is None:
            import anyio  # type: ignore[import-not-found,unused-ignore]
            self._lock = anyio.Lock()
        async with self._lock:
            try:
                return await super().call_tool(name, arguments, context)
            except UnexpectedToolError as e:
                cause = e.__cause__
                if isinstance(cause, (RuntimeError, ValueError, OSError)):
                    raise ToolError(
                        f"Error executing tool {name}: {cause}") from cause
                raise


mcp = _Server(
    "lobole",
    instructions="Tools for driving a running Loböle CAD instance. Read the "
    "modeling-workflow prompt before modeling. The listed tools are the "
    "conversational surface; EVERY other RPC (assemblies, drawings, studies, "
    "configurations, sketch constraints, imports and exports, raw GUI "
    "actions) is reached "
    "with rpc('method', {...}) and documented by help('method'). For building "
    "a part with loops or computed arguments, execute_script (a python "
    "snippet with the client pre-bound as `d`) beats a tool call per step. "
    "For many edits at once, rpc('batch', {'calls': [...]}) runs them as ONE "
    "transaction — one regeneration, one undo step, and a rollback if any "
    "call fails; {'$ref': n} inside a call's params is the n-th call's id, "
    "so a sketch and the extrude that names it go in one batch. A file is "
    "imported with rpc('import_step', {'path': ...}) whatever its 3D format "
    "(STEP, IGES, STL, OBJ, PLY, glTF, BREP; an STL's holes and flats "
    "recognised as "
    "true faces), and a DXF or SVG into a sketch with rpc('import_sketch').",
)

_client = None
# An EXPLICIT port (flag or env) is an instruction, not a guess: it pins the
# instance and never falls back to discovery, so a client told to drive one
# app cannot silently start driving another.
_pinned_port = os.environ.get("LOBOLE_PORT")
_port = int(_pinned_port) if _pinned_port else None
_attached = None  # what ping said about the instance we are talking to

DEFAULT_PORT = 8437


def _running_instances():
    try:
        return Client.instances()
    except Exception:
        return []  # discovery is a convenience, never a failure mode


def _lobole_argv():
    """THIS machine's Loböle as an argv, probed — not guessed.

    The server can be installed far from any checkout (`uv tool install`), so
    a refusal that always says `lobole --remote …` names a command half the
    machines don't have. Suggest only what is actually here: a dev-tree
    binary in the checkout this file is in, `lobole` on PATH, or the flatpak.
    """
    import pathlib
    import shutil
    import subprocess
    dev = pathlib.Path(__file__).resolve().parents[2] / "build" / "lobole"
    if dev.is_file():
        return [str(dev)]
    if shutil.which("lobole"):
        return ["lobole"]
    if shutil.which("flatpak"):
        try:
            probe = subprocess.run(
                ["flatpak", "info", "org.lobole.Lobole"],
                capture_output=True, timeout=10)
            if probe.returncode == 0:
                return ["flatpak", "run", "org.lobole.Lobole"]
        except OSError:
            pass
    return None


def _start_hint():
    argv = _lobole_argv()
    return " ".join(argv) if argv else None


# The instance this server spawned, if any. Autostart triggers ONLY when
# discovery finds nothing running (a pinned port never autostarts), spawns
# --offscreen (no window to shadow the user's document, and renders — the two
# objections the old autostart rejection rested on), and the child dies with
# this process. Off switch: --no-autostart / LOBOLE_AUTOSTART=0.
_autostarted = None
_autostart_enabled = os.environ.get(
    "LOBOLE_AUTOSTART", "").lower() not in ("0", "off", "no")


def _descends_from(pid, ancestor):
    """Whether `pid` is `ancestor` or runs under it. An AppImage's runtime
    and `flatpak run` start the app as their CHILD, so the instance announces
    a pid that is not the one we spawned. Linux reads /proc; elsewhere only
    the pid itself matches."""
    for _ in range(16):
        if pid == ancestor:
            return True
        try:
            with open(f"/proc/{pid}/stat") as f:
                # The command name is in parentheses and may hold spaces.
                pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            return False
        if pid <= 1:
            return False
    return False


def _stop_autostarted():
    global _autostarted
    proc = _autostarted
    _autostarted = None
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()


def _autostart():
    """Spawn `--offscreen --remote 0`; return its announced port, or None
    when no Loböle is installed here (the caller then refuses, naming what
    was probed). A spawn that dies or never announces raises with the app's
    own words — an agent must see WHY, not a generic timeout."""
    global _autostarted
    import atexit
    import pathlib
    import shutil
    import subprocess
    import time
    argv = _lobole_argv()
    if argv is None:
        return None
    scratch = tempfile.mkdtemp(prefix="lobole-mcp-autostart-")
    env = dict(os.environ)
    # Same reasoning as tools/headless-preview.sh: set_settings persists
    # immediately, so a scratch instance must not write the human's real
    # preferences. Seed from them so it still looks like their app.
    cfg = pathlib.Path(scratch, "config", "lobole")
    cfg.mkdir(parents=True)
    real_dir = pathlib.Path(env.get("XDG_CONFIG_HOME",
                                    os.path.expanduser("~/.config")),
                            "lobole")
    if (real_dir / "settings.json").is_file():
        shutil.copy(real_dir / "settings.json", cfg / "settings.json")
    # ...and the MACHINE store. A CAM setup names its machine BY NAME, so a
    # scratch instance without one refuses every job with "no machine called
    # '6090'" against a machine plainly on disk. Copied, so editing one here
    # cannot touch the shop's.
    #
    # settings.json and machines/ by name, never the whole directory: that
    # would take anything else kept there with it, and a scratch dir under
    # /tmp is not where credentials belong.
    if (real_dir / "machines").is_dir():
        shutil.copytree(real_dir / "machines", cfg / "machines")
    env["XDG_CONFIG_HOME"] = str(pathlib.Path(scratch, "config"))
    # "It dies with this server" has to hold when the server is SIGKILLed
    # too, where atexit never runs: the app asks the kernel for SIGTERM when
    # this process exits (src/app/ParentWatch.h). An older binary ignores the
    # variable; `flatpak run` does not forward it. The kernel watches the
    # THREAD that forks, so only the main thread arms it: a pool thread
    # retiring after idling would take the instance with it. Not as PID 1 (a
    # container's entry point): the app refuses 1, which is also what an
    # orphan's parent reads, and the namespace's other processes die with
    # its init anyway.
    import threading
    if (threading.current_thread() is threading.main_thread()
            and os.getpid() != 1):
        env["LOBOLE_EXIT_WITH_PARENT"] = str(os.getpid())
    log_path = os.path.join(scratch, "app.log")
    with open(log_path, "w") as log:
        proc = subprocess.Popen(argv + ["--offscreen", "--remote", "0"],
                                stdout=log, stderr=subprocess.STDOUT, env=env)
    _autostarted = proc
    atexit.register(_stop_autostarted)
    deadline = time.time() + 60
    while time.time() < deadline and proc.poll() is None:
        # The registry is the rendezvous: the child announces its kernel-
        # chosen port under its pid, so there is no port to race for.
        for info in _running_instances():
            if _descends_from(int(info.get("pid", 0)), proc.pid):
                print(f"lobole-mcp: autostarted `{' '.join(argv)} "
                      f"--offscreen` on port {info['port']} (it dies with "
                      "this server; disable with --no-autostart)",
                      file=sys.stderr)
                return int(info["port"])
        time.sleep(0.2)
    try:
        with open(log_path) as f:
            said = f.read()[-800:].strip()
    except OSError:
        said = ""
    _stop_autostarted()
    raise RuntimeError(
        f"autostart failed: `{' '.join(argv)} --offscreen --remote 0` "
        + (f"exited {proc.poll()}" if proc.poll() is not None
           else "never announced a port")
        + (". It said:\n" + said if said else ""))


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
        if _autostart_enabled:
            port = _autostart()
            if port is not None:
                return port
        cmd = _start_hint()
        if cmd is None:
            raise RuntimeError(
                "No running Loböle found, and no installed one to start: "
                "probed for build/lobole beside this server, `lobole` on "
                "PATH, and the org.lobole.Lobole flatpak. "
                "Install the app, then start it with `lobole --remote "
                f"{DEFAULT_PORT}`.")
        raise RuntimeError(
            f"No running Loböle found (autostart is off). Start one with:  "
            f"{cmd} --remote {DEFAULT_PORT}   (add --offscreen for no window "
            "with renders, or --headless for the document engine alone). If "
            "it is already running, it is on a build older than instance "
            "discovery: pass --port/LOBOLE_PORT.")
    listed = ", ".join(
        f"{i['port']} ({i.get('mode', '?')})" for i in live)
    raise RuntimeError(
        f"{len(live)} Loböle instances are running: {listed}. Which one? "
        "Set LOBOLE_PORT (or --port) — attaching to the wrong window would "
        "answer questions about a document you are not looking at.")


def _connect():
    global _client, _attached
    port = _resolve_port()
    try:
        # Generous timeout: render_still runs synchronously in-app.
        _client = Client(port, timeout=600.0)
    except OSError as e:
        raise RuntimeError(
            f"Loböle announced port {port} but is not answering there "
            f"({e}). If it just exited, start it again with: lobole "
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
        return f"Loböle on {where}"
    dirty = " (unsaved changes)" if _attached.get("dirty") else ""
    return f"Loböle on {where}: {project}{dirty}, {mode}"


def _text(out):
    """A subprocess's captured output as text, or "(none)"."""
    if isinstance(out, bytes):
        out = out.decode(errors="replace")
    return out or "(none)"


def _given(**params):
    """The arguments a caller actually gave: a tool passes only those, so
    every default is the app's (CLAUDE.md, "Defaults")."""
    return {k: v for k, v in params.items() if v is not None}


def _call(method, /, **params):
    """Call the app, reconnecting once across a dropped connection.

    `method` is positional-ONLY, like lobole.client.call's: an RPC
    parameter named "method" (describe narrows by one) would otherwise
    collide with this function's own first argument — which happened the
    first time an MCP client called describe(method=...). The body goes
    through request() because that is the call that takes a timeout.
    """
    global _client
    for attempt in (1, 2):
        if _client is None:
            _connect()
        try:
            return _client.request(method, params)
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

_REFERENCE: dict[str, dict[str, str]] = {}  # method -> {"doc": str}


def _reference(method, description):
    """The prose for an RPC reached through rpc(), which help() gives beside
    the app's own row. Not a tool and not a wrapper: rpc() sends every call
    straight to the app, so the app's parameters and defaults are the only
    ones (docs/scripting/mcp.md#the-tool-surface)."""
    _REFERENCE[method] = {"doc": description}


# The tools this server announces, for help()'s index. Kept next to the
# meta tools rather than derived from the server's registry, so the index says
# what the surface is MEANT to be, and --only-tools narrows only the schemas.
_EXPOSED = {
    "describe", "get_state", "check_geometry", "measure", "mass_properties",
    "list_elements", "list_variables", "set_variable", "screenshot",
    "render_still", "view", "set_camera", "set_section", "set_render_options",
    "new_project", "open_project", "save_project", "export_stl", "export_step",
    "add_feature", "modify_feature", "delete_feature", "undo", "rpc", "help",
    "instances", "execute_script",
}

@mcp.tool(description="API introspection — call this FIRST to learn the "
          "surface. Top-level keys: methods (name + one-line doc + the "
          "`params` keys it accepts + requires_part_doc), feature_types and "
          "sketch_entity_types (each with a `params` string naming every "
          "field AND its unit), constraint_kinds, constraint_value_units "
          "(what the number beside each dimensional kind means), "
          "mate_types, relation_kinds, "
          "extrude_modes, extrude_end_modes, axis_kinds, material_slugs, "
          "drawing_dim_kinds, inspection_contract, "
          "document_format_version. Pass `method` to get just that one "
          "instead of every method — the params list is what to read before "
          "guessing a key name, since an unrecognised key is REFUSED, not "
          "ignored. A method with params_free_form: true reads its params "
          "object opaquely, so the prose is the only guide there. The "
          "authoritative companion to the format prose in this server's "
          "tool descriptions.")
def describe(method: str = "") -> dict:
    out = _call("describe", **({"method": method} if method else {}))
    # Which instance answered. Everything below describes an app; this says
    # WHICH app, so a session cannot quietly be about the wrong window.
    out["attached"] = attached_to()
    return out


@mcp.tool(description="Full document state: features (with ids and per-type "
          "fields, plus 'error' if regeneration failed and 'warning' for a "
          "valid-but-likely-unintended no-op such as a cut that removed no "
          "material), bodies (name, volume_mm3, bbox_min/bbox_max, material, and "
          "the render 'mesh' id select_face needs — mesh ids change on every "
          "regeneration, always read them fresh), current selection, camera, "
          "undo/redo availability. Call after every mutating step to verify.")
def get_state(part: str | int | None = None) -> dict:
    return _call("get_state", **_given(part=part))


@mcp.tool(description="List variables: name, expression, evaluated value, "
          "scope ('project' = shared by every part, or 'part' = the active "
          "document's local scope, which shadows a same-named project var), "
          "comment, and error if the expression fails to evaluate. Project "
          "variables come first.")
def list_variables() -> dict:
    return _call("list_variables")


@mcp.tool(description="Toggle the viewport SECTION VIEW — a clipping plane "
          "that cuts the model open so you can see inside (screenshot to "
          "view it). enabled=False clears it. Plane by base_plane "
          "('xy'|'xz'|'yz' — anything else is refused) + offset in mm. Works "
          "on part AND assembly tabs, with filled cut caps on both. For an "
          "off-axis plane use rpc('add_section', {plane_id | tilt_u, "
          "tilt_v}). The default keeps the half BELOW the plane (z <= "
          "offset for xy, y <= for xz, x <= for yz); reverse=True keeps "
          "the half above, and the answer's `keeps` says which. View the "
          "cut from the removed side. (This is the visual cut; use "
          "`section` for the cross-section AREA.)")
def set_section(enabled: bool = True, base_plane: str | None = None,
                offset: float | None = None, reverse: bool | None = None,
                add: bool | None = None) -> dict:
    # Only what was given: an omitted offset cuts through the middle of what
    # is shown, which a default of 0 here used to override.
    if not enabled:
        return _call("set_section", enabled=False)
    return _call("set_section", enabled=True, **_given(
        base_plane=base_plane, offset=offset, reverse=reverse, add=add))


_reference("section", "Cross-section area where a plane cuts through all "
          "bodies (inspection). Plane: base_plane 'xy'|'xz'|'yz' + offset, "
          "or explicit origin+normal. Returns {area_mm2}. Use to check wall "
          "thickness / material at a cut. regions=True adds each connected "
          "ISLAND ({area_mm2, centroid}, biggest first) — two islands where you "
          "expected one is a feature that is not joined. loops=True adds "
          "each island's OUTLINE as line/arc segments, which is how you "
          "recover the shape of a part you did not draw. The area a cut "
          "ENCLOSES (the floor inside separate wall bodies) and a cut of "
          "only some bodies are rpc('section', {enclosed: true, bodies: "
          "[names or feature ids]}).")


_reference("interference", "Interference (clash) check: returns {clash: bool, "
          "volume_mm3} — the volume of the overlap. On a PART tab a/b are body "
          "indices. On an active ASSEMBLY tab a/b are occurrence ids (int) "
          "or occurrence names (str); each occurrence's part bodies are "
          "placed by its solved transform and checked in world space — or "
          "OMIT a/b to sweep EVERY visible occurrence pair (returns {clash, "
          "pairs: [{a, b, a_name, b_name, a_id?, b_id?, clash, volume_mm3, "
          "centroid_mm, bbox_min_mm, bbox_max_mm}] — "
          "a/b and a_name/b_name are the names, a_id/b_id the occurrence "
          "ids when both are occurrences of this assembly, and the centroid "
          "and world bounds (mm) say WHERE the overlap is — and the same "
          "list again as "
          "`clashes`, which is what contact_report calls it). Run it after "
          "mating — clash checks matter most in assemblies.")


@mcp.tool(description="Mass properties per body (and a document total): "
          "volume_mm3, surface_area_mm2, center_of_mass_mm, inertia matrix (about the "
          "CoM), density and mass_g (grams, assuming model units are mm; "
          "null when the body has no material assigned). The geometry "
          "oracle — verify against the brief with this, not just volume. "
          "Optional body index restricts to one body. On an ASSEMBLY tab "
          "(no body arg) it aggregates every visible, non-suppressed "
          "occurrence in WORLD space: `occurrences` lists per-part rows "
          "(volume_mm3, world center_of_mass_mm, mass_g, mass_from: printed | "
          "solid) and `total` is the combined solid mass/volume/world CoM; "
          "when an occurrence's part is printed, `total.printed` weighs the "
          "assembly as built (printed parts printed, inertia_g_mm2).")
def mass_properties(body: int | None = None,
                    part: str | int | None = None) -> dict:
    return _call("mass_properties", **_given(body=body, part=part))


@mcp.tool(description="Did the model build? Returns {ok, valid, body_count, "
          "feature_errors, bodies:[{body, name, valid, bad_faces, bad_edges, "
          "bad_wires, reasons}]}. `ok` is the probe you want: every existing "
          "body is a valid B-Rep AND no feature errored. `valid` is only the "
          "B-Rep check (OCC BRepCheck_Analyzer) — vacuously true over zero "
          "bodies, so read it with body_count. Read-only — run it on imported "
          "STEP or after booleans. Optional body index restricts to one body.")
def check_geometry(body: int | None = None) -> dict:
    return _call("check_geometry", **_given(body=body))


_reference("project_geometry", "Project external geometry into a sketch (FreeCAD's "
          "ShapeBinder): reference a FACE or EDGE of a body earlier in the "
          "history, projected onto the sketch plane as LOCKED construction "
          "geometry that tracks the source when it moves. Returns "
          "{external_index, gen_ids}. `element` is the stable element name "
          "(from list_elements). Use it to align new geometry to existing "
          "edges/holes. params: sketch_id, feature_id (the body's producer), "
          "element, kind ('face' or 'edge').")


_reference("project_section", "Draw a scan's (or any body's) outline into a "
          "sketch: where the sketch's plane cuts the bodies before it, as "
          "LOCKED construction geometry refitted from the cut on every "
          "regeneration, so it follows a re-scan or an upstream edit. "
          "params: sketch_id, bodies? (names or creating-feature ids; "
          "default every body before the sketch), tolerance_mm? (default "
          "1/1000 of their bounding diagonal), fit? ('polyline' | 'arcs', "
          "the default | 'spline'). Returns {external_index, gen_ids, "
          "tolerance_mm, deviation_mm (largest distance from the true cut "
          "to what was drawn), chains, open_chains}. For a stand or a "
          "pocket shaped to the object, rpc('offset', {sketch_id, chain: "
          "gen_ids, distance}) grows the outline by the clearance into real "
          "geometry, then extrude it -- a fully parametric 2.5-D part, where "
          "Cradle is the one shaped in 3-D.")


@mcp.tool(description="Measure geometry over stable element names (from "
          "list_elements). With `a` only: that element's own measure — a "
          "face's area or an edge's length. With `a` and `b`: the minimum "
          "distance between the two elements (faces or edges). Each ref is "
          "{feature_id, element}. Optional `part` (slot name or stable id) "
          "measures against that part WITHOUT switching the active tab — "
          "works from an assembly tab too. Verify dimensions with this "
          "instead of trusting your inputs.")
def measure(a: dict, b: dict | None = None, kind: str | None = None,
            distance_kind: str | None = None, mode: str | None = None,
            part: str | int | None = None) -> dict:
    return _call("measure", a=a, **_given(
        b=b, kind=kind, distance_kind=distance_kind, mode=mode, part=part))


@mcp.tool(description="Stable element names (topological naming) for each "
          "body's faces and edges, plus the body's created_by feature id. "
          "Every face and edge says what MADE it: `made_by` (a feature id) "
          "and `role` -- start_cap | end_cap (a cut's floor) | side (swept "
          "from sketch `entity`) | generated (a fillet's or chamfer's face) "
          "| bore | bottom | counterbore | counterbore_floor | countersink "
          "| point | roof (a Hole's) | outside | inner_side | outer_side | "
          "shield (a "
          "bearing's) | positional (a feature that names by index) | "
          "boundary | pole | seam; `instance` for a pattern copy, `derived` "
          "for one that came in through a Derived feature. ASK for faces "
          "by what made them rather than by geometry: made_by (feature id "
          "or name; a pattern means its copies), role (also floor = "
          "end_cap, walls = side), entity, and via (a Derived feature: "
          "made_by then names a feature of its SOURCE part) -- 'the "
          "pocket's floor' is made_by='Pocket', role='floor'. A filter that "
          "matches nothing answers `why`. Every face also carries kind, "
          "`area_mm2`, `centroid` and outward `normal`; every edge kind, "
          "`length_mm`, "
          "`midpoint`, radius when circular, and the names of the `faces` "
          "it joins -- the edges where a top meets its walls are the ones "
          "joining one of each. This is how Fillet/Chamfer "
          "edge refs are built: edges: [{feature_id: <created_by>, element: "
          "<edge name>}]. Names survive upstream edits; re-query after "
          "changing the feature history. Optional body index restricts to "
          "one body; optional `part` (slot name or stable id) reads another "
          "part WITHOUT switching the active tab — works from an assembly "
          "tab too — and with it `configuration` (name or id) lists the part "
          "as it is in that configuration: what a placement or a job part "
          "in it is made of.")
def list_elements(body: int | None = None, part: str | int | None = None,
                  configuration: str | int | None = None,
                  made_by: str | int | None = None, role: str | None = None,
                  entity: int | None = None, via: str | int | None = None,
                  max_faces: int | None = None,
                  walls: bool | None = None) -> dict:
    return _call("list_elements", **_given(
        body=body, part=part, configuration=configuration, made_by=made_by,
        role=role, entity=entity, via=via, max_faces=max_faces, walls=walls))


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
def add_feature(feature: dict, expect_revision: dict | None = None) -> dict:
    return _call("add_feature", feature=feature,
                 **_given(expect_revision=expect_revision))


@mcp.tool(description="Replace the feature at a history index with new "
          "feature JSON (same formats as add_feature; keep the same type and "
          "id). Regenerates. Get indices from get_state.")
def modify_feature(feature: dict, feature_id: int | None = None,
                   index: int | None = None,
                   expect_revision: dict | None = None) -> dict:
    return _call("modify_feature", feature=feature, **_given(
        feature_id=feature_id, index=index, expect_revision=expect_revision))


@mcp.tool(description="Delete the feature at a history index. Later features "
          "referencing it will fail to regenerate — check get_state after.")
def delete_feature(feature_id: int | None = None, index: int | None = None,
                   expect_revision: dict | None = None) -> dict:
    return _call("delete_feature", **_given(
        feature_id=feature_id, index=index, expect_revision=expect_revision))


_reference("set_material", "Assign a material — it drives mass, BOM and FEM as "
          "well as appearance. Target ONE of: feature_id "
          "(the body's created_by from get_state/list_elements) or body (an "
          "index into get_state's bodies — add/cut keep the original creator, "
          "so this recolors everything fused into that body). In an "
          "assembly, an occurrence's own material "
          "(set_occurrence_material) overrides this for that placement. "
          "Slugs: "
          + MATERIAL_SLUGS + ". Empty string restores the default; any other "
          "unrecognised slug is refused.")


# ---------------------------------------------------------------------------
# Sketch
# ---------------------------------------------------------------------------

_reference("add_constraint", "Add a constraint to a sketch (by feature id), solve, "
          "and regenerate. The solve may move geometry — read positions back "
          "via get_state, never assume.\n\n" + CONSTRAINT_FORMATS)


_reference("delete_entity", "Delete a sketch entity by id; constraints referencing "
          "it are dropped (count returned as dropped_constraints).")


_reference("mirror", "Mirror sketch entities across a line entity. sources: "
          "entity ids; axis: the mirror line's entity id (construction lines "
          "work). link=true keeps mirrors following their sources. Returns "
          "generated entity ids.")


_reference("fillet", "Fillet the corner where two sketch lines meet with an "
          "arc of the given radius. Returns the arc's entity id (arc_id).")


_reference("pattern", "Repeat sketch entities in a linear or circular "
          "pattern. sources: entity ids; count includes the originals. "
          "Linear: step=[du,dv] spacing in mm. Circular: circular=true, "
          "center=[u,v], and the PER-STEP angle about the centre — "
          "angle_deg=45 for three instances 45 degrees apart, or angle= in "
          "radians. Not the total sweep. link=true keeps copies following "
          "their sources. Returns generated entity ids.")


_reference("offset", "Offset a connected chain of sketch entities by a "
          "distance (sign picks the side). chain: entity ids in order. "
          "join_arc=true rounds convex corners. Returns generated entity ids.")


# ---------------------------------------------------------------------------
# Reference images (sketch underlays)
# ---------------------------------------------------------------------------

_reference("add_reference_image", "Import a raster image (PNG/JPEG/BMP) into a sketch (by "
          "feature id) as a reference underlay to trace geometry over. path is "
          "read by the running app (must be reachable by it). Placement, the "
          "same here as in set_reference_image: 'at' [u,v] is the image's "
          "CENTRE, 'origin' [u,v] its (0,0) pixel corner, never both "
          "(default: centred on the plane origin), at a default scale; "
          "recalibrate to a real dimension with calibrate_reference_image. "
          "rotation_deg turns it about its centre (degrees, CCW). "
          "show_always=True "
          "keeps it drawn when the sketch is not being edited and in quick "
          "stills, to check a model against the plan it was traced from. "
          "Returns {image_id, width_px, height_px, asset, at, origin}.")


_reference("calibrate_reference_image", "Scale a sketch reference image so its two picked "
          "plane-UV points p1,p2 are 'distance' plane units apart (p1 held "
          "fixed). This is how you match an image's printed dimension: click "
          "the two ends of a dimension line, pass its real length. "
          "align='horizontal'|'vertical' also rotates the image so the p1->p2 "
          "line lands on that axis; 'none' (default) keeps the orientation.")


_reference("set_reference_image", "Edit a sketch reference image (by image_id): pass any "
          "of opacity (0..1), hidden (bool), show_always (bool: drawn outside "
          "sketch edit and in quick stills), rotation (RADIANS) or "
          "rotation_deg (degrees) but never both, units_per_pixel (sketch mm "
          "per image pixel), and the placement as in add_reference_image: "
          "at [u,v] (the image's centre) or origin [u,v] (its (0,0) pixel "
          "corner), never both. A turn or a rescale with neither keeps the "
          "centre where it was. Only the fields you pass change.")


_reference("delete_reference_image", "Remove a sketch reference image by image_id.")


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
          "evaluated value + scope (and error if it fails), and the active "
          "part's regen and digest. Name every driving "
          "dimension of the design — parameters first.")
def set_variable(name: str, expression: str, comment: str = "",
                 scope: str = "project") -> dict:
    params = {"name": name, "expression": expression, "scope": scope}
    if comment:
        params["comment"] = comment
    return _call("set_variable", **params)


_reference("rename_variable", "Rename a variable and rewrite every expression that "
          "references it (a project variable's references span all parts). "
          "scope auto-detects (project first, then the active part's local) "
          "when omitted.")


_reference("delete_variable", "Delete a variable. Refused (with the referencing fields "
          "listed) while anything still uses it — a project variable checks "
          "every part. scope auto-detects (project first, then the active "
          "part's local) when omitted.")


# ---------------------------------------------------------------------------
# Configurations (parametric variants of the active part OR assembly)
# ---------------------------------------------------------------------------

_reference("add_configuration", "Add a configuration to the active part or assembly — a "
          "named variant. On a part it overrides variables / suppresses "
          "features; on an assembly it suppresses occurrences. Empty until you "
          "set_configuration it. Returns its id.")


_reference("remove_configuration", "Remove a configuration by id (falls back to base if it "
          "was active).")


_reference("list_configurations", "List the active part's or assembly's configurations "
          "and which is active (0 = base, the model as authored) -- or "
          "`part`'s (id or name) from any tab. A part's "
          "carry parameter overrides and suppressed feature ids; an "
          "assembly's carry suppressed occurrence, mate and relation ids and "
          "the mate DOFs they hold (mate_overrides).")


_reference("set_configuration", "Edit a configuration (by id). On a PART: "
          "parameter_overrides = a list of {name, expression} replacing the "
          "config's variable overrides (e.g. {'name':'depth','expression':"
          "'12'}); suppressed_features = feature ids to suppress. On an "
          "ASSEMBLY: suppressed_occurrences / suppressed_mates / "
          "suppressed_relations = ids taken out of the solve; mate_overrides "
          "= [{mate, rotation (radians) | rotation_deg, offset (mm)}], DOFs "
          "the configuration HOLDS — an overlay, so leaving it restores the "
          "mates. The other kind's keys are refused. name renames. Each list "
          "replaces. Read list_configurations, adjust, send back. One "
          "undoable edit; editing the active one re-solves.")


_reference("set_active_configuration", "Switch the active configuration of the part or "
          "assembly (0 = base). A part regenerates against it — subsequent "
          "geometry, export, drawings and FEM all reflect it — and returns "
          "the resulting body count and volumes; an assembly re-solves with "
          "its suppressions and held DOFs.")


# ---------------------------------------------------------------------------
# History & project
# ---------------------------------------------------------------------------

_reference("suppress_feature", "Suppress or unsuppress a feature (by feature id): "
          "suppressed features are skipped by regeneration but keep their "
          "parameters; anything referencing their outputs errors like a "
          "deleted reference. One undoable edit.")


_reference("set_rollback", "Move the rollback bar: regeneration executes only "
          "features [0, index); -1 = end. Features after the bar keep their "
          "params but contribute nothing — use it to inspect intermediate "
          "history states: its answer's digest is the bodies as of the bar. "
          "Not undoable (a view, not an edit); "
          "get_state.rollback shows the position.")


@mcp.tool(description="Undo the last document edit. Prefer this over "
          "compensating edits when a step went wrong. On a part tab the "
          "answer carries the bodies' digest, feature_count and revision, "
          "so what it gave back needs no get_state.")
def undo() -> dict:
    return _call("undo")


_reference("redo", "Redo the last undone edit.")


@mcp.tool(description="Replace the document with an empty one. Not undoable "
          "— save first if the current model matters.")
def new_project() -> dict:
    return _call("new_project")


@mcp.tool(description="Load a Loböle project (.json) from a path, "
          "replacing the current document. A damaged file is refused, "
          "naming it; `dropped` lists what the load kept going past (a file "
          "the index does not list; a saved result that does not read, "
          "`derived`), and while a drop that is not derived stands, "
          "save_project to the same path is refused.")
def open_project(path: str) -> dict:
    return _call("open_project", path=path)


@mcp.tool(description="Save the document. A .lob path (the default "
          "format) is a project DIRECTORY, git-backed: every save commits "
          "with `message`. A .json path is the single-file interchange "
          "form. A .dsgn (the extension before the rename) is read, never "
          "written: saving the project opened from one writes the .lob "
          "beside it with its history, and the reply's `path` says where. "
          "A save back over a project whose open_project reported a "
          "drop that is not derived is refused, naming it; "
          "overwrite_lossy: true saves anyway and the reply's `warning` "
          "says what was lost. A save to another path is never refused "
          "for it.")
def save_project(path: str, message: str = "save",
                 force: bool | None = None,
                 overwrite_lossy: bool | None = None) -> dict:
    return _call("save_project", path=path, message=message,
                 **_given(force=force, overwrite_lossy=overwrite_lossy))


@mcp.tool(description="Export to STEP, the exact B-Rep other CAD opens. "
          "The active part (or `part`, from any tab), or the active "
          "assembly at its solved placements. The file keeps names, "
          "colours and structure: each (part, configuration, material) is "
          "one product, named \"part (configuration) material\", placed once "
          "per occurrence in its configuration; sub-assemblies nest. `body` "
          "exports one body of a "
          "part by index.")
def export_step(path: str, body: int | None = None,
                part: str | None = None) -> dict:
    return _call("export_step", path=path, **_given(body=body, part=part))


@mcp.tool(description="Export to binary STL (re-meshed at "
          "`stl_deflection_mm`, the chord deviation, smaller = finer). Whole document fuses to one solid, or one body "
          "by index. STL is how a part gets 3D-printed. The reply reports "
          "the source shape's watertightness ({watertight, shells, "
          "free_edges}); a non-watertight source still exports, but flag it "
          "before anyone slices the file.")
def export_stl(path: str, body: int | None = None,
               stl_deflection_mm: float | None = None) -> dict:
    return _call("export_stl", path=path,
                 **_given(body=body, stl_deflection_mm=stl_deflection_mm))


_reference("export_brep", "Export to native OCC BREP text (exact, no "
          "tessellation) — useful for kernel bug reports.")


_reference("export_visual", "Export the tessellated mesh to glTF (.glb/.gltf), OBJ, "
          "or PLY, chosen by the path extension — for viewers (Blender, web, "
          "engines), not CAD round-trips. Materials carry through (glTF is "
          "PBR). Works on a part OR the active assembly (all occurrences at "
          "their solved placements). `deflection` sets mesh fineness.")


_reference("export_motion_video", "Film a motion study of the active assembly to a "
          "video: format mp4, webm or gif (encoded by ffmpeg, which must be on "
          "the app's PATH — ping's features.ffmpeg says; without it these are "
          "REFUSED by name) or png (numbered frames into a FOLDER, no "
          "ffmpeg). Omit format and the path's extension says it. It plays "
          "as the Motion window's Play does (the run's poses, 24 samples a "
          "second, interpolated) and each frame is the viewport's own render "
          "of the model, from the CURRENT camera — set_camera / fit_view "
          "{width, height} first. width/height are PIXELS (default the "
          "viewport's; mp4/webm need them even), fps frames per SECOND "
          "(default 60), frames defaults to as many as Play takes, quality "
          "low|medium|high. No run needed (without a fresh one it makes its "
          "own, collision-free); the assembly is not moved. Blocks until the "
          "file is written and answers its "
          "bytes; rpc('export_motion_video', {..., 'async': True}) with "
          "motion_video_status for a long one.")


_reference("list_parts", "List the project's tabs (parts and assemblies) with "
          "each one's type and stable id, and which is active. Modeling tools "
          "act on the active part; assembly tools on the active assembly.")


_reference("add_part", "Add a new empty part to the project and switch to it "
          "(subsequent modeling goes into it). Returns its index.")


_reference("set_active_part", "Switch the active part by index (from list_parts). "
          "The document tools then act on that part.")


_reference("delete_part", "Delete a part by index (never the last one).")


_reference("rename_part", "Rename an existing part/assembly/drawing tab so the "
          "default 'Part 1' isn't permanent (it feeds BOM + add_occurrence). "
          "`part` is a name, stable id, or slot index (from list_parts). "
          "Occurrences reference parts by stable id, so the rename is safe.")


# ---------------------------------------------------------------------------
# Assemblies (A2): an assembly tab holds occurrences — placed instances of
# parts. list_parts reports each tab's type ("part"/"assembly") and stable id.
# ---------------------------------------------------------------------------

_reference("add_assembly", "Add a new empty ASSEMBLY tab to the project and switch "
          "to it. An assembly places copies (occurrences) of parts by "
          "transform. Part-editing tools do not act on an assembly tab — use "
          "add_occurrence. Returns its index and stable id.")


_reference("add_occurrence", "Place a part OR ANOTHER ASSEMBLY into the ACTIVE assembly "
          "as an occurrence (A5 sub-assemblies). part = the name of either, "
          "resolved to a stable id, so renames are safe; parts are matched "
          "first. transform = {position:[x,y,z], orientation:[x,y,z,w] "
          "quaternion}; omitted = identity. fixed=true grounds it as the "
          "assembly anchor; visible=false hides it. A sub-assembly is one "
          "rigid unit: it contributes its whole subtree to the BOM, mass and "
          "clash checks, and a reference that would close a cycle is refused. "
          "configuration (name or id) places it in that configuration of what "
          "it places -- a part's or a sub-assembly's; two placements of one "
          "part can differ (a pawn and a rook of one piece part), and one the "
          "part lacks or does not build in is refused. material (a slug) "
          "makes this placement of that material: every body of it, for "
          "mass, BOM, rendering and STEP. flexible=true places a "
          "sub-assembly whose own mechanism moves here "
          "(set_occurrence_flexible). Change them later with "
          "set_occurrence_configuration / set_occurrence_material. "
          "Returns {id, part_id, ref_kind}.")


_reference("remove_occurrence", "Remove an occurrence from the active assembly by id.")


_reference("set_occurrence_transform", "Reposition an occurrence. transform = {position:[x,y,z], "
          "orientation:[x,y,z,w] quaternion}. Undoable. Returns the occurrence.")


_reference("set_occurrence_visible", "Show or hide an occurrence (hidden ones are not "
          "rendered or pickable). Undoable.")


_reference("set_occurrence_fixed", "Ground an occurrence (fixed=true) or free it. A fixed "
          "occurrence is the assembly's immovable anchor. Undoable.")


_reference("rename_occurrence", "Rename an occurrence (its display label; the referenced "
          "part is unchanged). Undoable.")


_reference("set_occurrence_configuration", "Choose which configuration of the placed part or "
          "sub-assembly an occurrence uses: configuration = a name or id, "
          "or None for that document's active one. Two placements of one "
          "part can differ, and the part is drawn, measured, mated and listed "
          "in the BOM as that configuration; one it lacks or does not build "
          "in is refused, saying which feature fails. Undoable.")


_reference("set_occurrence_material", "What this placement of a part is made of: material = "
          "a slug, or \"\" for the part's own. It overrides every body's "
          "material for mass, BOM, rendering and STEP (one piece part placed "
          "in maple and in walnut is one part). A sub-assembly placement is "
          "refused, as is an unknown slug. Undoable.")


_reference("list_occurrences", "List the active assembly's occurrences: id, name, "
          "part_id, transform, visible, fixed, configuration_id (0 = the "
          "placed document's active configuration) and material (present "
          "when the placement overrides its part's).")


_reference("add_mate", "Add a MATE between two occurrences of the active "
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
          "axes (default true, so touching faces mate); rotation_deg/offset/dx/dy "
          "and ball_rotation ([x,y,z,w]) set the DOF pose — except on a "
          "FASTENED mate, which leaves nothing free and whose `rotation_deg` "
          "(degrees) is the AUTHORED twist about the connector axis: "
          "flipped says which side, rotation_deg says which way up, and two "
          "faces can be right on both counts and still land a quarter turn "
          "out. offset slides `b` "
          "along +Z of `a`'s connector frame (a's outward face normal) — for "
          "revolute it's the axial seat depth (slide up/down the shaft), for "
          "slider/cylindrical the DOF, for planar the plane gap; POSITIVE = "
          "along a's normal. Solves immediately.")


_reference("set_mate", "Adjust a mate (by id) and re-solve — how you MOVE a "
          "mechanism numerically. rotation_deg (revolute/cylindrical angle, "
          "degrees — on a FASTENED mate the same field is its authored "
          "twist about the connector axis, the GUI's Rotate, not a pose), "
          "offset (slide along +Z of a's connector normal: "
          "slider/cylindrical DOF, revolute axial seat depth, planar gap; "
          "POSITIVE = along a's outward normal), dx/dy (planar in-plane "
          "slide), ball_rotation ([x,y,z,w] for a ball), flipped, or name. "
          "e.g. set a revolute's rotation_deg to swing a hinge, or its offset to "
          "seat the part along the shaft.")


_reference("remove_mate", "Remove a mate (by id) and re-solve. Occurrences it "
          "positioned keep their last solved pose.")


_reference("list_mates", "List the active assembly's mates: id, name, type, the "
          "two connectors, DOF pose, and a per-mate 'error' when a connector "
          "ref is dangling or the mate is over-constrained.")


_reference("solve_assembly", "Re-solve the active assembly's mates (place every "
          "mated, non-fixed occurrence by frame alignment). Always returns "
          "errored_mates, placed, occurrences, placed_by_mates, "
          "placed_by_transform and mates[] (each with its status); the loop "
          "keys — loop_mates, dof, loops_closed, residual_mm, "
          "redundant_mates — only when the assembly HAS mate loops, so read "
          "them with a default. Solves run "
          "automatically after mate/occurrence edits; call this to inspect.")


_reference("add_exploded_view", "Add a named exploded view to the active assembly "
          "(A4). Returns its id. Views are persistent; apply one with "
          "set_explode.")


_reference("remove_exploded_view", "Remove an exploded view by id.")


_reference("list_exploded_views", "List the active assembly's exploded views (id, name, "
          "per-occurrence offsets) plus the applied view id and factor.")


_reference("set_explode_step", "Set one occurrence's world-space offset in an "
          "exploded view (creates the step if missing). offset is [x,y,z] "
          "at factor 1.")


_reference("auto_explode", "Seed an exploded view radially: every occurrence "
          "pushed away from the assembly centroid, scaled by assembly size. "
          "A starting point to refine with set_explode_step. Omit `view` (or "
          "pass 0) to auto-create a fresh exploded view; the returned `view` "
          "is its id.")


_reference("set_explode", "Apply an exploded view to the display (transient, "
          "never saved): view id (0 = none) and/or factor 0..1. The solved "
          "transforms are untouched — screenshot an exploded assembly with "
          "set_explode + screenshot.")


_reference("bom", "Bill of materials for the active assembly (A4): one "
          "row per (part, configuration, material) placed, with "
          "configuration (its name; \"\" = the part's active one), "
          "configuration_id, quantity, material, unit/total mass "
          "(grams; null when a body has no material density) and a mass "
          "total. Derived fresh from the document on every call.")


_reference("export_bom", "Write the active assembly's bill of materials to a "
          "CSV file at `path` (same rows as `bom`, with a `configuration` "
          "column after `part`).")


_reference("import_step", "The importer for EVERY 3D file, meshes "
          "included, whatever its name says: a STEP, IGES, STL, OBJ, PLY, "
          "glTF (.glb/.gltf) or BREP "
          "file's bodies as an Import feature (any other extension is "
          "refused; a DXF or SVG goes into a sketch with import_sketch). "
          "as_assembly=True makes a STEP or IGES parts + an assembly. "
          "`representation` is what an STL becomes: \"recognize\" rebuilds "
          "the planes, cylinders, cones and spheres its triangles were made "
          "from as true faces (a hole has a diameter and an axis), "
          "\"facets\" keeps a face per triangle; absent, the user's "
          "mesh_representation preference. `min_sides` (with recognize; 5 "
          "to 1024, default 12) is the fewest sides a ring of facets needs "
          "to be round. The reply's `recognition` lists every cylinder, cone "
          "and sphere found, the polygons left flat, and `fell_back` when "
          "the rebuild failed a check and the mesh was kept as facets. "
          "`units` is the "
          "length unit an STL/OBJ/PLY/BREP was drawn in (mm default, cm, m, "
          "in) — those formats carry none; a STEP or IGES declares its own "
          "and a glTF is metres (read as mm, its Y up stood on our Z), and "
          "each refuses it. keep_origin=True keeps the file's OWN coordinates: "
          "without it the import is centred on its bounding box, so a bought "
          "part loses the placement it was exported at and every one of them "
          "lands on the origin. up_axis (default auto: read from the file's "
          "header, a SolidWorks file is +y) says which axis of the FILE is "
          "up, and turn_deg turns it about world Z after; the reply's "
          "`up_axis` says what was applied and why. A mesh is counted "
          "first: the reply's `mesh` {triangles, expected_s, limit} says what "
          "it was expected to cost, and past `limit` the call is refused "
          "naming the time unless allow_large_mesh=True. The read runs as "
          "the `import` background job: cancel_background_job "
          "{kind: \"import\"} stops it. A recognised mesh sews nothing, "
          "so it is not held to `limit`. placement {translate_mm, "
          "rotate_deg} and scale place it after the centring; align_import "
          "writes them from a floor, three points, the principal axes or a "
          "known distance.")


_reference("checkpoint_create", "Save the WHOLE project and name this state a "
          "version (save_project to a .lob project first). Name a version "
          "before risky multi-step edits; restoring beats reconstructing.")


_reference("checkpoint_list", "List the project's named versions (name + unix "
          "time, newest first).")


_reference("checkpoint_restore", "Put the WHOLE project back to a named version and "
          "reopen it. The restore is itself recorded, so the state you left "
          "stays in history_log.")


_reference("history_log", "History of a directory (.lob) project: every save "
          "is a version, named ones are tagged. Returns {log: [{hash, "
          "subject, unix, named}]}, newest first.")


_reference("history_restore", "Put the WHOLE project back to a version (hash, tag "
          "like 'cp/v1', or any git rev) and reopen it. History stays linear "
          "— the restore is recorded on top, so nothing is lost.")


# ---------------------------------------------------------------------------
# View & verify
# ---------------------------------------------------------------------------

@mcp.tool(description="Capture the viewport as a PNG and return the image — "
          "look at it. Optional view (front|back|left|right|top|bottom|iso) "
          "orients + fits first; iso for shape checks, axis views for "
          "dimensional ones. Optional path also keeps the file on disk.")
def screenshot(view: str = "", path: str = "",
               fit: bool | None = None) -> Image:
    out = path or os.path.join(tempfile.gettempdir(), "lobole_mcp_shot.png")
    _call("screenshot", path=out, **_given(view=view or None, fit=fit))
    return Image(path=out)


@mcp.tool(description="Snap the camera to a standard view "
          "(front|back|left|right|top|bottom|iso); fit=true (default) frames "
          "the whole model. Instant — safe to screenshot right after.")
def view(name: str, fit: bool = True) -> dict:
    return _call("view", name=name, fit=fit)


_reference("fit_view", "Frame the whole model at the current orientation.")


_reference("get_settings", "Read the user's preferences: GPU device, font size, "
          "shadows/SSAO/shadow map, autosave interval, the mouse navigation "
          "(nav_preset; nav_orbit/nav_pan/nav_zoom/nav_roll gesture lists; "
          "speeds; wheel direction). Also returns `path` "
          "(the settings file) and `devices` (what gpu_device may be set to). "
          "These are per-USER and persist; a project's own settings do not "
          "live here.")


_reference("set_settings", "Change user preferences, e.g. "
          "{\"ssao\": \"full\", \"ui_font_scale\": 1.2}. Validates first "
          "— a bad value changes nothing and names itself — then applies and "
          "writes the file. A gpu_device change rebuilds the viewport's device "
          "in place and answers after it, with `renderer`; everything else "
          "is immediate.")


@mcp.tool(description="Viewport render-quality options; all fields optional, "
          "returns the resulting state (call with no arguments to just "
          "read). shadows: key-light shadow map on/off. ssao: 'off'|'half'|"
          "'full' — ambient occlusion off, half-res (default) or full-res. "
          "shadow_map: 1024|2048 texels. Useful for A/B-ing renders via "
          "screenshot. Needs a rendering instance — gui or --offscreen; "
          "errors under --headless.")
def set_render_options(shadows: bool | None = None, ssao: str | None = None,
                       shadow_map: int | None = None) -> dict:
    return _call("set_render_options", **_given(
        shadows=shadows, ssao=ssao, shadow_map=shadow_map))


_reference("select_face", "Select a B-Rep face. Preferred: by STABLE element name "
          "(list_elements), e.g. element='2/g4' — survives regeneration. "
          "Legacy: by render mesh id + face index (get_state bodies[].mesh; "
          "ids change every regeneration). Returns the face normal and "
          "center. Follow with orient_to_face to look straight at it.")


@mcp.tool(description="Direct camera control (view/fit only snap to standard "
          "views): optional target [x,y,z], direction [x,y,z] (unit offset "
          "from target toward the camera), up [x,y,z], and distance (mm, "
          "zoom). Each field optional, applied in that order. `up` sets "
          "the roll and is refused within 1 degree of the view direction. "
          "Returns the resulting position/target/distance.")
def set_camera(target: list | None = None, eye: list | None = None,
               direction: list | None = None, up: list | None = None,
               distance: float | None = None) -> dict:
    return _call("set_camera", **_given(target=target, eye=eye,
                                        direction=direction, up=up,
                                        distance=distance))


_reference("reset_layout", "Rebuild the default panel layout (Model left, "
          "Properties right, 3D center) — recovery for a wedged or "
          "misdocked panel arrangement, without restarting the app.")


_reference("orient_to_face", "Orient the camera to look squarely at the selected "
          "face (select_face first).")


@mcp.tool(description="A rendered still of the model, returned as an image. "
          "Works on part AND assembly tabs. Frame with view/fit_view first. "
          "mode='quick' (the default) is the model as the VIEWPORT draws it "
          "— materials, lights, shadows, ambient occlusion, outlines, without "
          "sketches, gizmos or selection — in about a second, supersampled "
          "`supersample` (1-4) times; outlines=False drops the edge lines and "
          "line_weight (0.25-4 px of the output image) thickens them (quick "
          "only). mode='path' is the offline path tracer "
          "(needs a ray-tracing GPU; the app freezes for the duration): "
          "samples=64 is a fast preview, 512+ presentation quality, ground "
          "adds a shadow-catcher plane. backdrop (path only): 'studio' (a "
          "neutral sweep), 'environment' (the studio HDRI) or 'sky' (an "
          "outdoor sky that also lights the model, its sun the key light; "
          "sun_azimuth_deg CCW from +X and sun_elevation_deg place the sun, "
          "defaults derived from the camera and 40). Optional path also "
          "keeps the PNG.")
def render_still(width: int = 1920, height: int = 1080, mode: str = "quick",
                 supersample: int = 2, samples: int = 512, seed: int = 1,
                 ground: bool = True, outlines: bool = True,
                 line_weight: float = 0.0, path: str = "",
                 backdrop: str | None = None,
                 sun_azimuth_deg: float | None = None,
                 sun_elevation_deg: float | None = None,
                 fit: bool | None = None) -> Image:
    out = path or os.path.join(tempfile.gettempdir(), "lobole_mcp_still.png")
    # A backdrop or sun the caller GAVE goes to the app in either mode, so a
    # quick still asked for a sky is refused there rather than quietly drawn
    # without one (CLAUDE.md, "Refuse, don't substitute").
    given = _given(backdrop=backdrop, sun_azimuth_deg=sun_azimuth_deg,
                   sun_elevation_deg=sun_elevation_deg, fit=fit)
    if mode == "quick":
        extra = {"line_weight": line_weight} if line_weight else {}
        r = _call("render_still", width=width, height=height, mode="quick",
                  supersample=supersample, outlines=outlines, path=out,
                  **extra, **given)
    else:
        r = _call("render_still", width=width, height=height, mode=mode,
                  samples=samples, seed=seed, ground=ground,
                  outlines=outlines, path=out, **given)
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


_reference("add_study", "Create a FEM study on the active part. kind = "
          "'static' (default) | 'modal' | 'topology' | 'buckling' | "
          "'thermal' (steady heat: temperature, heat_flux, convection and "
          "heat_generation loads; docs/simulation/README.md#thermal). Returns "
          "{id, name, kind}. A study fixes/loads faces and solves for "
          "stress. A loaded part gets a static study of each way it is "
          "loaded and a buckling study of each load that compresses "
          "something, with requirements on factor_of_safety and "
          "buckling_factor (both the governing values).")


_reference("remove_study", "Delete a study (by id) and its cached results.")


_reference("list_studies", "List studies: id, name, evaluated mesh_size, loads, "
          "refinement count, has_results, and stale (results outdated by an "
          "edit).")


_reference("set_study", "Set a study's name, global mesh size, analysis type, or "
          "loads. mesh_size (mm; 0 = auto) may be a number or an expression "
          "string. type is 'static' (stress), 'modal' (natural frequencies), "
          "'topology' (generative shape), 'buckling' (load factors) or "
          "'thermal' (temperature). thermal_load {from_study, stress_free_c?} "
          "or {uniform_c, stress_free_c} expands a static study under a "
          "temperature. "
          "num_modes sets how many modes a modal study computes, or how many "
          "factors a buckling one does (default 3). gravity ([x,y,z] m/s^2, e.g. [0,0,-9.81]) applies "
          "self-weight to a static study (needs a material density). For a "
          "topology study: volume_fraction (0..1 target kept, number or "
          "expression), filter_radius (mm minimum member size; 0 = auto), and "
          "max_iterations.")


_reference("add_keep_face", "Add a KEEP FACE to a topology study (frozen solid, so a "
          "bolt boss or bearing seat survives the optimization). face is "
          "{feature_id, element}.")


_reference("remove_keep_face", "Remove a keep face ({feature_id, element}) from a "
          "topology study.")


_reference("add_refinement", "Add a per-face mesh refinement (smaller elements on one "
          "face). face is {feature_id, element}; size is mm (number or "
          "expression).")


_reference("remove_refinement", "Remove the refinement on a face ({feature_id, element}).")


_reference("add_load", "Add a boundary condition to a study. type is 'fixed' "
          "(zero displacement), 'pin' (a rigid pin in a CYLINDRICAL face -- a "
          "bore or a shaft -- holding it only where the part presses on it, "
          "free to turn unless fix_rotation: true; a static part study's), "
          "'force' (a total vector in N over the face), 'pressure' (MPa "
          "along the inward normal), 'torque' (torque_nmm) or 'remote' "
          "(force_n at point_mm); help('add_load') has them all. face is "
          "{feature_id, element}. magnitude is N/MPa (number or expression); "
          "direction is [x,y,z] for a force. region? puts it on PART of its "
          "face: {normal, from_mm, to_mm} (a band) or {center_mm, radius_mm}. "
          "Returns {id} of the new load. Every loaded body needs a support "
          "(fixed or pin); a part free to turn on one pin is refused.")


_reference("remove_load", "Remove a load (by id) from a study.")


_reference("mesh_study", "Mesh a study's bodies without solving (a quick check). "
          "Returns per-body node/element counts and worst element quality.")


_reference("run_study", "Mesh + solve a study now (explicit; never automatic). "
          "A static study returns nodes, elements, max_von_mises_mpa {value, "
          "at, body}, max_displacement_mm, factor_of_safety (vs yield), and "
          "stale:false. A modal study returns per-body frequencies_hz (the "
          "natural frequencies). A buckling study returns per-body "
          "buckling_factors, buckling_factor, knockdown, yield_factor and "
          "governing_factor, and governing_factor at the top. A TOPOLOGY study runs ASYNC (minutes on a "
          "worker thread): it returns {started:true} immediately — poll "
          "study_status until running:false, then read study_results. "
          "async_run=True runs a static/modal study the same way — use it "
          "for big meshes (a synchronous solve can outlive the client "
          "timeout; note a running static solve cannot be cancelled). "
          "Errors name the offending face/material/support.")


_reference("study_status", "Poll an async topology run. While running, returns "
          "{running:true, iteration, max_iterations, compliance, "
          "volume_fraction}. Once done, {running:false, has_results, converged, "
          "cancelled, iterations, stale}.")


_reference("cancel_study", "Cancel an in-progress topology run (keeps the best "
          "shape found so far as a 'cancelled' result). Returns {cancelled}.")


_reference("study_results", "Read a study's cached results (never solves). Same "
          "summary as run_study plus the current stale flag. For topology: "
          "iterations, converged, volume_fraction, compliance.")


_reference("export_topo_stl", "Export a topology study's optimized shape (the "
          "iso-surface, Taubin-smoothed) to a binary STL for 3D printing. "
          "threshold (0..1, 0 = auto volume-matched) sets how much material to "
          "keep; smooth is the number of smoothing passes (default 3). The "
          "reply reports {triangles, boundary_edges, watertight} — flag a "
          "non-watertight mesh before anyone slices it. Note: the organic STL "
          "cannot be re-imported as a B-Rep; view results via "
          "set_result_display field='shape'.")


_reference("set_topo_ghost", "Toggle the topology SHAPE GUIDE: a translucent overlay "
          "of a study's optimized shape in the normal modeling workspace, to "
          "re-model clean geometry against. on=false hides it. The opaque "
          "body can hide the guide from most angles — to LOOK at a result, "
          "set_result_display field='shape' is the reliable view.")


_reference("set_result_display", "Show a study's result overlay in the viewport (for a "
          "screenshot). field is 'von_mises', 'displacement', or 'none' for a "
          "static/modal study, or 'shape' (grey optimized iso-surface) / "
          "'density' (colormap) for a topology study. deform_scale exaggerates "
          "the deformed shape (0 = undeformed). For a modal or buckling study, "
          "mode selects "
          "which mode shape (0-based). For a topology 'shape' view, threshold "
          "(0..1, 0 = auto) sets the iso-surface level. Enters simulation mode; "
          "display-only, like set_explode.")


# ---------------------------------------------------------------------------
# Drawings (workstream 13): a drawing tab holds a 2D sheet with projected views
# of a part or assembly. list_parts reports drawing tabs with type "drawing".
# ---------------------------------------------------------------------------

_reference("create_drawing", "Add a new DRAWING tab (a 2D sheet) and switch to it. "
          "Optional sheet = {size: 'a4'|'a3'|'a2'|'a1'|'a0'|'letter', "
          "landscape: bool, title_block: {project, document, author, date}}. "
          "Views and dimensions are added in later tools. Returns its index, "
          "stable id, and sheet size.")


_reference("list_drawings", "List the project's drawing tabs with each one's stable "
          "id, index, and view/dimension counts.")


_reference("add_drawing_view", "Add a projected VIEW of a part to the ACTIVE drawing. "
          "source = the part's name. orientation = 'front'|'back'|'left'|"
          "'right'|'top'|'bottom'|'iso' (default front). scale = drawing:model "
          "ratio, e.g. 0.5 for 1:2 (default 1.0). position = [x,y] sheet mm "
          "(default sheet centre). base_view = a view id to fold this one from "
          "(D2b). configuration = the name of one of the part's "
          "configurations to draw (default: whatever it has active) — 'Flat' "
          "draws a sheet-metal part's blank with its bend lines. "
          "rotation_deg turns the view on the sheet, counter-clockwise "
          "(a part modelled lying along X drawn upright is front at 90). "
          "The view is added but NOT projected — call update_drawing. "
          "Returns the new view id.")


_reference("remove_drawing_view", "Remove a view (by id) from the active drawing.")


_reference("set_drawing_view", "Edit a view in place: position [x,y] sheet mm, scale, "
          "name, show_hidden, quality ('exact'|'fast'), configuration (a "
          "configuration of the source part, by name; '' leaves it alone and "
          "'base' draws whatever the part has active), base_view (0 clears "
          "folding), rotation_deg (the view turned on the sheet, "
          "counter-clockwise). Re-projects. A "
          "folded child's shared axis follows its base — the reply's "
          "effective_position/folded_axes say when a requested coordinate "
          "was overridden.")


_reference("set_drawing_sheet", "Edit the active drawing's sheet after creation: size "
          "('a4'..'a0'|'letter', case-insensitive — unknown sizes ERROR), "
          "landscape, title_block fields. Views keep their positions; run "
          "check_drawing to see what no longer fits.")


_reference("check_drawing", "Validate the active drawing's sheet layout — the "
          "checks a drafter does by eye: every view inside the border, no "
          "view/view or view/title-block overlaps, dimensions on the paper, "
          "nothing dangling or stale. Returns {ok, issues:[{kind, detail, "
          "view?, other_view?, dim?}]}. update=False judges the cached "
          "projection instead of re-projecting. Run before export_drawing "
          "(whose reply echoes the same issues).")


_reference("update_drawing", "Re-project every view of the active drawing (the "
          "explicit hidden-line-removal pass — expensive, never automatic). "
          "Returns per-view curve counts (visible/hidden/silhouettes) and any "
          "projection error. Run after adding views or editing the source part.")


_reference("list_drawing_views", "List the active drawing's views with their orientation, "
          "scale, position, projected curve count, and whether each is stale "
          "(source changed since last projection).")


_reference("export_drawing", "Export the active drawing to a vector file (path ending "
          ".svg or .pdf). Re-projects first so the file reflects the current "
          "geometry, then writes the sheet — border, title block, every view's "
          "visible (solid)/hidden (dashed) edges, and each resolved dimension. "
          "Returns the path, view count, and the check_drawing issue list "
          "(export never blocks on issues — read them).")


_reference("list_drawing_geometry", "List a projected view's dimensionable geometry: each "
          "distinct source edge / silhouette with the `ref` to pass to "
          "add_drawing_dim, its type (line/circle/curve), sheet-mm endpoints "
          "p1/p2 and `bbox` [[min],[max]] (how far a curve reaches -- a "
          "curved outline's top is neither end), or circle centre+radius, "
          "`visible`, `closed` (a full circle = a "
          "bore rim; false = an arc such as a corner fillet — do NOT put a "
          "diameter dim on those), and `source_feature` {type,name} (part "
          "sources). Call update_drawing first.")


_reference("add_drawing_dim", "Add a driven dimension to a view of the active drawing. "
          "kind = 'distance'|'distance_x'|'distance_y'|'radius'|'diameter'|"
          "'angle'|'hole_callout'|'pattern_callout'. refs = a list of {edge OR "
          "sil_face: {feature_id, element} (from list_drawing_geometry), "
          "occurrence?: source_id, end?: 0 whole / 1|2 endpoint, extreme?: "
          "'left'|'right'|'top'|'bottom' -- the curve's furthest point that "
          "way on the sheet, e.g. a piece's top; not with end}: two refs for "
          "distance/angle, one for radius/diameter, ONE closed circle of a "
          "drilled hole for hole_callout ('Ø4.5 THRU', 'Ø4.5 CSK Ø9.5 × 90°' "
          "— text derived from the Hole feature; part sources only), 2+ "
          "equal-radius circles for pattern_callout ('3× Ø4.5', '+ PITCH p' "
          "when collinear and evenly spaced). offset = [x,y] label offset in "
          "sheet mm; OMIT it to auto-place the dim clear of the view. The "
          "value is MEASURED from the projected geometry (read-only). Returns "
          "the id, measured value/text, and the stored offset.")


_reference("remove_drawing_dim", "Remove a dimension (by id) from the active drawing.")


_reference("list_drawing_dims", "List the active drawing's dimensions with each one's "
          "kind, measured value + formatted text, and whether it resolved "
          "(false = a ref dangles after a source edit).")


# ---------------------------------------------------------------------------
# Escape hatch: raw UI events (for driving/testing the Loböle GUI itself)
# ---------------------------------------------------------------------------

_UI_ACTIONS = ("menu_click", "item_click", "item_double_click", "item_input",
               "item_hover", "item_combo", "ui_items", "viewport_click",
               "viewport_drag", "key_press", "scroll")


_UI_ACTION_DOC = ("Raw GUI event, for driving or testing the Loböle "
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


_reference("ui_action", _UI_ACTION_DOC)




# ---------------------------------------------------------------------------
# Meta: reach everything, and read the manual for it
# ---------------------------------------------------------------------------

@mcp.tool(description="Call any Loböle RPC by name — the whole API, not "
          "just the tools listed here. Assemblies, drawings, studies, "
          "configurations, sketch constraints, patterns, checkpoints, "
          "exports and the GUI-driving actions are all reached this way: "
          "rpc('add_mate', {...}). Use describe() for the method list and "
          "help('add_mate') for its arguments and gotchas. Importing a "
          "file: rpc('import_step', {path}) reads EVERY 3D format (STEP, "
          "IGES, STL, OBJ, PLY, glTF, BREP), rpc('import_sketch', {path}) a "
          "DXF or SVG "
          "into a sketch. Prefer this over "
          "guessing; a wrong method name returns the list of close matches.")
def rpc(method: str, params: dict | None = None) -> dict:
    # Straight to the app: its row is the contract, its defaults the only
    # ones. Python wrappers in front of it kept a hand-copied subset of each
    # method's keys and drifted from them — set_material without body_name,
    # delete_feature without feature_id — and a key the app accepted died
    # here as a Python TypeError. ui_action is this server's own, a door to
    # the GUI-action RPCs, and the one name that is not an RPC.
    if method == "ui_action":
        return ui_action(**(params or {}))
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


@mcp.tool(description="Run a python script against the attached instance — "
          "the loop-and-computed-args path: ONE round trip for work that "
          "would be dozens of tool calls (feature patterns with computed "
          "positions, build-measure-adjust loops, batch queries). The "
          "script runs in a subprocess with `d` PRE-BOUND to a connected "
          "lobole.client.Client: call d.request(method, params) "
          "/ d.call(method, **params) directly — no imports, no port "
          "(LOBOLE_PORT is set; the lobole package is importable "
          "for anything fancier). print() what matters: stdout/stderr come "
          "back (capped at 20000 chars each). Relative file paths land in "
          "the returned cwd, a scratch directory. Same trust domain as the "
          "rpc tool — it drives the same instance with the same powers.")
def execute_script(script: str, timeout: float = 120.0) -> dict:
    import pathlib
    import subprocess
    _connect_if_needed()
    port = _attached.get("port") if _attached else None
    if not port:
        raise RuntimeError("no instance attached")
    # The directory holding the lobole package, for the script's sys.path.
    pkg_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cwd = tempfile.mkdtemp(prefix="designer-script-")
    prelude = (
        "import os, sys\n"
        f"sys.path.insert(0, {pkg_root!r})\n"
        "from lobole.client import Client\n"
        "d = Client(int(os.environ['LOBOLE_PORT']), timeout=600.0)\n"
        "# --- script ---\n")
    path = pathlib.Path(cwd, "script.py")
    path.write_text(prelude + script)
    env = dict(os.environ)
    env["LOBOLE_PORT"] = str(port)
    try:
        proc = subprocess.run([sys.executable, str(path)], cwd=cwd, env=env,
                              capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        raise RuntimeError(
            f"script exceeded {timeout:g}s (pass a larger timeout for long "
            "solves/renders; partial stdout:\n"
            + _text(e.stdout)[-2000:] + ")")

    def cap(s):
        return s if len(s) <= 20000 else s[:10000] + "\n…[capped]…\n" + s[-10000:]
    out = {"exit": proc.returncode, "stdout": cap(proc.stdout),
           "stderr": cap(proc.stderr), "cwd": cwd,
           "attached": attached_to()}
    return out


def _connect_if_needed():
    global _client
    if _client is None:
        _connect()


@mcp.tool(description="Which Loböle instances are running, and which one "
          "these tools are talking to. Instances announce themselves when "
          "they open a remote port, so no port has to be guessed. Use this "
          "when a call reports several instances (set LOBOLE_PORT to "
          "choose) or nothing reachable.")
def instances() -> dict:
    live = _running_instances()
    cmd = _start_hint() or "lobole"
    return {"attached": attached_to(),
            "instances": live,
            "hint": (f"Start one with `{cmd} --remote {DEFAULT_PORT}`."
                     if not live else
                     "Pin one with LOBOLE_PORT=<port> if several are up.")}


@mcp.tool(description="Documentation for one RPC: what it does, its "
          "arguments, and the traps. Call with no argument for the index of "
          "everything reachable through rpc(). This is the prose that used "
          "to be preloaded as 118 tool descriptions — same words, fetched "
          "when they matter.")
def help(method: str = "") -> dict:
    if not method:
        return {"methods": sorted(_REFERENCE),
                "exposed_as_tools": sorted(_EXPOSED),
                # By file, since the method names do not say it: the one
                # importer for every 3D format is called import_step.
                "importing": {
                    "STEP, IGES, STL, OBJ, PLY, glTF, BREP": "import_step",
                    "DXF, SVG (into a sketch)": "import_sketch"},
                "hint": "help('add_mate') for one; describe() for the "
                        "app's own authoritative type tables."}
    if method in _EXPOSED and method not in _REFERENCE:
        return {"method": method,
                "doc": "Exposed directly as the MCP tool of the same "
                       "name — see its tool description."}
    # The lookup is lobole.client.method_help, which the python client's
    # help() also is: the app's describe row (parameters, their types, what
    # comes back), with this server's longer prose in place of the row's
    # one-liner where it keeps some.
    from lobole.client import method_help
    from lobole.errors import Refused
    reference = {name: {"doc": e["doc"]} for name, e in _REFERENCE.items()}
    try:
        out = method_help(_call, method, reference)
    except Refused as e:
        raise ValueError(str(e)) from None
    if method not in reference:
        out["source"] = ("the app's describe row; reach it with "
                         f"rpc('{method}', {{...}})")
    return out

@mcp.prompt(name="modeling-workflow",
            description="How to model a part in Loböle with these tools")
def modeling_workflow() -> str:
    return WORKFLOW


def _restrict_tools(keep):
    """Hide every tool but `keep`. Used by benchmark arms.

    Refuses an unknown name rather than serving the rest: serving the full
    surface after being asked for six tools would silently turn a benchmark
    arm into a duplicate of the arm it is being compared against, and the run
    would look valid. So it also checks that what is left is exactly `keep`.
    """
    import anyio  # type: ignore[import-not-found,unused-ignore]
    registered = {t.name for t in anyio.run(mcp.list_tools)}
    missing = [t for t in keep if t not in registered]
    if missing:
        raise SystemExit(f"--only-tools: no such tool(s): {', '.join(missing)}. "
                         f"Known: {', '.join(sorted(registered))}")
    for name in registered - set(keep):
        mcp.remove_tool(name)
    left = {t.name for t in anyio.run(mcp.list_tools)}
    if left != set(keep):
        raise SystemExit("--only-tools: the server still lists "
                         f"{', '.join(sorted(left - set(keep)))}; refusing to "
                         "serve the full surface as if it were restricted")


def main():
    global _port, _autostart_enabled
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=None,
                        help="pin a port; omit to attach to the running "
                             "instance that announced itself")
    parser.add_argument("--no-autostart", action="store_true",
                        help="never spawn an instance; with nothing running, "
                             "refuse and name the start command instead")
    parser.add_argument("--only-tools", default="",
                        help="serve ONLY these tools (comma-separated), "
                             "hiding the rest. For benchmark arms that measure "
                             "what the size of the tool surface costs — "
                             "benchmarks/README.md. rpc/help still reach every "
                             "RPC, so this narrows the SCHEMAS, not the API.")
    args = parser.parse_args()
    if args.port is not None:
        _port = args.port
    if args.no_autostart:
        _autostart_enabled = False
    if args.only_tools:
        _restrict_tools([t.strip() for t in args.only_tools.split(",")
                         if t.strip()])
    mcp.run()  # stdio


if __name__ == "__main__":
    main()
