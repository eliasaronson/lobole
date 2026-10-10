// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteAssembly.h"

#include "cad/CADMaterials.h"
#include "core/Angles.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/MeshCollide.h"
#include "core/WormGear.h"
#include "core/catalog/Catalog.h"
#include "core/features/FrameFeature.h"
#include "core/io/Serialization.h"
#include "core/parts/BearingTable.h"
#include "core/parts/RetainingRingTable.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteProject.h"
#include "ui/DocumentSlots.h"
#include "ui/SelectionPipeline.h"

#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BearingActions.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument
#include "ui/FlexibleActions.h"
#include "ui/RetainingRingActions.h"
#include <map>
#include <set>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Assembly RPCs (add_occurrence..) carved out of RemoteServer::HandleSemantic.
// Act on the active assembly tab / project.
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc AssemblyMethods[] = {
    {"set_active_part",
     "Switch the active tab {part: name|id|index} (legacy: name, index).",
     "part?:string|int, name?:string|int, index?:int>=0", remote::NotInBatch},
    {"delete_part",
     "Delete a part/assembly/drawing tab {part: name|id|index} (legacy: "
     "index); refused while any assembly places it.",
     "part?:string|int, index?:int>=0", remote::NotInBatch},
    {"add_occurrence",
     "Place a part OR another assembly in the active assembly {part: name or "
     "slot id of either, transform?}. Returns {id, part_id, ref_kind}; a "
     "reference that would close a cycle is refused. {flexible: true} places "
     "a sub-assembly whose own mechanism MOVES here (see "
     "set_occurrence_flexible), and answers with the solve's errored_mates. "
     "{configuration: name|id} places it in that configuration of what it "
     "places (a part's or a sub-assembly's; see "
     "set_occurrence_configuration); {material: slug} makes this placement "
     "of that material (see set_occurrence_material).",
     "part:string|int, name?:string, transform?:object, visible?:bool, "
     "fixed?:bool, configuration?:string|int|null, material?:string|null, "
     "flexible?:bool"},
    {"remove_occurrence",
     "Remove an occurrence. Takes `occurrence` as well as `id`.",
     "id, occurrence"},
    {"list_occurrences",
     "List occurrences with transforms, state and ref_kind (part|assembly); a "
     "rider (`attached_to`, a placed fastener) names the faces it is seated "
     "on (its hole) in `attach_seat`, says `attach_error` when they no longer "
     "resolve and `attach_warning` when it rides at a fixed offset; a "
     "sub-assembly also reports leaf_count and the leaves' paths. "
     "{bounds: true} adds each one's WORLD bbox_min/bbox_max — the extent "
     "where it actually sits, which get_state cannot give you because its "
     "boxes are in the part's own space. For a single named point or axis "
     "use `frame` with an occurrence instead.",
     "bounds?:bool", remote::NoFlags,
     "occurrences[]{type,id,name,part_id,ref_kind,part_version,transform,"
     "visible,fixed,configuration_id,attached_to?,attach_offset?,attach_"
     "rotation?,attach_seat?{face,axis,inner_path?},attach_error?,"
     "attach_warning?,material?,flexible?,inner_poses?,inner_mate_dofs?,bbox_"
     "min?,"
     "bbox_max?,leaf_count?,leaves?}"},
    {"set_occurrence_transform",
     "Set an occurrence's world transform {id, transform: {position, "
     "orientation [x,y,z,w] OR rotate {axis, angle_deg}}}. Either half may "
     "be omitted and keeps its current value, so sliding a part along one "
     "axis needs no quaternion.",
     "id?:int>=0, occurrence?:int>=0, transform:object"},
    {"set_occurrence_visible",
     "Show/hide an occurrence. Takes `occurrence` as well as `id` — its "
     "neighbours here spell that argument `occurrence`, and one method "
     "spelling it differently is a refusal in the middle of a loop.",
     "id, occurrence, visible"},
    {"set_occurrence_flexible",
     "Make a placed SUB-ASSEMBLY flexible or rigid {occurrence, flexible} "
     "(docs/assemblies/README.md#flexible-sub-assemblies). Flexible: its "
     "occurrences, mates and relations join this assembly's solve and "
     "motion studies, instanced for THIS placement (two placements pose "
     "independently; the sub-assembly's document is not touched), under the "
     "configuration set_occurrence_configuration chose for it — which is "
     "what holds or suppresses its mates, never this assembly's. A relation "
     "or a study driver here then names one of its mates as {path: "
     "[occurrence], mate}; set_mate {id, path} poses one. Refused, naming "
     "why, for a part, a sub-assembly with nothing fixed, one holding a "
     "freewheel or a click, one that places something flexible itself, or "
     "when this assembly is itself placed flexibly — only one level moves. "
     "Rigid again drops the poses it kept. Answers the occurrence and "
     "`solve` {errored_mates, flexible: the solve_assembly row}.",
     "flexible, id, occurrence"},
    {"set_occurrence_fixed",
     "Fix/free an occurrence for the solver. Takes `occurrence` as well as "
     "`id`.",
     "fixed, id, occurrence"},
    {"rename_occurrence",
     "Rename an occurrence. Takes `occurrence` as well as `id`.",
     "id, name, occurrence"},
    {"set_occurrence_material",
     "What this placement of a part is made of {id, material: slug | \"\" "
     "for the part's own}: every body of it, for mass, the BOM and how it "
     "draws. One piece part placed in maple and in walnut is one part. A "
     "sub-assembly placement is refused (set its parts'); an unknown slug is "
     "refused, naming the known ones.",
     "id, material, occurrence"},
    {"set_occurrence_configuration",
     "Choose which configuration of the placed part or sub-assembly this "
     "occurrence uses {id, configuration: name|id|null}; null = that "
     "document's active one. Two placements of one part can differ -- a "
     "pawn and a rook of one piece part -- and the part is drawn, measured, "
     "mated and listed in the BOM as that configuration; one it does not "
     "build in is refused, saying which feature fails. A part placement, "
     "or a FLEXIBLE one (which runs its mechanism under the configuration), "
     "is re-solved.",
     "configuration, id, occurrence"},
    {"select_occurrence",
     "Select an occurrence in the UI; nothing (or 0) clears it. Takes "
     "`occurrence` as well as `id`.",
     "id, occurrence"},
    {"add_mate",
     "Add a mate {type: fastened|revolute|slider|cylindrical|planar|ball, "
     "a, b: connectors}. A connector is {occurrence: id|name, frame: <Frame "
     "id|name of that part>} (v86 — the mate as DESIGNED: a bolt-circle "
     "centre, a shaft axis), {occurrence, face: {feature_id, element}, "
     "snap_edge?} (a face's centroid + normal, or a snapped hole centre), or "
     "{occurrence, origin: true}; inner_path? reaches into a sub-assembly. "
     "The mate's starting position along whatever it leaves free is "
     "`rotation` (RADIANS) or `rotation_deg`, and `offset` in mm. On a "
     "FASTENED mate — which leaves nothing free — `rotation` is still the "
     "authored twist about the connector axis: `flipped` says which side, "
     "`rotation` says which way up, and two faces can be right on both "
     "counts and still land a quarter turn out. `flipped` "
     "DEFAULTS TO TRUE: the two connectors meet face to face with their "
     "normals opposed, which is what two touching FACES do and the opposite "
     "of what two seat FRAMES want — frame to frame, pass flipped:false, or "
     "the part lands turned over and sunk into whatever it was to sit on. "
     "A mate is ONE joint between two occurrences. To COUPLE two mates' "
     "degrees of freedom — a gear pair, a rack and pinion, a screw, a cam — "
     "the thing to add is a RELATION over those two mates (add_relation); "
     "that is also what drops a geared loop's DOF count. `describe` lists "
     "both vocabularies as `mate_types` and `relation_kinds`. "
     "`derive` READS those numbers off where the two occurrences already "
     "lie, for a fastened mate: 'twist' takes `flipped` and `rotation` and "
     "leaves the anchors coincident (an exact joint, turned the way the part "
     "already roughly was — which is the answer to 'it landed a quarter turn "
     "out'), 'pose' takes all five so the mate holds the parts exactly where "
     "they are, reproducing any error in that placement. Refused when the "
     "two anchors are not square to one axis, because then no turn seats "
     "them and a nearest quarter turn would be a guess wearing a joint's "
     "clothes.",
     "type?:@mate_types, a:object, b:object, name?:string, flipped?:bool, "
     "rotation?:number, rotation_deg?:number, offset?:number, dx?:number, "
     "dy?:number, ball_rotation?:number[4], derive?:'twist'|'pose'"},
    {"set_mate",
     "Edit a mate's parameters, and the way to POSE a mechanism: turn a "
     "revolute or cylindrical mate with `rotation` (RADIANS) or "
     "`rotation_deg` — not both — slide it with `offset` (mm), and the "
     "solver holds what you set instead of treating it as one more unknown. "
     "The same `rotation` on a FASTENED mate is its authored twist about "
     "the connector axis (the GUI's Rotate), not a pose. "
     "To sweep a range rather than jump to one pose use a motion study: it "
     "walks its samples in order, so each solve starts from the last and a "
     "closed loop stays on the branch it is already on. "
     "With {path: [occurrence]} `id` is a mate INSIDE that flexible "
     "sub-assembly placement (its own id there): only rotation, "
     "rotation_deg or offset — THIS placement's pose of it, kept on the "
     "occurrence; refused where the placement's configuration holds it.",
     "id:int>=0, path?:int[]|null, name?:string, flipped?:bool, "
     "rotation?:number, rotation_deg?:number, offset?:number, dx?:number, "
     "dy?:number, ball_rotation?:number[4]"},
    {"remove_mate", "Remove a mate by id.", "id:int>=0"},
    {"list_mates",
     "List mates with `status` (satisfied | redundant | error) and the "
     "error text when there is one. `type` is the mate's KIND — fastened, "
     "revolute, slider, cylindrical, planar or ball, the word add_mate "
     "takes. Each carries `rotation` in radians and "
     "`rotation_deg` beside it. A mate that closes a loop which did not "
     "close carries `residual_mm`, its own closure error from the last "
     "solve.",
     "", remote::NoFlags,
     "mates[]{type,id,name,a,b,flipped,rotation,offset,rotation_deg,"
     "status,dx?,dy?,ball_rotation?,error?,residual_mm?,suppressed?,held?}"},
    {"add_relation",
     "Couple two mates' degrees of freedom {type: "
     "gear|chain|rack|screw|linear|cam|freewheel|click|worm, a: mate id, b: "
     "mate id}: "
     "dof_b = ratio * dof_a + phase. The ratio is DERIVED from the kind's "
     "own numbers when given (screw may instead reference thread_part and "
     "thread_feature) — gear: {teeth_a, teeth_b} (ratio = "
     "-teeth_a/teeth_b, so meshing gears counter-rotate; with {internal: "
     "true} +teeth_a/teeth_b, a gear meshing inside a ring; teeth and a "
     "ratio together are refused); CHAIN: the same "
     "but the ratio KEEPS its sign, because a chain or belt turns both "
     "sprockets the same way; rack: {pitch_radius} mm (a's rotation drives "
     "b's slide); screw: {pitch} mm per turn on ONE cylindrical mate (pass "
     "the same id as a and b); linear: a bare {ratio}; WORM: {starts, teeth, "
     "hand: right|left}, a the worm's mate and b the wheel's, on crossed "
     "axes — the ratio is starts/teeth and its SIGN is read from the hand "
     "and the side the worm sits on (a ratio is refused; `warnings` names "
     "axes more than 5 degrees off square; "
     "docs/assemblies/relations.md#worm). CAM is the exception "
     "— a's rotation drives b's slide OR turn through a LIFT TABLE, not a "
     "ratio: {lift: [[angle_deg, lift], ...]} spanning one turn, the lift in "
     "mm for a sliding follower and in DEGREES for a turning one, or {rise "
     "(mm) | rise_deg, duration_deg?, start_deg?} for a cosine lobe built "
     "for you. The follower is the DOF mate b has: a slider slides, a "
     "revolute turns, and a cylindrical b takes {dof_b: offset|rotation} "
     "(offset when omitted). FREEWHEEL is a one-way clutch, rotation to "
     "rotation: {ratio, drives: positive|negative} — b follows a while a "
     "drives it in that direction and slips when a goes back; b driven on "
     "ahead leaves a still, and b pushed back drags a — unless a can be "
     "reached from b through the other relations, where dragging would feed "
     "itself and the clutch HOLDS and reports instead. Its history is "
     "`lead` (b's unit, radians), how far b has run ahead of a's drive "
     "line: it starts as the gap between the two parts as they stand (a "
     "clutch engages where it is made, so adding one moves nothing) unless "
     "{lead} says otherwise, and is saved with the document and undone "
     "with the pose; a freewheel "
     "inside a mate loop is reported, not held "
     "(docs/assemblies/relations.md#freewheel). "
     "With {teeth} it re-engages only where teeth meet, so the input travels "
     "up to a pitch before driving again, and {ride_mate, ride_dof, ride_mm "
     "| ride_deg} is what riding over them LIFTS: nothing seated in a "
     "notch, the whole of it off one (solid dogs cannot stand part way over "
     "each other), ADDED to whatever else drives that DOF "
     "(generators/watch/README.md#keyless-works-and-winding). CLICK is a "
     "pawl — ONE mate's "
     "rotation (pass the same id as a and b) free the way it {drives} and "
     "held at its mark the other way, where the relation that pushed it "
     "reports; what rides over the teeth it holds is a cam, which follows "
     "their shape. A cam is the "
     "one relation that is not invertible (many angles give one lift), so it "
     "drives forward only. `ratio` may be given directly for any other kind; "
     "refused when neither is. {phase?} in b's unit (rad or mm) is what makes "
     "teeth mesh. Dragging or set_mate on EITHER mate turns the other; inside "
     "a mate loop the relation is a solver row and lowers the DOF count. "
     "Refused for a mate that lacks the degree of freedom (a fastened mate "
     "has no rotational DOF — its `rotation` is authored, not free), and "
     "for a rack whose slider runs ALONG the pinion's "
     "axis instead of across it; `warnings` names gear axles more than 5 "
     "degrees off parallel (docs/assemblies/relations.md). "
     "a_path / b_path ([occurrence id] of a FLEXIBLE sub-assembly placement) "
     "make that side a mate INSIDE it, by the sub-assembly's own mate id — "
     "an engine's chain driving its pump's camshaft; refused for a "
     "freewheel or a click, whose history would be per placement. "
     "OVER SEVERAL MATES "
     "(docs/assemblies/relations.md#relations-over-several-mates) the "
     "relation is sum(coefficient * dof) = constant, "
     "its members named by role instead of a and b (each a mate id, or "
     "{mate, path} inside a flexible placement): LINEAR_N {terms: [{mate, "
     "dof?, coefficient}, ...]} (two or more; dof required of a cylindrical "
     "mate); EPICYCLIC {sun, ring, carrier, teeth_sun, teeth_ring, planets?: "
     "[{mate, teeth, constant?}]} — Willis, teeth_sun*sun + teeth_ring*ring "
     "- (teeth_sun+teeth_ring)*carrier = constant, all three revolute "
     "rotations measured in one frame; each planet adds a PLANET relation "
     "for its own turn on the carrier (planet + (teeth_sun/teeth)*(sun - "
     "carrier) = constant), returned in `planets`; PLANET {planet, sun, "
     "carrier, teeth_planet, teeth_sun} is that one alone. `constant` "
     "(radians) defaults to where the members stand, so adding one moves "
     "nothing; ratio and phase are refused. In a mate tree the last "
     "unsettled member is solved for; when two are left a member is HELD "
     "where it stands, the first in term order (the ring before the carrier "
     "when the sun is driven), and the relation's `held` says which.",
     "thread_part, thread_feature, a, a_path, b, b_path, carrier, constant, "
     "dof_b, drives, duration_deg, hand, internal, lead, "
     "lift, name, planet, planets, ride_deg, ride_dof, ride_mate, ride_mm, "
     "ring, sun, teeth, teeth_planet, teeth_ring, teeth_sun, terms, "
     "phase, pitch, pitch_radius, ratio, "
     "rise, rise_deg, start_deg, starts, teeth_a, teeth_b, type"},
    {"set_relation",
     "Edit a relation's {ratio?, phase?, name?} by {id}; a freewheel's "
     "{lead?} too (b's unit, radians), which moves b onto a's drive line "
     "at that lead — re-engaging it by hand. A gear or chain made from "
     "teeth takes {teeth_a?, teeth_b?} (and a gear {internal?}) instead of "
     "a ratio; an epicyclic {teeth_sun?, teeth_ring?}, a planet "
     "{teeth_planet?, teeth_sun?}, and every relation over several mates "
     "{constant?} (radians) instead of a phase.",
     "constant, id, internal, lead, name, phase, ratio, teeth_a, teeth_b, "
     "teeth_planet, teeth_ring, teeth_sun"},
    {"remove_relation", "Remove a relation by id.", "id"},
    {"list_relations",
     "List relations with status/errors. A relation over several mates "
     "(linear_n, epicyclic, planet) lists `terms` and `constant` in place "
     "of mate_a/mate_b/ratio/phase, and `held` names a member the last "
     "solve held where it stood because nothing drove it.",
     "", remote::NoFlags,
     "relations[]{type,id,name,kind,mate_a?,mate_b?,dof_a?,dof_b?,ratio?,"
     "phase?,teeth_a?,teeth_b?,internal?,terms?,constant?,"
     "thread_part?,thread_feature?,lift?,drives?,lead?,teeth?,ride?,"
     "mate_a_path?,mate_b_path?,error?,held?}"},
    {"select_mate", "Select a mate in the UI.", "id"},
    {"solve_assembly",
     "Run the mate solver. Returns every mate with a `status` (satisfied | "
     "redundant | error) and its error text when there is one; how many "
     "occurrences the mates DETERMINE (`placed_by_mates`) against how many "
     "hold the transform they were given (`placed_by_transform` — legitimate "
     "for a nut or a camera, and the number to read before trusting `dof`); "
     "and, for an assembly with mate loops, `dof` = the freedom left IN THE "
     "LOOPED COMPONENTS only, whether every loop closed and by how much it "
     "missed, and which mates were redundant but consistent (three bolts "
     "through a flange: satisfied, saying nothing new — not an error). "
     "ALWAYS present: errored_mates, placed, occurrences, placed_by_mates, "
     "placed_by_transform, mates[]. `errored_attachments` and "
     "`attachment_errors` only when a rider's seat no longer resolves (it "
     "stays where it was; the error says which face). ONLY when the "
     "assembly has mate loops: "
     "loop_mates, dof, loops_closed, residual_mm, redundant_mates — a tree "
     "assembly has no loop to report on, so read them with a default. A "
     "mate whose loop did not close carries its OWN `residual_mm`. When a "
     "loop did not close, `suspects` names the mates whose axis, tilted by "
     "`tilt_deg` at `connector` (a|b; absent when either side does), would "
     "close it: `closes` lists the loop's failing mates, and `sole` is false "
     "when several suspects close the same loop equally well and none is "
     "claimed. Empty when the fault is not a single tilted axis. "
     "`flexible` (only when a sub-assembly is placed flexibly) reports each "
     "such placement per path: {occurrence, name, configuration_id, "
     "placed_by_mates, placed_by_transform, mates[]{path, id, name, status, "
     "error?, residual_mm?, rotation_deg?, offset?}} or its `error` when it "
     "could not move.",
     "", remote::NoFlags,
     "errored_mates,placed,occurrences,placed_by_mates,placed_by_transform,"
     "mates[]{id,name,status,error?,residual_mm?},errored_attachments?,"
     "attachment_errors?[]{occurrence,name,error},loop_mates?,dof?,"
     "loops_closed?,residual_mm?,redundant_mates?,"
     "suspects?[]{mate,name,tilt_deg,connector?,closes,sole},"
     "flexible?[]{occurrence,name,configuration_id,placed_by_mates?,"
     "placed_by_transform?,mates?[]{path,id,name,status,error?,residual_mm?,"
     "rotation_deg?,offset?},error?}"},
    {"add_fastener",
     "Place a standard fastener in the active assembly (docs/parts/README.md). "
     "Every dimension but the length is READ from the vendored table for that "
     "standard, so an unstocked size is refused naming the nearest that are. "
     "One generated part per distinct spec, named as its BOM designation "
     "(\"ISO 4762 M6x20\") and reused by every occurrence of it, so `bom` "
     "aggregates without knowing about fasteners. Refused on a part tab: a "
     "screw body there would be machined by CAM and counted as the part's own "
     "mass. Optional {position, orientation} place it; `fit_fastener` derives "
     "all of that from a picked hole and is what you usually want. Standards: "
     "iso4762, iso14579, iso4017, iso7380 (screws — `length` required), "
     "asme_b18_3 (inch socket head cap screw, ASME B18.3: `size` a UNC "
     "designation, \"#4-40\" to \"3/4-10\"), "
     "iso4032 (nut), iso7089 (washer — no `length`). `length` is the shank "
     "length in MILLIMETRES (the 20 of \"M6x20\"; an inch screw's 3/4 in. "
     "is 19.05), `size` is the thread "
     "designation (\"M6\", \"1/4-20\") rather than a measure, and "
     "`position` is model mm.",
     "drive, length, orientation, position, size, standard"},
    {"fit_fastener",
     "Derive a fastener from a picked hole RIM and place it "
     "(docs/parts/README.md). `rim` is a circular EDGE — the one the head sits "
     "against; a cylindrical face has an axis but no direction and cannot say "
     "which end it goes in from. An ARC of a rim the faces round it have split "
     "is accepted: every edge of the body on that circle is gathered, and arcs "
     "that do not close it are refused (\"this rim is N arcs covering M°\"). "
     "`rim` is {feature_id, element, occurrence?, inner_path?} "
     "— the occurrence is needed whenever more than one placement answers to "
     "that element, which is every part placed twice AND every part built the "
     "same way as another (mirror sides of a cart carry the same feature ids "
     "and the same mapped names). Leaving it out THERE is refused, naming "
     "them, rather than resolved to the first. `inner_path` (the occurrence "
     "ids below it) picks the leaf inside a placed sub-assembly. "
     "The SIZE comes from the hole's own diameter "
     "(the clearance table read backwards) and the LENGTH from the stack the "
     "axis passes through, snapped to a length that size is made in. Reading "
     "a diameter backwards is AMBIGUOUS for 46 of the table's 111 sizes — 11 "
     "mm is both an M9 Loose and an M10 Normal clearance — and those are "
     "refused naming every candidate rather than answered with whichever the "
     "table lists first; pass `size` (\"M10\") to say which, or drill the "
     "hole with a Hole feature, which records the fit it was made for. A "
     "`size` given is CHECKED against the hole, not believed. Returns "
     "{size, clearance, grip_mm, length_mm, blind, summary} — the summary "
     "decomposes the stack (\"Plate 12.0, gap 3.0, Bracket 26.5\"), because a "
     "gap is either an unmodelled gasket or two parts that were meant to "
     "touch, and saying so is what makes the second visible. "
     "{dry_run: true} derives without placing. The JOINT, not just the screw: "
     "{nut: \"iso4032\", washer_under_head: true, washer_under_nut: true} "
     "place those too and the LENGTH accounts for them — a screw sized for "
     "the plates and then fitted with washers is too short and looks right. "
     "A nut is refused on a blind hole. `standard` picks the screw: iso4762 "
     "(hex socket cap), iso14579 (torx socket cap), iso7380 (button), "
     "iso4017 (hex head); the drive follows the standard, so leave it empty. "
     "The optional SECOND pick, `far_end` ({feature_id, element, "
     "occurrence?}), says where the joint ENDS when the axis carries on past "
     "it — a bolt through a housing flange whose line continues into a boss "
     "behind it would otherwise be sized to span the lot. It truncates rather "
     "than replaces: everything between the two picks is still traversed, so "
     "the washers and the nut are still found. It takes an occurrence for the "
     "same reason the rim does, and refuses the same way without one. "
     "Placing also MATES the parts the bolt clamps (returned as `mates`), so "
     "dragging one carries the rest instead of tearing the joint in half. One "
     "per INTERFACE it spans — a bolt through three parts makes two, chained, "
     "so the one in the middle is held too — and one per BOLT: each bolt of "
     "a pattern mates at its own hole, so deleting one leaves the parts held "
     "by the others. The extra mates across an interface already joined are "
     "consistent, and solve_assembly lists them in `redundant_mates` rather "
     "than as errors. Each piece RIDES the part it bears on and follows "
     "its HOLE (the face and bore it is seated on, by name) when the hole "
     "moves inside that part; a piece that could not be tied to a hole is "
     "listed in "
     "`warnings`.",
     "drive, dry_run, far_end, nut, rim, size, standard, washer, "
     "washer_under_head, washer_under_nut"},
    {"bom",
     "Bill of materials (aggregated part counts, masses): one row per (part, "
     "configuration, material) placed, carrying `configuration` (its name; "
     "\"\" = the part's active one) and `configuration_id`. A row generated "
     "from a standard carries {standard, designation} — the two fields a "
     "purchase order needs — and a modelled part leaves them empty, so a "
     "filter on either splits bought from made.",
     ""},
    {"check_fasteners",
     "Every placed fastener re-derived against the stack it is IN, and what "
     "is wrong with it (docs/parts/README.md). A length derived from other "
     "documents' geometry cannot be a live binding (A2), so a stack that "
     "changed under a screw has to be REPORTED — this is that report: too "
     "short, longer than its stack needs (a shorter stocked length spans "
     "it), bottoming out in a blind hole, spanning nothing at all, or no "
     "longer in its hole (the hole it follows was deleted or renamed). "
     "Nothing is stored to make it work: a placed fastener's position and "
     "orientation are its rim and its axis, so it also checks one nobody "
     "derived. Returns `examined` and `skipped` beside `ok`/`findings`: empty "
     "findings over ZERO screws and over forty-two used to read alike, and a "
     "screw whose standard this build cannot size is not judged rather than "
     "passed. The receipt is unverifiable when nothing was examined and never "
     "verified while any screw was skipped.",
     "", remote::NoFlags,
     "ok, examined, skipped, findings[]{occurrence,part,why}, "
     "receipt{schema,standing,summary,claims}"},
    {"add_bearing",
     "Place an ISO 15 deep-groove ball bearing in the active assembly where "
     "told (docs/parts/README.md#bearings). `designation` is what a supplier "
     "calls it — \"6200\", \"608\", \"618/4\" — from series 618, 619, 60, "
     "62 and 63, bores 3 to 30 mm; d, D, B and the chamfer are READ from the "
     "standard's table, and anything else is refused naming what is carried "
     "(a shield/seal suffix like 6200-2Z is refused naming the bare one). "
     "One generated part per designation (\"ISO 15 6200\"), so `bom` "
     "aggregates. Refused on a part tab. {position (model mm), orientation "
     "[x,y,z,w]} place it: the bearing's axis is its local +Z, side A on "
     "z = 0. `fit_bearing` derives all of that from a picked seat and is "
     "what you usually want.",
     "designation, orientation, position", remote::NoFlags, "occurrence, part"},
    {"list_bearings",
     "The ISO 15 table this build carries, before anything is placed "
     "(docs/parts/README.md#bearings): {series?, min_bore_mm?} answers the "
     "rows of that series (every series when omitted) whose bore is at "
     "least min_bore_mm, SMALLEST BORE FIRST, each {designation, series, "
     "d_mm, D_mm, B_mm, r_min_mm}. So a generator sizing a shaft from its "
     "load asks for the first row over the diameter it needs and models "
     "the journal and the housing bore from that row's d, D and B, instead "
     "of typing a table out of a catalogue. An unknown series is refused "
     "naming the carried ones; a bore past the largest carried is refused "
     "saying how far the table reaches. Each row also carries the "
     "supplier's abutment limits check_seat judges a shoulder by (d_a_min_mm, "
     "d_a_max_mm for the shaft shoulder, D_a_max_mm for the housing's, "
     "r_a_max_mm for either fillet; SKF's, null where SKF prints none), so "
     "the shoulders are modelled inside them.",
     "min_bore_mm, series", remote::NoFlags,
     "bearings[]{designation,series,d_mm,D_mm,B_mm,r_min_mm,d_a_min_mm,"
     "d_a_max_mm,D_a_max_mm,r_a_max_mm}"},
    {"add_retaining_ring",
     "Place a DIN 471 (external, for a shaft) or DIN 472 (internal, for a "
     "bore) retaining ring in the active assembly where told "
     "(docs/parts/README.md#retaining-rings). `standard` is din471 or "
     "din472; `size` the nominal shaft or bore diameter in mm (25) — 3 to 56 "
     "for din471, 8 to 72 for din472. Thickness, free diameter and section "
     "are READ from the table (Seeger-Orbis's DIN 471/472 data charts); a "
     "size it does not carry is refused naming the nearest and the list. "
     "One generated part per spec (\"DIN 471 25x1.2\"), so `bom` "
     "aggregates (standard din471, designation 25x1.2). Refused on a part "
     "tab. {position (model mm), orientation [x,y,z,w]} place it: its axis "
     "is local +Z, side A (`ring_side_a`) on z = 0. It is drawn at its FREE "
     "diameter, so in its groove it overlaps the floor by (d2 - d3)/2. Its "
     "groove is a RetainingRingGroove feature on the part (add_feature). "
     "fit_retaining_ring puts one INTO its groove; this places one where "
     "told.",
     "orientation, position, size, standard", remote::NoFlags,
     "occurrence, part"},
    {"fit_retaining_ring",
     "Put the retaining ring a GROOVE takes into it "
     "(docs/parts/README.md#a-ring-in-its-groove). `groove` is {feature_id, "
     "element, occurrence?, inner_path?} naming any face of a "
     "RetainingRingGroove feature (its groove_floor or a groove_flank) on a "
     "placed part. The ring's standard and size are the groove's (DIN 471 in "
     "a shaft, 472 in a bore); it is added as a bought part, ATTACHED to that "
     "placement and SEATED on the groove's faces by name -- its ring_side_a "
     "on groove_flank_a, coaxial with groove_floor -- so it follows the "
     "groove when the part changes (and `attach_error` says so if the groove "
     "is lost). The free ring overlaps the groove's floor by (d2 - d3)/2: that "
     "grip is relieved by interference, contact_report and motion studies, "
     "and only for a ring of the groove's own standard and size sitting "
     "coaxially inside its width (`ring_grip_pairs`, `ring_grip_volume` on a "
     "named pair). check_seat counts a ring seated beside a bearing ring as "
     "locating it. Refused: a face no groove made, a groove on several "
     "placements without `occurrence`, a groove that already holds a ring. "
     "`dry_run` works it out without placing.",
     "dry_run, groove", remote::NoFlags,
     "occurrence, part, standard, size_mm, designation, parent, "
     "groove_feature, position, orientation, summary"},
    {"list_retaining_rings",
     "The DIN 471 / DIN 472 table this build carries "
     "(docs/parts/README.md#retaining-rings): {standard, min_size_mm?} "
     "answers its rows whose nominal size is at least min_size_mm, smallest "
     "first, each {size_mm (the shaft or bore diameter), designation, s_mm "
     "(thickness), d3_mm (free diameter: a 471 ring's inside, a 472 ring's "
     "outside), a_mm (lug height max), b_mm (section), d5_mm (lug hole "
     "min), groove {d2_mm (diameter), m_mm (width min), t_mm (depth), n_mm "
     "(edge distance min)}}, and the `source` the table was typed from. So a "
     "generator models a journal long enough for its groove and land (B + "
     "m + n past a shoulder) before cutting it. An unknown standard is "
     "refused naming din471 and din472.",
     "min_size_mm, standard", remote::NoFlags,
     "standard, source, rings[]{size_mm,designation,s_mm,d3_mm,a_mm,b_mm,"
     "d5_mm,groove{d2_mm,m_mm,t_mm,n_mm}}"},
    {"fit_bearing",
     "Put a bearing on a picked SEAT and mate it there "
     "(docs/parts/README.md#bearings). `seat` is {feature_id, element, "
     "occurrence?, inner_path?} naming a CYLINDRICAL face — a shaft journal "
     "(the bearing's bore goes on it) or a housing bore (its outside goes in "
     "it) — or the circular RIM where that face meets its shoulder. The "
     "bearing's diameter is the seat's: a Ø10 journal takes 61800, 61900, "
     "6000, 6200 or 6300, so `designation` (\"6200\") or `series` (\"62\") "
     "says which, and several candidates with neither is REFUSED naming "
     "them. Its side sits against the shoulder (past any fillet); a face "
     "with a shoulder at both ends or neither is refused asking for the rim. "
     "Placing adds a FASTENED mate `Bearing seat` between the ring and the "
     "seat as placed, so nothing moves and `check_fits` reads the fit off "
     "it. Once placed, the OTHER ring is read the way check_seat reads it "
     "- through the seat it sits on, on the bearing's axis, though no mate "
     "names it - and returned as `other_ring` {ring, seat?, "
     "diameter_check?}. `mate_other: true` also mates that ring to that "
     "seat, REVOLUTE (off by default: in a mechanism the shaft usually "
     "turns in the housing by a joint of its own, and a second through the "
     "bearing closes a redundant loop; ask for it where the bearing is the "
     "joint); with no seat found, other_mate is 0 and the summary says so. "
     "{dry_run: true} derives "
     "without placing.",
     "designation, dry_run, mate_other, seat, series", remote::NoFlags,
     "designation, candidates[], shaft, seat_diameter_mm, shoulder, "
     "overhang_mm, summary, occurrence, mate, other_ring, other_mate"},
    {"check_seat",
     "Check a PLACED bearing's seats against the bearing "
     "(docs/parts/README.md#bearings): its d, D, B and r_s min come from the "
     "part, and the observations are MEASURED off the seats its bore and "
     "outside sit on: the seat a mate names, or, for a ring no mate "
     "reaches, the coaxial cylinder of the right sense under the ring "
     "(`found: coaxial`), since a one-solid bearing in a mechanism is mated "
     "to one seat only — the journal/bore diameter (must be the ring's "
     "basic size), whether the ring sits on its shoulder, the fillet into "
     "the shoulder and the shoulder's diameter. All in mm. The shoulder and "
     "fillet are judged against the SUPPLIER'S abutment limits, carried for "
     "every bearing SKF lists (SKF Rolling bearings, PUB BU/P1 17000 EN, "
     "2018, table 1.1; docs/parts/README.md#abutment-limits): a shaft "
     "shoulder below d_a min FAILS (the ring is unsupported), above d_a max "
     "FAILS (it reaches the shield), a housing shoulder's bore above D_a max "
     "FAILS, a fillet above r_a max FAILS, each naming its `limit`; a limit "
     "SKF does not print (d_a max for a size it lists no capped variant of; "
     "every limit for a size it does not make, judged then only against the "
     "ring's chamfer) is unverified with the reason, and the row used comes "
     "back as `abutment` (null for a size SKF does not list). An installed "
     "catalog {catalog, revision, hash, "
     "designation?} is checked against the same measurements on top "
     "(designation defaults to the bearing's). Each check is pass, fail or "
     "unverified with a reason; a ring with no seat found is unverified, "
     "never a pass.",
     "catalog, designation, hash, occurrence, revision", remote::NoFlags,
     "occurrence, designation, d, D, B, r_min, abutment{source,d_a_min,"
     "d_a_max,D_a_max,r_a_max,variant}, seats[]{ring,mate,found?,"
     "part?,occurrence?,inner_path?,feature_id?,"
     "face?,diameter?,shoulder?,shoulder_diameter?,fillet_radius?,gap_mm?,"
     "error?}, checks[]{characteristic,status,observed?,required?,minimum?,"
     "maximum?,limit?,source?,variant?,reason?}, measurements, catalog?, "
     "status, evidence"},
    {"export_bom",
     "Export the BOM as CSV {path}: the bom rows, with a `configuration` "
     "column after `part`.",
     "path"},
    {"edit_in_context",
     "Enter in-context editing of a part within the assembly.", "occurrence"},
    {"add_exploded_view", "Add an exploded view.", "name"},
    {"remove_exploded_view", "Remove an exploded view.", "id"},
    {"list_exploded_views", "List exploded views.", "", remote::NoFlags,
     "views[]{type,id,name,steps}, active, factor"},
    {"set_explode_step",
     "Add/edit an explode step (occurrence + offset). `offset` is a [x, y, z] "
     "displacement in MILLIMETRES, applied for display only — it never moves "
     "the occurrence the mates solved. With `path` [placement] the step "
     "names a part INSIDE that flexible placement (occurrence = the "
     "sub-assembly's id, as list_occurrences on its tab or solve_assembly's "
     "flexible rows give it), which then moves on its own on top of the "
     "unit's step; refused for a rigid placement, whose parts explode as one "
     "block.",
     "occurrence, offset, path, view"},
    {"auto_explode",
     "Generate explode steps automatically (creates and "
     "activates a view when none is given).",
     "view"},
    {"set_explode",
     "Set the live explode factor (0..1); auto-activates the only view, "
     "errors if ambiguous. view: 0 deactivates.",
     "factor, view"},
};
std::span<const MethodDoc> AssemblyMethodDocs() { return AssemblyMethods; }

