#include "remote/RemoteAssembly.h"

#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/features/FrameFeature.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteProject.h"
#include "ui/DocumentSlots.h"
#include "ui/SelectionPipeline.h"

#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include <map>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
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
     "index, name, part", remote::NotInBatch},
    {"delete_part",
     "Delete a part/assembly/drawing tab {part: name|id|index} (legacy: "
     "index); refused while any assembly places it.",
     "index, part"},
    {"add_occurrence",
     "Place a part OR another assembly in the active assembly {part: name or "
     "slot id of either, transform?}. Returns {id, part_id, ref_kind}; a "
     "reference that would close a cycle is refused.",
     "fixed, name, part, transform, visible"},
    {"remove_occurrence", "Remove an occurrence by id.", "id"},
    {"list_occurrences",
     "List occurrences with transforms, state and ref_kind (part|assembly); a "
     "sub-assembly also reports leaf_count and the leaves' paths. "
     "{bounds: true} adds each one's WORLD bbox_min/bbox_max — the extent "
     "where it actually sits, which get_state cannot give you because its "
     "boxes are in the part's own space. For a single named point or axis "
     "use `frame` with an occurrence instead.",
     "bounds"},
    {"set_occurrence_transform",
     "Set an occurrence's world transform {id, transform: {position, "
     "orientation [x,y,z,w] OR rotate {axis, angle_deg}}}. Either half may "
     "be omitted and keeps its current value, so sliding a part along one "
     "axis needs no quaternion.",
     "id, transform"},
    {"set_occurrence_visible", "Show/hide an occurrence.", "id, visible"},
    {"set_occurrence_fixed", "Fix/free an occurrence for the solver.",
     "fixed, id"},
    {"rename_occurrence", "Rename an occurrence.", "id, name"},
    {"set_occurrence_configuration",
     "Choose which configuration of a placed SUB-ASSEMBLY this occurrence "
     "uses {id, configuration: name|id|null}; null = that document's active "
     "one. Two placements of one assembly can differ.",
     "configuration, id"},
    {"select_occurrence", "Select an occurrence in the UI.", "id"},
    {"add_mate",
     "Add a mate {type: fastened|revolute|slider|cylindrical|planar|ball, "
     "a, b: connectors}. A connector is {occurrence: id|name, frame: <Frame "
     "id|name of that part>} (v86 — the mate as DESIGNED: a bolt-circle "
     "centre, a shaft axis), {occurrence, face: {feature_id, element}, "
     "snap_edge?} (a face's centroid + normal, or a snapped hole centre), or "
     "{occurrence, origin: true}; inner_path? reaches into a sub-assembly. "
     "The mate's starting position along whatever it leaves free is "
     "`rotation` (RADIANS) or `rotation_deg`, and `offset` in mm.",
     "a, b, ball_rotation, dx, dy, flipped, name, offset, rotation, "
     "rotation_deg, type"},
    {"set_mate",
     "Edit a mate's parameters, and the way to POSE a mechanism: turn a "
     "revolute or cylindrical mate with `rotation` (RADIANS) or "
     "`rotation_deg` — not both — slide it with `offset` (mm), and the "
     "solver holds what you set instead of treating it as one more unknown. "
     "To sweep a range rather than jump to one pose use a motion study: it "
     "walks its samples in order, so each solve starts from the last and a "
     "closed loop stays on the branch it is already on.",
     "ball_rotation, dx, dy, flipped, id, name, offset, rotation, "
     "rotation_deg"},
    {"remove_mate", "Remove a mate by id.", "id"},
    {"list_mates",
     "List mates with status/errors. Each carries `rotation` in radians and "
     "`rotation_deg` beside it.",
     ""},
    {"add_relation",
     "Couple two mates' degrees of freedom {type: "
     "gear|chain|rack|screw|linear|cam, a: mate id, b: mate id}: "
     "dof_b = ratio * dof_a + phase. The ratio is DERIVED from the kind's "
     "own numbers when given — gear: {teeth_a, teeth_b} (ratio = "
     "-teeth_a/teeth_b, so meshing gears counter-rotate); CHAIN: the same "
     "but the ratio KEEPS its sign, because a chain or belt turns both "
     "sprockets the same way; rack: {pitch_radius} mm (a's rotation drives "
     "b's slide); screw: {pitch} mm per turn on ONE cylindrical mate (pass "
     "the same id as a and b); linear: a bare {ratio}. CAM is the exception "
     "— a's rotation drives b's offset through a LIFT TABLE, not a ratio: "
     "{lift: [[angle_deg, lift_mm], ...]} spanning one turn, or {rise, "
     "duration_deg?, start_deg?} for a cosine lobe built for you. It is the "
     "one relation that is not invertible (many angles give one lift), so it "
     "drives forward only. `ratio` may be given directly for any other kind; "
     "refused when neither is. {phase?} in b's unit (rad or mm) is what makes "
     "teeth mesh. Dragging or set_mate on EITHER mate turns the other; inside "
     "a mate loop the relation is a solver row and lowers the DOF count. "
     "Refused for a mate that lacks the degree of freedom (a fastened mate "
     "has no rotation), and for a rack whose slider runs ALONG the pinion's "
     "axis instead of across it; `warnings` names gear axles more than 5 "
     "degrees off parallel (docs/assemblies/relations-plan.md).",
     "a, b, duration_deg, lift, name, phase, pitch, pitch_radius, ratio, "
     "rise, start_deg, teeth_a, teeth_b, type"},
    {"set_relation", "Edit a relation's {ratio?, phase?, name?} by {id}.",
     "id, name, phase, ratio"},
    {"remove_relation", "Remove a relation by id.", "id"},
    {"list_relations", "List relations with status/errors.", ""},
    {"select_mate", "Select a mate in the UI.", "id"},
    {"solve_assembly",
     "Run the mate solver. Returns the per-mate status plus, for an assembly "
     "with mate loops, how many degrees of freedom are left, whether every "
     "loop closed and by how much it missed, and which mates were redundant "
     "but consistent (three bolts through a flange: satisfied, saying nothing "
     "new — not an error).",
     ""},
    {"add_fastener",
     "Place a standard fastener in the active assembly (docs/parts/plan.md). "
     "Every dimension but the length is READ from the vendored table for that "
     "standard, so an unstocked size is refused naming the nearest that are. "
     "One generated part per distinct spec, named as its BOM designation "
     "(\"ISO 4762 M6x20\") and reused by every occurrence of it, so `bom` "
     "aggregates without knowing about fasteners. Refused on a part tab: a "
     "screw body there would be machined by CAM and counted as the part's own "
     "mass. Optional {position, orientation} place it; `fit_fastener` derives "
     "all of that from a picked hole and is what you usually want. Standards: "
     "iso4762, iso14579, iso4017, iso7380 (screws — `length` required), "
     "iso4032 (nut), iso7089 (washer — no `length`).",
     "drive, length, orientation, position, size, standard"},
    {"fit_fastener",
     "Derive a fastener from a picked hole RIM and place it "
     "(docs/parts/plan.md). `rim` is a circular EDGE — the one the head sits "
     "against; a cylindrical face has an axis but no direction and cannot say "
     "which end it goes in from. `rim` is {feature_id, element, occurrence?} "
     "— the occurrence is needed only when the part is placed more than once, "
     "and leaving it out THERE is refused rather than resolved to the first. "
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
     "the washers and the nut are still found. "
     "Placing also MATES the parts the bolt clamps (returned as `mates`), so "
     "dragging one carries the rest instead of tearing the joint in half. One "
     "per INTERFACE it spans — a bolt through three parts makes two, chained, "
     "so the one in the middle is held too — and once per interface, since a "
     "second bolt across one already joined would close a loop the solver "
     "reports as over-constrained. `mates` is empty when there was nothing "
     "new to join.",
     "drive, dry_run, far_end, nut, rim, size, standard, washer, "
     "washer_under_head, washer_under_nut"},
    {"bom",
     "Bill of materials (aggregated part counts, masses). A row generated "
     "from a standard carries {standard, designation} — the two fields a "
     "purchase order needs — and a modelled part leaves them empty, so a "
     "filter on either splits bought from made.",
     ""},
    {"check_fasteners",
     "Every placed fastener re-derived against the stack it is IN, and what "
     "is wrong with it (docs/parts/plan.md). A length derived from other "
     "documents' geometry cannot be a live binding (A2), so a stack that "
     "changed under a screw has to be REPORTED — this is that report: too "
     "short, bottoming out in a blind hole, or spanning nothing at all. "
     "Nothing is stored to make it work: a placed fastener's position and "
     "orientation are its rim and its axis, so it also checks one nobody "
     "derived. Empty findings mean every fastener still fits.",
     ""},
    {"export_bom", "Export the BOM as CSV {path}.", "path"},
    {"edit_in_context",
     "Enter in-context editing of a part within the assembly.", "occurrence"},
    {"add_exploded_view", "Add an exploded view.", "name"},
    {"remove_exploded_view", "Remove an exploded view.", "id"},
    {"list_exploded_views", "List exploded views.", ""},
    {"set_explode_step", "Add/edit an explode step (occurrence + offset).",
     "occurrence, offset, view"},
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
  const glm::vec3 axis(ax.at(0).get<float>(), ax.at(1).get<float>(),
                       ax.at(2).get<float>());
  if (glm::length(axis) < 1e-9f)
    throw std::runtime_error("rotate.axis must be non-zero");
  const float rad = glm::radians(r.at("angle_deg").get<float>());
  const glm::quat q = glm::angleAxis(rad, glm::normalize(axis));
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
constexpr double kDegPerRad = 57.29577951308232;

