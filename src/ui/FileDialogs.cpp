#include "ui/FileDialogs.h"

#include "Logger.h"
#include "CADKernel.h"
#include "core/io/Autosaver.h"
#include "core/io/Serialization.h"
#include "ui/AppActions.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef HAVE_NATIVEFILEDIALOG
#include <nfd.h>
#endif

namespace ui {

#ifdef HAVE_NATIVEFILEDIALOG

void OpenStepFileDialog(AppState& state) {
  nfdchar_t* outPath = nullptr;
  const nfdchar_t* filterList = "step,STEP,stp,iges,igs";

  nfdresult_t result = NFD_OpenDialog(filterList, nullptr, &outPath);
  if (result == NFD_OKAY) {
    std::string filePath(outPath);
    free(outPath);
    LOG_INFO("Selected file: {}", filePath);
    LoadSTEPFile(state, filePath);
  } else if (result == NFD_CANCEL) {
    LOG_DEBUG("User cancelled file dialog");
  } else {
    LOG_ERROR("Error opening file dialog: {}", NFD_GetError());
  }
}

void OpenStepAssemblyFileDialog(AppState& state) {
  nfdchar_t* outPath = nullptr;
  if (NFD_OpenDialog("step,STEP,stp", nullptr, &outPath) == NFD_OKAY) {
    std::string filePath(outPath);
    free(outPath);
    std::string error;
    if (!ImportStepAsAssembly(state, filePath, &error))
      LOG_ERROR("Assembly import failed: {}", error);
  }
}

namespace {

// Load `path` (a project or a recovery sidecar) into the app.
void OpenProjectFile(AppState& state, const std::string& path) {
  std::string error;
  if (auto doc = DocumentIO::Load(path, &error)) {
    ReplaceDocument(state, std::move(doc));
    FitCameraToDocument(state);
  } else {
    LOG_ERROR("Failed to open project: {}", error);
  }
}

} // namespace

void OpenProjectDialog(AppState& state) {
  nfdchar_t* outPath = nullptr;
  if (NFD_OpenDialog("json,dsgn", nullptr, &outPath) == NFD_OKAY) {
    std::string path(outPath);
    free(outPath);
    if (Autosaver::HasNewerAutosave(path)) {
      // Defer to the recovery modal (DrawFileBrowser) — the user picks
      // between the saved file and the newer autosave.
      state.recoveryOffer = path;
      return;
    }
    OpenProjectFile(state, path);
    MarkProjectOpened(state, path);
  }
}

void SaveProjectDialog(AppState& state) {
  if (!state.document)
    return;
  nfdchar_t* outPath = nullptr;
  if (NFD_SaveDialog("dsgn,json", nullptr, &outPath) == NFD_OKAY) {
    std::string path(outPath);
    free(outPath);
    if (std::filesystem::path(path).extension().empty()) {
      path += ".dsgn"; // directory format is the default for new saves (F3)
    }
    std::string error;
    if (!DocumentIO::Save(*state.document, path, &error)) {
      LOG_ERROR("Failed to save project: {}", error);
    } else {
      MarkProjectSaved(state, path);
    }
  }
}

void ExportDialog(AppState& state) {
  // A drawing tab exports to vector sheet formats (SVG / PDF), not the mesh /
  // B-Rep writers below.
  if (state.drawing) {
    if (state.drawing->GetViews().empty())
      return;  // nothing projected to export yet
    nfdchar_t* out = nullptr;
    if (NFD_SaveDialog("svg,pdf", nullptr, &out) != NFD_OKAY)
      return;
    std::string path(out);
    free(out);
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".svg" && ext != ".pdf") path += ".pdf";  // default
    std::string error;
    if (ExportActiveDrawing(state, path, &error))
      LOG_INFO("Exported drawing to {}", path);
    else
      LOG_ERROR("Drawing export failed: {}", error);
    return;
  }