namespace {

// A configuration of what an occurrence places -- a part's or a
// sub-assembly's -- by name or id; null, 0 or "" is that document's active
// one. Refused naming the ones it has.
// What a placement is made of (v135): a material slug, or "" / null for its
// part's own. Refused for a sub-assembly (its parts are what is made of
// something) and for a slug this build does not know.
std::string OccurrenceMaterial(const Occurrence& occ, const json& m) {
  const std::string slug = m.is_null() ? std::string() : m.get<std::string>();
  if (slug.empty())
    return slug;
  if (occ.refKind == OccurrenceRef::Assembly)
    throw std::runtime_error("'" + occ.name +
                             "' places a sub-assembly: set the material on "
                             "the parts inside it");
  CADMaterials::MaterialType unused;
  if (!CADMaterials::MaterialLibrary::TryMaterialTypeFromString(slug, unused))
    throw std::runtime_error("unknown material '" + slug + "' (known: " +
                             CADMaterials::MaterialLibrary::SlugList() + ")");
  return slug;
}

uint64_t PlacedConfiguration(ui::AppState& state, OccurrenceRef kind,
                             uint64_t docId, const std::string& label,
                             const json& c) {
  if (c.is_null())
    return 0;
  std::vector<Configuration> cfgs;
  if (kind == OccurrenceRef::Assembly) {
    if (const AssemblyDocument* sub = ui::AssemblyDocById(state, docId))
      cfgs = sub->GetConfigurations();
  } else if (const Document* part = ui::PartDocById(state, docId)) {
    cfgs = part->GetConfigurations();
  }
  std::string known;
  for (const Configuration& cfg : cfgs) {
    known += (known.empty() ? "" : ", ") + cfg.name;
    if ((c.is_number() && cfg.id == c.get<uint64_t>()) ||
        (c.is_string() && cfg.name == c.get<std::string>()))
      return cfg.id;
  }
  if ((c.is_number() && c.get<uint64_t>() == 0) ||
      (c.is_string() && c.get<std::string>().empty()))
    return 0;
  throw std::runtime_error("no configuration " + c.dump() + " in '" + label +
                           "'. Known: " + (known.empty() ? "(none)" : known));
}

// V5: {rotate: {axis: [x,y,z], angle_deg}} as a friendlier alternative to a
// raw quaternion in occurrence transforms — the hand-derived-quaternion
// error class from the dock round. Converted here so the document loader
// (and the file format) still see orientation only.
json NormalizeTransform(json t) {
  if (!t.is_object() || !t.contains("rotate"))
    return t;
  if (t.contains("orientation"))
    throw std::runtime_error(
        "pass either transform.orientation or transform.rotate, not both");
  const json& r = t.at("rotate");
  const json& ax = r.at("axis");
  const glm::dvec3 axis(ax.at(0).get<double>(), ax.at(1).get<double>(),
                        ax.at(2).get<double>());
  if (glm::length(axis) < 1e-9f)
    throw std::runtime_error("rotate.axis must be non-zero");
  const double rad = glm::radians(r.at("angle_deg").get<double>());
  const glm::dquat q = glm::angleAxis(rad, glm::normalize(axis));
  t["orientation"] = {q.x, q.y, q.z, q.w};
  t.erase("rotate");
  return t;
}

// A mate's rotational DOF is stored, and carried across this boundary, in
// RADIANS -- `phase` on a relation is radians for the same reason, and a
// rack's ratio is mm per radian, so degrees here would make `dof_b = ratio *
// dof_a + phase` incoherent.
//
// The bug was never the unit. It was that `rotation` NAMED no unit, in a
// file whose other angles all do: `transform.rotate.angle_deg`,
// `duration_deg` and `start_deg` on a cam relation, a motion study's
// degrees-in-degrees-out. So `set_mate(rotation=60)` looked like 60 degrees
// and pinned the mate to 60 radians -- 3437.7 degrees, 197.7 folded -- and
// stepping a linkage 10, 20, 30, 40, 50, 60 really asked for 213, 66, 279,
// 132, 345, 198. That reads exactly like a loop solver jumping between
// branches, and was diagnosed as one twice.
//
// So `rotation_deg` joins the family that says what it means, and every
// response carries both. Converting `rotation` itself to degrees was tried
// first and reverted: it left relations speaking radians and mates speaking
// degrees, which is a worse trap than the one being fixed.

// Radians from whichever the caller used, refusing both at once — a mate
// pinned to two different angles has no right answer. Takes the values
// rather than the params so both key names stay visible at the call site:
// `rpc_params_gate` reads the handler for the keys it touches, and a helper
// that reached into `p` itself made add_mate look as though it had quietly
// stopped accepting either.
double MateRotationRadians(bool hasRad, double rad, bool hasDeg, double deg,
                           double current) {
  if (hasRad && hasDeg)
    throw std::runtime_error(
        "pass rotation (radians) or rotation_deg, not both");
  if (hasDeg)
    return angles::ToRadians(deg);
  if (hasRad)
    return rad;
  return current;
}

json MateJson(const Mate& m) {
  json j = DocumentIO::MateToJson(m);
  // On the wire `type` is the mate KIND — the word add_mate(type=…) takes.
  // The file keeps `mate_type` beside a constant "Mate", and a list whose
  // every row said type "Mate" read as if the kind had been lost.
  j["type"] = j.at("mate_type");
  j.erase("mate_type");
  j["rotation_deg"] = angles::ToDegrees(m.rotation);
  return j;
}

// Error-audit round: "no such occurrence" names what DOES exist (id + name)
// so the correction needs no list_occurrences round-trip.
std::string OccurrenceCandidates(const AssemblyDocument& asmDoc) {
  std::vector<std::string> known;
  for (const Occurrence& o : asmDoc.GetOccurrences())
    known.push_back(std::to_string(o.id) + " (" + o.name + ")");
  return KnownList(known);
}
} // namespace

