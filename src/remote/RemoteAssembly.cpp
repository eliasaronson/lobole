#include "remote/RemoteAssembly.h"

#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "remote/RemoteProject.h"
#include "ui/AppActions.h"

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
     "index, name, part"},
    {"delete_part",
     "Delete a part/assembly/drawing tab {part: name|id|index} (legacy: "
     "index); refused while any assembly places it.",
     "index, part"},
    {"add_occurrence",
     "Place a part OR another assembly in the active assembly {part: name of "
     "either, transform?}. Returns {id, part_id, ref_kind}; a reference that "
     "would close a cycle is refused.",
     "fixed, name, part, transform, visible"},
    {"remove_occurrence", "Remove an occurrence by id.", "id"},
    {"list_occurrences",
     "List occurrences with transforms, state and ref_kind (part|assembly); a "
     "sub-assembly also reports leaf_count and the leaves' paths.",
     ""},
    {"set_occurrence_transform",
     "Set an occurrence's world transform {id, transform: {position, "
     "orientation [x,y,z,w] OR rotate {axis, angle_deg}}}.",
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
     "connectors...}.",
     "a, b, ball_rotation, dx, dy, flipped, name, offset, rotation, type"},
    {"set_mate", "Edit a mate's parameters.",
     "ball_rotation, dx, dy, flipped, id, name, offset, rotation"},
    {"remove_mate", "Remove a mate by id.", "id"},
    {"list_mates", "List mates with status/errors.", ""},
    {"add_relation",
     "Couple two mates' degrees of freedom {type: gear|rack|screw|linear, a: "
     "mate id, b: mate id}: dof_b = ratio * dof_a + phase. The ratio is "
     "DERIVED from the kind's own numbers when given — gear: {teeth_a, "
     "teeth_b} (ratio = -teeth_a/teeth_b, so meshing gears counter-rotate); "
     "rack: {pitch_radius} mm (a's rotation drives b's slide); screw: "
     "{pitch} mm per turn on ONE cylindrical mate (pass the same id as a and "
     "b); linear: a bare {ratio}. `ratio` may be given directly for any kind; "
     "refused when neither is. {phase?} in b's unit (rad or mm) is what makes "
     "teeth mesh. Dragging or set_mate on EITHER mate turns the other; inside "
     "a mate loop the relation is a solver row and lowers the DOF count. "
     "Refused for a mate that lacks the degree of freedom (a fastened mate "
     "has no rotation) (docs/assemblies/relations-plan.md).",
     "a, b, name, phase, pitch, pitch_radius, ratio, teeth_a, teeth_b, type"},
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
     "axis passes through, snapped to a length that size is made in. Returns "
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
     "drive, dry_run, far_end, nut, rim, standard, washer, washer_under_head, "
     "washer_under_nut"},
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
    const std::string refName = p.at("part").get<std::string>();
    uint64_t refId = ui::PartIdByName(state, refName);
    OccurrenceRef refKind = OccurrenceRef::Part;
    if (refId == 0) {
      for (const ui::PartSlot& slot : state.parts) {
        if (slot.kind == ui::DocKind::Assembly && slot.name == refName) {
          refId = slot.id;
          refKind = OccurrenceRef::Assembly;
          break;
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
    for (const assembly::FlatInstance& inst : ui::FlattenActiveAssembly(state))
      if (inst.visible)
        leavesByRoot[inst.rootOccurrenceId].push_back(inst.path);
    json arr = json::array();
    for (const Occurrence& o : state.assembly->GetOccurrences()) {
      json oj = DocumentIO::OccurrenceToJson(o);
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
      const json t = NormalizeTransform(p.at("transform"));
      const json& pos = t.at("position");
      occ->position = glm::vec3(pos.at(0).get<float>(), pos.at(1).get<float>(),
                                pos.at(2).get<float>());
      const json& q = t.at("orientation"); // [x,y,z,w]
      occ->orientation = glm::quat(q.at(3).get<float>(), q.at(0).get<float>(),
                                   q.at(1).get<float>(), q.at(2).get<float>());
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
      // A5: a face anchor names a face of a PART. A placed sub-assembly has
      // none of its own, so refuse it HERE rather than storing a mate that
      // can only ever fail to solve (which is what it used to do, reporting
      // "the part document is not loaded" — a missing-file message for a
      // file that is not missing).
      if (!c.value("origin", false) && c.contains("face") &&
          state.assembly != nullptr && c.contains("occurrence")) {
        const Occurrence* o = nullptr;
        if (c.at("occurrence").is_number())
          o = state.assembly->FindById(c.at("occurrence").get<uint64_t>());
        else
          for (const Occurrence& cand : state.assembly->GetOccurrences())
            if (cand.name == c.at("occurrence").get<std::string>())
              o = &cand;
        // v57: a face anchor on a sub-assembly is fine WITH a path naming the
        // part inside it whose face is meant — "bolt this bracket to the
        // flange of the pump inside the pump unit". Without one there is
        // still nothing to anchor to: a placed assembly has no faces.
        if (o != nullptr && o->refKind == OccurrenceRef::Assembly &&
            !c.contains("inner_path"))
          throw std::runtime_error(
              "'" + o->name +
              "' is a sub-assembly, which has no faces of its own: add "
              "inner_path (the occurrence ids inside it, down to the part "
              "whose face you mean — list_occurrences on that tab), mate it "
              "by origin, or open that assembly and mate the part there");
      }
      if (c.value("origin", false))
        anchor = {{"origin", true}};
      else if (c.contains("face")) {
        anchor = {{"face",
                   {{"feature_id", c.at("face").at("feature_id")},
                    {"element", c.at("face").at("element")}}}};
        if (c.contains("snap_edge")) // inference snap (hole center) — v26
          anchor["snap_edge"] = {
              {"feature_id", c.at("snap_edge").at("feature_id")},
              {"element", c.at("snap_edge").at("element")}};
      } else
        throw std::runtime_error(
            "connector needs 'origin:true' or 'face:{feature_id,element}'");
      json out{{"occurrence", c.at("occurrence")}, {"anchor", anchor}};
      if (c.contains("inner_path"))
        out["inner_path"] = c.at("inner_path");
      return out;
    };
    json mateJson = {{"id", 0},
                     {"name", p.value("name", std::string{"Mate"})},
                     {"mate_type", p.value("type", std::string{"fastened"})},
                     {"a", connToJson(p.at("a"))},
                     {"b", connToJson(p.at("b"))},
                     {"flipped", p.value("flipped", true)},
                     {"rotation", p.value("rotation", 0.0f)},
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
    if (p.contains("rotation"))
      mate->rotation = p.at("rotation").get<float>();
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
    ui::SolveActiveAssembly(state);
    json out = DocumentIO::MateToJson(*mate);
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
      json mj = DocumentIO::MateToJson(mate);
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
    }
    if (p.contains("ratio"))
      r.ratio = p.at("ratio").get<double>();
    else if (!(p.contains("teeth_a") || p.contains("pitch_radius") ||
               p.contains("pitch")))
      throw std::runtime_error(
          "give the ratio, or what it is derived from: teeth_a + teeth_b for "
          "a gear, pitch_radius for a rack, pitch for a screw");
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
    ui::SnapshotAssembly(state);
    const uint64_t id = state.assembly->AddRelation(std::move(r));
    ui::SolveActiveAssembly(state);
    json out =
        DocumentIO::RelationToJson(*state.assembly->FindRelationById(id));
    if (const Relation* made = state.assembly->FindRelationById(id);
        made != nullptr && !made->error.empty())
      out["error"] = made->error;
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