  const bool haveAssembly =
      state.assembly && state.assembly->OccurrenceCount() > 0;
  const bool havePart =
      state.document && !state.document->GetBodies().empty();
  if (!haveAssembly && !havePart)
    return;
  // Assemblies export only through the visual (tessellation) writers; a part
  // additionally offers the B-Rep CAD formats.
  const char* filter = haveAssembly ? "glb,gltf,obj,ply"
                                     : "step,stp,stl,brep,glb,gltf,obj,ply";
  nfdchar_t* outPath = nullptr;
  if (NFD_SaveDialog(filter, nullptr, &outPath) != NFD_OKAY)
    return;
  std::string path(outPath);
  free(outPath);
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext.empty()) {
    ext = haveAssembly ? ".glb" : ".step";
    path += ext;
  }

  // Visual mesh formats (glTF/OBJ/PLY): tessellation of the part or the whole
  // assembly, materials carried through. Works on either tab.
  if (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".ply") {
    const std::vector<MeshExport::Item> items = BuildVisualExportItems(state);
    std::string error;
    if (MeshExport::SaveVisual(path, items, 0.1, &error))
      LOG_INFO("Exported {} item(s) to {}", items.size(), path);
    else
      LOG_ERROR("Export failed: {}", error);
    return;
  }

  // B-Rep CAD formats (part only).
  if (!havePart) {
    LOG_WARN("STEP/STL/BREP export needs a part tab; use glTF/OBJ/PLY for an "
             "assembly");
    return;
  }
  auto& kernel = CADKernel::Instance();
  const auto& bodies = state.document->GetBodies();
  bool ok = false;
  if (ext == ".stl" || ext == ".brep") {
    // Single-shape formats: fuse the document into one solid.
    CADShape shape = bodies[0].shape;
    for (size_t i = 1; i < bodies.size(); i++)
      shape = shape.Union(bodies[i].shape);
    ok = ext == ".stl" ? kernel.SaveSTL(path, shape)
                       : kernel.SaveBREP(path, shape);
  } else {
    std::vector<CADShape> shapes;
    for (const auto& b : bodies)
      shapes.push_back(b.shape);
    ok = kernel.SaveSTEP(path, shapes);
  }
  if (ok)
    LOG_INFO("Exported {} bodies to {}", bodies.size(), path);
  else
    LOG_ERROR("Export failed: {}", path);
}

void ExportBomDialog(AppState& state) {
  if (!state.assembly)
    return;
  nfdchar_t* outPath = nullptr;
  if (NFD_SaveDialog("csv", nullptr, &outPath) != NFD_OKAY)
    return;
  std::string path(outPath);
  free(outPath);
  if (!path.ends_with(".csv"))
    path += ".csv";
  std::ofstream out(path);
  if (!out) {
    LOG_ERROR("BOM export failed: cannot write {}", path);
    return;
  }
  out << BomToCsv(ComputeBom(state));
  LOG_INFO("BOM exported to {}", path);
}

