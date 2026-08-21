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
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
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
    {"save_project", "Save the project to {path}."},
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
};
std::span<const MethodDoc> ProjectMethodDocs() { return ProjectMethods; }

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
    // params: {path, message?} — message becomes the git commit subject for
    // directory (.dsgn) projects (F5); ignored otherwise.
    const std::string path = p.at("path").get<std::string>();
    std::string error;
    if (!ui::SaveProject(state, path, &error)) // multi-part aware (A1)
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
                          {"ref", GitVault::TagRef(e.subject)},
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
      const std::string ref = GitVault::TagRef(name);
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
