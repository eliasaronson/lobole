#include "remote/RemoteAssembly.h"

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
    {"select_mate", "Select a mate in the UI.", "id"},
    {"solve_assembly", "Run the mate solver; returns satisfied/errored mates.",
     ""},
    {"bom", "Bill of materials (aggregated part counts, masses).", ""},
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
    const int errored = ui::SolveActiveAssembly(state);
    json arr = json::array();
    for (const Mate& mate : state.assembly->GetMates()) {
      json mj = {{"id", mate.id}, {"name", mate.name}};
      if (!mate.error.empty())
        mj["error"] = mate.error;
      arr.push_back(mj);
    }
    return json{{"errored_mates", errored}, {"mates", arr}};
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
      json row = {{"part_id", r.partId},
                  {"part", r.partName},
                  {"quantity", r.quantity},
                  {"material", r.material},
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