void DrawFileBrowser(AppState& state) {
  // Crash-recovery prompt: OpenProjectDialog found an autosave newer than
  // the picked project file.
  if (state.recoveryOffer.empty())
    return;
  if (!ImGui::IsPopupOpen("Recover autosave?"))
    ImGui::OpenPopup("Recover autosave?");
  if (ImGui::BeginPopupModal("Recover autosave?", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextWrapped(
        "An autosave newer than\n%s\nexists (probably from a crash). "
        "Recover the autosaved changes?",
        state.recoveryOffer.c_str());
    if (ImGui::Button("Recover autosave")) {
      OpenProjectFile(state, Autosaver::SidecarPath(state.recoveryOffer));
      MarkProjectRecovered(state, state.recoveryOffer);
      state.recoveryOffer.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open saved file")) {
      OpenProjectFile(state, state.recoveryOffer);
      MarkProjectOpened(state, state.recoveryOffer);
      Autosaver::Clear(state.recoveryOffer); // explicit discard
      state.recoveryOffer.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

#else // fallback ImGui browser

static void UpdateDirectoryContents(AppState& state) {
  state.directoryContents.clear();

  try {
    if (std::filesystem::exists(state.currentPath) &&
        std::filesystem::is_directory(state.currentPath)) {
      if (state.currentPath != "." && state.currentPath != "/" &&
          state.currentPath.find("..") == std::string::npos) {
        state.directoryContents.push_back("..");
      }

      for (const auto& entry :
           std::filesystem::directory_iterator(state.currentPath)) {
        std::string filename = entry.path().filename().string();

        if (entry.is_directory()) {
          state.directoryContents.push_back("[DIR] " + filename);
        } else {
          std::string ext = entry.path().extension().string();
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

          if (ext == ".step" || ext == ".stp" || ext == ".iges" ||
              ext == ".igs") {
            state.directoryContents.push_back("[CAD] " + filename);
          } else {
            state.directoryContents.push_back(filename);
          }
        }
      }
    }
  } catch (const std::exception& e) {
    LOG_ERROR("Error reading directory '{}': {}", state.currentPath, e.what());
    state.directoryContents.push_back("Error reading directory");
  }

  std::sort(state.directoryContents.begin(), state.directoryContents.end());
}

void OpenStepFileDialog(AppState& state) {
  state.showFileBrowser = true;
  UpdateDirectoryContents(state);
}

void OpenStepAssemblyFileDialog(AppState&) {
  LOG_WARN("Import as Assembly requires nativefiledialog");
}

void OpenProjectDialog(AppState&) {
  LOG_WARN("Open Project requires nativefiledialog");
}

void SaveProjectDialog(AppState&) {
  LOG_WARN("Save Project requires nativefiledialog");
}

void ExportDialog(AppState&) {
  LOG_WARN("Export requires nativefiledialog");
}

void ExportBomDialog(AppState&) {
  LOG_WARN("BOM export requires nativefiledialog");
}

void DrawFileBrowser(AppState& state) {
  if (!state.showFileBrowser)
    return;

  ImGui::Begin("Open STEP File", &state.showFileBrowser,
               ImGuiWindowFlags_AlwaysAutoResize);

  ImGui::Text("Current Path: %s", state.currentPath.c_str());
  ImGui::Separator();

  if (ImGui::Button("Refresh")) {
    UpdateDirectoryContents(state);
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    state.showFileBrowser = false;
  }

  ImGui::Separator();

  if (ImGui::BeginListBox("##files", ImVec2(400, 300))) {
    for (const auto& item : state.directoryContents) {
      if (ImGui::Selectable(item.c_str(), false,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          if (item == "..") {
            try {
              std::filesystem::path parentPath =
                  std::filesystem::path(state.currentPath).parent_path();
              state.currentPath =
                  parentPath.empty()
                      ? "."
                      : std::filesystem::weakly_canonical(parentPath).string();
              UpdateDirectoryContents(state);
            } catch (const std::exception& e) {
              LOG_ERROR("Error navigating to parent directory: {}", e.what());
            }
          } else if (item.starts_with("[DIR]")) {
            try {
              std::filesystem::path newPath =
                  std::filesystem::path(state.currentPath) / item.substr(6);
              state.currentPath =
                  std::filesystem::weakly_canonical(newPath).string();
              UpdateDirectoryContents(state);
            } catch (const std::exception& e) {
              LOG_ERROR("Error entering directory: {}", e.what());
            }
          } else if (item.starts_with("[CAD]")) {
            std::string fullPath =
                (std::filesystem::path(state.currentPath) / item.substr(6))
                    .string();
            LOG_INFO("Loading selected file: {}", fullPath);
            LoadSTEPFile(state, fullPath);
            state.showFileBrowser = false;
          }
        }
      }
    }
    ImGui::EndListBox();
  }

  ImGui::Text("Double-click directories to enter, CAD files to load");

  ImGui::End();
}

#endif

} // namespace ui
