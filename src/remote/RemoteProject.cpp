// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RemoteProject.h"
#include "core/GeneratedComponent.h"
#include "core/Measure.h"
#include "remote/InspectionContracts.h"
#include "ui/AssemblyActions.h"
#include "ui/StepExport.h"
#include "ui/StlExport.h"
#include <set>

#include "cad/CADKernel.h"
#include "cad/MeshExport.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/Units.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/ConflictsPanel.h"
#include "ui/DocumentSlots.h"
#include "ui/HistoryPanel.h"
#include "ui/ImportActions.h"
#include "ui/ProjectActions.h"
#include "ui/VariableActions.h"
#include "ui/ViewActions.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Project IO / checkpoint / history / export-import RPCs (new_project..) carved
// out of RemoteServer::HandleSemantic. `doc` is the active part document (may
// be null on an assembly tab, matching the original un-guarded behavior).
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc ProjectMethods[] = {
    {"new_project",
     "Reset to an empty project (one empty part). DISCARDS whatever is open, "
     "unsaved changes included — a script that resets between phases means "
     "exactly that, so it is not gated, and `ping` is how a caller checks "
     "which instance and which project it is talking to first.",
     "", remote::NotInBatch},
    {"open_project",
     "Open a project {path} (.json file or .dsgn directory). Returns "
     "rebuilt_parts (parts whose saved geometry could not be used, so they "
     "were rebuilt from their features) and rebuild_reason when they share "
     "one; saving re-bakes them, and the next open is a load.",
     "path:string", remote::NotInBatch},
    {"save_project",
     "Save the project to {path, message?, force?}. Refuses when the project "
     "changed on disk since it was opened; {force:true} saves over it.",
     "path:string, message?:string, force?:bool", remote::NotInBatch},
    {"validation_revision",
     "Session revision of all model tabs, plus unresolved occurrence "
     "references. "
     "Use before/after staged validation; any mismatch makes the report stale.",
     ""},
    {"component_status",
     "Read generated component ownership and a guarded revision by stable "
     "part_id, or find it by namespace/key. Modified means human edits "
     "conflict.",
     "part_id, namespace, key"},
    {"claim_component",
     "Explicitly assign generated ownership to an existing part. Requires its "
     "current revision; never steals another component's ownership. Undoable.",
     "part_id, namespace, key, fingerprint, expected_revision"},
    {"update_component",
     "Preview or apply a validated staging part to an owned component. "
     "Preserves part/feature IDs, validates dependent assemblies, refuses "
     "human "
     "edits and stale revisions. apply defaults false. One undoable update.",
     "part_id, source_part_id, fingerprint, expected_revision, "
     "source_revision, apply"},
    {"import_step",
     "Import a STEP, STL or BREP file into the current project (STEP via "
     "XCAF: names, colors, real assemblies). `units` is the length unit an "
     "STL or BREP was drawn in — mm (default), cm, m, in — since neither "
     "format carries one; refused for STEP, which declares its own. "
     "`keep_origin` keeps the file's OWN coordinates; without it the import "
     "is centred on its bounding box, which loses the placement a bought "
     "part was exported at and stacks every one of them on the origin — and "
     "the reply's `recentred_by` [x, y, z] (mm) says by how much.",
     "as_assembly, keep_origin, path, units",
     remote::NeedsPart | remote::NotInBatch},
    {"export_step",
     "Export to STEP {path}: the active part (or {part}, from any tab), or — "
     "on an assembly tab — the assembly at its SOLVED placements. The file "
     "carries product structure: each (part, configuration, material) is ONE "
     "named, coloured product -- \"part (configuration) material\" -- placed "
     "once per occurrence in its configuration, each sub-assembly a nested "
     "assembly, each body of a multi-body part its own named product; "
     "millimetres. {body} "
     "exports one body of a part. Replies {bodies} (part) or {shapes} "
     "(assembly) — placed body shapes — plus {products} and {instances}. "
     "No `deflection`: STEP is exact, so it is refused.",
     "path:string, part?:string|int, body?:int", remote::NotInBatch},
    {"export_stl",
     "Export to STL {path, stl_deflection_mm?, body?}. `stl_deflection_mm` "
     "is the tessellation chord tolerance (smaller = finer, more "
     "triangles); STEP and BREP are exact solids and refuse it. Left out, an "
     "unprinted part meshes at 0.1 mm and a PRINTED part (set_print_process) "
     "at its print profile's stl_deflection_mm, else a tenth of its smallest "
     "gap a side, so the chords cannot close what the print needs open. A "
     "coarser one asked for is exported as asked and the reply's `warning` "
     "says how much of the smallest gap the chords can take. Replies the "
     "chord deviation used and where it came from (asked, profile, gap, "
     "default), and the SOURCE B-Rep's watertightness. The name is "
     "check_print's for the same deviation; `deflection` is refused, naming "
     "it.",
     "body, path, stl_deflection_mm", remote::NeedsPart | remote::NotInBatch,
     "bodies:int, deflection_from:string, deflection_mm:number, "
     "free_edges:int, path:string, shells:int, watertight:bool, "
     "chord_share?:number, smallest_gap_side_mm?:number, warning?:string"},
    {"export_brep",
     "Export to BREP {path}: the exact solid, so no `deflection` (that is "
     "export_stl's tessellation tolerance).",
     "body, path", remote::NeedsPart | remote::NotInBatch},
    {"export_visual",
     "Export a visual-quality mesh (glTF/OBJ by extension) of "
     "the part or whole assembly. glTF is written in METRES and Y-up, as "
     "the format defines; OBJ/PLY stay in millimetres, Z-up. `deflection` "
     "is the tessellation chord tolerance in MILLIMETRES (smaller = finer, "
     "more triangles); STEP and BREP are exact solids and ignore it.",
     "deflection, path", remote::NotInBatch},
    {"checkpoint_create",
     "Name the current state of the whole project a version {name}.", "name",
     remote::NotInBatch},
    {"checkpoint_list", "List named versions.", ""},
    {"checkpoint_restore", "Put the project back to a named version {name}.",
     "name", remote::NotInBatch},
    {"history_log", "List the project's versions (every save).", "limit"},
    {"history_restore", "Put the project back to a version {commit}.", "commit",
     remote::NotInBatch},
    {"branch_list", "List separate lines of work, and which one is current.",
     ""},
    {"branch_create", "Start a separate line of work {name, switch?:true}.",
     "name, switch", remote::NotInBatch},
    {"branch_switch", "Move to a separate line of work {name}.", "name",
     remote::NotInBatch},
    {"branch_merge",
     "Bring {name} into the current line of work. A "
     "'conflicted' status is an outcome, not an error.",
     "name", remote::NotInBatch},
    {"conflict_list",
     "What two merged versions disagree about, in document "
     "terms — including the duplicate ids git merges "
     "cleanly and never reports.",
     ""},
    {"conflict_resolve",
     "Resolve one conflict {path, take: mine|theirs|auto, field?}. 'auto' is "
     "the union merge for an order, a rebuild for derived geometry, and a "
     "renumber for a duplicate id.",
     "field, path, take", remote::NotInBatch},
    {"merge_finish",
     "Verify the merged project LOADS AND REGENERATES, then "
     "keep it. Refuses with {broken} when it does not.",
     "", remote::NotInBatch},
    {"merge_abort", "Throw an in-progress merge away.", "", remote::NotInBatch},
    {"set_variable",
     "Add or redefine a variable {name, expression, comment?, "
     "scope: project|part}; returns value + regen.",
     "name:string, expression:string|number, scope?:'project'|'part', "
     "comment?:string",
     remote::Deferrable},
    {"list_variables", "Project variables and the active part's local ones.",
     "", remote::NoFlags,
     "variables:{comment?:string, expression:string, name:string, "
     "scope:string, value:number, error?:string}[]"},
    {"set_table",
     "Add or replace a PROJECT table {name, columns: [names], rows, "
     "comment?} (docs/variables/README.md#tables), read in any expression "
     "as lookup(\"name\", key, \"column\") - so bearing and screw sizes "
     "live in the model. A row is {key, values: [one per column]} or {key, "
     "<column>: value, ...}; a key is a number or a string, and a numeric "
     "key matches a numeric expression (lookup(\"bearings\", 6002, "
     "\"D\")). Cells are numbers in base units (mm, deg), like a "
     "variable. Names and columns are identifiers. Undoable with the "
     "project variables; every part re-evaluates. A malformed table is "
     "refused saying what is wrong, never stored as a different one.",
     "columns, comment, name, rows", remote::Deferrable,
     "regen:{failed:any[], ok:bool, warnings:any[]}, "
     "table:{columns:string[], name:string, rows:object[], "
     "comment?:string}"},
    {"list_tables",
     "The project's tables {name?}: every one, or the one named (refused "
     "naming the tables there are).",
     "name", remote::NoFlags,
     "tables:{columns:string[], name:string, rows:object[], "
     "comment?:string}[]"},
    {"delete_table",
     "Delete a project table {name}; refused while any expression reads it "
     "(naming them), or when there is no such table (naming the ones "
     "there are).",
     "name", remote::Deferrable},
    {"set_units",
     "The units the ACTIVE document is READ in {length?: mm|cm|m|in, mass?: "
     "g|kg|lb, angle?: deg|rad}. Display only — storage is always mm/g/deg, "
     "so this cannot move a vertex. Omitted keys keep their current value; "
     "an unrecognised one is refused, naming what is accepted.",
     "angle, length, mass"},
    {"get_units", "The active document's display units.", "", remote::NoFlags,
     "angle:string, length:string, mass:string"},
    {"rename_variable",
     "Rename a variable {name, new_name, scope?}, rewriting "
     "every referencing expression.",
     "name, new_name, scope", remote::Deferrable},
    {"delete_variable",
     "Delete a variable {name, scope?}; refused while "
     "referenced.",
     "name, scope", remote::Deferrable},
    {"remote_list",
     "The project's remotes, and how far ahead/behind this "
     "branch is.",
     ""},
    {"remote_add",
     "Give the project a remote {name?:origin, url}. A shared "
     "project stops committing built geometry.",
     "name, url"},
    {"remote_remove", "Forget a remote {name}.", "name"},
    {"fetch",
     "Bring down refs from {remote?:origin} without touching the "
     "working tree.",
     "remote", remote::NotInBatch},
    {"pull",
     "Fetch {remote?:origin} and merge it in. A 'conflicted' status "
     "is an outcome — see conflict_list. It is also the status of a merge "
     "git calls clean that leaves a project which does not load (both sides "
     "minted the same id); `open_error` then carries the loader's message.",
     "remote", remote::NotInBatch},
    {"push",
     "Send this branch to {remote?:origin}. 'rejected' means someone "
     "else pushed first; pull, then push again.",
     "remote", remote::NotInBatch},
    {"clone_project", "Clone {url} into {path} and open it.", "path, url",
     remote::NotInBatch},
    {"autosave_now",
     "Autosave every dirty document immediately, ignoring "
     "the interval; returns how many were written.",
     "", remote::NotInBatch},
    {"autosave_status",
     "Which documents have autosaved work waiting to be "
     "recovered.",
     ""},
    {"project_changed_on_disk",
     "Has anything else written to the project since it was opened or last "
     "saved? (a terminal command, another editor, a second instance)",
     ""},
    {"create_folder",
     "Create a bottom-tab-bar folder {folder}, empty. Display grouping only "
     "— ids, references and storage are unaffected. Idempotent.",
     "folder"},
    {"move_to_folder",
     "Put tabs in a folder: {part, folder}. `part` is a name/id/index or an "
     "ARRAY of them (parts, assemblies and drawings alike); folder \"\" "
     "moves them back to the top level. The folder must already exist, so a "
     "typo is an error instead of a new folder.",
     "folder, part"},
    {"rename_folder",
     "Rename folder {folder} to {new_name}, members and "
     "all. Renaming onto an existing folder MERGES them "
     "(reported as merged:true).",
     "folder, new_name"},
    {"dissolve_folder",
     "Remove folder {folder}; its tabs return to the top level. Documents "
     "are never deleted with their folder — see delete_part for that.",
     "folder"},
};
std::span<const MethodDoc> ProjectMethodDocs() { return ProjectMethods; }