// Radians from whichever the caller used, refusing both at once — a mate
// pinned to two different angles has no right answer. Takes the values
// rather than the params so both key names stay visible at the call site:
// `rpc_params_gate` reads the handler for the keys it touches, and a helper
// that reached into `p` itself made add_mate look as though it had quietly
// stopped accepting either.
float MateRotationRadians(bool hasRad, double rad, bool hasDeg, double deg,
                          float current) {
  if (hasRad && hasDeg)
    throw std::runtime_error(
        "pass rotation (radians) or rotation_deg, not both");
  if (hasDeg)
    return static_cast<float>(deg / kDegPerRad);
  if (hasRad)
    return static_cast<float>(rad);
  return current;
}

json MateJson(const Mate& m) {
  json j = DocumentIO::MateToJson(m);
  j["rotation_deg"] = m.rotation * kDegPerRad;
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
    // first (docs/parts/gui-plan.md).
    rim.occurrence = r.value("occurrence", uint64_t{0});
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
                {"mates", fit.mates}};
  }
  if (m == "add_fastener") {
    glm::vec3 position(0.0f);
    if (p.contains("position")) {
      const auto& v = p.at("position");
      position = glm::vec3(v.at(0).get<float>(), v.at(1).get<float>(),
                           v.at(2).get<float>());
    }
    glm::quat orientation(1.0f, 0.0f, 0.0f, 0.0f);
    if (p.contains("orientation")) {
      const auto q = p.at("orientation");
      orientation = glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                              q.at(1).get<float>(), q.at(2).get<float>());
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
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddOccurrence(occ);
    return json{
        {"id", id},
        {"part_id", refId},
        {"ref_kind", refKind == OccurrenceRef::Assembly ? "assembly" : "part"}};
  }
  if (m == "remove_occurrence") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    ui::SnapshotAssembly(state);
    if (!state.assembly->RemoveOccurrence(p.at("id").get<uint64_t>())) {
      state.assemblyHistory.undo
          .pop_back(); // nothing changed; drop the snapshot
      throw std::runtime_error(
          "no occurrence with id " +
          std::to_string(p.at("id").get<uint64_t>()) +
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
    std::map<uint64_t, std::pair<glm::vec3, glm::vec3>> boundsByRoot;
    for (const assembly::FlatInstance& inst : ui::FlattenActiveAssembly(state))
      if (inst.visible) {
        leavesByRoot[inst.rootOccurrenceId].push_back(inst.path);
        glm::vec3 lmn(0.0f), lmx(0.0f);
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
      if (wantBounds) {
        glm::vec3 bmn(0.0f), bmx(0.0f);
        bool have = false;
        if (o.refKind == OccurrenceRef::Assembly) {
          const auto it = boundsByRoot.find(o.id);
          if (it != boundsByRoot.end()) {
            bmn = it->second.first;
            bmx = it->second.second;
            have = true;
          }
        } else if (Document* pd = ui::PartDocById(state, o.partId)) {
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
      m == "set_occurrence_configuration") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    Occurrence* occ = state.assembly->FindById(p.at("id").get<uint64_t>());
    if (!occ)
      throw std::runtime_error(
          "no occurrence with id " +
          std::to_string(p.at("id").get<uint64_t>()) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
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
            glm::vec3(pos.at(0).get<float>(), pos.at(1).get<float>(),
                      pos.at(2).get<float>());
      }
      if (t.contains("orientation")) {
        const json& q = t.at("orientation"); // [x,y,z,w]
        occ->orientation =
            glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                      q.at(1).get<float>(), q.at(2).get<float>());
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
    } else if (m == "set_occurrence_configuration") {
      // A5/v56: which configuration of the REFERENCED document this
      // placement uses. Named or by id; 0 / "" goes back to that document's
      // active one.
      uint64_t cfgId = 0;
      if (p.contains("configuration") && !p.at("configuration").is_null()) {
        const json& c = p.at("configuration");
        if (c.is_number()) {
          cfgId = c.get<uint64_t>();
        } else {
          const std::string want = c.get<std::string>();
          const AssemblyDocument* sub =
              occ->refKind == OccurrenceRef::Assembly
                  ? ui::AssemblyDocById(state, occ->partId)
                  : nullptr;
          if (sub == nullptr)
            throw std::runtime_error(
                "'" + occ->name +
                "' places a part, and per-occurrence PART configurations are "
                "not supported (a part configuration changes geometry, which "
                "would mean regenerating it per configuration) — configure "
                "sub-assemblies, or set the part document's own active "
                "configuration");
          std::string known;
          for (const Configuration& cfg : sub->GetConfigurations()) {
            known += (known.empty() ? "" : ", ") + cfg.name;
            if (cfg.name == want)
              cfgId = cfg.id;
          }
          if (cfgId == 0)
            throw std::runtime_error(
                "no configuration '" + want + "' in '" + occ->name +
                "'. Known: " + (known.empty() ? "(none)" : known));
        }
      }
      occ->configurationId = cfgId;
    } else if (m == "rename_occurrence") {
      occ->name = p.at("name").get<std::string>();
    }
    state.assembly->BumpVersion();
    // Moving/grounding an occurrence can shift what the mates solve to (a fixed
    // anchor moved, or a mated occurrence was nudged and must be re-placed).
    if (m == "set_occurrence_transform" || m == "set_occurrence_fixed")
      ui::SolveActiveAssembly(state);
    return DocumentIO::OccurrenceToJson(*occ);
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
      json out{{"occurrence", c.at("occurrence")}, {"anchor", anchor}};
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
                             p.value("rotation_deg", 0.0), 0.0f)},
        {"offset", p.value("offset", 0.0f)}};
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
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddMate(mate);
    const int errored = ui::SolveActiveAssembly(state);
    json out = DocumentIO::MateToJson(*state.assembly->FindMateById(id));
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
  if (m == "set_mate") {
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    Mate* mate = state.assembly->FindMateById(p.at("id").get<uint64_t>());
    if (!mate)
      throw std::runtime_error("no such mate");
    ui::SnapshotAssembly(state);
    if (p.contains("rotation") || p.contains("rotation_deg"))
      mate->rotation =
          MateRotationRadians(p.contains("rotation"), p.value("rotation", 0.0),
                              p.contains("rotation_deg"),
                              p.value("rotation_deg", 0.0), mate->rotation);
    if (p.contains("offset"))
      mate->offset = p.at("offset").get<float>();
    if (p.contains("flipped"))
      mate->flipped = p.at("flipped").get<bool>();
    if (p.contains("name"))
      mate->name = p.at("name").get<std::string>();
    if (p.contains("dx"))
      mate->dx = p.at("dx").get<float>(); // planar
    if (p.contains("dy"))
      mate->dy = p.at("dy").get<float>(); // planar
    if (p.contains("ball_rotation")) {    // ball [x,y,z,w]
      const json& q = p.at("ball_rotation");
      mate->ballRotation =
          glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                    q.at(1).get<float>(), q.at(2).get<float>());
    }
    // The mate just edited drives whatever it is related to: turning the
    // wheel turns the pinion as readily as the other way round.
    (void)assembly::PropagateRelations(*state.assembly, mate->id);
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
      if (!mate.error.empty())
        mj["error"] = mate.error;
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
    r.mateA = p.at("a").get<uint64_t>();
    r.mateB = p.at("b").get<uint64_t>();
    r.name = p.value("name", std::string{});
    r.phase = p.value("phase", 0.0);
    const Mate* a = state.assembly->FindMateById(r.mateA);
    const Mate* b = state.assembly->FindMateById(r.mateB);
    if (a == nullptr || b == nullptr)
      throw std::runtime_error(
          "no mate with id " +
          std::to_string(a == nullptr ? r.mateA : r.mateB) +
          "; list_mates shows the ones that exist");
    // The kind fixes which DOF of each mate is coupled, and how the ratio is
    // derived from the kind's own numbers.
    switch (r.kind) {
    case RelationKind::Gear:
      r.dofA = r.dofB = MateDof::Rotation;
      if (p.contains("teeth_a") || p.contains("teeth_b")) {
        const double za = p.value("teeth_a", 0.0), zb = p.value("teeth_b", 0.0);
        if (za <= 0.0 || zb <= 0.0)
          throw std::runtime_error(
              "a gear ratio needs both teeth_a and teeth_b (positive)");
        r.ratio = -za / zb;
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
      if (r.mateA != r.mateB)
        throw std::runtime_error(
            "a screw relation is one CYLINDRICAL mate's rotation driving its "
            "own offset: pass the same mate id as a and b");
      if (p.contains("pitch")) {
        const double pitch = p.at("pitch").get<double>();
        if (pitch <= 0.0)
          throw std::runtime_error("pitch must be positive (mm per turn)");
        r.ratio = pitch / (2.0 * M_PI);
      }
      break;
    case RelationKind::Linear:
      r.dofA = r.dofB = MateDof::Offset;
      break;
    case RelationKind::Chain:
      // A gear whose ratio KEEPS its sign: a chain or a belt turns both
      // sprockets the same way. Expressing one as a gear with a negated
      // tooth count works and makes the sign a thing to remember instead of
      // a thing to say, which is how a reversed conveyor gets modelled.
      r.dofA = r.dofB = MateDof::Rotation;
      if (p.contains("teeth_a") || p.contains("teeth_b")) {
        const double za = p.value("teeth_a", 0.0), zb = p.value("teeth_b", 0.0);
        if (za <= 0.0 || zb <= 0.0)
          throw std::runtime_error(
              "a chain ratio needs both teeth_a and teeth_b (positive)");
        r.ratio = za / zb;
      }
      break;
    case RelationKind::Cam: {
      // A lift table, not a ratio: the follower's offset is a function of
      // the cam's angle. Either the table itself, or the three numbers a
      // cosine lobe is described by.
      r.dofA = MateDof::Rotation;
      r.dofB = MateDof::Offset;
      if (p.contains("lift")) {
        for (const json& pt : p.at("lift")) {
          if (!pt.is_array() || pt.size() != 2)
            throw std::runtime_error(
                "lift is [[angle_deg, lift_mm], ...] — a point is a pair");
          r.lift.emplace_back(pt.at(0).get<double>(), pt.at(1).get<double>());
        }
      } else if (p.contains("rise") || p.contains("duration_deg")) {
        const double rise = p.value("rise", 0.0);
        const double duration = p.value("duration_deg", 180.0);
        const double start = p.value("start_deg", 0.0);
        if (rise <= 0.0)
          throw std::runtime_error("a cosine lobe needs rise > 0 (mm)");
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
            "a cam needs its profile: lift [[angle_deg, lift_mm], ...], or "
            "rise + duration_deg (+ start_deg) for a cosine lobe");
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
    }
    if (p.contains("ratio") && r.kind != RelationKind::Cam)
      r.ratio = p.at("ratio").get<double>();
    else if (r.kind != RelationKind::Cam &&
             !(p.contains("teeth_a") || p.contains("pitch_radius") ||
               p.contains("pitch")))
      throw std::runtime_error(
          "give the ratio, or what it is derived from: teeth_a + teeth_b for "
          "a gear or a chain, pitch_radius for a rack, pitch for a screw, a "
          "lift table for a cam");
    if (r.ratio == 0.0)
      throw std::runtime_error("a relation's ratio cannot be zero");
    const auto lacks = [](const Mate& mm, MateDof dof) {
      return !assembly::MateHasDof(mm.type, dof);
    };
    if (lacks(*a, r.dofA) || lacks(*b, r.dofB)) {
      const Mate& bad = lacks(*a, r.dofA) ? *a : *b;
      const MateDof dof = lacks(*a, r.dofA) ? r.dofA : r.dofB;
      throw std::runtime_error(
          "mate " + std::to_string(bad.id) + " (" +
          DocumentIO::MateToJson(bad).at("mate_type").get<std::string>() +
          ") has no " + DocumentIO::MateDofToString(dof) + " to couple — a " +
          DocumentIO::RelationKindToString(r.kind) + " needs " +
          (r.dofA == MateDof::Rotation ? "revolute" : "slider") +
          " or cylindrical mates");
    }
    // The geometry a kind implies, checked against the mates' own frames —
    // a mate's DOF axis is its A connector's Z. A rack that slides ALONG the
    // pinion's axis is never a rack (the first demo did exactly that, and
    // read as "the pinion moves up"), so it is refused; gears on skew axes
    // are unusual but real (bevel, crossed helical), so those get a warning.
    json warnings = json::array();
    {
      const auto fa = ui::ConnectorWorldFrame(state, a->a);
      const auto fb = ui::ConnectorWorldFrame(state, b->a);
      if (fa && fb) {
        const glm::vec3 za = glm::normalize(glm::vec3((*fa)[2]));
        const glm::vec3 zb = glm::normalize(glm::vec3((*fb)[2]));
        const float along = std::abs(glm::dot(za, zb)); // 1 parallel, 0 across
        if (r.kind == RelationKind::Rack && along > 0.7071f)
          throw std::runtime_error(
              "mate " + std::to_string(b->id) + " slides along mate " +
              std::to_string(a->id) +
              "'s axis; a rack slides ACROSS the pinion's axis. Mate the "
              "rack by faces whose normal is the direction it travels "
              "(a slider slides along its connector frames' Z), not by "
              "origin frames, which are Z-up");
        if (r.kind == RelationKind::Gear && along < 0.9962f) // > 5 degrees
          warnings.push_back(
              "the two axles are not parallel (" +
              std::to_string(glm::degrees(std::acos(along))) +
              " degrees apart): fine for a bevel or crossed pair, wrong "
              "for spur gears");
      }
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
    ui::SnapshotAssembly(state);
    if (p.contains("ratio")) {
      r->ratio = p.at("ratio").get<double>();
      if (r->ratio == 0.0)
        throw std::runtime_error("a relation's ratio cannot be zero");
    }
    if (p.contains("phase"))
      r->phase = p.at("phase").get<double>();
    if (p.contains("name"))
      r->name = p.at("name").get<std::string>();
    state.assembly->BumpVersion();
    ui::SolveActiveAssembly(state);
    json out = DocumentIO::RelationToJson(*r);
    if (!r->error.empty())
      out["error"] = r->error;
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
    const uint64_t id = p.value("id", uint64_t{0}); // 0 clears the selection
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
    for (const Mate& mate : state.assembly->GetMates()) {
      json mj = {{"id", mate.id}, {"name", mate.name}};
      if (!mate.error.empty())
        mj["error"] = mate.error;
      arr.push_back(mj);
    }
    // A6: report what the loop pass found HERE, in the mutating call's own
    // response, rather than leaving it for a get_state the caller has to know
    // to poll (the regen-honesty rule).
    json out{{"errored_mates", rep.erroredMates},
             {"placed", rep.placed},
             {"mates", arr}};
    if (rep.loopMates > 0) {
      out["loop_mates"] = rep.loopMates;
      out["dof"] = rep.dof;
      out["loops_closed"] = rep.loopsClosed;
      out["residual_mm"] = rep.residualMm;
      out["redundant_mates"] = rep.redundantMates;
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
    if (!state.assembly->FindById(occId))
      throw std::runtime_error(
          "no occurrence with id " + std::to_string(occId) +
          ". Occurrences: " + OccurrenceCandidates(*state.assembly));
    const auto& o = p.at("offset");
    const glm::vec3 offset(o.at(0).get<float>(), o.at(1).get<float>(),
                           o.at(2).get<float>());
    ui::SnapshotAssembly(state);
    ExplodeStep* step = nullptr;
    for (ExplodeStep& s : v->steps)
      if (s.occurrenceId == occId) {
        step = &s;
        break;
      }
    if (!step) {
      v->steps.push_back({occId, offset});
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
      const float factor = std::clamp(p.at("factor").get<float>(), 0.0f, 1.0f);
      // A positive factor with no active view used to succeed and visibly do
      // NOTHING. Auto-activate the only view when that's unambiguous;
      // otherwise say what's missing. factor 0 stays a no-op success (the
      // reset idiom).
      if (factor > 0.0f && state.activeExplodedView == 0) {
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
    json findings = json::array();
    for (const ui::FastenerCheck& c : ui::CheckFasteners(state))
      findings.push_back(
          json{{"occurrence", c.occurrence}, {"part", c.part}, {"why", c.why}});
    return json{{"ok", findings.empty()}, {"findings", findings}};
  }
  if (m == "bom") {
    // Bill of materials (A4): derived, one row per referenced part.
    // Unknown masses (a body without a density) are null and taint the total.
    if (!state.assembly)
      throw std::runtime_error(
          "the active document is not an assembly — switch tabs with "
          "set_active_part (see list_parts)");
    const ui::BomTable bom = ui::ComputeBom(state);
    json rows = json::array();
    for (const ui::BomRow& r : bom.rows) {
      json row = {{"part_id", r.partId},        {"part", r.partName},
                  {"quantity", r.quantity},     {"material", r.material},
                  {"standard", r.standard},     {"designation", r.designation},
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
    // Legacy {name}/{index} stay accepted.
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
