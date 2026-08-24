#include "remote/RemoteProject.h"

#include "cad/CADKernel.h"
#include "cad/MeshExport.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/ConflictsPanel.h"
#include "ui/HistoryPanel.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Project IO / checkpoint / history / export-import RPCs (new_project..) carved
// out of RemoteServer::HandleSemantic. `doc` is the active part document (may
// be null on an assembly tab, matching the original un-guarded behavior).
// describe: one row per `m ==` branch below — extend together.
static constexpr MethodDoc ProjectMethods[] = {
    {"new_project", "Reset to an empty project (one empty part)."},
    {"open_project", "Open a project {path} (.json file or .dsgn directory)."},
    {"save_project",
     "Save the project to {path, message?, force?}. Refuses when the project "
     "changed on disk since it was opened; {force:true} saves over it."},
    {"import_step", "Import a STEP file into the current project (XCAF: names, "
                    "colors, real assemblies)."},
    {"export_step", "Export the active part (or {part}) to STEP {path}."},
    {"export_stl", "Export to STL {path, deflection?}."},
    {"export_brep", "Export to BREP {path}."},
    {"export_visual", "Export a visual-quality mesh (glTF/OBJ by extension) of "
                      "the part or whole assembly."},
    {"checkpoint_create",
     "Name the current state of the whole project a version {name}."},
    {"checkpoint_list", "List named versions."},
    {"checkpoint_restore", "Put the project back to a named version {name}."},
    {"history_log", "List the project's versions (every save)."},
    {"history_restore", "Put the project back to a version {commit}."},
    {"branch_list", "List separate lines of work, and which one is current."},
    {"branch_create", "Start a separate line of work {name, switch?:true}."},
    {"branch_switch", "Move to a separate line of work {name}."},
    {"branch_merge", "Bring {name} into the current line of work. A "
                     "'conflicted' status is an outcome, not an error."},
    {"conflict_list", "What two merged versions disagree about, in document "
                      "terms — including the duplicate ids git merges "
                      "cleanly and never reports."},
    {"conflict_resolve",
     "Resolve one conflict {path, take: mine|theirs|auto, field?}. 'auto' is "
     "the union merge for an order, a rebuild for derived geometry, and a "
     "renumber for a duplicate id."},
    {"merge_finish", "Verify the merged project LOADS AND REGENERATES, then "
                     "keep it. Refuses with {broken} when it does not."},
    {"merge_abort", "Throw an in-progress merge away."},
    {"set_variable", "Add or redefine a variable {name, expression, comment?, "
                     "scope: project|part}; returns value + regen."},
    {"list_variables", "Project variables and the active part's local ones."},
    {"rename_variable", "Rename a variable {name, new_name, scope?}, rewriting "
                        "every referencing expression."},
    {"delete_variable", "Delete a variable {name, scope?}; refused while "
                        "referenced."},
    {"remote_list", "The project's remotes, and how far ahead/behind this "
                    "branch is."},
    {"remote_add", "Give the project a remote {name?:origin, url}. A shared "
                   "project stops committing built geometry."},
    {"remote_remove", "Forget a remote {name}."},
    {"fetch", "Bring down refs from {remote?:origin} without touching the "
              "working tree."},
    {"pull", "Fetch {remote?:origin} and merge it in. A 'conflicted' status "
             "is an outcome — see conflict_list."},
    {"push", "Send this branch to {remote?:origin}. 'rejected' means someone "
             "else pushed first; pull, then push again."},
    {"clone_project", "Clone {url} into {path} and open it."},
    {"autosave_now", "Autosave every dirty document immediately, ignoring "
                     "the interval; returns how many were written."},
    {"autosave_status", "Which documents have autosaved work waiting to be "
                        "recovered."},
    {"project_changed_on_disk",
     "Has anything else written to the project since it was opened or last "
     "saved? (a terminal command, another editor, a second instance)"},
    {"create_folder",
     "Create a bottom-tab-bar folder {folder}, empty. Display grouping only "
     "— ids, references and storage are unaffected. Idempotent."},
    {"move_to_folder",
     "Put tabs in a folder: {part, folder}. `part` is a name/id/index or an "
     "ARRAY of them (parts, assemblies and drawings alike); folder \"\" "
     "moves them back to the top level. The folder must already exist, so a "
     "typo is an error instead of a new folder."},
    {"rename_folder", "Rename folder {folder} to {new_name}, members and "
                      "all. Renaming onto an existing folder MERGES them "
                      "(reported as merged:true)."},
    {"dissolve_folder",
     "Remove folder {folder}; its tabs return to the top level. Documents "
     "are never deleted with their folder — see delete_part for that."},
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

std::optional<json> HandleProjectRpc(ui::AppState& state, const std::string& m,
                                     const RpcParams& p) {
  Document* doc = state.document.get();
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
    std::string error;
    if (!ui::SaveProject(state, path, &error, p.value("force", false)))
      throw std::runtime_error(error);
    // Also clears the autosave sidecar and commits git-backed projects.
    ui::MarkProjectSaved(state, path, p.value("message", "save"));
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
    return json{{"autosave_available", autosaveAvailable},
                {"autosave_path", Autosaver::SidecarPath(path)}};
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
    return json::object();
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
    return json::object();
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
      return json::object();
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
    return json::object();
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
    if (scope == "part") {
      if (!state.commands->Execute(
              std::make_unique<RenameVariableCommand>(name, newName), *doc))
        throw std::runtime_error(
            "rename failed: '" + name + "' -> '" + newName +
            "'. Either the source does not exist, the new name is not a "
            "valid identifier, or it is already taken — list_variables "
            "shows what exists in each scope");
      return json::object();
    }
    if (!ui::RenameProjectVariable(state, name, newName))
      throw std::runtime_error(
          "rename failed: '" + name + "' -> '" + newName +
          "'. Either the source does not exist, the new name is not a "
          "valid identifier, or it is already taken — list_variables "
          "shows what exists in each scope");
    return json::object();
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
    if (status != GitVault::MergeStatus::Conflicted &&
        status != GitVault::MergeStatus::UpToDate) {
      std::string openError;
      if (!ui::OpenProject(state, state.projectPath, &openError))
        throw std::runtime_error(openError);
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
    return json{{"path", path}};
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
    // {path, body?: index, deflection?: 0.1 (STL only)}
    const std::string path = p.at("path").get<std::string>();
    const auto& bodies = doc->GetBodies();
    if (bodies.empty())
      throw std::runtime_error("no bodies to export");
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");

    auto& kernel = CADKernel::Instance();
    bool ok = false;
    if (m == "export_step") {
      std::vector<CADShape> shapes;
      for (size_t i = 0; i < bodies.size(); i++)
        if (only < 0 || i == static_cast<size_t>(only))
          shapes.push_back(bodies[i].shape);
      ok = kernel.SaveSTEP(path, shapes);
    } else {
      // STL/BREP are single-shape; a whole-document export of several bodies
      // fuses them (Onshape behavior) so one file holds the whole part.
      CADShape shape;
      if (only >= 0) {
        shape = bodies[only].shape;
      } else {
        shape = bodies[0].shape;
        for (size_t i = 1; i < bodies.size(); i++)
          shape = shape.Union(bodies[i].shape);
      }
      ok = m == "export_stl"
               ? kernel.SaveSTL(path, shape, p.value("deflection", 0.1))
               : kernel.SaveBREP(path, shape);
      if (ok && m == "export_stl") {
        // Watertightness report on the SOURCE B-Rep (not the mesh): a
        // non-watertight input still exports — the report tells the caller
        // whether the STL can be trusted for printing/slicing.
        const Measure::ShellReport r = Measure::AnalyzeWatertight(shape);
        return json{{"path", path},
                    {"bodies", only < 0 ? bodies.size() : size_t{1}},
                    {"watertight", r.watertight},
                    {"shells", r.shells},
                    {"free_edges", r.freeEdges}};
      }
    }
    if (!ok)
      throw std::runtime_error("export failed (see log)");
    return json{{"path", path},
                {"bodies", only < 0 ? bodies.size() : size_t{1}}};
  }
  if (m == "export_visual") {
    // Visual-mesh export (interchange X2): glTF (.glb/.gltf), OBJ, or PLY,
    // chosen by the path extension. Works on a part (bodies at identity) OR
    // the active assembly (occurrences at solved placements). Materials carry
    // through (glTF is PBR). params: {path, deflection?: 0.1}
    const std::string path = p.at("path").get<std::string>();
    const std::vector<MeshExport::Item> items =
        ui::BuildVisualExportItems(state);
    if (items.empty())
      throw std::runtime_error("nothing to export");
    std::string err;
    if (!MeshExport::SaveVisual(path, items, p.value("deflection", 0.1), &err))
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
    if (p.value("as_assembly", false)) {
      // Product structure → part tabs + an assembly (interchange I2).
      std::string error;
      if (!ui::ImportStepAsAssembly(state, p.at("path").get<std::string>(),
                                    &error)) {
        throw std::runtime_error(error);
      }
      json occs = json::array();
      for (const Occurrence& o : state.assembly->GetOccurrences())
        occs.push_back({{"id", o.id}, {"name", o.name}});
      return json{{"assembly_index", state.activePart}, {"occurrences", occs}};
    }
    const size_t before = doc->GetBodies().size();
    std::string error;
    if (!ui::ImportAsFeature(state, p.at("path").get<std::string>(), &error))
      throw std::runtime_error(error);
    return json{{"bodies", doc->GetBodies().size() - before}};
  }
  return std::nullopt;
}

} // namespace remote