size_t ResolveSlotIndex(ui::AppState& state, const json& sel) {
  ui::EnsureParts(state);
  const auto byId = [&](uint64_t v) -> int {
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].id == v)
        return static_cast<int>(i);
    // A stable id wins over a slot index, so echoing list_parts' `id` back
    // can never silently address a different tab.
    return v < state.parts.size() ? static_cast<int>(v) : -1;
  };
  int slot = -1;
  if (sel.is_number_integer() || sel.is_number_unsigned()) {
    slot = byId(sel.get<uint64_t>());
  } else if (sel.is_string()) {
    const std::string s = sel.get<std::string>();
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].name == s) {
        slot = static_cast<int>(i);
        break;
      }
    if (slot < 0) {
      try { // numeric string: stable id, then slot index
        slot = byId(std::stoull(s));
      } catch (...) {
      }
    }
  }
  if (slot < 0) {
    std::vector<std::string> names;
    for (const ui::PartSlot& ps : state.parts)
      names.push_back(ps.name);
    // "part" is this API's word for a tab of any kind (list_parts returns
    // assemblies and drawings too), so the wording stays with it.
    throw std::runtime_error("no part " + sel.dump() +
                             " — pass a name, stable id or slot index; "
                             "list_parts shows every tab. Existing: " +
                             KnownList(names));
  }
  return static_cast<size_t>(slot);
}

void RequireFreshTabName(ui::AppState& state, const std::string& name) {
  ui::EnsureParts(state);
  for (const ui::PartSlot& ps : state.parts)
    if (ps.name == name)
      throw std::runtime_error(
          "a tab named '" + name +
          "' already exists — names address tabs (add_occurrence, "
          "set_active_part), so two answering to one would be ambiguous; "
          "pick another or rename_part the existing one");
}

std::string FreshDefaultTabName(ui::AppState& state, const char* stem) {
  ui::EnsureParts(state);
  for (size_t n = state.parts.size() + 1;; n++) {
    const std::string candidate = std::string(stem) + " " + std::to_string(n);
    bool taken = false;
    for (const ui::PartSlot& ps : state.parts)
      taken = taken || ps.name == candidate;
    if (!taken)
      return candidate;
  }
}

namespace {

// The tab-bar folder registry as JSON, appended to every folder RPC's reply.
// A caller that just changed the set should not have to ask what it is now.
json FolderList(const ui::AppState& state) {
  json arr = json::array();
  for (const std::string& f : state.tabFolders)
    arr.push_back(f);
  return arr;
}

bool FolderExists(const ui::AppState& state, const std::string& folder) {
  if (std::find(state.tabFolders.begin(), state.tabFolders.end(), folder) !=
      state.tabFolders.end())
    return true;
  // A folder can also exist only as membership — an older project, or a
  // registry entry dropped by a merge. Treat that as existing rather than
  // telling someone a folder they can see is not there.
  for (const ui::PartSlot& ps : state.parts)
    if (ps.folder == folder)
      return true;
  return false;
}

// Non-empty, and it exists. The error names what there is, because "no such
// folder" without the list costs a round-trip to find a typo.
void RequireFolder(const ui::AppState& state, const std::string& folder,
                   const char* what) {
  if (folder.empty())
    throw std::runtime_error(std::string(what) + " must be a folder name");
  if (!FolderExists(state, folder)) {
    std::vector<std::string> known(state.tabFolders.begin(),
                                   state.tabFolders.end());
    throw std::runtime_error("no folder \"" + folder +
                             "\" — existing: " + KnownList(known) +
                             " (create_folder makes a new one)");
  }
}

} // namespace

namespace {
// An export's `body`: a body's 0-based index, or absent for all of them.
// `p.value("body", -1)` read -5 as "all" and 1e308 as whatever the narrowing
// made of it.
int BodyIndexParam(const RpcParams& p) {
  if (!p.contains("body"))
    return -1;
  const json& b = p.at("body");
  if (!b.is_number_integer() || b.get<long long>() < 0 ||
      b.get<long long>() > 1000000)
    throw std::runtime_error("body is a body's 0-based index, as get_state "
                             "lists them (omit it for every body); got " +
                             b.dump());
  return b.get<int>(); // checked above
}
// A mesh's chord deviation in mm: at least a tenth of a micron, at most a
// metre. Zero, less or 1e-308 reached BRepMesh as "invalid parameter value"
// (an OpenCASCADE throw).
std::optional<double> DeflectionParam(const RpcParams& p, const char* key) {
  if (!p.contains(key))
    return std::nullopt;
  const double d = p.at(key).get<double>();
  if (!(d >= 1e-4 && d <= 1000.0))
    throw std::runtime_error(std::string(key) +
                             " is the chord deviation in mm, in "
                             "[0.0001, 1000]; got " +
                             p.at(key).dump());
  return d;
}
} // namespace