// A mate named {path, mate} from a request: `path` is the key holding the
// path ([occurrence id] of a flexible placement), `id` the mate. Refused
// naming what is wrong with the path, or the mates that are there.
const Mate* MateFromRequest(ui::AppState& state, const RpcParams& p,
                            const char* pathKey, uint64_t id,
                            std::vector<uint64_t>& path) {
  path.clear();
  if (p.contains(pathKey) && !p.at(pathKey).is_null())
    path = p.at(pathKey).get<std::vector<uint64_t>>();
  std::string why;
  const Mate* mate = assembly::MateAtPath(*state.assembly, path, id,
                                          ui::AssemblyResolverFor(state), &why);
  if (mate == nullptr)
    throw std::runtime_error(path.empty() ? why
                                          : std::string(pathKey) + ": " + why);
  return mate;
}

// A member of a relation over several mates, as add_relation takes it: a
// mate id, or {mate, path?} for one inside a flexible placement, plus, for a
// linear_n term, {dof?, coefficient}. A cylindrical mate has both DOFs, so
// its term must say which; any other mate has one and `dof` may be left out.
RelationTerm TermFromRequest(ui::AppState& state, const json& j,
                             const std::string& role, bool linear) {
  RelationTerm t;
  if (j.is_number_unsigned() || j.is_number_integer()) {
    t.mate = j.get<uint64_t>();
  } else if (j.is_object() && j.contains("mate")) {
    t.mate = j.at("mate").get<uint64_t>();
    if (j.contains("path") && !j.at("path").is_null())
      t.path = j.at("path").get<std::vector<uint64_t>>();
  } else {
    throw std::runtime_error(role + " is a mate id, or {mate, path} for a "
                                    "mate inside a flexible placement");
  }
  if (j.is_object())
    for (auto it = j.begin(); it != j.end(); ++it) {
      const std::string& key = it.key();
      if (key != "mate" && key != "path" &&
          !(linear && (key == "dof" || key == "coefficient")) &&
          !(!linear && key == "teeth"))
        throw std::runtime_error(role + " has no '" + key + "' — " +
                                 (linear ? "a term is {mate, path?, dof?, "
                                           "coefficient}"
                                         : "a member is a mate id or {mate, "
                                           "path?}"));
    }
  std::string why;
  const Mate* m = assembly::MateAtPath(*state.assembly, t.path, t.mate,
                                       ui::AssemblyResolverFor(state), &why);
  if (m == nullptr)
    throw std::runtime_error(role + ": " + why);
  const bool turns = assembly::MateHasDof(m->type, MateDof::Rotation);
  const bool slides = assembly::MateHasDof(m->type, MateDof::Offset);
  if (linear && j.is_object() && j.contains("dof"))
    t.dof = DocumentIO::MateDofFromString(j.at("dof").get<std::string>());
  else if (linear && turns && slides)
    throw std::runtime_error(
        role + ": mate " + std::to_string(t.mate) +
        " is cylindrical and both turns and slides — give dof: rotation or "
        "offset");
  else
    t.dof = linear && slides && !turns ? MateDof::Offset : MateDof::Rotation;
  if (linear) {
    if (!j.is_object() || !j.contains("coefficient"))
      throw std::runtime_error(role + " needs its coefficient: the relation "
                                      "is sum(coefficient * dof) = constant");
    t.coefficient = j.at("coefficient").get<double>();
  }
  return t;
}

// A tooth count: a whole positive number.
int TeethFromRequest(const json& j, const std::string& key) {
  const double z = j.get<double>();
  if (!(z > 0.0) || z != std::floor(z) || z > 1e6)
    throw std::runtime_error(key + " is a tooth count, a whole number above "
                                   "zero");
  return static_cast<int>(z);
}

// Where a relation over several mates stands: its constant, as the members
// are posed now, so adding one moves nothing.
double ConstantAsItStands(ui::AppState& state, const Relation& r) {
  double k = 0.0;
  for (const RelationTerm& t : r.terms)
    k += t.coefficient * assembly::MateDofAt(*state.assembly, t.path, t.mate,
                                             t.dof,
                                             ui::AssemblyResolverFor(state))
                             .value_or(0.0);
  return k;
}

// The mates inside every flexible placement, by path, as solve_assembly
// reports them (v129): how many of its occurrences move and how many fold
// into it, and every one of its mates with a status.
json FlexibleRows(ui::AppState& state, const assembly::SolveReport& rep) {
  json rows = json::array();
  for (const Occurrence& F : state.assembly->GetOccurrences()) {
    if (!F.flexible)
      continue;
    const AssemblyDocument* sub = ui::AssemblyDocById(state, F.partId);
    json row{{"occurrence", F.id},
             {"name", F.name},
             {"configuration_id", F.configurationId}};
    std::string unitError;
    for (const assembly::SolveReport::InnerMate& im : rep.innerMates)
      if (im.occurrence == F.id && im.mate == 0)
        unitError = im.error;
    if (!unitError.empty() || sub == nullptr) {
      row["error"] =
          unitError.empty() ? "its sub-assembly is not loaded" : unitError;
      rows.push_back(std::move(row));
      continue;
    }
    // Per path, as the top level reports its own: what the mates determine
    // (moving) against what stays where the sub-assembly put it.
    row["placed_by_mates"] = F.innerPoses.size();
    row["placed_by_transform"] =
        sub->GetOccurrences().size() - F.innerPoses.size();
    json mates = json::array();
    for (const Mate& m : sub->GetMates()) {
      if (sub->MateSuppressedIn(F.configurationId, m.id))
        continue;
      json mj{{"path", {F.id}},
              {"id", m.id},
              {"name", m.name},
              {"status", "satisfied"}};
      for (const assembly::SolveReport::InnerMate& im : rep.innerMates)
        if (im.occurrence == F.id && im.mate == m.id) {
          mj["status"] = !im.error.empty() ? "error" : "redundant";
          if (!im.error.empty())
            mj["error"] = im.error;
          if (im.residualMm > 0.0)
            mj["residual_mm"] = im.residualMm;
        }
      if (const InnerMateDof* d = F.FindInnerMateDof(m.id)) {
        if (assembly::MateHasDof(m.type, MateDof::Rotation))
          mj["rotation_deg"] = angles::ToDegrees(d->rotation);
        if (assembly::MateHasDof(m.type, MateDof::Offset))
          mj["offset"] = d->offset;
      }
      mates.push_back(std::move(mj));
    }
    row["mates"] = std::move(mates);
    rows.push_back(std::move(row));
  }
  return rows;
}

