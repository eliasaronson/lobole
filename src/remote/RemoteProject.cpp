#include "remote/RemoteProject.h"

#include "cad/CADKernel.h"
#include "cad/MeshExport.h"
#include "core/io/Autosaver.h"
#include "core/io/Checkpoints.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/io/GitVault.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Project IO / checkpoint / history / export-import RPCs (new_project..) carved
// out of RemoteServer::HandleSemantic. `doc` is the active part document (may be
// null on an assembly tab, matching the original un-guarded behavior).
std::optional<json> HandleProjectRpc(ui::AppState& state,
                                     const std::string& m, const RpcParams& p) {
  Document* doc = state.document.get();
  if (m == "new_project") {
    ui::ReplaceDocument(state, std::make_unique<Document>());
    state.parts.clear(); // reset to a single fresh part (A1)
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
    // Snapshot the live document under <project>.checkpoints/<name>.json.
    // Needs a saved project (the checkpoints live beside it). params: {name}
    const std::string name = p.at("name").get<std::string>();
    std::string error;
    if (!Checkpoints::Create(*doc, state.projectPath, name, &error))
      throw std::runtime_error(error);
    // Git-backed projects (F5): a checkpoint is also a commit + tag of the
    // live state.
    if (state.projectPath.ends_with(".dsgn") && GitVault::Available()) {
      if (DocumentIO::Save(*doc, state.projectPath)) {
        ui::MarkProjectSaved(state, state.projectPath, "checkpoint: " + name);
        GitVault::Tag(state.projectPath, "cp/" + name);
      }
    }
    return json{{"path", Checkpoints::PathOf(state.projectPath, name)}};
  }
  if (m == "checkpoint_list") {
    json list = json::array();
    for (const auto& info : Checkpoints::List(state.projectPath))
      list.push_back(
          {{"name", info.name}, {"modified_unix", info.modifiedUnix}});
    return json{{"checkpoints", list}};
  }
  if (m == "checkpoint_restore") {
    // Load the checkpoint as the current document: same project path,
    // deliberately dirty (the restored state is unsaved until saved).
    const std::string name = p.at("name").get<std::string>();
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
    for (const auto& e :
         GitVault::Log(state.projectPath, p.value("limit", 20)))
      entries.push_back({{"hash", e.hash},
                         {"subject", e.subject},
                         {"unix", e.unixTime}});
    return json{{"log", entries}};
  }
  if (m == "history_restore") {
    // Check out a commit's tree over the project and reload it; history
    // stays linear (the restored state commits on the next save).
    // params: {commit} (hash, tag like "cp/v1", or any git rev)
    const std::string commit = p.at("commit").get<std::string>();
    std::string error;
    if (!GitVault::RestoreTo(state.projectPath, commit, &error))
      throw std::runtime_error(error);
    auto loaded = DocumentIO::Load(state.projectPath);
    if (!loaded) throw std::runtime_error(loaded.error());
    ui::ReplaceDocument(state, std::move(*loaded));
    ui::FitCameraToDocument(state);
    ui::MarkProjectRecovered(state, state.projectPath);
    return json::object();
  }
  if (m == "export_step" || m == "export_stl" || m == "export_brep") {
    // Export the whole document (all bodies) or one body (by index) to
    // STEP / binary STL / native BREP. params:
    // {path, body?: index, deflection?: 0.1 (STL only)}
    const std::string path = p.at("path").get<std::string>();
    const auto& bodies = doc->GetBodies();
    if (bodies.empty()) throw std::runtime_error("no bodies to export");
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
    if (!ok) throw std::runtime_error("export failed (see log)");
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
      return json{{"assembly_index", state.activePart},
                  {"occurrences", occs}};
    }
    const size_t before = doc->GetBodies().size();
    ui::LoadSTEPFile(state, p.at("path").get<std::string>());
    if (doc->GetBodies().size() <= before)
      throw std::runtime_error("import produced no bodies");
    return json{{"bodies", doc->GetBodies().size() - before}};
  }
  return std::nullopt;
}

}  // namespace remote