std::optional<json> HandleProjectRpc(ui::AppState& state, const std::string& m,
                                     const RpcParams& p) {
  Document* doc = state.document.get();
  AssemblyDocument* asmDoc = state.assembly.get();
  const auto componentPart = [&](uint64_t id) -> Document& {
    ui::EnsureParts(state);
    auto* part = ui::PartDocById(state, id);
    if (!part)
      throw std::runtime_error("no part with stable id " + std::to_string(id));
    return *part;
  };
  const auto componentInfo = [&](uint64_t id, const Document& part) {
    return json{{"part_id", id},
                {"generator", part.Generator()},
                {"revision", generated::Revision(part)},
                {"state", part.Generator().is_null()  ? "unowned"
                          : generated::Modified(part) ? "modified"
                                                      : "owned"}};
  };
  if (m == "validation_revision") {
    ui::EnsureParts(state);
    json rows = json::array(), missing = json::array();
    for (const auto& slot : state.parts) {
      if (const auto* part = ui::PartDocById(state, slot.id)) {
        rows.push_back({{"id", slot.id},
                        {"instance", part->InstanceId()},
                        {"version", part->Version()}});
      } else if (const auto* assembly = ui::AssemblyDocById(state, slot.id)) {
        json recipe = json::array();
        for (const auto& occurrence : assembly->GetOccurrences()) {
          recipe.push_back(DocumentIO::OccurrenceToJson(occurrence));
          // the live part: whether it exists at all
          if (!ui::PartDocById(state, occurrence.partId) &&
              !ui::AssemblyDocById(state, occurrence.partId))
            missing.push_back(
                {{"assembly", slot.id}, {"occurrence", occurrence.id}});
        }
        recipe.push_back(
            {{"active_configuration", assembly->ActiveConfiguration()}});
        for (const auto& config : assembly->GetConfigurations())
          recipe.push_back(DocumentIO::ConfigurationToJson(config));
        for (const auto& mate : assembly->GetMates())
          recipe.push_back(DocumentIO::MateToJson(mate));
        for (const auto& relation : assembly->GetRelations())
          recipe.push_back(DocumentIO::RelationToJson(relation));
        for (const auto& study : assembly->GetMotionStudies())
          recipe.push_back(DocumentIO::MotionStudyToJson(study));
        rows.push_back({{"id", slot.id},
                        {"instance", reinterpret_cast<uintptr_t>(assembly)},
                        {"recipe", catalog::Sha256(recipe.dump())}});
      }
    }
    return json{{"revision", rows}, {"missing_references", missing}};
  }
  if (m == "component_status") {
    if (p.contains("part_id")) {
      const auto id = p.at("part_id").get<uint64_t>();
      return componentInfo(id, componentPart(id));
    }
    const auto space = p.at("namespace").get<std::string>();
    const auto key = p.at("key").get<std::string>();
    ui::EnsureParts(state);
    json found;
    for (const auto& slot : state.parts) {
      auto* part = ui::PartDocById(state, slot.id);
      if (!part || !part->Generator().is_object())
        continue;
      const auto& stamp = part->Generator();
      if (stamp.value("namespace", std::string{}) == space &&
          stamp.value("key", std::string{}) == key) {
        if (!found.is_null())
          throw std::runtime_error("duplicate component ownership");
        found = componentInfo(slot.id, *part);
      }
    }
    return found.is_null() ? json{{"state", "missing"}} : found;
  }
  if (m == "claim_component") {
    const auto id = p.at("part_id").get<uint64_t>();
    auto& part = componentPart(id);
    const auto expected = p.at("expected_revision");
    if (expected != generated::Revision(part))
      throw ContractError(m, "stale_revision", "/params/expected_revision",
                          generated::Revision(part), expected);
    const auto space = p.at("namespace").get<std::string>();
    const auto key = p.at("key").get<std::string>();
    const auto fingerprint = p.at("fingerprint").get<std::string>();
    if (space.empty() || key.empty() || fingerprint.empty())
      throw std::runtime_error(
          "component namespace, key and fingerprint must be nonempty");
    for (const auto& slot : state.parts) {
      auto* other = ui::PartDocById(state, slot.id);
      if (slot.id != id && other && other->Generator().is_object() &&
          other->Generator().value("namespace", std::string{}) == space &&
          other->Generator().value("key", std::string{}) == key)
        throw std::runtime_error(
            "component key already belongs to another part");
    }
    if (!part.Generator().is_null()) {
      if (part.Generator().value("namespace", std::string{}) != space ||
          part.Generator().value("key", std::string{}) != key ||
          generated::Modified(part))
        throw std::runtime_error("component ownership conflict: cannot claim "
                                 "edited or differently owned output");
      if (part.Generator().value("fingerprint", std::string{}) == fingerprint)
        return componentInfo(id, part);
      throw std::runtime_error(
          "use update_component to change an existing ownership fingerprint");
    }
    const json stamp{{"namespace", space},
                     {"key", key},
                     {"fingerprint", fingerprint},
                     {"recipe_hash", generated::RecipeHash(part)}};
    for (size_t i = 0; i < state.parts.size(); ++i)
      if (state.parts[i].id == id) {
        ui::SetActivePart(state, i);
        break;
      }
    class Claim : public Command {
      json before_, after_;

    public:
      Claim(json before, json after)
          : before_(std::move(before)), after_(std::move(after)) {}
      std::string Description() const override {
        return "Claim generated component";
      }
      bool Execute(Document& d) override {
        d.SetGeneratorRaw(after_);
        d.Regenerate();
        return true;
      }
      void Undo(Document& d) override {
        d.SetGeneratorRaw(before_);
        d.Regenerate();
      }
    };
    state.commands->Execute(std::make_unique<Claim>(part.Generator(), stamp),
                            part);
    return componentInfo(id, part);
  }
  if (m == "update_component") {
    const auto id = p.at("part_id").get<uint64_t>();
    const auto sourceId = p.at("source_part_id").get<uint64_t>();
    if (sourceId == id)
      throw std::runtime_error("staging source must be a different part");
    auto& part = componentPart(id);
    auto& source = componentPart(sourceId);
    const auto fingerprint = p.at("fingerprint").get<std::string>();
    if (fingerprint.empty())
      throw std::runtime_error("fingerprint must be nonempty");
    if (p.at("expected_revision") != generated::Revision(part) ||
        p.at("source_revision") != generated::Revision(source))
      throw ContractError(m, "stale_revision", "/params",
                          "unchanged source and target revisions", nullptr);
    if (!part.Generator().is_object() || generated::Modified(part))
      throw ContractError(m, "ownership_conflict", "/params/part_id",
                          "unmodified owned output", id);
    for (const Document* d : {&part, &source}) {
      if (!d->GetAssets().empty() || !d->GetConfigurations().empty() ||
          !d->GetStudies().empty() || d->Rollback() >= 0)
        throw std::runtime_error(
            "component update pilot does not replace assets, configurations, "
            "studies or rollback states");
    }
    if (generated::Recipe(part)["units"] != generated::Recipe(source)["units"])
      throw std::runtime_error(
          "component update requires matching document units");
    std::set<uint64_t> ids;
    for (const auto& f : source.GetFeatures()) {
      if (!ids.insert(f->GetId()).second)
        throw std::runtime_error("staging component has duplicate feature IDs");
      if (const auto* old = part.FindFeatureById(f->GetId());
          old &&
          (old->GetName() != f->GetName() || old->TypeName() != f->TypeName()))
        throw std::runtime_error(
            "feature identity conflict: stage with stable feature keys");
    }
    for (const auto& f : part.GetFeatures())
      if (!ids.contains(f->GetId()))
        throw std::runtime_error(
            "component update pilot cannot retire existing features; keep or "
            "suppress their stable keys");
    if (source.GetBodies().empty())
      throw std::runtime_error("staging component is empty");
    for (const auto& f : source.GetFeatures())
      if (!f->GetError().empty())
        throw std::runtime_error("staging feature failed: " + f->GetError());
    for (const auto& b : source.GetBodies())
      if (!Measure::CheckGeometry(b.shape).valid || b.shape.Volume() <= 0)
        throw std::runtime_error(
            "staging component has invalid or empty geometry");
    // Solve copies of every dependent assembly with the candidate resolver.
    // Missing datums and broken mates fail before any target mutation.
    json affected = json::array();
    // The staged part stands in for every configuration of it: its
    // configured copies do not exist yet, and its active one is what the
    // import changed.
    const assembly::PartResolver others = ui::PartResolverFor(state);
    const auto parts = [&](uint64_t partId,
                           uint64_t configuration) -> const Document* {
      return partId == id ? &source : others(partId, configuration);
    };
    for (const auto& slot : state.parts) {
      const auto* assembly = ui::AssemblyDocById(state, slot.id);
      if (!assembly)
        continue;
      bool depends = false;
      for (const auto& [child, version] :
           assembly::SubtreeVersions(*assembly, ui::PartResolverFor(state),
                                     ui::AssemblyResolverFor(state)))
        depends |= child == id;
      if (!depends)
        continue;
      AssemblyDocument candidate = *assembly;
      // Only the counts are read, so skip naming the mate at fault.
      const auto solved =
          assembly::Solve(candidate, parts, ui::AssemblyResolverFor(state), {},
                          /*diagnose=*/false);
      if (solved.erroredMates || !solved.loopsClosed)
        throw std::runtime_error("staging component breaks mates in assembly " +
                                 slot.name);
      affected.push_back(slot.id);
    }
    json stamp = part.Generator();
    stamp["fingerprint"] = fingerprint;
    stamp["recipe_hash"] = generated::RecipeHash(source);
    const bool same =
        stamp == part.Generator() &&
        generated::RecipeHash(part) == generated::RecipeHash(source);
    json out{{"action", same ? "reuse" : "update"},
             {"part_id", id},
             {"affected_assemblies", affected},
             {"applied", false}};
    if (!p.value("apply", false) || same)
      return out;
    for (size_t i = 0; i < state.parts.size(); ++i)
      if (state.parts[i].id == id) {
        ui::SetActivePart(state, i);
        break;
      }
    if (!state.commands->Execute(
            std::make_unique<generated::UpdateCommand>(part, source, stamp),
            part))
      throw std::runtime_error("component update failed and was rolled back");
    out["applied"] = true;
    out["revision"] = generated::Revision(part);
    out["regen"] = remote::RegenSummaryJson(part);
    out["regen"]["executed"] = part.LastRegenStats().executed;
    out["regen"]["cached"] = part.LastRegenStats().cached;
    bool reused = part.GetBodies().size() == source.GetBodies().size();
    for (size_t i = 0; reused && i < part.GetBodies().size(); ++i)
      reused =
          part.GetBodies()[i].shape.SameGeometryAs(source.GetBodies()[i].shape);
    out["geometry_reused"] = reused;
    return out;
  }
  if (m == "new_project") {
    ui::ReplaceDocument(state, std::make_unique<Document>());
    state.parts.clear(); // reset to a single fresh part (A1)
    state.tabFolders.clear();
    ui::ClearSections(state); // a new project starts uncut
    ui::EnsureParts(state, "Part 1");
    ui::MarkProjectOpened(state, "");
    return json::object();
  }
  if (m == "save_project") {
    // params: {path, message?, force?} — message becomes the git commit
    // subject for directory (.dsgn) projects (F5); ignored otherwise. `force`
    // saves over a project that changed on disk (F8), which an agent that
    // just did the changing may legitimately want.
    const std::string path = p.at("path").get<std::string>();
    // Every key read before the file is written: a `message` of the wrong
    // type used to be refused after the save had landed.
    const bool force = p.value("force", false);
    const std::string message = p.value("message", std::string("save"));
    std::string error;
    if (!ui::SaveProject(state, path, &error, force))
      throw std::runtime_error(error);
    // Also clears the autosave sidecar and commits git-backed projects.
    ui::MarkProjectSaved(state, path, message);
    return json::object();
  }
  if (m == "open_project") {
    // autosave_available: a sidecar newer than this file exists (crash
    // recovery). Recovering = open_project on Autosaver::SidecarPath(path);
    // callers see the same signal the GUI recovery modal acts on.
    const std::string path = p.at("path").get<std::string>();
    const bool autosaveAvailable = Autosaver::HasNewerAutosave(path);
    std::string error;
    if (!ui::OpenProject(state, path, &error)) // multi-part aware (A1)
      throw std::runtime_error(error);
    ui::MarkProjectOpened(state, path);
    // Loading is where a broken model most often ARRIVES: a project saved
    // against an older kernel, or one whose feature no longer builds, used to
    // open with a silent success and nothing to poll.
    json out = {{"autosave_available", autosaveAvailable},
                {"autosave_path", Autosaver::SidecarPath(path)}};
    // Parts whose baked geometry could not be used and were rebuilt, and
    // why: the difference between a 0.04 s open and a 20 s one, and what a
    // save would fix (docs/ui/long-operations.md#opening-a-project).
    out["rebuilt_parts"] = state.openRebuiltParts;
    if (!state.openRebuildReason.empty())
      out["rebuild_reason"] = state.openRebuildReason;
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "checkpoint_create") {
    // A checkpoint is a NAMED VERSION of the whole project (F6): save, then
    // tag. It used to snapshot `*doc` alone — on a multi-part project that
    // silently left out the assembly, the other parts and the drawings.
    // params: {name}
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (ui::CanVersionProject(state)) {
      if (!ui::NameCurrentVersion(state, name, &error))
        throw std::runtime_error(error);
      return json{{"name", name}, {"ref", GitVault::TagRef(name)}};
    }
    // No git: fall back to F4's file snapshot, which is part-only — say so
    // rather than pretending the whole project was captured.
    if (!doc)
      throw std::runtime_error("no active part to snapshot (and git, which "
                               "would version the whole project, is missing)");
    if (!Checkpoints::Create(*doc, state.projectPath, name, &error))
      throw std::runtime_error(error);
    return json{{"name", name},
                {"path", Checkpoints::PathOf(state.projectPath, name)},
                {"part_only", true}};
  }
  if (m == "checkpoint_list") {
    json list = json::array();
    if (ui::ProjectHasHistory(state)) {
      for (const auto& e : GitVault::Log(state.projectPath, 200))
        if (e.named)
          list.push_back({{"name", e.subject},
                          {"ref", e.tagRef}, // READ, not recomputed (F9 C5)
                          {"modified_unix", e.unixTime}});
    } else {
      for (const auto& info : Checkpoints::List(state.projectPath))
        list.push_back(
            {{"name", info.name}, {"modified_unix", info.modifiedUnix}});
    }
    return json{{"checkpoints", list}};
  }
  if (m == "checkpoint_restore") {
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (ui::ProjectHasHistory(state)) {
      // The whole project goes back to the tagged version, and the restore
      // is committed on top so history stays linear.
      // The ref may have been disambiguated when someone else had already
      // used this name (F9 C5), so find the one that exists rather than
      // recomputing it from the name.
      std::string ref = GitVault::TagRef(name);
      for (const auto& e : GitVault::Log(state.projectPath, 200)) {
        if (e.named && e.subject == name) {
          ref = e.tagRef;
          break;
        }
      }
      if (ref.empty() || !ui::RestoreVersion(state, ref, name, &error))
        throw std::runtime_error(error.empty() ? "no such version: " + name
                                               : error);
      return json::object();
    }
    const std::string path = Checkpoints::PathOf(state.projectPath, name);
    if (path.empty())
      throw std::runtime_error("no such checkpoint: " + name);
    auto loaded = DocumentIO::Load(path);
    if (!loaded)
      throw std::runtime_error(loaded.error().empty()
                                   ? "no such checkpoint: " + name
                                   : loaded.error());
    ui::ReplaceDocument(state, std::move(*loaded));
    ui::FitCameraToDocument(state);
    ui::MarkProjectRecovered(state, state.projectPath);
    json out = json::object();
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "history_log") {
    // Git history of a directory project (F5). params: {limit?: 20}
    json entries = json::array();
    for (const auto& e : GitVault::Log(state.projectPath, p.value("limit", 20)))
      entries.push_back({{"hash", e.hash},
                         {"subject", e.subject},
                         {"unix", e.unixTime},
                         {"named", e.named}});
    return json{{"log", entries}};
  }
  if (m == "history_restore") {
    // Put the WHOLE project back to a revision and reopen it; the restore is
    // committed on top, so history stays linear. It used to reload only the
    // active document, which on a multi-part project left the other tabs
    // showing pre-restore geometry.
    // params: {commit} (hash, tag like "cp/v1", or any git rev)
    const std::string commit = p.at("commit").get<std::string>();
    std::string error;
    if (!ui::RestoreVersion(state, commit, commit, &error))
      throw std::runtime_error(error);
    json out = json::object();
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "project_changed_on_disk") {
    return json{{"changed", ui::ProjectChangedOnDisk(state)},
                {"path", state.projectPath}};
  }
  if (m == "autosave_now") {
    // Testing and scripting: the interval is a wall clock, and a test should
    // not sleep for it. Uses its own Autosaver so the app's interval timing
    // is untouched.
    Autosaver immediate(0.001);
    immediate.Prime(0.0);
    const int written = ui::AutosaveProject(state, immediate, 1.0);
    return json{{"documents", written}};
  }
  if (m == "autosave_status") {
    json docs = json::array();
    for (const std::string& rel :
         Autosaver::RecoverableDocuments(state.projectPath))
      docs.push_back(rel);
    return json{
        {"documents", docs},
        {"single_file", Autosaver::HasNewerAutosave(state.projectPath)}};
  }
  // --- Variables (moved here from RemoteModeling 2026-08-22) -------------
  //
  // They default to PROJECT scope, so they are not a property of the tab you
  // happen to be on — and living in the modeling module meant exactly that,
  // because HandleModelingRpc takes a Document REFERENCE and so cannot be
  // called at all on an assembly or drawing tab. Setting a project variable
  // from a drawing tab came back "the active document is a drawing, not a
  // part". Here `doc` is a pointer, and the handlers ask for a part only when
  // the scope is "part".
  if (m == "set_units" || m == "get_units") {
    // Units live on the DOCUMENT (core/Units.h), and an assembly tab has no
    // `doc` at all — the BOM's masses are shown there, so the assembly needs
    // its own. Hence the two branches rather than a Document& gate.
    units::System sys;
    if (asmDoc != nullptr)
      sys = asmDoc->Units();
    else if (doc != nullptr)
      sys = doc->Units();
    else
      throw std::runtime_error("no active document");

    if (m == "set_units") {
      // Refuse, don't substitute: a caller who writes "inch" and gets
      // millimetres is wrong in every dimension and looks fine.
      // The keys reach RpcParams through this lambda's argument, so the
      // describe-schema scanner cannot see them at the p.at() call.
      // rpc-params: length, mass, angle
      const auto parse = [&p](const char* key, auto&& from, auto& out,
                              const std::vector<const char*>& accepted) {
        if (!p.contains(key))
          return;
        const std::string v = p.at(key).get<std::string>();
        if (!from(v, out)) {
          std::string list;
          for (const char* a : accepted)
            list += (list.empty() ? "" : ", ") + std::string(a);
          throw std::runtime_error("unknown " + std::string(key) + " unit '" +
                                   v + "' (expected one of: " + list + ")");
        }
      };
      parse("length", units::LengthFromString, sys.length,
            units::LengthNames());
      parse("mass", units::MassFromString, sys.mass, units::MassNames());
      parse("angle", units::AngleFromString, sys.angle, units::AngleNames());
      if (asmDoc != nullptr)
        asmDoc->SetUnits(sys);
      else
        doc->SetUnits(sys); // saved, no version: moves the save stamp
    }
    return json{{"length", units::ToString(sys.length)},
                {"mass", units::ToString(sys.mass)},
                {"angle", units::ToString(sys.angle)}};
  }

  if (m == "set_variable") {
    // Add or redefine a variable; one undoable edit, regenerates every
    // expression-driven field. params: {name, expression, comment?,
    // scope?}. scope defaults to "project" (Phase V: define a driving
    // dimension once, shared by every part); "part" writes the active
    // document's local scope, which shadows a same-named project var.
    // Returns the evaluated value + scope (plus "error" if it didn't evaluate).
    const std::string name = p.at("name").get<std::string>();
    const json& expr = p.at("expression");
    const std::string expression =
        expr.is_string() ? expr.get<std::string>() : expr.dump();
    const std::string scope = p.value("scope", std::string("project"));
    if (scope != "project" && scope != "part")
      throw std::runtime_error("scope must be \"project\" or \"part\"");
    const bool partScope = scope == "part";
    // A PART-scope variable needs a part; a project-scope one does not, and
    // used to be refused on an assembly or drawing tab because the dispatch
    // gate asked before the scope was known.
    if (partScope && doc == nullptr)
      throw std::runtime_error(
          "a part-scope variable needs a part tab; scope \"project\" works "
          "from anywhere");
    // Omitting "comment" preserves an existing one (redefinitions usually
    // only change the expression).
    std::string comment;
    if (p.contains("comment")) {
      comment = p.at("comment").get<std::string>();
    } else {
      const Parameter* prior = partScope
                                   ? doc->FindParameter(name)
                                   : ui::FindProjectParameter(state, name);
      if (prior)
        comment = prior->comment;
    }
    if (partScope) {
      if (!state.commands->Execute(
              std::make_unique<SetVariableCommand>(name, expression, comment),
              *doc))
        throw std::runtime_error("invalid variable name: " + name);
    } else if (!ui::SetProjectVariable(state, name, expression, comment)) {
      throw std::runtime_error("invalid variable name: " + name);
    }
    const Parameter* param = partScope ? doc->FindParameter(name)
                                       : ui::FindProjectParameter(state, name);
    json result = {{"value", param->value}, {"scope", scope}};
    if (!param->error.empty())
      result["error"] = param->error;
    if (doc != nullptr) // no active part on an assembly or drawing tab
      result["regen"] = RegenSummaryJson(*doc);
    return result;
  }
  if (m == "delete_variable") {
    // Refused while referenced, reporting what references it. A project
    // variable's references span every part. params: {name, scope?}. scope
    // auto-detects (project first, then the active part's local) when omitted.
    const std::string name = p.at("name").get<std::string>();
    const bool haveProject = ui::FindProjectParameter(state, name) != nullptr;
    const bool haveLocal =
        doc != nullptr && doc->FindParameter(name) != nullptr;
    const std::string scope =
        p.value("scope", haveProject || !haveLocal ? "project" : "part");
    // Refused, not read as "project": a typo would delete the other one.
    if (scope != "project" && scope != "part")
      throw std::runtime_error("scope must be \"project\" or \"part\", not \"" +
                               scope + "\"");
    if (scope == "part") {
      if (!haveLocal)
        throw std::runtime_error("no such variable: " + name);
      std::vector<std::string> refs = doc->FindParameterReferences(name);
      if (!refs.empty()) {
        std::string msg = "variable is referenced by:";
        for (const std::string& r : refs)
          msg += " " + r + ";";
        throw std::runtime_error(msg);
      }
      state.commands->Execute(std::make_unique<DeleteVariableCommand>(name),
                              *doc);
      json out = json::object();
      AttachRegen(out, state.document.get());
      return out;
    }
    if (!haveProject)
      throw std::runtime_error("no such variable: " + name);
    std::vector<std::string> refs;
    if (!ui::RemoveProjectVariable(state, name, &refs)) {
      std::string msg = "variable is referenced by:";
      for (const std::string& r : refs)
        msg += " " + r + ";";
      throw std::runtime_error(msg);
    }
    json out = json::object();
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "rename_variable") {
    // Renames + rewrites every referencing expression (a project rename spans
    // all parts). params: {name, new_name, scope?}. scope auto-detects like
    // delete_variable.
    const std::string name = p.at("name").get<std::string>();
    const std::string newName = p.at("new_name").get<std::string>();
    const bool haveProject = ui::FindProjectParameter(state, name) != nullptr;
    const bool haveLocal =
        doc != nullptr && doc->FindParameter(name) != nullptr;
    const std::string scope =
        p.value("scope", haveProject || !haveLocal ? "project" : "part");
    if (scope != "project" && scope != "part")
      throw std::runtime_error("scope must be \"project\" or \"part\", not \"" +
                               scope + "\"");
    if (scope == "part") {
      if (!state.commands->Execute(
              std::make_unique<RenameVariableCommand>(name, newName), *doc))
        throw std::runtime_error(
            "rename failed: '" + name + "' -> '" + newName +
            "'. Either the source does not exist, the new name is not a "
            "valid identifier, or it is already taken — list_variables "
            "shows what exists in each scope");
      json out = json::object();
      AttachRegen(out, state.document.get());
      return out;
    }
    if (!ui::RenameProjectVariable(state, name, newName))
      throw std::runtime_error(
          "rename failed: '" + name + "' -> '" + newName +
          "'. Either the source does not exist, the new name is not a "
          "valid identifier, or it is already taken — list_variables "
          "shows what exists in each scope");
    json out = json::object();
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "set_table") {
    // A project table (docs/variables/README.md#tables). Rows come as
    // {key, values} or {key, <column>: value}; anything else is refused.
    lookuptable::Table t;
    t.name = p.at("name").get<std::string>();
    t.comment = p.value("comment", std::string{});
    if (!p.contains("columns") || !p.at("columns").is_array())
      throw std::runtime_error(
          "set_table: `columns` is the list of column names, e.g. [\"d\", "
          "\"D\", \"B\"]");
    for (const json& c : p.at("columns")) {
      if (!c.is_string())
        throw std::runtime_error("set_table: a column name is a string, not " +
                                 c.dump());
      t.columns.push_back(c.get<std::string>());
    }
    for (const json& rj : p.value("rows", json::array())) {
      if (!rj.is_object() || !rj.contains("key"))
        throw std::runtime_error(
            "set_table: a row is {key, values: [...]} or {key, <column>: "
            "value, ...}, not " +
            rj.dump());
      std::string why;
      const auto key = lookuptable::KeyFromJson(rj.at("key"), &why);
      if (!key)
        throw std::runtime_error("set_table: " + why);
      lookuptable::Row row;
      row.key = *key;
      const auto number = [&](const json& v, const std::string& where) {
        if (!v.is_number())
          throw std::runtime_error("set_table: row '" + row.key + "', " +
                                   where + " is not a number: " + v.dump());
        return v.get<double>();
      };
      if (rj.contains("values")) {
        if (rj.size() != 2 || !rj.at("values").is_array())
          throw std::runtime_error(
              "set_table: row '" + row.key +
              "' gives `values` as a list, and nothing else beside its key");
        for (const json& v : rj.at("values"))
          row.values.push_back(number(v, "a value"));
      } else {
        for (const auto& [k, v] : rj.items()) {
          if (k == "key")
            continue;
          if (std::find(t.columns.begin(), t.columns.end(), k) ==
              t.columns.end()) {
            std::string cols;
            for (const std::string& c : t.columns)
              cols += (cols.empty() ? "" : ", ") + c;
            throw std::runtime_error("set_table: row '" + row.key +
                                     "' names column '" + k +
                                     "', and the columns are " + cols);
          }
        }
        for (const std::string& c : t.columns) {
          if (!rj.contains(c))
            throw std::runtime_error("set_table: row '" + row.key +
                                     "' has no value for column '" + c + "'");
          row.values.push_back(number(rj.at(c), "column '" + c + "'"));
        }
      }
      t.rows.push_back(std::move(row));
    }
    if (const std::string why = ui::SetProjectTable(state, t); !why.empty())
      throw std::runtime_error("set_table: " + why);
    json out = {{"table", lookuptable::ToJson(t)}};
    if (doc != nullptr)
      out["regen"] = RegenSummaryJson(*doc);
    return out;
  }
  if (m == "list_tables") {
    if (p.contains("name")) {
      const std::string name = p.at("name").get<std::string>();
      const lookuptable::Table* t =
          lookuptable::Find(state.projectTables, name);
      if (t == nullptr) {
        std::string names;
        for (const lookuptable::Table& x : state.projectTables)
          names += (names.empty() ? "" : ", ") + x.name;
        throw std::runtime_error(
            "no table '" + name + "' - " +
            (names.empty() ? std::string("this project has no tables")
                           : "the tables are " + names));
      }
      return json{{"tables", json::array({lookuptable::ToJson(*t)})}};
    }
    return json{{"tables", lookuptable::ToJson(state.projectTables)}};
  }
  if (m == "delete_table") {
    if (const std::string why =
            ui::RemoveProjectTable(state, p.at("name").get<std::string>());
        !why.empty())
      throw std::runtime_error("delete_table: " + why);
    json out = json::object();
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "list_variables") {
    // Both scopes, each tagged. Project variables first (the default scope),
    // then the active part's local variables. params: none.
    json variables = json::array();
    const auto emit = [&](const Parameter& param, const char* scope) {
      json entry = {{"name", param.name},
                    {"expression", param.expression},
                    {"value", param.value},
                    {"scope", scope}};
      if (!param.comment.empty())
        entry["comment"] = param.comment;
      if (!param.error.empty())
        entry["error"] = param.error;
      variables.push_back(entry);
    };
    for (const Parameter& param : state.projectParameters)
      emit(param, "project");
    if (state.document)
      for (const Parameter& param : state.document->GetParameters())
        emit(param, "part");
    return json{{"variables", variables}};
  }

  if (m == "remote_list") {
    json list = json::array();
    for (const auto& r : GitVault::Remotes(state.projectPath))
      list.push_back({{"name", r.name}, {"url", r.url}});
    const auto d = GitVault::AheadBehind(state.projectPath);
    return json{{"remotes", list},
                {"upstream", GitVault::Upstream(state.projectPath)},
                {"has_upstream", d.hasUpstream},
                {"ahead", d.ahead},
                {"behind", d.behind}};
  }
  if (m == "remote_add") {
    std::string error;
    if (!ui::AddProjectRemote(state, p.value("name", "origin"),
                              p.at("url").get<std::string>(), &error))
      throw std::runtime_error(error);
    return json::object();
  }
  if (m == "remote_remove") {
    std::string error;
    if (!GitVault::RemoveRemote(state.projectPath,
                                p.at("name").get<std::string>(), &error))
      throw std::runtime_error(error);
    return json::object();
  }
  if (m == "fetch") {
    // Synchronous here on purpose: the RPC has no frame to keep drawing, and
    // a caller that asked for a fetch is waiting for its answer. The worker
    // thread exists for the GUI (ui::SyncJob).
    std::string error;
    if (!GitVault::Fetch(state.projectPath, p.value("remote", "origin"),
                         &error))
      throw std::runtime_error(error);
    const auto d = GitVault::AheadBehind(state.projectPath);
    return json{{"ahead", d.ahead}, {"behind", d.behind}};
  }
  if (m == "pull") {
    const std::string remote = p.value("remote", "origin");
    std::string error;
    if (!GitVault::Fetch(state.projectPath, remote, &error))
      throw std::runtime_error(error);
    const std::string upstream = GitVault::Upstream(state.projectPath);
    if (upstream.empty())
      throw std::runtime_error(
          "this branch does not track a remote yet — push it first");
    const GitVault::MergeStatus status =
        GitVault::Merge(state.projectPath, upstream, &error);
    if (status == GitVault::MergeStatus::Failed)
      throw std::runtime_error(error);
    static constexpr const char* kStatusName[] = {"up_to_date", "fast_forward",
                                                  "merged", "conflicted"};
    std::string openError;
    if (status != GitVault::MergeStatus::Conflicted &&
        status != GitVault::MergeStatus::UpToDate &&
        !ui::OpenProject(state, state.projectPath, &openError)) {
      // A merge git calls clean can still leave a project that does not
      // load — both sides minted the same feature id. That is a conflict to
      // decide, and conflict_list names it; the loader's message alone reads
      // like a corrupt file.
      ui::InvalidateConflicts(state);
      const auto report = ProjectMerge::Scan(state.projectPath);
      if (report.conflicts.empty())
        throw std::runtime_error(openError);
      return json{{"status", "conflicted"},
                  {"conflicts", report.conflicts.size()},
                  {"needs_decision", report.NeedsDecision()},
                  {"open_error", openError}};
    }
    ui::InvalidateConflicts(state);
    const auto report = ProjectMerge::Scan(state.projectPath);
    return json{{"status", kStatusName[static_cast<int>(status)]},
                {"conflicts", report.conflicts.size()},
                {"needs_decision", report.NeedsDecision()}};
  }
  if (m == "push") {
    const std::string remote = p.value("remote", "origin");
    const std::string branch = GitVault::CurrentBranch(state.projectPath);
    std::string error;
    const GitVault::PushResult result =
        GitVault::Push(state.projectPath, remote, branch,
                       GitVault::Upstream(state.projectPath).empty(), &error);
    if (result == GitVault::PushResult::Failed)
      throw std::runtime_error(error);
    if (result == GitVault::PushResult::Rejected)
      throw std::runtime_error(error + " — pull, then push again");
    return json{{"status", result == GitVault::PushResult::UpToDate
                               ? "up_to_date"
                               : "pushed"}};
  }
  if (m == "clone_project") {
    const std::string path = p.at("path").get<std::string>();
    std::string error;
    if (!GitVault::Clone(p.at("url").get<std::string>(), path, &error))
      throw std::runtime_error(error);
    if (!ui::OpenProject(state, path, &error))
      throw std::runtime_error(error);
    ui::MarkProjectOpened(state, path);
    json out = {{"path", path}};
    AttachRegen(out, state.document.get());
    return out;
  }
  if (m == "branch_list") {
    json names = json::array();
    for (const std::string& branch : GitVault::Branches(state.projectPath))
      names.push_back(branch);
    return json{{"branches", names},
                {"current", GitVault::CurrentBranch(state.projectPath)}};
  }
  if (m == "branch_create") {
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (!GitVault::CreateBranch(state.projectPath, name,
                                p.value("switch", true), &error))
      throw std::runtime_error(error);
    return json{{"current", GitVault::CurrentBranch(state.projectPath)}};
  }
  if (m == "branch_switch") {
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (!ui::SwitchToBranch(state, name, &error))
      throw std::runtime_error(error);
    return json{{"current", GitVault::CurrentBranch(state.projectPath)}};
  }
  if (m == "branch_merge") {
    const std::string name = p.at("name").get<std::string>();
    std::string mergeError;
    const GitVault::MergeStatus status =
        GitVault::Merge(state.projectPath, name, &mergeError);
    if (status == GitVault::MergeStatus::Failed)
      throw std::runtime_error(mergeError);
    static constexpr const char* kStatusName[] = {"up_to_date", "fast_forward",
                                                  "merged", "conflicted"};
    // Reopen on a clean merge; a conflicted one leaves the tree mid-merge on
    // purpose, for conflict_list to describe.
    if (status != GitVault::MergeStatus::Conflicted &&
        status != GitVault::MergeStatus::UpToDate) {
      std::string error;
      if (!ui::OpenProject(state, state.projectPath, &error))
        throw std::runtime_error(error);
    }
    const auto report = ProjectMerge::Scan(state.projectPath);
    return json{{"status", kStatusName[static_cast<int>(status)]},
                {"conflicts", report.conflicts.size()},
                {"needs_decision", report.NeedsDecision()}};
  }
  if (m == "conflict_list") {
    const auto report = ProjectMerge::Scan(state.projectPath);
    static constexpr const char* kKindName[] = {
        "item", "order", "duplicate_id", "project", "derived", "other"};
    json list = json::array();
    for (const ProjectMerge::Conflict& c : report.conflicts) {
      json fields = json::array();
      for (const ProjectMerge::FieldDiff& f : c.fields)
        fields.push_back({{"field", f.field},
                          {"mine", f.ours},
                          {"theirs", f.theirs},
                          {"base", f.base}});
      list.push_back({{"kind", kKindName[static_cast<int>(c.kind)]},
                      {"path", c.path},
                      {"document", c.document},
                      {"document_kind", c.documentKind},
                      {"item_kind", c.itemKind},
                      {"name", c.name},
                      {"id", c.id},
                      {"summary", c.summary},
                      {"auto_resolvable", c.autoResolvable},
                      {"mine_deleted", c.oursDeleted},
                      {"theirs_deleted", c.theirsDeleted},
                      {"fields", fields}});
    }
    return json{{"in_merge", report.inMerge},
                {"conflicts", list},
                {"needs_decision", report.NeedsDecision()}};
  }
  if (m == "conflict_resolve") {
    const std::string path = p.at("path").get<std::string>();
    const std::string take = p.value("take", "auto");
    const auto report = ProjectMerge::Scan(state.projectPath);
    const ProjectMerge::Conflict* found = nullptr;
    for (const ProjectMerge::Conflict& c : report.conflicts)
      if (c.path == path)
        found = &c;
    if (!found)
      throw std::runtime_error("no conflict at " + path);

    std::string error;
    bool ok = false;
    if (take == "auto") {
      ok = found->kind == ProjectMerge::ConflictKind::DuplicateId
               ? ProjectMerge::RenumberDuplicate(state.projectPath, *found,
                                                 &error)
               : ProjectMerge::AutoResolve(state.projectPath, *found, &error);
    } else if (take == "mine" || take == "theirs") {
      const GitVault::MergeSide side = take == "mine"
                                           ? GitVault::MergeSide::Ours
                                           : GitVault::MergeSide::Theirs;
      const std::string field = p.value("field", "");
      ok = field.empty()
               ? ProjectMerge::TakeSide(state.projectPath, *found, side, &error)
               : ProjectMerge::TakeField(state.projectPath, *found, field, side,
                                         &error);
    } else {
      throw std::runtime_error("take must be mine, theirs or auto");
    }
    if (!ok)
      throw std::runtime_error(error);
    ui::InvalidateConflicts(state);
    const auto after = ProjectMerge::Scan(state.projectPath);
    return json{{"needs_decision", after.NeedsDecision()}};
  }
  if (m == "merge_finish") {
    std::string error;
    if (!ui::FinishMerge(state, &error)) {
      json broken = json::array();
      for (const ProjectMerge::Breakage& b : state.conflictsBreakage)
        broken.push_back({{"document", b.document},
                          {"document_kind", b.documentKind},
                          {"reason", b.reason}});
      // The bar is "the project works", so a failure has to say what broke
      // rather than leave a tree that opens to an error tomorrow.
      throw std::runtime_error(error +
                               (broken.empty() ? "" : ": " + broken.dump()));
    }
    return json::object();
  }
  if (m == "merge_abort") {
    std::string error;
    if (!ui::AbandonMerge(state, &error))
      throw std::runtime_error(error);
    return json::object();
  }
  if (m == "export_step" || m == "export_stl" || m == "export_brep") {
    // Export the whole document (all bodies) or one body (by index) to
    // STEP / binary STL / native BREP. params:
    // {path, body?: index, stl_deflection_mm? (STL only; `deflection` is
    // refused, naming it: RenamedParams)}
    const std::string path = p.at("path").get<std::string>();
    // Where it goes and what it takes, checked before anything is meshed or
    // written: an empty path, a directory or a missing parent fell through to
    // the writer's "export failed (see log)", and `body: -5` exported every
    // body as if it had been left out (hostile_params_smoke).
    if (std::string why; !DocumentIO::AtomicTargetOk(path, &why))
      throw std::runtime_error(m + ": " + why);
    const int bodyIndex = BodyIndexParam(p);
    // An ASSEMBLY exports every occurrence at its solved placement — the
    // same walk export_visual uses. STEP is the interchange format, and
    // "export the assembly" refusing with "switch to a part tab" made the
    // one document kind a customer actually asks for unexportable except as
    // a mesh (found by the T4 benchmark task, agent-scale plan G6).
    // STEP goes through ui::ExportStep (X4): named, coloured products placed
    // by reference, nested as the assembly is.
    if (m == "export_step") {
      const bool onAssembly = state.assembly && !p.contains("part");
      Document* target = p.contains("part")
                             ? remote::ResolvePartParam(state, p.at("part"))
                             : doc;
      if (!onAssembly && target == nullptr)
        throw std::runtime_error(
            "no part to export: switch to a part tab or pass `part`");
      const auto counts =
          ui::ExportStep(state, onAssembly ? nullptr : target, bodyIndex, path);
      if (!counts)
        throw std::runtime_error(counts.error());
      json out{{"path", path},
               {"products", counts->products},
               {"instances", counts->instances}};
      // `bodies` for a part, `shapes` for an assembly: the counts each reply
      // has always carried, so a caller reading either keeps working.
      out[onAssembly ? "shapes" : "bodies"] = counts->bodies;
      return out;
    }
    // STL: the one helper File > Export calls too
    // (docs/tolerances/README.md#exporting-a-printed-part). With no
    // `deflection`, a printed part meshes at its print profile's chord
    // deviation; an explicit coarser one is exported as asked and warned.
    // Read in a scope of its own so that only export_stl's row lists it, and
    // the dispatcher refuses it on BREP before anything is written.
    if (m == "export_stl") {
      // rpc-params: body, path, stl_deflection_mm
      const auto r = ui::ExportPartStl(*doc, path, bodyIndex,
                                       DeflectionParam(p, "stl_deflection_mm"));
      if (!r)
        throw std::runtime_error(r.error());
      json out{{"path", path},
               {"bodies", r->bodies},
               {"watertight", r->shells.watertight},
               {"shells", r->shells.shells},
               {"free_edges", r->shells.freeEdges},
               {"deflection_mm", r->deflectionMm},
               {"deflection_from", r->deflectionFrom}};
      if (r->smallestGapSideMm > 0.0) {
        out["smallest_gap_side_mm"] = r->smallestGapSideMm;
        out["chord_share"] = r->chordShare;
      }
      if (!r->warning.empty())
        out["warning"] = r->warning;
      return out;
    }
    // Derived reference bodies are other parts' geometry: never exported.
    // `body` indexes the document's body list as get_state reports it.
    std::vector<Body> bodies;
    for (const Body& b : doc->GetBodies())
      if (!b.reference)
        bodies.push_back(b);
    if (bodies.empty())
      throw std::runtime_error(
          doc->GetBodies().empty()
              ? "no bodies to export"
              : "only reference bodies (derived ghosts); nothing to export");
    const int only = bodyIndex;
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    // BREP is single-shape; a whole-document export of several bodies fuses
    // them (the usual CAD behavior) so one file holds the whole part.
    CADShape shape;
    if (only >= 0) {
      shape = bodies[only].shape;
    } else {
      shape = bodies[0].shape;
      for (size_t i = 1; i < bodies.size(); i++)
        shape = shape.Union(bodies[i].shape);
    }
    if (!CADKernel::Instance().SaveBREP(path, shape))
      throw std::runtime_error(
          "export_brep: OpenCASCADE could not write " + path +
          " (BRepTools::Write failed: the disk full or the file not "
          "writable, or a shape it cannot write -- run check_geometry)");
    return json{{"path", path},
                {"bodies", only < 0 ? bodies.size() : size_t{1}}};
  }
  if (m == "export_visual") {
    // Visual-mesh export (interchange X2): glTF (.glb/.gltf), OBJ, or PLY,
    // chosen by the path extension. Works on a part (bodies at identity) OR
    // the active assembly (occurrences at solved placements). Materials carry
    // through (glTF is PBR). params: {path, deflection?: 0.1}
    const std::string path = p.at("path").get<std::string>();
    if (std::string why; !DocumentIO::AtomicTargetOk(path, &why))
      throw std::runtime_error(m + ": " + why);
    // rpc-params: deflection, path
    const double deflection = DeflectionParam(p, "deflection").value_or(0.1);
    const std::vector<MeshExport::Item> items =
        ui::BuildVisualExportItems(state);
    if (items.empty())
      throw std::runtime_error("nothing to export");
    std::string err;
    if (!MeshExport::SaveVisual(path, items, deflection, &err))
      throw std::runtime_error(err);
    return json{{"path", path}, {"items", items.size()}};
  }
  if (m == "create_folder") {
    const std::string folder = p.at("folder").get<std::string>();
    if (folder.empty())
      throw std::runtime_error("folder must be a non-empty name");
    const bool existed = FolderExists(state, folder);
    ui::CreateFolder(state, folder);
    return json{{"folder", folder},
                {"created", !existed},
                {"folders", FolderList(state)}};
  }
  if (m == "move_to_folder") {
    // "" is the one legal empty folder: it means the top level, which is
    // how a tab gets OUT of a folder without a second verb.
    const std::string folder = p.at("folder").get<std::string>();
    if (!folder.empty())
      RequireFolder(state, folder, "folder");
    const json& sel = p.at("part");
    // One tab or many: a 100-part STEP import is the case that needs this,
    // and one call per tab would be the whole reason to skip organising it.
    std::vector<json> targets;
    if (sel.is_array())
      targets.assign(sel.begin(), sel.end());
    else
      targets.push_back(sel);
    if (targets.empty())
      throw std::runtime_error("part must name at least one tab");
    // Resolve EVERY tab before moving any: a half-applied move over a typo
    // in the tenth name is a mess to unpick by hand.
    std::vector<size_t> slots;
    for (const json& one : targets)
      slots.push_back(ResolveSlotIndex(state, one));
    json moved = json::array();
    for (size_t slot : slots) {
      ui::MoveToFolder(state, slot, folder);
      moved.push_back({{"index", slot},
                       {"name", state.parts[slot].name},
                       {"id", state.parts[slot].id}});
    }
    return json{
        {"folder", folder}, {"moved", moved}, {"folders", FolderList(state)}};
  }
  if (m == "rename_folder") {
    const std::string from = p.at("folder").get<std::string>();
    RequireFolder(state, from, "folder");
    const std::string to = p.at("new_name").get<std::string>();
    if (to.empty())
      throw std::runtime_error("new_name must be a non-empty folder name");
    // Renaming onto an existing folder merges the two. That is the GUI's
    // behavior and a reasonable one, but it is not what "rename" implies —
    // so say which of the two happened.
    const bool merged = to != from && FolderExists(state, to);
    ui::RenameFolder(state, from, to);
    return json{{"folder", to},
                {"renamed_from", from},
                {"merged", merged},
                {"folders", FolderList(state)}};
  }
  if (m == "dissolve_folder") {
    const std::string folder = p.at("folder").get<std::string>();
    RequireFolder(state, folder, "folder");
    json freed = json::array();
    for (size_t i = 0; i < state.parts.size(); i++)
      if (state.parts[i].folder == folder)
        freed.push_back({{"index", i},
                         {"name", state.parts[i].name},
                         {"id", state.parts[i].id}});
    ui::DissolveFolder(state, folder);
    return json{{"folder", folder},
                {"moved_to_top_level", freed},
                {"folders", FolderList(state)}};
  }
  if (m == "import_step") {
    units::Length unit = units::Length::Millimetre;
    if (p.contains("units")) {
      if (p.value("as_assembly", false))
        throw std::runtime_error("units: an assembly import reads STEP, which "
                                 "declares its own unit");
      const std::string u = p.at("units").get<std::string>();
      if (!units::LengthFromString(u, unit)) {
        std::string accepted;
        for (const char* n : units::LengthNames())
          accepted += (accepted.empty() ? "" : ", ") + std::string(n);
        throw std::runtime_error("unknown units '" + u +
                                 "'. Accepted: " + accepted);
      }
    }
    if (p.value("as_assembly", false)) {
      // Product structure → part tabs + an assembly (interchange I2).
      const size_t tabsBefore = state.parts.size();
      std::string error;
      if (!ui::ImportStepAsAssembly(state, p.at("path").get<std::string>(),
                                    &error)) {
        throw std::runtime_error(error);
      }
      json occs = json::array();
      for (const Occurrence& o : state.assembly->GetOccurrences())
        occs.push_back({{"id", o.id}, {"name", o.name}});
      // What did NOT come through. A STEP whose products include shapes
      // OpenCASCADE will not rebuild leaves those part tabs empty and their
      // occurrences drawing nothing, and this was visible only by opening
      // each of 97 tabs: 12 were broken on the SO-101 arm and the response
      // said the import had succeeded (the regen-honesty rule, for the one
      // mutating RPC that was not keeping it).
      json failed = json::array();
      size_t partTabs = 0;
      for (size_t i = tabsBefore; i < state.parts.size(); i++) {
        const Document* part = ui::PartDocById(state, state.parts[i].id);
        if (part == nullptr)
          continue; // the assembly tab the import also creates
        partTabs++;
        for (const auto& f : part->GetFeatures())
          if (!f->GetError().empty())
            failed.push_back({{"part", state.parts[i].name},
                              {"feature", f->GetName()},
                              {"error", f->GetError()}});
      }
      json out{{"assembly_index", state.activePart},
               {"occurrences", occs},
               {"parts", partTabs},
               {"ok", failed.empty()}};
      if (!failed.empty())
        out["failed"] = failed;
      return out;
    }
    const size_t before = doc->GetBodies().size();
    const json preDigest = BodyDigestJson(*doc);
    std::string error;
    if (!ui::ImportAsFeature(state, p.at("path").get<std::string>(), nullptr,
                             &error, unit, p.value("keep_origin", false)))
      throw std::runtime_error(error);
    json out = {{"bodies", doc->GetBodies().size() - before},
                {"regen", RegenSummaryJson(*doc)},
                {"digest", DigestJson(preDigest, *doc)}};
    // Centred unless told otherwise: say by how much, as add_feature does for
    // an Import, so a caller who expected file coordinates is not left to
    // find the move by measuring.
    if (doc->FeatureCount() > 0)
      if (const auto* imp = dynamic_cast<const ImportFeature*>(
              doc->GetFeature(doc->FeatureCount() - 1));
          imp != nullptr && imp->Shift() != glm::dvec3(0.0))
        out["recentred_by"] =
            json::array({imp->Shift().x, imp->Shift().y, imp->Shift().z});
    return out;
  }
  return std::nullopt;
}

} // namespace remote