std::optional<json> HandleAssemblyRpc(ui::AppState& state, const std::string& m,
                                      const RpcParams& p) {
  if (m == "fit_fastener") {
    ui::RimPick rim;
    const auto& r = p.at("rim");
    rim.element.featureId = r.at("feature_id").get<uint64_t>();
    rim.element.name = r.at("element").get<std::string>();
    // Optional, and only optional because a part placed ONCE has nothing to
    // disambiguate. Placed twice, the same element name is on both, and
    // leaving it out is refused rather than resolved to whichever comes
    // first (docs/parts/README.md).
    rim.occurrence = r.value("occurrence", uint64_t{0});
    if (r.contains("inner_path")) // a leaf inside a sub-assembly
      rim.innerPath = r.at("inner_path").get<std::vector<uint64_t>>();
    ui::FastenerRequest request;
    request.standard = p.value("standard", std::string("iso4762"));
    request.drive = p.value("drive", std::string{});
    request.washer = p.value("washer", std::string("iso7089"));
    request.size = p.value("size", std::string{});
    request.nut = p.value("nut", std::string{});
    request.washerUnderHead = p.value("washer_under_head", false);
    request.washerUnderNut = p.value("washer_under_nut", false);
    if (p.contains("far_end")) {
      const auto& e = p.at("far_end");
      request.farEnd.element.featureId = e.at("feature_id").get<uint64_t>();
      request.farEnd.element.name = e.at("element").get<std::string>();
      request.farEnd.occurrence = e.value("occurrence", uint64_t{0});
      if (e.contains("inner_path"))
        request.farEnd.innerPath =
            e.at("inner_path").get<std::vector<uint64_t>>();
    }
    request.dryRun = p.value("dry_run", false);
    const ui::FastenerFit fit = ui::FitFastenerToHole(state, rim, request);
    if (!fit.ok)
      throw std::runtime_error(fit.why);
    return json{{"occurrence", fit.occurrence},
                {"size", fit.size},
                {"clearance", fit.clearance},
                {"grip_mm", fit.grip},
                {"length_mm", fit.length},
                {"blind", fit.blind},
                {"summary", fit.summary},
                {"placed", fit.placed},
                {"parts", fit.parts},
                // The joint, not the hardware: the mates that now hold the
                // clamped parts to each other, one per interface the bolt
                // spans. EMPTY when they were already held — the second bolt
                // through one bracket adds nothing, and saying so is how a
                // caller tells that from a failure to make one.
                {"mates", fit.mates},
                {"warnings", fit.warnings}};
  }
  if (m == "add_fastener") {
    glm::dvec3 position(0.0);
    if (p.contains("position")) {
      const auto& v = p.at("position");
      position = glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                            v.at(2).get<double>());
    }
    glm::dquat orientation(1.0, 0.0, 0.0, 0.0);
    if (p.contains("orientation")) {
      const auto q = p.at("orientation");
      orientation = glm::dquat(q.at(3).get<double>(), q.at(0).get<double>(),
                               q.at(1).get<double>(), q.at(2).get<double>());
    }
    std::string error;
    const uint64_t id = ui::AddFastener(
        state, p.value("standard", std::string("iso4762")),
        p.at("size").get<std::string>(), p.value("length", 0.0),
        p.value("drive", std::string{}), position, orientation, &error);
    if (id == 0)
      throw std::runtime_error(error);
    std::string placed;
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (o.id == id)
        placed = o.name;
    return json{{"occurrence", id}, {"part", placed}};
  }
  if (m == "add_bearing") {
    glm::dvec3 position(0.0);
    if (p.contains("position")) {
      const auto& v = p.at("position");
      position = glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                            v.at(2).get<double>());
    }
    glm::dquat orientation(1.0, 0.0, 0.0, 0.0);
    if (p.contains("orientation")) {
      const auto q = p.at("orientation");
      orientation = glm::dquat(q.at(3).get<double>(), q.at(0).get<double>(),
                               q.at(1).get<double>(), q.at(2).get<double>());
    }
    std::string error;
    const uint64_t id =
        ui::AddBearing(state, p.at("designation").get<std::string>(), position,
                       orientation, &error);
    if (id == 0)
      throw std::runtime_error(error);
    const Occurrence* placed = state.assembly->FindById(id);
    return json{{"occurrence", id},
                {"part", placed != nullptr ? placed->name : std::string{}}};
  }
  if (m == "list_bearings") {
    std::string why;
    const auto rows = parts::BearingsFrom(p.value("series", std::string{}),
                                          p.value("min_bore_mm", 0.0), &why);
    if (rows.empty())
      throw std::runtime_error(why);
    json out = json::array();
    for (const parts::BearingRow* r : rows) {
      json row = {{"designation", r->designation},
                  {"series", r->series},
                  {"d_mm", r->d},
                  {"D_mm", r->D},
                  {"B_mm", r->B},
                  {"r_min_mm", r->rMin}};
      // The supplier's abutment limits, so a generator models its shoulders
      // inside them; null where the catalogue prints none
      // (docs/parts/README.md#abutment-limits).
      const parts::BearingAbutment* a = parts::AbutmentOf(r->designation);
      const auto has = [&](double v) {
        return a != nullptr && parts::BearingAbutment::Has(v) ? json(v)
                                                              : json(nullptr);
      };
      row["d_a_min_mm"] = has(a != nullptr ? a->daMin : 0.0);
      row["d_a_max_mm"] = has(a != nullptr ? a->daMax : 0.0);
      row["D_a_max_mm"] = has(a != nullptr ? a->DaMax : 0.0);
      row["r_a_max_mm"] = has(a != nullptr ? a->raMax : 0.0);
      out.push_back(std::move(row));
    }
    return json{{"bearings", std::move(out)}};
  }
  if (m == "add_retaining_ring") {
    glm::dvec3 position(0.0);
    if (p.contains("position")) {
      const auto& v = p.at("position");
      position = glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                            v.at(2).get<double>());
    }
    glm::dquat orientation(1.0, 0.0, 0.0, 0.0);
    if (p.contains("orientation")) {
      const auto q = p.at("orientation");
      orientation = glm::dquat(q.at(3).get<double>(), q.at(0).get<double>(),
                               q.at(1).get<double>(), q.at(2).get<double>());
    }
    std::string error;
    const uint64_t id = ui::AddRetainingRing(
        state, p.at("standard").get<std::string>(), p.at("size").get<double>(),
        position, orientation, &error);
    if (id == 0)
      throw std::runtime_error(error);
    const Occurrence* placed = state.assembly->FindById(id);
    return json{{"occurrence", id},
                {"part", placed != nullptr ? placed->name : std::string{}}};
  }
  if (m == "list_retaining_rings") {
    const std::string name = p.at("standard").get<std::string>();
    parts::RetainingRingStandard which{};
    if (!parts::RetainingRingStandardFromString(name, which))
      throw std::runtime_error(
          "unknown retaining ring standard \"" + name +
          "\" — accepted: " + parts::KnownRetainingRingStandards());
    const double minSize = p.value("min_size_mm", 0.0);
    json out = json::array();
    for (const parts::RetainingRingRow& r : parts::RetainingRings(which)) {
      if (r.d1 < minSize - 1e-9)
        continue;
      out.push_back(
          {{"size_mm", r.d1},
           {"designation", parts::RetainingRingDesignation(which, r)},
           {"s_mm", r.s},
           {"d3_mm", r.d3},
           {"a_mm", r.a},
           {"b_mm", r.b},
           {"d5_mm", r.d5},
           {"groove",
            {{"d2_mm", r.d2}, {"m_mm", r.m}, {"t_mm", r.t}, {"n_mm", r.n}}}});
    }
    if (out.empty())
      throw std::runtime_error(
          "no " + name + " ring is " + std::to_string(minSize) +
          " mm or larger: it carries " + parts::KnownRetainingRingSizes(which));
    return json{{"standard", name},
                {"source", parts::RetainingRingSource(which)},
                {"rings", std::move(out)}};
  }
  if (m == "fit_retaining_ring") {
    ui::RimPick groove;
    const auto& r = p.at("groove");
    groove.element.featureId = r.at("feature_id").get<uint64_t>();
    groove.element.name = r.at("element").get<std::string>();
    groove.occurrence = r.value("occurrence", uint64_t{0});
    if (r.contains("inner_path"))
      groove.innerPath = r.at("inner_path").get<std::vector<uint64_t>>();
    const ui::RetainingRingFit fit =
        ui::FitRetainingRing(state, groove, p.value("dry_run", false));
    if (!fit.ok)
      throw std::runtime_error(fit.why);
    const Occurrence* placed = fit.occurrence != 0
                                   ? state.assembly->FindById(fit.occurrence)
                                   : nullptr;
    return json{{"occurrence", fit.occurrence},
                {"part", placed != nullptr ? placed->name : fit.designation},
                {"standard", fit.standard},
                {"size_mm", fit.size},
                {"designation", fit.designation},
                {"parent", fit.parent},
                {"groove_feature", fit.groove},
                {"position", {fit.position.x, fit.position.y, fit.position.z}},
                {"orientation",
                 {fit.orientation.x, fit.orientation.y, fit.orientation.z,
                  fit.orientation.w}},
                {"summary", fit.summary}};
  }
  if (m == "fit_bearing") {
    ui::RimPick seat;
    const auto& r = p.at("seat");
    seat.element.featureId = r.at("feature_id").get<uint64_t>();
    seat.element.name = r.at("element").get<std::string>();
    seat.occurrence = r.value("occurrence", uint64_t{0});
    if (r.contains("inner_path"))
      seat.innerPath = r.at("inner_path").get<std::vector<uint64_t>>();
    ui::BearingRequest request;
    request.designation = p.value("designation", std::string{});
    request.series = p.value("series", std::string{});
    request.dryRun = p.value("dry_run", false);
    request.mateOther = p.value("mate_other", false);
    const ui::BearingFit fit = ui::FitBearing(state, seat, request);
    if (!fit.ok)
      throw std::runtime_error(fit.why);
    return json{{"designation", fit.designation},
                {"candidates", fit.candidates},
                {"shaft", fit.shaft},
                {"seat_diameter_mm", fit.seatDiameter},
                {"shoulder", fit.shoulder},
                {"overhang_mm", fit.overhang},
                {"summary", fit.summary},
                {"occurrence", fit.occurrence},
                {"mate", fit.mate},
                {"other_ring", fit.otherRing},
                {"other_mate", fit.otherMate}};
  }
  if (m == "check_seat") {
    // rpc-returns-from: src/ui/BearingActions.cpp
    const nlohmann::json* entry = nullptr;
    if (p.contains("catalog")) {
      entry = &catalog::Find(state.catalogs, p.at("catalog").get<std::string>(),
                             p.at("revision").get<std::string>());
      if (!p.contains("hash"))
        throw std::runtime_error(
            "hash is required to check against a catalog revision");
      if (p.at("hash") != entry->at("hash"))
        throw std::runtime_error(
            "catalog handle is stale: content hash differs");
    }
    return ui::CheckSeat(state, p.at("occurrence").get<uint64_t>(), entry,
                         p.value("designation", std::string{}));
  }
  if (m == "add_occurrence") {
    // Places a part OR another assembly (A5) in the active assembly. `part`
    // is its NAME, resolved to a stable slot id; optional `transform` =
    // {position:[x,y,z], orientation:[x,y,z,w] OR rotate:{axis, angle_deg}}.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    // A5: `part` names a part OR another assembly. Parts are tried first (the
    // overwhelmingly common case and the pre-A5 meaning); an assembly of the
    // same name would need renaming, which is a better problem than silently
    // resolving to the wrong kind.
    // A NAME or a stable slot id: list_parts and list_occurrences both hand
    // out ids (`part_id`), and passing one back used to leak a json type
    // error instead of working.
    uint64_t refId = 0;
    OccurrenceRef refKind = OccurrenceRef::Part;
    std::string refName;
    if (p.at("part").is_number()) {
      const uint64_t asId = p.at("part").get<uint64_t>();
      refName = std::to_string(asId);
      for (const ui::PartSlot& slot : state.parts) {
        if (slot.id != asId)
          continue;
        if (slot.kind != ui::DocKind::Part &&
            slot.kind != ui::DocKind::Assembly)
          break;
        refId = slot.id;
        refKind = slot.kind == ui::DocKind::Assembly ? OccurrenceRef::Assembly
                                                     : OccurrenceRef::Part;
        refName = slot.name;
        break;
      }
    } else {
      refName = p.at("part").get<std::string>();
      refId = ui::PartIdByName(state, refName);
      if (refId == 0) {
        for (const ui::PartSlot& slot : state.parts) {
          if (slot.kind == ui::DocKind::Assembly && slot.name == refName) {
            refId = slot.id;
            refKind = OccurrenceRef::Assembly;
            break;
          }
        }
      }
    }
    if (refId == 0) {
      // Both vocabularies, because "no part named X" sends someone looking
      // for a typo when what they wanted was an assembly (error-style.md).
      std::string parts, assemblies;
      for (const ui::PartSlot& slot : state.parts) {
        std::string& into =
            slot.kind == ui::DocKind::Assembly ? assemblies : parts;
        if (slot.kind != ui::DocKind::Part &&
            slot.kind != ui::DocKind::Assembly)
          continue;
        into += (into.empty() ? "" : ", ") + slot.name;
      }
      throw std::runtime_error(
          "no part or assembly named '" + refName +
          "'. parts: " + (parts.empty() ? "(none)" : parts) +
          "; assemblies: " + (assemblies.empty() ? "(none)" : assemblies));
    }
    // An assembly cannot contain itself, directly or through any chain — a
    // cycle renders forever, so it is refused here rather than discovered at
    // render time.
    if (refKind == OccurrenceRef::Assembly) {
      uint64_t hostId = 0;
      for (const ui::PartSlot& slot : state.parts)
        if (slot.kind == ui::DocKind::Assembly &&
            ui::AssemblyDocById(state, slot.id) == state.assembly.get())
          hostId = slot.id;
      std::string why;
      if (assembly::WouldCycle(*state.assembly, hostId, refId, refKind,
                               ui::AssemblyResolverFor(state), &why))
        throw std::runtime_error("cannot place '" + refName + "' here: " + why);
    }
    const json identityTransform = {{"position", {0.0, 0.0, 0.0}},
                                    {"orientation", {0.0, 0.0, 0.0, 1.0}}};
    // Build one occurrence-JSON and reuse the loader (id is reassigned on add).
    Occurrence occ = DocumentIO::OccurrenceFromJson(
        {{"id", 0},
         {"name", p.value("name", refName)},
         {"part_id", refId},
         {"ref_kind", refKind == OccurrenceRef::Assembly ? "assembly" : "part"},
         {"part_version", nullptr},
         {"transform",
          NormalizeTransform(p.value("transform", identityTransform))},
         {"visible", p.value("visible", true)},
         {"fixed", p.value("fixed", false)}});
    // Placed in a configuration of what it places; a part's has to build.
    occ.configurationId =
        PlacedConfiguration(state, refKind, refId, refName,
                            p.value("configuration", json(nullptr)));
    if (occ.configurationId != 0 && refKind == OccurrenceRef::Part) {
      std::string bad;
      if (ui::ConfiguredPartDocument(state, refId, occ.configurationId, &bad) ==
          nullptr)
        throw std::runtime_error("cannot place '" + refName + "': " + bad);
    }
    if (p.contains("material"))
      occ.material = OccurrenceMaterial(occ, p.at("material"));
    // v129: placed FLEXIBLE, its sub-assembly's mechanism moves here.
    occ.flexible = p.value("flexible", false);
    if (occ.flexible) {
      const std::string why = ui::FlexibleRefusalHere(state, occ);
      if (!why.empty())
        throw std::runtime_error("cannot place '" + refName +
                                 "' flexible: " + why);
    }
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddOccurrence(occ);
    json out{
        {"id", id},
        {"part_id", refId},
        {"ref_kind", refKind == OccurrenceRef::Assembly ? "assembly" : "part"}};
    if (occ.flexible) {
      out["flexible"] = true;
      out["errored_mates"] = ui::SolveActiveAssembly(state);
    }
    return out;
  }
  if (m == "remove_occurrence") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ui::SnapshotAssembly(state);
    if (!p.contains("id") && !p.contains("occurrence"))
      throw std::runtime_error(
          "which occurrence? Pass `occurrence` (or `id`). Occurrences: " +
          OccurrenceCandidates(*state.assembly));
    const uint64_t gone = p.contains("id") ? p.at("id").get<uint64_t>()
                                           : p.at("occurrence").get<uint64_t>();
    if (!state.assembly->RemoveOccurrence(gone)) {
      state.assemblyHistory.undo
          .pop_back(); // nothing changed; drop the snapshot
      throw std::runtime_error(
          "no occurrence with id " + std::to_string(gone) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
    }
    return json::object();
  }
  if (m == "list_occurrences") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    // A5: what is INSIDE a placed sub-assembly, so an agent can see the tree
    // without switching tabs. Paths are display/diagnostic strings — RPCs
    // still address top-level occurrences, which is why `leaves` is a plain
    // list and not a set of handles.
    std::map<uint64_t, std::vector<std::string>> leavesByRoot;
    // ...and their bounds, because a placed SUB-ASSEMBLY has no part document
    // to measure: asking for its extent by way of `partId` returns nothing,
    // which is worse than refusing — the caller reads a missing key as "no
    // geometry" rather than "not implemented for this kind".
    std::map<uint64_t, std::pair<glm::dvec3, glm::dvec3>> boundsByRoot;
    for (const assembly::FlatInstance& inst : ui::FlattenActiveAssembly(state))
      if (inst.visible) {
        leavesByRoot[inst.rootOccurrenceId].push_back(inst.path);
        glm::dvec3 lmn(0.0), lmx(0.0);
        if (inst.part != nullptr &&
            Measure::WorldBounds(inst.part->GetBodies(), inst.world, lmn,
                                 lmx)) {
          const auto it = boundsByRoot.find(inst.rootOccurrenceId);
          if (it == boundsByRoot.end())
            boundsByRoot[inst.rootOccurrenceId] = {lmn, lmx};
          else {
            it->second.first = glm::min(it->second.first, lmn);
            it->second.second = glm::max(it->second.second, lmx);
          }
        }
      }
    // WHERE each one actually is, on request. `get_state` gives a body's
    // bbox in its PART's space, and there was nothing that gave an
    // occurrence's extent where it sits — so the carburettor round rolled
    // its own by transforming the eight corners of the local box, which is
    // a box around a rotated box and not the part: it claimed the throttle
    // lever spanned z 8..40 when the real part came near neither end, and
    // two diagnoses were made from it. Opt-in because it is a bbox per
    // occurrence and a 197-body import is a listing people do casually.
    const bool wantBounds = p.value("bounds", false);
    json arr = json::array();
    for (const Occurrence& o : state.assembly->GetOccurrences()) {
      json oj = DocumentIO::OccurrenceToJson(o);
      if (!o.attachError.empty())
        oj["attach_error"] = o.attachError;
      if (!o.attachWarning.empty())
        oj["attach_warning"] = o.attachWarning;
      if (wantBounds) {
        glm::dvec3 bmn(0.0), bmx(0.0);
        bool have = false;
        if (o.refKind == OccurrenceRef::Assembly) {
          const auto it = boundsByRoot.find(o.id);
          if (it != boundsByRoot.end()) {
            bmn = it->second.first;
            bmx = it->second.second;
            have = true;
          }
        } else if (const Document* pd = ui::OccurrencePartDoc(state, o)) {
          have = Measure::WorldBounds(pd->GetBodies(), ui::OccurrenceMatrix(o),
                                      bmn, bmx);
        }
        if (have) {
          oj["bbox_min"] = {bmn.x, bmn.y, bmn.z};
          oj["bbox_max"] = {bmx.x, bmx.y, bmx.z};
        }
      }
      if (o.refKind == OccurrenceRef::Assembly) {
        const auto it = leavesByRoot.find(o.id);
        oj["leaf_count"] =
            it == leavesByRoot.end() ? 0 : static_cast<int>(it->second.size());
        if (it != leavesByRoot.end())
          oj["leaves"] = it->second;
      }
      arr.push_back(std::move(oj));
    }
    return json{{"occurrences", arr}};
  }
  // Occurrence property edits. All snapshot for undo, then bump the assembly
  // version so the renderer resyncs. `id` names the occurrence.
  if (m == "set_occurrence_transform" || m == "set_occurrence_visible" ||
      m == "set_occurrence_fixed" || m == "rename_occurrence" ||
      m == "set_occurrence_configuration" || m == "set_occurrence_flexible" ||
      m == "set_occurrence_material") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    // `occurrence` as well as `id`: every neighbouring RPC names this
    // argument `occurrence`, and these five named it `id` alone.
    if (!p.contains("id") && !p.contains("occurrence"))
      throw std::runtime_error(
          "which occurrence? Pass `occurrence` (or `id`). Occurrences: " +
          OccurrenceCandidates(*state.assembly));
    const uint64_t occId = p.contains("id")
                               ? p.at("id").get<uint64_t>()
                               : p.at("occurrence").get<uint64_t>();
    Occurrence* occ = state.assembly->FindById(occId);
    if (!occ)
      throw std::runtime_error(
          "no occurrence with id " + std::to_string(occId) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
    // The two a flexible placement is made of go through the SAME actions as
    // the occurrence panel's checkbox and picker (ui/FlexibleActions.h), each
    // its own undo step and solve.
    // The panel's material picker is this same action (ui/FlexibleActions.h).
    if (m == "set_occurrence_material") {
      const json& mj = p.at("material");
      std::string why;
      if (!ui::SetOccurrenceMaterial(
              state, occId,
              mj.is_null() ? std::string() : mj.get<std::string>(), &why))
        throw std::runtime_error(why);
      return DocumentIO::OccurrenceToJson(*state.assembly->FindById(occId));
    }
    if (m == "set_occurrence_flexible" || m == "set_occurrence_configuration") {
      std::string why;
      assembly::SolveReport rep;
      bool solved = false;
      if (m == "set_occurrence_flexible") {
        const bool want = p.at("flexible").get<bool>();
        if (!ui::SetOccurrenceFlexible(state, occId, want, &why, &rep))
          throw std::runtime_error(why);
        solved = true;
      } else if (m == "set_occurrence_configuration") {
        // A5/v56: which configuration of the REFERENCED document this
        // placement uses. Named or by id; 0 / "" goes back to that
        // document's active one.
        const uint64_t cfgId =
            PlacedConfiguration(state, occ->refKind, occ->partId, occ->name,
                                p.value("configuration", json(nullptr)));
        if (!ui::SetOccurrenceConfiguration(state, occId, cfgId, &why, &rep,
                                            &solved))
          throw std::runtime_error(why);
      }
      occ = state.assembly->FindById(occId);
      json out = DocumentIO::OccurrenceToJson(*occ);
      if (solved) {
        json regen{{"errored_mates", rep.erroredMates}};
        if (occ->flexible)
          for (const json& row : FlexibleRows(state, rep))
            if (row.at("occurrence").get<uint64_t>() == occ->id)
              regen["flexible"] = row;
        out["solve"] = std::move(regen);
      }
      return out;
    }
    ui::SnapshotAssembly(state);
    if (m == "set_occurrence_transform") {
      // EITHER HALF ALONE. `add_occurrence` treats `orientation` as optional
      // and places a part from `{position}` by itself; this threw the raw
      // nlohmann error `key 'orientation' not found`, which names neither
      // the RPC nor the field that is optional three lines away in the same
      // document. Moving a part along one axis is the common case, and the
      // occurrence's current pose is the only sensible default for the half
      // the caller did not mention.
      const json t = NormalizeTransform(p.at("transform"));
      if (t.contains("position")) {
        const json& pos = t.at("position");
        occ->position =
            glm::dvec3(pos.at(0).get<double>(), pos.at(1).get<double>(),
                       pos.at(2).get<double>());
      }
      if (t.contains("orientation")) {
        const json& q = t.at("orientation"); // [x,y,z,w]
        occ->orientation =
            glm::dquat(q.at(3).get<double>(), q.at(0).get<double>(),
                       q.at(1).get<double>(), q.at(2).get<double>());
      }
      if (!t.contains("position") && !t.contains("orientation"))
        throw std::runtime_error(
            "set_occurrence_transform: give `position`, `orientation` "
            "([x,y,z,w]) or `rotate` ({axis, angle_deg}) — whichever you "
            "omit keeps its current value, but omitting both moves nothing");
    } else if (m == "set_occurrence_visible") {
      occ->visible = p.at("visible").get<bool>();
    } else if (m == "set_occurrence_fixed") {
      occ->fixed = p.at("fixed").get<bool>();
    } else if (m == "rename_occurrence") {
      occ->name = p.at("name").get<std::string>();
    }
    state.assembly->BumpVersion();
    // Moving/grounding an occurrence can shift what the mates solve to (a fixed
    // anchor moved, or a mated occurrence was nudged and must be re-placed).
    json regen;
    if (m == "set_occurrence_transform" || m == "set_occurrence_fixed") {
      const assembly::SolveReport rep = ui::SolveActiveAssemblyReport(state);
      regen = json{{"errored_mates", rep.erroredMates}};
      if (occ->flexible)
        for (const json& row : FlexibleRows(state, rep))
          if (row.at("occurrence").get<uint64_t>() == occ->id)
            regen["flexible"] = row;
    }
    occ = state.assembly->FindById(occId);
    json out = DocumentIO::OccurrenceToJson(*occ);
    if (!regen.is_null())
      out["solve"] = std::move(regen);
    return out;
  }
  // ---- Mates (A3) ---------------------------------------------------------
  // A connector spec is {occurrence, origin:true} or
  // {occurrence, face:{feature_id, element}}; translated to the on-disk anchor
  // shape and parsed by DocumentIO::MateFromJson.
  if (m == "add_mate") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    auto connToJson = [&state](const json& c) -> json {
      json anchor;
      const Occurrence* o = nullptr;
      if (state.assembly != nullptr && c.contains("occurrence")) {
        if (c.at("occurrence").is_number())
          o = state.assembly->FindById(c.at("occurrence").get<uint64_t>());
        else
          for (const Occurrence& cand : state.assembly->GetOccurrences())
            if (cand.name == c.at("occurrence").get<std::string>())
              o = &cand;
      }
      // A5: a face anchor names a face of a PART. A placed sub-assembly has
      // none of its own, so refuse it HERE rather than storing a mate that
      // can only ever fail to solve (which is what it used to do, reporting
      // "the part document is not loaded" — a missing-file message for a
      // file that is not missing).
      if (!c.value("origin", false) && c.contains("face") && o != nullptr) {
        // v57: a face anchor on a sub-assembly is fine WITH a path naming the
        // part inside it whose face is meant — "bolt this bracket to the
        // flange of the pump inside the pump unit". Without one there is
        // still nothing to anchor to: a placed assembly has no faces.
        if (o->refKind == OccurrenceRef::Assembly && !c.contains("inner_path"))
          throw std::runtime_error(
              "'" + o->name +
              "' is a sub-assembly, which has no faces of its own: add "
              "inner_path (the occurrence ids inside it, down to the part "
              "whose face you mean — list_occurrences on that tab), mate it "
              "by origin, or open that assembly and mate the part there");
      }
      if (c.contains("frame")) {
        // v86: a named Frame of the occurrence's part — the mate as
        // designed. A name is resolved HERE against that part's history so
        // the stored anchor is an id (names are cosmetic and renamable); a
        // name that is not a Frame there is refused naming the frames that
        // are.
        if (o == nullptr)
          throw std::runtime_error(
              "connector 'frame' needs 'occurrence' (id or name — see "
              "list_occurrences)");
        if (o->refKind == OccurrenceRef::Assembly && !c.contains("inner_path"))
          throw std::runtime_error(
              "'" + o->name +
              "' is a sub-assembly, which has no frames of its own: add "
              "inner_path down to the part whose frame you mean, or mate it "
              "by origin");
        uint64_t partId = o->partId;
        if (c.contains("inner_path")) {
          // Walk the path to the part the frame belongs to.
          const AssemblyDocument* sub = ui::AssemblyDocById(state, o->partId);
          const Occurrence* inner = nullptr;
          for (const json& idj : c.at("inner_path")) {
            if (sub == nullptr)
              throw std::runtime_error(
                  "inner_path goes deeper than the sub-assembly does");
            inner = sub->FindById(idj.get<uint64_t>());
            if (inner == nullptr)
              throw std::runtime_error("inner_path names occurrence " +
                                       idj.dump() + ", which is not inside '" +
                                       o->name + "'");
            sub = inner->refKind == OccurrenceRef::Assembly
                      ? ui::AssemblyDocById(state, inner->partId)
                      : nullptr;
          }
          if (inner != nullptr)
            partId = inner->partId;
        }
        const Document* part = ui::PartDocById(state, partId);
        if (part == nullptr)
          throw std::runtime_error("the part of '" + o->name +
                                   "' is not loaded");
        const json& fv = c.at("frame");
        const FrameFeature* hit = nullptr;
        std::string frames;
        for (const auto& f : part->GetFeatures()) {
          const auto* fr = dynamic_cast<const FrameFeature*>(f.get());
          if (fr == nullptr)
            continue;
          if ((fv.is_number() && fr->GetId() == fv.get<uint64_t>()) ||
              (fv.is_string() && fr->GetName() == fv.get<std::string>()))
            hit = fr;
          if (!frames.empty())
            frames += ", ";
          frames += std::to_string(fr->GetId()) + " (" + fr->GetName() + ")";
        }
        if (hit == nullptr)
          throw std::runtime_error(
              "no Frame " + fv.dump() + " on the part of '" + o->name +
              "'; frames there: " +
              (frames.empty()
                   ? std::string("none (add_feature type Frame on that part)")
                   : frames));
        anchor = {{"frame", hit->GetId()}};
      } else if (c.value("origin", false))
        anchor = {{"origin", true}};
      else if (c.contains("face")) {
        // A bare element name here used to leak a json type error
        // ("cannot use at() with string") instead of the contract.
        if (!c.at("face").is_object() || !c.at("face").contains("feature_id") ||
            !c.at("face").contains("element"))
          throw std::runtime_error(
              "connector 'face' is an object {feature_id, element}: the id of "
              "the feature that CREATED the body (created_by in get_state) "
              "and the element name (list_elements)");
        anchor = {{"face",
                   {{"feature_id", c.at("face").at("feature_id")},
                    {"element", c.at("face").at("element")}}}};
        if (c.contains("snap_edge")) // inference snap (hole center) — v26
          anchor["snap_edge"] = {
              {"feature_id", c.at("snap_edge").at("feature_id")},
              {"element", c.at("snap_edge").at("element")}};
      } else
        throw std::runtime_error(
            "connector needs 'frame: <Frame id|name>', 'origin:true' or "
            "'face:{feature_id,element}'");
      // The RESOLVED id, not what the caller wrote. The lookup above
      // already accepts a name — as the doc promises — but passing the raw
      // value on meant a name reached the parser, which wants a number, and
      // came back as "type must be number, but is string" from a call whose
      // own documentation says id|name.
      if (!c.contains("occurrence"))
        throw std::runtime_error(
            "connector needs 'occurrence' (id or name — see list_occurrences)");
      if (o == nullptr)
        throw std::runtime_error(
            "connector 'occurrence' " + c.at("occurrence").dump() +
            " names nothing in this assembly. Occurrences: " +
            (state.assembly ? OccurrenceCandidates(*state.assembly)
                            : std::string("none")));
      json out{{"occurrence", o->id}, {"anchor", anchor}};
      if (c.contains("inner_path"))
        out["inner_path"] = c.at("inner_path");
      return out;
    };
    json mateJson = {
        {"id", 0},
        {"name", p.value("name", std::string{"Mate"})},
        {"mate_type", p.value("type", std::string{"fastened"})},
        {"a", connToJson(p.at("a"))},
        {"b", connToJson(p.at("b"))},
        {"flipped", p.value("flipped", true)},
        {"rotation",
         MateRotationRadians(p.contains("rotation"), p.value("rotation", 0.0),
                             p.contains("rotation_deg"),
                             p.value("rotation_deg", 0.0), 0.0)},
        {"offset", p.value("offset", 0.0)}};
    if (p.contains("dx"))
      mateJson["dx"] = p.at("dx"); // planar
    if (p.contains("dy"))
      mateJson["dy"] = p.at("dy"); // planar
    if (p.contains("ball_rotation"))
      mateJson["ball_rotation"] = p.at("ball_rotation");
    Mate mate = DocumentIO::MateFromJson(mateJson);
    // Both occurrences must exist before we solve against them.
    if (!state.assembly->FindById(mate.a.occurrenceId) ||
        !state.assembly->FindById(mate.b.occurrenceId))
      throw std::runtime_error("mate references a missing occurrence");
    // `derive` reads the numbers off where the two parts already lie, which
    // only the app can do: it can see both frames, and the caller is
    // otherwise reduced to trying the eight ways round and testing the
    // result (docs/assemblies/README.md#deriving-a-mate-from-the-placement).
    if (p.contains("derive")) {
      if (mate.type != MateType::Fastened)
        throw std::runtime_error(
            "derive is for a FASTENED mate — the others leave the DOF it "
            "would set free for the solver to move");
      const std::string how = p.at("derive").get<std::string>();
      assembly::MateDerive kind{};
      if (how == "twist")
        kind = assembly::MateDerive::Twist;
      else if (how == "pose")
        kind = assembly::MateDerive::Pose;
      else
        throw std::runtime_error(
            "derive is 'twist' (which side and how far round, anchors "
            "coincident) or 'pose' (all five, holding the parts where they "
            "are); got '" +
            how + "'");
      if (!ui::DeriveMateFromPlacement(state, mate, kind))
        throw std::runtime_error(
            "derive cannot read this joint off the placement: the two "
            "anchors are not square to one axis, so no turn about the "
            "connector axis seats one on the other. Place the part roughly "
            "right first, or mate frames that share an axis");
    }
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddMate(mate);
    const int errored = ui::SolveActiveAssembly(state);
    // MateJson, not MateToJson: the same shape `list_mates` reports, which
    // matters now that `derive` decides some of these numbers — the reply is
    // where a caller reads what the app chose, and it should not have to
    // convert radians to know.
    json out = MateJson(*state.assembly->FindMateById(id));
    // errored_mates is the GLOBAL count after the re-solve (an earlier
    // broken mate keeps it non-zero even when THIS mate is fine) — name the
    // actual offenders so a script never has to guess which one failed.
    out["errored_mates"] = errored;
    if (errored > 0) {
      json list = json::array();
      for (const Mate& mm : state.assembly->GetMates())
        if (!mm.error.empty())
          list.push_back(
              {{"id", mm.id}, {"name", mm.name}, {"error", mm.error}});
      out["errored"] = std::move(list);
    }
    if (const Mate* mm = state.assembly->FindMateById(id);
        mm && !mm->error.empty())
      out["error"] = mm->error;
    return out;
  }
  if (m == "set_mate" && p.contains("path") && !p.at("path").is_null() &&
      !p.at("path").empty()) {
    // v129: POSE a mate inside a flexible placement — this placement's own
    // value, kept on its occurrence; the sub-assembly's document is shared
    // by every placement of it and is not touched.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    std::vector<uint64_t> path;
    const Mate* mate =
        MateFromRequest(state, p, "path", p.at("id").get<uint64_t>(), path);
    for (const char* k : {"flipped", "name", "dx", "dy", "ball_rotation"})
      if (p.contains(k))
        throw std::runtime_error(
            std::string(k) +
            " is the sub-assembly's to edit, on its own tab; "
            "a mate inside a flexible placement takes rotation, rotation_deg "
            "or offset here — this placement's pose of it");
    const bool turn = p.contains("rotation") || p.contains("rotation_deg");
    const MateDof dof = turn ? MateDof::Rotation : MateDof::Offset;
    if (!turn && !p.contains("offset"))
      throw std::runtime_error("set_mate with a path poses the mate: give "
                               "rotation, rotation_deg or offset");
    if (!assembly::MateHasDof(mate->type, dof))
      throw std::runtime_error("mate " + std::to_string(mate->id) + " (" +
                               mate->name + ") has no " +
                               DocumentIO::MateDofToString(dof) + " to set");
    const Occurrence* F = state.assembly->FindById(path.front());
    const AssemblyDocument* sub = ui::AssemblyDocById(state, F->partId);
    if (const Configuration* c = sub->ConfigurationFor(F->configurationId))
      if (const MateOverride* hold = c->OverrideForMate(mate->id))
        if ((turn && hold->rotation) || (!turn && hold->offset))
          throw std::runtime_error(
              "mate " + std::to_string(mate->id) + "'s " +
              (turn ? "rotation" : "offset") + " is held by '" + F->name +
              "''s configuration '" + c->name +
              "' — choose another with set_occurrence_configuration");
    const double current =
        assembly::MateDofAt(*state.assembly, path, mate->id, dof,
                            ui::AssemblyResolverFor(state))
            .value_or(0.0);
    const double value =
        turn ? MateRotationRadians(p.contains("rotation"),
                                   p.value("rotation", 0.0),
                                   p.contains("rotation_deg"),
                                   p.value("rotation_deg", 0.0), current)
             : p.at("offset").get<double>();
    ui::SnapshotAssembly(state);
    assembly::SetMateDofAt(*state.assembly, path, mate->id, dof, value,
                           ui::AssemblyResolverFor(state));
    const assembly::SolveReport rep = ui::SolveActiveAssemblyReport(
        state, assembly::Driven{mate->id, dof, path});
    json out{{"id", mate->id}, {"path", path}, {"name", mate->name}};
    const Occurrence* after = state.assembly->FindById(path.front());
    if (const InnerMateDof* d = after->FindInnerMateDof(mate->id)) {
      out["rotation"] = d->rotation;
      out["rotation_deg"] = angles::ToDegrees(d->rotation);
      out["offset"] = d->offset;
    }
    out["errored_mates"] = rep.erroredMates;
    for (const assembly::SolveReport::InnerMate& im : rep.innerMates)
      if (im.occurrence == path.front() && im.mate == mate->id &&
          !im.error.empty())
        out["error"] = im.error;
    return out;
  }
  if (m == "set_mate") {
    // The branch above takes a non-empty `path`; this one an empty one.
    // rpc-params: path
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    Mate* mate = state.assembly->FindMateById(p.at("id").get<uint64_t>());
    if (!mate)
      throw std::runtime_error("no such mate");
    // A DOF the active configuration holds is the configuration's to set:
    // writing the mate here would edit the AUTHORED value under the hold —
    // another configuration's mate — and move nothing on screen.
    if (const MateOverride* hold = state.assembly->MateOverrideFor(mate->id)) {
      const bool turn = p.contains("rotation") || p.contains("rotation_deg");
      if ((turn && hold->rotation) || (p.contains("offset") && hold->offset)) {
        const Configuration* c = state.assembly->ActiveConfig();
        throw std::runtime_error(
            "mate " + std::to_string(mate->id) + "'s " +
            (turn && hold->rotation ? "rotation" : "offset") +
            " is held by configuration '" + (c ? c->name : "") + "' (id " +
            std::to_string(c ? c->id : 0) +
            ") — change the hold with set_configuration(mate_overrides), or "
            "set_active_configuration(0) to edit the mate itself");
      }
    }
    ui::SnapshotAssembly(state);
    if (p.contains("rotation") || p.contains("rotation_deg"))
      mate->rotation =
          MateRotationRadians(p.contains("rotation"), p.value("rotation", 0.0),
                              p.contains("rotation_deg"),
                              p.value("rotation_deg", 0.0), mate->rotation);
    if (p.contains("offset"))
      mate->offset = p.at("offset").get<double>();
    if (p.contains("flipped"))
      mate->flipped = p.at("flipped").get<bool>();
    if (p.contains("name"))
      mate->name = p.at("name").get<std::string>();
    if (p.contains("dx"))
      mate->dx = p.at("dx").get<double>(); // planar
    if (p.contains("dy"))
      mate->dy = p.at("dy").get<double>(); // planar
    if (p.contains("ball_rotation")) {    // ball [x,y,z,w]
      const json& q = p.at("ball_rotation");
      mate->ballRotation =
          glm::dquat(q.at(3).get<double>(), q.at(0).get<double>(),
                     q.at(1).get<double>(), q.at(2).get<double>());
    }
    // The mate just edited drives whatever it is related to: turning the
    // wheel turns the pinion as readily as the other way round.
    (void)assembly::PropagateRelations(*state.assembly, mate->id,
                                       ui::PartResolverFor(state));
    // Hold what was just set: in a closed loop the solver would otherwise
    // treat this DOF as one more unknown and move it.
    ui::SolveActiveAssemblyDriven(state, mate->id,
                                  p.contains("offset") ? MateDof::Offset
                                                       : MateDof::Rotation);
    json out = MateJson(*mate);
    if (!mate->error.empty())
      out["error"] = mate->error;
    return out;
  }
  if (m == "remove_mate") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveMate(p.at("id").get<uint64_t>())) {
      state.assemblyHistory.undo.pop_back(); // nothing changed; drop snapshot
      throw std::runtime_error("no such mate");
    }
    ui::SolveActiveAssembly(state); // freed occurrences keep their last pose
    return json::object();
  }
  if (m == "list_mates") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    json arr = json::array();
    for (const Mate& mate : state.assembly->GetMates()) {
      json mj = MateJson(mate);
      // Same word as solve_assembly's. list_mates does not re-solve, so
      // "redundant" is not knowable here: a mate is either carrying an
      // error from the last solve, or it is satisfied.
      mj["status"] = mate.error.empty() ? "satisfied" : "error";
      if (!mate.error.empty())
        mj["error"] = mate.error;
      if (mate.residualMm > 0.0) // a loop this mate closes did not close
        mj["residual_mm"] = mate.residualMm;
      // The active configuration's overlay (v101). rotation/offset above
      // are the AUTHORED values; the part is placed at the held ones, and a
      // caller reading only the former saw a mate and a placement disagree.
      if (state.assembly->MateSuppressed(mate.id)) {
        mj["suppressed"] = true;
        mj["status"] = "suppressed";
      }
      if (const MateOverride* hold = state.assembly->MateOverrideFor(mate.id)) {
        json h = json::object();
        if (hold->rotation) {
          h["rotation"] = *hold->rotation;
          h["rotation_deg"] = angles::ToDegrees(*hold->rotation);
        }
        if (hold->offset)
          h["offset"] = *hold->offset;
        mj["held"] = h;
      }
      arr.push_back(mj);
    }
    return json{{"mates", arr}};
  }
  if (m == "add_relation") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    Relation r;
    r.kind =
        DocumentIO::RelationKindFromString(p.at("type").get<std::string>());
    if (r.HasTerms()) {
      // Over several mates (v171,
      // docs/assemblies/relations.md#relations-over-several-mates): the
      // members are named by role, not a and b, and the constant is not a
      // phase in one member's unit.
      for (const char* k : {"a", "b", "a_path", "b_path"})
        if (p.contains(k))
          throw std::runtime_error(
              std::string("a ") + DocumentIO::RelationKindToString(r.kind) +
              " relation names its members, not a and b: " +
              (r.kind == RelationKind::LinearN
                   ? "terms [{mate, dof?, coefficient}, ...]"
               : r.kind == RelationKind::Epicyclic
                   ? "sun, ring and carrier"
                   : "planet, sun and carrier"));
      if (p.contains("ratio"))
        throw std::runtime_error(
            std::string("a ") + DocumentIO::RelationKindToString(r.kind) +
            " relation has no ratio: " +
            (r.kind == RelationKind::LinearN
                 ? "each term carries its coefficient"
                 : "its coefficients follow from the tooth counts"));
      if (p.contains("phase"))
        throw std::runtime_error(
            "a relation over several mates takes `constant` (sum of "
            "coefficient * dof), not a phase in one member's unit");
      // rpc-params: sun, ring, carrier, planet
      // rpc-params: teeth_sun, teeth_ring, teeth_planet
      const auto member = [&](const char* key) {
        return TermFromRequest(state, p.at(key), key, false);
      };
      const auto teeth = [&](const char* key) {
        return TeethFromRequest(p.at(key), key);
      };
      std::vector<Relation> made;
      if (r.kind == RelationKind::LinearN) {
        const json& terms = p.at("terms");
        if (!terms.is_array())
          throw std::runtime_error(
              "terms is [{mate, path?, dof?, coefficient}, ...]");
        for (size_t i = 0; i < terms.size(); i++)
          r.terms.push_back(TermFromRequest(
              state, terms.at(i), "term " + std::to_string(i + 1), true));
        r.ratio = 1.0;
        made.push_back(std::move(r));
      } else if (r.kind == RelationKind::Epicyclic) {
        made.push_back(assembly::MakeEpicyclic(
            member("sun"), member("ring"), member("carrier"),
            teeth("teeth_sun"), teeth("teeth_ring")));
        // Each planet's own turn on the carrier, which the Willis
        // equation does not give: a planet relation per planet.
        if (p.contains("planets")) {
          const json& planets = p.at("planets");
          if (!planets.is_array())
            throw std::runtime_error(
                "planets is [{mate, teeth, path?, constant?}, ...]");
          for (size_t i = 0; i < planets.size(); i++) {
            const json& pj = planets.at(i);
            const std::string role = "planet " + std::to_string(i + 1);
            if (!pj.is_object() || !pj.contains("teeth"))
              throw std::runtime_error(role +
                                       " is {mate, teeth, path?, constant?}");
            json bare = pj;
            bare.erase("teeth");
            bare.erase("constant");
            Relation planet = assembly::MakePlanet(
                TermFromRequest(state, bare, role, false), made[0].terms[0],
                made[0].terms[2],
                TeethFromRequest(pj.at("teeth"), role + " teeth"),
                made[0].terms[0].teeth);
            planet.name = p.value("name", std::string{}).empty()
                              ? std::string{}
                              : p.value("name", std::string{}) + " / " + role;
            if (pj.contains("constant"))
              planet.phase = pj.at("constant").get<double>();
            else
              planet.phase = ConstantAsItStands(state, planet);
            made.push_back(std::move(planet));
          }
        }
      } else {
        made.push_back(assembly::MakePlanet(
            member("planet"), member("sun"), member("carrier"),
            teeth("teeth_planet"), teeth("teeth_sun")));
      }
      made[0].name = p.value("name", std::string{});
      made[0].phase = p.contains("constant")
                          ? p.at("constant").get<double>()
                          : ConstantAsItStands(state, made[0]);
      for (const Relation& each : made)
        if (std::string why = assembly::RelationProblem(
                *state.assembly, each, ui::AssemblyResolverFor(state));
            !why.empty())
          throw std::runtime_error(why);
      // One planet mate named twice across the planets is a member used
      // twice too, though no single relation shows it.
      for (size_t i = 1; i < made.size(); i++)
        for (size_t k = 1; k < i; k++)
          if (made[i].terms[2].mate == made[k].terms[2].mate &&
              made[i].terms[2].path == made[k].terms[2].path)
            throw std::runtime_error("mate " +
                                     std::to_string(made[i].terms[2].mate) +
                                     " is planet " + std::to_string(k) +
                                     " and planet " + std::to_string(i) +
                                     " — each planet turns on a mate of its "
                                     "own");
      // Gears share an axis direction: a member more than 5 degrees off the
      // first one's is named, as a gear pair's skew axles are.
      json warnings = json::array();
      if (r.kind != RelationKind::LinearN) {
        std::optional<glm::dvec3> axis;
        for (const Relation& each : made)
          for (size_t i = 0; i < each.terms.size(); i++) {
            const RelationTerm& t = each.terms[i];
            const Mate* m =
                assembly::MateAtPath(*state.assembly, t.path, t.mate,
                                     ui::AssemblyResolverFor(state));
            const auto f = m ? ui::MateConnectorWorldAt(state, t.path, m->a)
                             : std::nullopt;
            if (!f)
              continue;
            const glm::dvec3 z = glm::normalize(glm::dvec3((*f)[2]));
            if (!axis)
              axis = z;
            else if (std::abs(glm::dot(*axis, z)) < 0.9962)
              warnings.push_back(
                  "the " + assembly::RelationTermRole(each, i) +
                  "'s axis (mate " + std::to_string(t.mate) + ") is " +
                  std::to_string(glm::degrees(
                      std::acos(std::min(1.0, std::abs(glm::dot(*axis, z)))))) +
                  " degrees off the sun's: an epicyclic train turns about "
                  "one axis");
          }
      }
      ui::SnapshotAssembly(state);
      std::vector<uint64_t> ids;
      for (Relation& each : made)
        ids.push_back(state.assembly->AddRelation(std::move(each)));
      ui::SolveActiveAssembly(state);
      const auto withStatus = [&](uint64_t id) {
        const Relation* rel = state.assembly->FindRelationById(id);
        json rj = DocumentIO::RelationToJson(*rel);
        if (!rel->error.empty())
          rj["error"] = rel->error;
        if (!rel->held.empty())
          rj["held"] = rel->held;
        return rj;
      };
      json out = withStatus(ids[0]);
      if (ids.size() > 1) {
        json planets = json::array();
        for (size_t i = 1; i < ids.size(); i++)
          planets.push_back(withStatus(ids[i]));
        out["planets"] = std::move(planets);
      }
      if (!warnings.empty())
        out["warnings"] = warnings;
      return out;
    }
    r.mateA = p.at("a").get<uint64_t>();
    r.mateB = p.at("b").get<uint64_t>();
    // rpc-params: a_path, b_path
    // v129: either side may be a mate inside a flexible placement, named
    // {a_path | b_path: [occurrence id], a | b: its sub-assembly's mate id}.
    const Mate* a = MateFromRequest(state, p, "a_path", r.mateA, r.mateAPath);
    const Mate* b = MateFromRequest(state, p, "b_path", r.mateB, r.mateBPath);
    for (const std::vector<uint64_t>* path : {&r.mateAPath, &r.mateBPath})
      if (!path->empty() &&
          (DocumentIO::RelationKindFromString(
               p.at("type").get<std::string>()) == RelationKind::Freewheel ||
           DocumentIO::RelationKindFromString(
               p.at("type").get<std::string>()) == RelationKind::Click))
        throw std::runtime_error(
            "a freewheel or a click into a flexible placement is refused: its "
            "history (lead) would be per-placement state — gear, chain, "
            "rack, screw, linear and cam relations can name an inner mate");
    const bool samePath = r.mateAPath == r.mateBPath;
    r.name = p.value("name", std::string{});
    r.phase = p.value("phase", 0.0);
    if (p.contains("drives") && r.kind != RelationKind::Freewheel &&
        r.kind != RelationKind::Click)
      throw std::runtime_error(
          "drives is a freewheel's or a click's direction; a " +
          std::string(DocumentIO::RelationKindToString(r.kind)) +
          " drives both ways");
    r.threadPart = p.value("thread_part", uint64_t{0});
    r.threadFeature = p.value("thread_feature", uint64_t{0});
    if ((r.threadPart || r.threadFeature) && r.kind != RelationKind::Screw)
      throw std::runtime_error(
          "thread references are only valid on screw relations");
    // The kind fixes which DOF of each mate is coupled, and how the ratio is
    // derived from the kind's own numbers.
    switch (r.kind) {
    case RelationKind::Gear:
    case RelationKind::Chain:
      // A chain is a gear whose ratio KEEPS its sign: a chain or a belt
      // turns both sprockets the same way. Expressing one as a gear with a
      // negated tooth count works and makes the sign a thing to remember
      // instead of a thing to say, which is how a reversed conveyor gets
      // modelled. So is an INTERNAL gear (v171): a planet in its ring
      // turns the same way as the ring.
      r.dofA = r.dofB = MateDof::Rotation;
      r.internal = p.value("internal", false);
      if (r.internal && r.kind != RelationKind::Gear)
        throw std::runtime_error(
            "internal is a gear's (one meshing inside a ring); a chain keeps "
            "its sign already");
      if (p.contains("teeth_a") || p.contains("teeth_b")) {
        if (!p.contains("teeth_a") || !p.contains("teeth_b"))
          throw std::runtime_error(
              std::string("a ") + DocumentIO::RelationKindToString(r.kind) +
              " ratio needs both teeth_a and teeth_b (positive)");
        if (p.contains("ratio"))
          throw std::runtime_error(
              std::string("a ") + DocumentIO::RelationKindToString(r.kind) +
              "'s ratio is derived from teeth_a and teeth_b: give the teeth "
              "or a ratio, not both");
        r.teethA = TeethFromRequest(p.at("teeth_a"), "teeth_a");
        r.teethB = TeethFromRequest(p.at("teeth_b"), "teeth_b");
        (void)assembly::DeriveFromTeeth(r);
      } else if (r.internal) {
        throw std::runtime_error(
            "internal derives the ratio from teeth_a and teeth_b (+za/zb); "
            "a bare ratio already carries its sign, so give the teeth");
      }
      break;
    case RelationKind::Rack:
      r.dofA = MateDof::Rotation;
      r.dofB = MateDof::Offset;
      if (p.contains("pitch_radius")) {
        r.ratio = p.at("pitch_radius").get<double>();
        if (r.ratio <= 0.0)
          throw std::runtime_error("pitch_radius must be positive (mm)");
      }
      break;
    case RelationKind::Screw:
      r.dofA = MateDof::Rotation;
      r.dofB = MateDof::Offset;
      if (r.mateA != r.mateB || !samePath)
        throw std::runtime_error(
            "a screw relation is one CYLINDRICAL mate's rotation driving its "
            "own offset: pass the same mate id as a and b");
      if (r.threadPart || r.threadFeature) {
        if (!r.threadPart || !r.threadFeature || p.contains("pitch") ||
            p.contains("ratio"))
          throw std::runtime_error("thread-backed screw needs thread_part and "
                                   "thread_feature, without pitch or ratio");
        r.ratio = 1.0; // derived by the solve
      } else if (p.contains("pitch")) {
        const double pitch = p.at("pitch").get<double>();
        if (pitch <= 0.0)
          throw std::runtime_error("pitch must be positive (mm per turn)");
        r.ratio = pitch / (2.0 * M_PI);
      }
      break;
    case RelationKind::Linear:
      r.dofA = r.dofB = MateDof::Offset;
      break;
    case RelationKind::Freewheel:
      // A one-way clutch (docs/assemblies/relations.md#freewheel). Its
      // direction is the whole of it, so it is required: a default would be a
      // plausible wrong answer, and it is exactly the one mistake a freewheel
      // exists for.
      r.dofA = r.dofB = MateDof::Rotation;
      if (!p.contains("drives"))
        throw std::runtime_error(
            "a freewheel needs drives: positive or negative — the direction "
            "b is driven in; it slips the other way");
      r.drives = DocumentIO::FreewheelDrivesFromString(
          p.at("drives").get<std::string>());
      if (!p.contains("ratio"))
        throw std::runtime_error("a freewheel needs its ratio (1 for a clutch "
                                 "between two parts on one arbor)");
      if (p.contains("teeth")) {
        r.teeth = p.at("teeth").get<int>();
        if (r.teeth <= 0)
          throw std::runtime_error(
              "teeth is how many dogs a freewheel re-engages on (positive)");
      }
      if (p.contains("ride_mate") || p.contains("ride_mm") ||
          p.contains("ride_deg") || p.contains("ride_dof")) {
        // What riding over the teeth lifts — a watch's clutch, along its
        // stem, which is the whole reason sawtooth dogs can slip.
        if (r.teeth <= 0)
          throw std::runtime_error("a ride needs teeth: the lift is where "
                                   "the clutch stands between them");
        if (!p.contains("ride_mate"))
          throw std::runtime_error(
              "a ride needs ride_mate: the mate whose degree of freedom "
              "riding over the teeth lifts");
        r.rideMate = p.at("ride_mate").get<uint64_t>();
        r.rideDof = p.contains("ride_dof")
                        ? DocumentIO::MateDofFromString(
                              p.at("ride_dof").get<std::string>())
                        : MateDof::Offset;
        const Mate* rm = state.assembly->FindMateById(r.rideMate);
        if (rm == nullptr)
          throw std::runtime_error("no mate with id " +
                                   std::to_string(r.rideMate) +
                                   " to lift; list_mates shows them");
        if (!assembly::MateHasDof(rm->type, r.rideDof))
          throw std::runtime_error(
              "mate " + std::to_string(r.rideMate) + " has no " +
              DocumentIO::MateDofToString(r.rideDof) + " to lift");
        const bool turns = r.rideDof == MateDof::Rotation;
        if (turns && p.contains("ride_mm"))
          throw std::runtime_error(
              "this ride lifts a ROTATION: give ride_deg, not ride_mm");
        if (!turns && p.contains("ride_deg"))
          throw std::runtime_error(
              "this ride lifts a SLIDE: give ride_mm, not ride_deg");
        r.rideAmount = turns ? angles::ToRadians(p.value("ride_deg", 0.0))
                             : p.value("ride_mm", 0.0);
        if (r.rideAmount == 0.0)
          throw std::runtime_error("a ride needs how far it lifts (ride_mm "
                                   "for a slide, ride_deg for a turn)");
      }
      break;
    case RelationKind::Worm:
      // A worm drives its wheel on crossed axes. The size of the ratio is
      // starts/teeth; its SIGN is the hand and the side the worm sits on,
      // read from the mates' frames below — a gear relation's fixed
      // -za/zb is right for one of the four ways a pair can be assembled.
      r.dofA = r.dofB = MateDof::Rotation;
      if (p.contains("ratio"))
        throw std::runtime_error(
            "a worm's ratio follows from starts, teeth, hand and where the "
            "worm sits; give those, not a ratio");
      if (!p.contains("starts") || !p.contains("teeth") || !p.contains("hand"))
        throw std::runtime_error(
            "a worm relation needs starts (the worm's), teeth (the wheel's) "
            "and hand: right or left — a is the worm's mate, b the wheel's");
      {
        const int z1 = p.at("starts").get<int>(), z2 = p.at("teeth").get<int>();
        if (z1 <= 0 || z2 <= 0)
          throw std::runtime_error("starts and teeth are positive counts");
        const std::string hand = p.at("hand").get<std::string>();
        if (hand != "right" && hand != "left")
          throw std::runtime_error("unknown hand '" + hand +
                                   "' (accepted: right, left)");
        r.ratio = static_cast<double>(z1) / z2 * (hand == "right" ? 1.0 : -1.0);
      }
      break;
    case RelationKind::Click:
      // A pawl: one mate's rotation, free one way and held the other.
      r.dofA = r.dofB = MateDof::Rotation;
      if (r.mateA != r.mateB)
        throw std::runtime_error(
            "a click holds ONE mate's rotation: pass the same mate id as a "
            "and b");
      if (!p.contains("drives"))
        throw std::runtime_error(
            "a click needs drives: positive or negative — the way it lets "
            "the mate turn; it holds the other");
      r.drives = DocumentIO::FreewheelDrivesFromString(
          p.at("drives").get<std::string>());
      r.ratio = 1.0;
      break;
    case RelationKind::Cam: {
      // A lift table, not a ratio: the follower's position is a function of
      // the cam's angle. Either the table itself, or the three numbers a
      // cosine lobe is described by.
      //
      // The follower is the DOF mate b HAS: a slider's offset, a revolute's
      // rotation (a rocker, a pallet fork). A cylindrical mate has both, so
      // `dof_b` names it, and omitted it is the offset — what every caller
      // meant before a follower could turn. A mate with neither falls to the
      // lacks-check below, which names both.
      r.dofA = MateDof::Rotation;
      const bool bSlides = assembly::MateHasDof(b->type, MateDof::Offset);
      const bool bTurns = assembly::MateHasDof(b->type, MateDof::Rotation);
      if (p.contains("dof_b"))
        r.dofB =
            DocumentIO::MateDofFromString(p.at("dof_b").get<std::string>());
      else
        r.dofB = bTurns && !bSlides ? MateDof::Rotation : MateDof::Offset;
      if (r.mateA == r.mateB && r.dofB == MateDof::Rotation)
        throw std::runtime_error(
            "a cam on one mate turns into its own slide, not its own turn: "
            "dof_b rotation would couple the rotation to itself");
      // The lift is in the FOLLOWER's unit — mm for a slide, degrees for a
      // turn — and the solve converts a turn's degrees where it reads them.
      const bool turns = r.dofB == MateDof::Rotation;
      if (p.contains("lift")) {
        for (const json& pt : p.at("lift")) {
          if (!pt.is_array() || pt.size() != 2)
            throw std::runtime_error(
                "lift is [[angle_deg, lift], ...] — a point is a pair, the "
                "lift in mm for a sliding follower and degrees for a turning "
                "one");
          r.lift.emplace_back(pt.at(0).get<double>(), pt.at(1).get<double>());
        }
      } else if (p.contains("rise") || p.contains("rise_deg") ||
                 p.contains("duration_deg")) {
        // The wrong rise for the follower is refused, never converted: 8
        // given as `rise` to a rocker is 8 mm to the caller and would be 8
        // degrees to the solve.
        if (turns && p.contains("rise"))
          throw std::runtime_error(
              "this cam's follower TURNS: its lobe is rise_deg, not rise "
              "(mm)");
        if (!turns && p.contains("rise_deg"))
          throw std::runtime_error(
              "this cam's follower SLIDES: its lobe is rise (mm), not "
              "rise_deg");
        const double rise = p.value(turns ? "rise_deg" : "rise", 0.0);
        const double duration = p.value("duration_deg", 180.0);
        const double start = p.value("start_deg", 0.0);
        if (rise <= 0.0)
          throw std::runtime_error(turns ? "a cosine lobe needs rise_deg > 0"
                                         : "a cosine lobe needs rise > 0 (mm)");
        if (duration <= 0.0 || duration >= 360.0)
          throw std::runtime_error(
              "duration_deg is the lobe's width, 0 < duration < 360");
        // A full-rise cosine over `duration`, flat elsewhere: the standard
        // harmonic lobe, sampled every 10 degrees of the lobe so the spline
        // through it is the lobe and not a corner-cutting of it.
        // The profile is PERIODIC, so every angle is wrapped into [0, 360):
        // a lobe that opens at 200 degrees and lasts 150 runs past the end
        // of the turn, and generating 455 made the table refuse itself.
        const auto wrap = [](double d) {
          d = std::fmod(d, 360.0);
          return d < 0.0 ? d + 360.0 : d;
        };
        // Both ENDS of the lobe, not just the opening one. `duration` is
        // less than a full turn, so the point at f = 1 is where the lobe
        // CLOSES and is a different angle from where it opens — dropping it
        // (on the reasoning that a periodic table's last point is its first)
        // left a 180-degree lobe with no point at 180. Its closing flank
        // then ran from the 170-degree sample all the way to the dwell in
        // the middle of the base circle, and the follower stood 0.18 mm off
        // its seat at the angle the lobe was specified to close at.
        const int steps = 18;
        for (int i = 0; i <= steps; i++) {
          const double f = static_cast<double>(i) / steps;
          r.lift.emplace_back(wrap(start + f * duration),
                              rise * 0.5 * (1.0 - std::cos(2.0 * M_PI * f)));
        }
        // The dwell: one point in the middle of the closed span, so the
        // spline has something to hold flat between lobes.
        r.lift.emplace_back(wrap(start + duration + (360.0 - duration) * 0.5),
                            0.0);
      } else {
        throw std::runtime_error(
            "a cam needs its profile: lift [[angle_deg, lift], ...], or "
            "rise (mm) or rise_deg + duration_deg (+ start_deg) for a cosine "
            "lobe");
      }
      if (r.lift.size() < 2)
        throw std::runtime_error("a cam's lift table needs at least 2 points");
      std::sort(r.lift.begin(), r.lift.end());
      for (size_t i = 0; i + 1 < r.lift.size(); i++)
        if (r.lift[i + 1].first - r.lift[i].first < 1e-9)
          throw std::runtime_error(
              "two lift points share the angle " +
              std::to_string(r.lift[i].first) +
              " degrees — a profile is a function of the angle");
      if (r.lift.front().first < 0.0 || r.lift.back().first >= 360.0)
        throw std::runtime_error(
            "lift angles are 0 <= angle < 360; the table wraps, so the last "
            "point joins the first");
      r.ratio = 1.0; // unused by a cam, but never zero (the zero check below)
      break;
    }
    case RelationKind::LinearN:
    case RelationKind::Epicyclic:
    case RelationKind::Planet:
      break; // handled above, before a and b are read
    }
    if (p.contains("ratio") && r.kind != RelationKind::Cam)
      r.ratio = p.at("ratio").get<double>();
    else if (r.kind != RelationKind::Cam && r.kind != RelationKind::Click &&
             r.kind != RelationKind::Worm &&
             !(p.contains("teeth_a") || p.contains("pitch_radius") ||
               p.contains("pitch") || r.threadPart))
      throw std::runtime_error(
          "give the ratio, or what it is derived from: teeth_a + teeth_b for "
          "a gear or a chain, pitch_radius for a rack, pitch for a screw, a "
          "lift table for a cam; a click needs none, holding one mate's own "
          "rotation");
    if (r.ratio == 0.0)
      throw std::runtime_error("a relation's ratio cannot be zero");
    const auto lacks = [](const Mate& mm, MateDof dof) {
      return !assembly::MateHasDof(mm.type, dof);
    };
    if (lacks(*a, r.dofA) || lacks(*b, r.dofB)) {
      const Mate& bad = lacks(*a, r.dofA) ? *a : *b;
      const MateDof dof = lacks(*a, r.dofA) ? r.dofA : r.dofB;
      // Name what THIS side needs, from the dof it lacks. Naming it from the
      // driver's dof refused a cam's revolute follower with "a cam needs
      // revolute or cylindrical mates" — the refused type offered as the
      // cure.
      const bool followerSide = !lacks(*a, r.dofA);
      // A cam's follower chosen from the mate (no dof_b) may slide OR turn,
      // so a mate with neither is told both.
      const bool camEither =
          r.kind == RelationKind::Cam && followerSide && !p.contains("dof_b");
      const std::string need = camEither ? "a slider, revolute or "
                                           "cylindrical mate"
                               : dof == MateDof::Rotation
                                   ? "a revolute or cylindrical mate"
                                   : "a slider or cylindrical mate";
      const std::string why = r.kind != RelationKind::Cam ? ""
                              : camEither    ? " — a cam's follower slides or "
                                               "turns"
                              : followerSide ? ""
                                             : " — a cam's driver turns";
      throw std::runtime_error(
          "mate " + std::to_string(bad.id) + " (" +
          DocumentIO::MateToJson(bad).at("mate_type").get<std::string>() +
          ") has no " + DocumentIO::MateDofToString(dof) + " to couple — " +
          (followerSide ? "b" : "a") + " of a " +
          DocumentIO::RelationKindToString(r.kind) + " needs " + need + why);
    }
    // The geometry a kind implies, checked against the mates' own frames —
    // a mate's DOF axis is its A connector's Z. A rack that slides ALONG the
    // pinion's axis is never a rack (the first demo did exactly that, and
    // read as "the pinion moves up"), so it is refused; gears on skew axes
    // are unusual but real (bevel, crossed helical), so those get a warning.
    json warnings = json::array();
    {
      const auto fa = ui::MateConnectorWorldAt(state, r.mateAPath, a->a);
      const auto fb = ui::MateConnectorWorldAt(state, r.mateBPath, b->a);
      if (r.kind == RelationKind::Worm && !(fa && fb))
        throw std::runtime_error(
            "a worm relation reads its sign from where the two mates' axes "
            "are, and these mates' frames cannot be resolved");
      if (fa && fb) {
        const glm::dvec3 za = glm::normalize(glm::dvec3((*fa)[2]));
        const glm::dvec3 zb = glm::normalize(glm::dvec3((*fb)[2]));
        const double along = std::abs(glm::dot(za, zb)); // 1 parallel, 0 across
        if (r.kind == RelationKind::Rack && along > 0.7071)
          throw std::runtime_error(
              "mate " + std::to_string(b->id) + " slides along mate " +
              std::to_string(a->id) +
              "'s axis; a rack slides ACROSS the pinion's axis. Mate the "
              "rack by faces whose normal is the direction it travels "
              "(a slider slides along its connector frames' Z), not by "
              "origin frames, which are Z-up");
        if (r.kind == RelationKind::Worm) {
          if (along > 0.0872) // > 5 degrees off square
            warnings.push_back(
                "a worm and its wheel turn on axes at 90 degrees; these are " +
                std::to_string(glm::degrees(std::acos(along))) +
                " degrees apart");
          std::string whyNot;
          const double ratio = worm::RelationRatio(
              *state.assembly, *a, *b, r.mateAPath == r.mateBPath,
              r.mateAPath.empty() && r.mateBPath.empty(), *fa, *fb,
              p.at("starts").get<int>(), p.at("teeth").get<int>(),
              r.ratio > 0.0, &whyNot);
          if (ratio == 0.0)
            throw std::runtime_error(whyNot);
          r.ratio = ratio;
        }
        if (r.kind == RelationKind::Gear && along < 0.9962) // > 5 degrees
          warnings.push_back(
              "the two axles are not parallel (" +
              std::to_string(glm::degrees(std::acos(along))) +
              " degrees apart): fine for a bevel or crossed pair, wrong "
              "for spur gears");
      }
    }
    if (r.kind == RelationKind::Freewheel) {
      // A clutch has no preferred angle. Made between two parts that already
      // stand somewhere, it engages WHERE THEY ARE: `lead` starts as the gap
      // between them, so adding one moves nothing and never reads as two
      // drivers disagreeing — which is what a reverser's second clutch is,
      // with both its ends already placed by the gears around it.
      r.lead =
          p.contains("lead")
              ? p.at("lead").get<double>()
              : assembly::MateDofValue(*b, r.dofB) -
                    (r.ratio * assembly::MateDofValue(*a, r.dofA) + r.phase);
    } else if (r.kind == RelationKind::Click) {
      // And a pawl is made where its wheel STANDS: its mark is that angle,
      // not zero, which would hold a wheel that starts behind it.
      r.lead = p.contains("lead") ? p.at("lead").get<double>()
                                  : assembly::MateDofValue(*b, r.dofB);
    }
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddRelation(std::move(r));
    ui::SolveActiveAssembly(state);
    json out =
        DocumentIO::RelationToJson(*state.assembly->FindRelationById(id));
    if (const Relation* made = state.assembly->FindRelationById(id);
        made != nullptr && !made->error.empty())
      out["error"] = made->error;
    if (!warnings.empty())
      out["warnings"] = warnings;
    return out;
  }
  if (m == "set_relation") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    Relation* r = state.assembly->FindRelationById(p.at("id").get<uint64_t>());
    if (!r)
      throw std::runtime_error("no such relation; list_relations shows them");
    if (p.contains("ratio") && r->threadPart)
      throw std::runtime_error(
          "edit the referenced thread; its pitch owns this ratio");
    if (p.contains("lead") && r->kind != RelationKind::Freewheel &&
        r->kind != RelationKind::Click)
      throw std::runtime_error(
          "lead is a freewheel's or a click's history; a " +
          std::string(DocumentIO::RelationKindToString(r->kind)) + " has none");
    // v171: what a relation's numbers came from is what is edited. Every
    // key is checked against the kind before anything changes.
    const std::string kindName = DocumentIO::RelationKindToString(r->kind);
    const auto refuse = [&](const std::string& key, const std::string& why) {
      throw std::runtime_error(key + " is not a " + kindName +
                               " relation's: " + why);
    };
    if (r->HasTerms()) {
      if (p.contains("ratio"))
        refuse("ratio", r->kind == RelationKind::LinearN
                            ? "its terms carry their coefficients"
                            : "its coefficients follow from its teeth");
      if (p.contains("phase"))
        refuse("phase", "a relation over several mates has a constant");
    } else if (p.contains("constant")) {
      refuse("constant", "it is dof_b = ratio * dof_a + phase");
    }
    if (p.contains("ratio") && r->teethA > 0)
      refuse("ratio", "it is derived from teeth_a and teeth_b — edit those");
    const bool toothed =
        r->kind == RelationKind::Gear || r->kind == RelationKind::Chain;
    for (const char* k : {"teeth_a", "teeth_b"})
      if (p.contains(k) && (!toothed || r->teethA <= 0))
        refuse(k, toothed ? "it was made from a bare ratio"
                          : "only a gear's or a chain's");
    if (p.contains("internal") && r->kind != RelationKind::Gear)
      refuse("internal", "only a gear's");
    if (p.contains("internal") && r->teethA <= 0)
      refuse("internal", "a gear made from a bare ratio carries its sign "
                         "in the ratio");
    if (p.contains("teeth_sun") && r->kind != RelationKind::Epicyclic &&
        r->kind != RelationKind::Planet)
      refuse("teeth_sun", "only an epicyclic's or a planet's");
    if (p.contains("teeth_ring") && r->kind != RelationKind::Epicyclic)
      refuse("teeth_ring", "only an epicyclic's");
    if (p.contains("teeth_planet") && r->kind != RelationKind::Planet)
      refuse("teeth_planet", "only a planet's");
    Relation edited = *r;
    if (p.contains("teeth_a"))
      edited.teethA = TeethFromRequest(p.at("teeth_a"), "teeth_a");
    if (p.contains("teeth_b"))
      edited.teethB = TeethFromRequest(p.at("teeth_b"), "teeth_b");
    if (p.contains("internal"))
      edited.internal = p.at("internal").get<bool>();
    if (p.contains("teeth_sun"))
      edited.terms[0].teeth = TeethFromRequest(p.at("teeth_sun"), "teeth_sun");
    if (p.contains("teeth_ring"))
      edited.terms[1].teeth =
          TeethFromRequest(p.at("teeth_ring"), "teeth_ring");
    if (p.contains("teeth_planet"))
      edited.terms[2].teeth =
          TeethFromRequest(p.at("teeth_planet"), "teeth_planet");
    if (edited.teethA > 0 || !edited.terms.empty())
      (void)assembly::DeriveFromTeeth(edited);
    if (p.contains("constant"))
      edited.phase = p.at("constant").get<double>();
    if (p.contains("ratio") && p.at("ratio").get<double>() == 0.0)
      throw std::runtime_error("a relation's ratio cannot be zero");
    ui::SnapshotAssembly(state);
    r->teethA = edited.teethA;
    r->teethB = edited.teethB;
    r->internal = edited.internal;
    r->terms = edited.terms;
    if (r->teethA > 0 || r->HasTerms()) {
      r->ratio = edited.ratio;
      r->phase = edited.phase;
    }
    if (p.contains("lead")) {
      // Putting the history back moves the OUTPUT onto the input's drive
      // line at that lead — otherwise the next solve reads the output as
      // standing ahead of the line and grows `lead` straight back.
      r->lead = p.at("lead").get<double>();
      const Mate* a = state.assembly->FindMateById(r->mateA);
      if (Mate* b = state.assembly->FindMateById(r->mateB); a && b)
        assembly::SetMateDof(
            *b, r->dofB,
            assembly::RelationValue(*r, assembly::MateDofValue(*a, r->dofA)));
    }
    if (p.contains("ratio"))
      r->ratio = p.at("ratio").get<double>();
    if (p.contains("phase"))
      r->phase = p.at("phase").get<double>();
    if (p.contains("name"))
      r->name = p.at("name").get<std::string>();
    state.assembly->BumpVersion();
    ui::SolveActiveAssembly(state);
    json out = DocumentIO::RelationToJson(*r);
    if (!r->error.empty())
      out["error"] = r->error;
    if (!r->held.empty())
      out["held"] = r->held;
    return out;
  }
  if (m == "remove_relation") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveRelation(p.at("id").get<uint64_t>())) {
      state.assemblyHistory.undo.pop_back(); // nothing changed; drop snapshot
      throw std::runtime_error("no such relation");
    }
    ui::SolveActiveAssembly(state);
    return json::object();
  }
  if (m == "list_relations") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    json arr = json::array();
    for (const Relation& r : state.assembly->GetRelations()) {
      json rj = DocumentIO::RelationToJson(r);
      if (!r.error.empty())
        rj["error"] = r.error;
      if (!r.held.empty()) // a member nothing drove, held where it stood
        rj["held"] = r.held;
      arr.push_back(rj);
    }
    return json{{"relations", arr}};
  }
  if (m == "select_mate") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const uint64_t id = p.value("id", uint64_t{0}); // 0 clears the selection
    if (id && !state.assembly->FindMateById(id))
      throw std::runtime_error("no such mate");
    ui::SelectMate(state, id);
    return json{{"selected", id}};
  }
  if (m == "select_occurrence") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    // `occurrence` as well as `id`, like its neighbours. 0 (or neither)
    // clears the selection.
    const uint64_t id = p.contains("occurrence")
                            ? p.at("occurrence").get<uint64_t>()
                            : p.value("id", uint64_t{0});
    if (id && !state.assembly->FindById(id))
      throw std::runtime_error(
          "no occurrence with id " + std::to_string(id) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
    ui::SelectOccurrence(state, id); // highlights + shows the drag gizmo
    return json{{"selected", id}};
  }
  if (m == "solve_assembly") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const assembly::SolveReport rep = ui::SolveActiveAssemblyReport(state);
    json arr = json::array();
    const std::set<uint64_t> redundant(rep.redundantMates.begin(),
                                       rep.redundantMates.end());
    for (const Mate& mate : state.assembly->GetMates()) {
      // `status` in words, because the absence of an `error` key is not a
      // statement: a caller filtering on a field that is only sometimes
      // there reads "no news" as "not satisfied" (or the reverse) and
      // reports every mate as broken.
      const char* status = !mate.error.empty()        ? "error"
                           : redundant.count(mate.id) ? "redundant"
                                                      : "satisfied";
      json mj = {{"id", mate.id}, {"name", mate.name}, {"status", status}};
      if (!mate.error.empty())
        mj["error"] = mate.error;
      if (mate.residualMm > 0.0) // its OWN miss, not the document's worst
        mj["residual_mm"] = mate.residualMm;
      arr.push_back(mj);
    }
    // A6: report what the loop pass found HERE, in the mutating call's own
    // response, rather than leaving it for a get_state the caller has to know
    // to poll (the regen-honesty rule).
    // How much of the assembly the mate system actually DETERMINES. Without
    // this, `dof: 0` reads as a full bill of health and is not one: it is the
    // freedom left in the LOOPED components (AssemblySolve.h says so), and it
    // is silent about every occurrence no mate touches. The drone reports
    // "56 mates, dof 0, loops True" over 105 occurrences of which 64 — all
    // four standoffs, both camera plates, the camera, every nut — hold their
    // position because someone typed it.
    //
    // That is not an error. Placing a nut by transform is a legitimate thing
    // to do, and refusing it would be wrong. It is an unstated fact, and the
    // fix for an unstated fact is to state it.
    std::set<uint64_t> mated;
    for (const Mate& mate : state.assembly->GetMates()) {
      mated.insert(mate.a.occurrenceId);
      mated.insert(mate.b.occurrenceId);
    }
    size_t byTransform = 0;
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (!mated.count(o.id))
        byTransform++;
    json out{{"errored_mates", rep.erroredMates},
             {"placed", rep.placed},
             {"occurrences", state.assembly->GetOccurrences().size()},
             {"placed_by_mates",
              state.assembly->GetOccurrences().size() - byTransform},
             {"placed_by_transform", byTransform},
             {"mates", arr}};
    // v129: every flexible placement, per path — its mates are the
    // sub-assembly's and are not in `mates` above.
    if (json rows = FlexibleRows(state, rep); !rows.empty())
      out["flexible"] = std::move(rows);
    // A rider whose hole is gone stays where it was: said here, in the
    // answer to the solve that found it
    // (docs/parts/README.md#attached-and-mated).
    if (rep.erroredAttachments > 0) {
      out["errored_attachments"] = rep.erroredAttachments;
      json lost = json::array();
      for (const Occurrence& o : state.assembly->GetOccurrences())
        if (!o.attachError.empty())
          lost.push_back(json{{"occurrence", o.id},
                              {"name", o.name},
                              {"error", o.attachError}});
      out["attachment_errors"] = std::move(lost);
    }
    if (rep.loopMates > 0) {
      out["loop_mates"] = rep.loopMates;
      out["dof"] = rep.dof;
      out["loops_closed"] = rep.loopsClosed;
      out["residual_mm"] = rep.residualMm;
      out["redundant_mates"] = rep.redundantMates;
      // Which connector, tilted, would close a loop that did not close
      // (docs/assemblies/solver.md#naming-the-mate-that-keeps-a-loop-open).
      // `closes` groups them by loop; `sole` is false when that loop has
      // several equally good suspects and the numbers do not pick one.
      json suspects = json::array();
      for (const assembly::LoopDiagnosis& d : rep.diagnoses)
        for (const assembly::LoopTiltTrial& t : d.suspects) {
          const Mate* sm = state.assembly->FindMateById(t.mate);
          json sj = {{"mate", t.mate},
                     {"name", sm != nullptr ? sm->name : std::string()},
                     {"tilt_deg", angles::ToDegrees(t.tiltRad)},
                     {"closes", d.conflicting},
                     {"sole", d.suspects.size() == 1}};
          if (t.connector != 0)
            sj["connector"] = std::string(1, t.connector);
          suspects.push_back(std::move(sj));
        }
      if (!rep.loopsClosed)
        out["suspects"] = std::move(suspects);
    }
    return out;
  }
  if (m == "add_exploded_view") {
    // params: {name?} — appends an empty view. Returns its id.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ui::SnapshotAssembly(state);
    ExplodedView v;
    v.name = p.value("name", std::string{"Exploded"});
    const uint64_t id = state.assembly->AddExplodedView(std::move(v));
    return json{{"id", id}};
  }
  if (m == "remove_exploded_view") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const uint64_t id = p.at("id").get<uint64_t>();
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveExplodedView(id))
      throw std::runtime_error("no such exploded view");
    if (state.activeExplodedView == id) {
      state.activeExplodedView = 0;
      state.syncedDocumentVersion = UINT64_MAX;
    }
    state.assembly->BumpVersion();
    return json::object();
  }
  if (m == "list_exploded_views") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    json views = json::array();
    for (const ExplodedView& v : state.assembly->GetExplodedViews())
      views.push_back(DocumentIO::ExplodedViewToJson(v));
    return json{{"views", views},
                {"active", state.activeExplodedView},
                {"factor", state.explodeFactor}};
  }
  if (m == "set_explode_step") {
    // params: {view, occurrence, offset: [x,y,z]} — find-or-create the step.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ExplodedView* v =
        state.assembly->FindExplodedViewById(p.at("view").get<uint64_t>());
    if (!v)
      throw std::runtime_error("no such exploded view");
    const uint64_t occId = p.at("occurrence").get<uint64_t>();
    std::vector<uint64_t> path;
    if (p.contains("path") && !p.at("path").is_null())
      path = p.at("path").get<std::vector<uint64_t>>();
    if (!path.empty()) {
      // v130: a moving part of a flexible placement, named as a mate is.
      const Occurrence* F =
          path.size() == 1 ? state.assembly->FindById(path.front()) : nullptr;
      if (path.size() != 1)
        throw std::runtime_error(
            "path: only one level of sub-assembly is flexible, so a path is "
            "one occurrence: [placement]");
      if (F == nullptr)
        throw std::runtime_error(
            "path: no occurrence with id " + std::to_string(path.front()) +
            ". Occurrences: " + OccurrenceCandidates(*state.assembly));
      const AssemblyDocument* sub = ui::FlexibleSub(state, *F);
      if (sub == nullptr)
        throw std::runtime_error(
            "path: '" + F->name +
            "' is not a flexible sub-assembly placement, so its parts explode "
            "as one block with it — set_occurrence_flexible first, or explode "
            "the placement itself");
      if (sub->FindById(occId) == nullptr) {
        std::vector<std::string> known;
        for (const Occurrence& o : sub->GetOccurrences())
          known.push_back(std::to_string(o.id) + " (" + o.name + ")");
        throw std::runtime_error("no occurrence " + std::to_string(occId) +
                                 " inside '" + F->name +
                                 "'. Its occurrences: " + KnownList(known));
      }
    } else if (!state.assembly->FindById(occId)) {
      throw std::runtime_error(
          "no occurrence with id " + std::to_string(occId) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
    }
    const auto& o = p.at("offset");
    const glm::dvec3 offset(o.at(0).get<double>(), o.at(1).get<double>(),
                            o.at(2).get<double>());
    ui::SnapshotAssembly(state);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : v->steps)
      if (s.occurrenceId == occId && s.path == path) {
        step = &s;
        break;
      }
    if (!step) {
      v->steps.push_back({occId, offset, path});
    } else {
      step->offset = offset;
    }
    state.assembly->BumpVersion(); // resyncs when this view is active
    return DocumentIO::ExplodedViewToJson(*v);
  }
  if (m == "auto_explode") {
    // params: {view?} — radial seed from the assembly centroid (snapshots).
    // With no `view` (or view 0), create a fresh exploded view first so the
    // common "just explode this" call doesn't fault on a missing view.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    uint64_t id = p.value("view", uint64_t{0});
    const bool created = id == 0;
    if (created) {
      ui::SnapshotAssembly(state);
      ExplodedView v;
      v.name = "Exploded";
      id = state.assembly->AddExplodedView(std::move(v));
    }
    if (!ui::AutoExplode(state, id)) {
      if (created)
        state.assembly->RemoveExplodedView(id); // don't leak it
      throw std::runtime_error(
          created ? "auto_explode needs at least two placed occurrences"
                  : "no such exploded view (or fewer than two placed "
                    "occurrences) — call add_exploded_view first, or omit "
                    "`view` to auto-create one");
    }
    // A just-created view becomes the active one (the GUI's create button
    // does the same) so a following set_explode {factor} applies it without
    // the silent-nothing trap.
    if (created)
      state.activeExplodedView = id;
    json out = DocumentIO::ExplodedViewToJson(
        *state.assembly->FindExplodedViewById(id));
    out["view"] = id; // the (possibly just-created) view id
    return out;
  }
  if (m == "set_explode") {
    // Display state (transient, never saved): which view is applied and how
    // far. params: {view?, factor?} — view omitted keeps the current view;
    // view 0 explicitly = none.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    if (p.contains("view")) {
      const uint64_t id = p.at("view").get<uint64_t>();
      if (id && !state.assembly->FindExplodedViewById(id))
        throw std::runtime_error("no such exploded view");
      state.activeExplodedView = id;
    }
    if (p.contains("factor")) {
      const double factor = std::clamp(p.at("factor").get<double>(), 0.0, 1.0);
      // A positive factor with no active view used to succeed and visibly do
      // NOTHING. Auto-activate the only view when that's unambiguous;
      // otherwise say what's missing. factor 0 stays a no-op success (the
      // reset idiom).
      if (factor > 0.0 && state.activeExplodedView == 0) {
        const auto& views = state.assembly->GetExplodedViews();
        if (views.size() == 1)
          state.activeExplodedView = views[0].id;
        else
          throw std::runtime_error(
              "no active exploded view — pass view: <id> (see "
              "list_exploded_views, or auto_explode to create one)");
      }
      state.explodeFactor = factor;
    }
    state.syncedDocumentVersion = UINT64_MAX; // display change: force resync
    return json{{"active", state.activeExplodedView},
                {"factor", state.explodeFactor}};
  }
  if (m == "check_fasteners") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — a fastener is placed in "
          "one, so switch tabs with set_active_part (see list_parts)");
    const ui::FastenerReport rep = ui::CheckFasteners(state);
    json findings = json::array();
    for (const ui::FastenerCheck& c : rep.findings)
      findings.push_back(
          json{{"occurrence", c.occurrence}, {"part", c.part}, {"why", c.why}});
    json out{{"ok", findings.empty()},
             {"examined", rep.examined},
             {"skipped", rep.skipped},
             {"findings", std::move(findings)}};
    {
      // Empty findings over zero screws and empty findings over forty-two are
      // the same `ok: true`. The receipt tells them apart: a fastener check
      // that examined nothing has not passed, it has not run.
      receipt::Receipt receipt;
      const receipt::Oracle oracle{"designer/fastener-stack", "1"};
      const std::string claim = "every fastener reaches the thread it is "
                                "supposed to reach";
      if (rep.examined == 0) {
        receipt.Unverifiable(claim, oracle,
                             rep.skipped > 0
                                 ? std::to_string(rep.skipped) +
                                       " screw(s) could not be sized, and no "
                                       "other fastener was examined"
                                 : "there are no fasteners in this assembly — "
                                   "`ok` is vacuously true here");
      } else {
        receipt::Claim c(claim, oracle,
                         rep.findings.empty() ? receipt::Verdict::Pass
                                              : receipt::Verdict::Fail);
        c.basis = receipt::Basis::Verified;
        c.value = static_cast<double>(rep.examined);
        c.unit = "fasteners";
        c.note = "each re-derived against the stack it is in" +
                 (rep.skipped > 0
                      ? "; " + std::to_string(rep.skipped) +
                            " more could not be sized and were NOT judged"
                      : std::string{});
        receipt.Add(std::move(c));
        // A skipped screw is not covered by the pass above, so the receipt
        // must not roll up verified while any remain unjudged.
        if (rep.skipped > 0)
          receipt.Unverifiable(
              "every fastener was judged", oracle,
              std::to_string(rep.skipped) +
                  " screw(s) have a standard or size this build cannot "
                  "resolve; nothing is known to be wrong with them, and "
                  "nothing is known to be right");
      }
      AttachReceipt(out, receipt);
    }
    return out;
  }
  if (m == "bom") {
    // Bill of materials (A4): derived, one row per (part, configuration,
    // material) placed.
    // Unknown masses (a body without a density) are null and taint the total.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const ui::BomTable bom = ui::ComputeBom(state);
    json rows = json::array();
    for (const ui::BomRow& r : bom.rows) {
      json row = {{"part_id", r.partId},
                  {"part", r.partName},
                  {"configuration", r.configuration},
                  {"configuration_id", r.configurationId},
                  {"quantity", r.quantity},
                  {"material", r.material},
                  {"standard", r.standard},
                  {"designation", r.designation},
                  {"unit_volume", r.unitVolume}};
      row["unit_mass_g"] = r.massKnown ? json(r.unitMassG) : json(nullptr);
      row["total_mass_g"] =
          r.massKnown ? json(r.unitMassG * r.quantity) : json(nullptr);
      rows.push_back(row);
    }
    json out = {
        {"rows", rows},
        {"total",
         {{"mass_g", bom.massKnown ? json(bom.totalMassG) : json(nullptr)}}}};
    if (!bom.warnings.empty())
      out["warnings"] = bom.warnings; // V2: WHY a mass went null
    return out;
  }
  if (m == "export_bom") {
    // params: {path} — writes the BOM as CSV. Same rows as `bom`.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const std::string path = p.at("path").get<std::string>();
    const ui::BomTable bom = ui::ComputeBom(state);
    std::ofstream out(path);
    if (!out)
      throw std::runtime_error("cannot write " + path +
                               " — does the parent directory exist?");
    out << ui::BomToCsv(bom);
    return json{{"path", path}, {"rows", bom.rows.size()}};
  }
  if (m == "set_active_part") {
    // {part: name|id|index} through the ONE resolver rename_part uses —
    // three sibling RPCs once took three different keys, and the
    // missing-param error named only 'index', hiding that names worked.
    // Legacy {name}/{index} stay accepted -- one of them: a second used to go
    // unread and be refused as unknown AFTER the tab had switched.
    if (int(p.contains("part")) + int(p.contains("name")) +
            int(p.contains("index")) >
        1)
      throw std::runtime_error(
          "name the tab once: {part} (or legacy {name} or {index}), not "
          "several");
    size_t slot;
    if (p.contains("part")) {
      slot = remote::ResolveSlotIndex(state, p.at("part"));
    } else if (p.contains("name")) {
      slot = remote::ResolveSlotIndex(state, p.at("name"));
    } else if (p.contains("index")) {
      slot = p.at("index").get<size_t>();
    } else {
      throw std::runtime_error(
          "pass the tab as {part: name|id|index} (legacy {name} or {index} "
          "also work); list_parts shows every tab");
    }
    ui::SetActivePart(state, slot);
    return json{{"active", state.activePart}};
  }
  if (m == "edit_in_context") {
    // In-context editing (A4): activates the occurrence's part tab with the
    // assembly ghosted around it. Exit with set_active_part. params:
    // {occurrence}
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    if (!ui::EnterInContextEdit(state, p.at("occurrence").get<uint64_t>()))
      throw std::runtime_error("no such occurrence (or its part is missing)");
    return json{{"active", state.activePart},
                {"assembly_slot", state.inContext.assemblySlotId},
                {"occurrence", state.inContext.occurrenceId}};
  }
  if (m == "delete_part") {
    // One way of naming the tab: a second went unread and was refused as
    // unknown only once the tab had been deleted.
    if (p.contains("part") && p.contains("index"))
      throw std::runtime_error(
          "name the tab once: {part} (or legacy {index}), not both");
    size_t slot;
    if (p.contains("part")) {
      slot = remote::ResolveSlotIndex(state, p.at("part"));
    } else if (p.contains("index")) {
      slot = p.at("index").get<size_t>();
    } else {
      throw std::runtime_error(
          "pass the tab as {part: name|id|index} (legacy {index} also "
          "works); list_parts shows every tab");
    }
    std::string why;
    if (!ui::DeletePart(state, slot, &why))
      throw std::runtime_error(why.empty() ? "cannot delete that tab" : why);
    return json{{"active", state.activePart}};
  }
  return std::nullopt;
}

} // namespace remote
