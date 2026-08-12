#include "ui/FileDialogs.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
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

namespace {

// ---------------------------------------------------------------------------
// Shared pieces, compiled in every build: the recovery modal and the ImGui
// fallback browser. The NFD build ALSO needs the browser — on a system
// without GSettings schemas (bare NixOS env), GLib calls abort() the moment
// a GTK file chooser opens, so native dialogs must be probed before use.
// ---------------------------------------------------------------------------

// Load `path` (a project or a recovery sidecar) into the app.
void OpenProjectFile(AppState& state, const std::string& path) {
  if (auto loaded = DocumentIO::Load(path)) {
    ReplaceDocument(state, std::move(*loaded));
    FitCameraToDocument(state);
  } else {
    LOG_ERROR("Failed to open project: {}", loaded.error());
  }
}

// A FILE picker can't select a directory-form project (a folder holding
// project.json), so users pick the project.json inside it instead — map
// that back to the project root the loaders expect.
std::string NormalizeProjectPick(const std::string& path) {
  std::filesystem::path p(path);
  if (p.filename() == "project.json")
    return p.parent_path().string();
  return path;
}

// Container-aware GUI open: ui::OpenProject handles both project forms
// (directory with project.json, flat single-document file) — unlike
// OpenProjectFile above, which predates multi-document projects and is now
// only right for recovery sidecars (always flat).
void OpenPickedProject(AppState& state, const std::string& path) {
  std::string error;
  if (!OpenProject(state, path, &error)) {
    LOG_ERROR("Failed to open project '{}': {}", path, error);
    return;
  }
  MarkProjectOpened(state, path);
}

void UpdateDirectoryContents(AppState& state) {
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

void OpenBrowserFallback(AppState& state, bool assemblyMode) {
  state.fileBrowserAssemblyMode = assemblyMode;
  state.showFileBrowser = true;
  if (state.currentPath.empty() ||
      !std::filesystem::is_directory(state.currentPath)) {
    state.currentPath = std::filesystem::current_path().string();
  }
  UpdateDirectoryContents(state);
}

// Crash-recovery prompt: OpenProjectDialog found an autosave newer than the
// picked project file.
void DrawRecoveryModal(AppState& state) {
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
      OpenPickedProject(state, state.recoveryOffer);
      Autosaver::Clear(state.recoveryOffer); // explicit discard
      state.recoveryOffer.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

void DrawBrowserWindow(AppState& state) {
  if (!state.showFileBrowser)
    return;

  const char* title = state.fileBrowserAssemblyMode ? "Import STEP as Assembly"
                                                    : "Open STEP File";
  ImGui::Begin(title, &state.showFileBrowser,
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
            if (state.fileBrowserAssemblyMode) {
              std::string error;
              if (!ImportStepAsAssembly(state, fullPath, &error))
                LOG_ERROR("Assembly import failed: {}", error);
            } else {
              LoadSTEPFile(state, fullPath);
            }
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

#ifdef HAVE_NATIVEFILEDIALOG
// Whether native dialogs are usable. nativefiledialog-extended talks to the
// XDG desktop portal over D-Bus on Linux (out-of-process — the old
// in-process GTK chooser aborted the app on bare environments); NFD_Init
// failing means no usable backend, so callers fall back to the ImGui
// browser instead.
bool NativeDialogsSafe() {
  static const bool ok = [] {
    if (NFD_Init() == NFD_OKAY)
      return true;
    LOG_WARN("Native file dialogs unavailable ({}); using the built-in "
             "browser where possible.",
             NFD_GetError() ? NFD_GetError() : "no backend");
    return false;
  }();
  return ok;
}
#endif

} // namespace

void DrawFileBrowser(AppState& state) {
  DrawRecoveryModal(state);
  DrawBrowserWindow(state);
}

#ifdef HAVE_NATIVEFILEDIALOG

void OpenStepFileDialog(AppState& state) {
  if (!NativeDialogsSafe()) {
    OpenBrowserFallback(state, /*assemblyMode=*/false);
    return;
  }
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"CAD files", "step,stp,iges,igs"}};

  nfdresult_t result = NFD_OpenDialogU8(&outPath, filters, 1, nullptr);
  if (result == NFD_OKAY) {
    std::string filePath(outPath);
    NFD_FreePathU8(outPath);
    LOG_INFO("Selected file: {}", filePath);
    LoadSTEPFile(state, filePath);
  } else if (result == NFD_CANCEL) {
    LOG_DEBUG("User cancelled file dialog");
  } else {
    LOG_ERROR("Error opening file dialog: {}", NFD_GetError());
    OpenBrowserFallback(state, /*assemblyMode=*/false);
  }
}

void OpenReferenceImageDialog(AppState& state) {
  if (!NativeDialogsSafe()) {
    state.sketchEdit.actionMsg =
        "Native file dialog unavailable; cannot pick an image.";
    return;
  }
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"Images", "png,jpg,jpeg,bmp,tga,gif"}};
  nfdresult_t result = NFD_OpenDialogU8(&outPath, filters, 1, nullptr);
  if (result == NFD_OKAY) {
    std::string filePath(outPath);
    NFD_FreePathU8(outPath);
    LOG_INFO("Reference image selected: {}", filePath);
    AddReferenceImageToSketch(state, filePath);
  } else if (result == NFD_CANCEL) {
    LOG_DEBUG("User cancelled reference-image dialog");
  } else {
    LOG_ERROR("Error opening reference-image dialog: {}", NFD_GetError());
  }
}

void OpenStepAssemblyFileDialog(AppState& state) {
  if (!NativeDialogsSafe()) {
    OpenBrowserFallback(state, /*assemblyMode=*/true);
    return;
  }
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"STEP", "step,stp"}};
  if (NFD_OpenDialogU8(&outPath, filters, 1, nullptr) == NFD_OKAY) {
    std::string filePath(outPath);
    NFD_FreePathU8(outPath);
    std::string error;
    if (!ImportStepAsAssembly(state, filePath, &error))
      LOG_ERROR("Assembly import failed: {}", error);
  }
}

void OpenProjectDialog(AppState& state) {
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"Designer project", "dsgn,json"}};
  if (NFD_OpenDialogU8(&outPath, filters, 1, nullptr) == NFD_OKAY) {
    const std::string path = NormalizeProjectPick(outPath);
    NFD_FreePathU8(outPath);
    if (Autosaver::HasNewerAutosave(path)) {
      // Defer to the recovery modal (DrawFileBrowser) — the user picks
      // between the saved file and the newer autosave.
      state.recoveryOffer = path;
      return;
    }
    OpenPickedProject(state, path);
  }
}

void OpenProjectFolderDialog(AppState& state) {
  // Directory-form projects (.dsgn / any folder with a project.json) can't
  // be selected in the FILE dialog above — this folder picker can.
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  if (NFD_PickFolderU8(&outPath, nullptr) == NFD_OKAY) {
    const std::string path(outPath);
    NFD_FreePathU8(outPath);
    if (Autosaver::HasNewerAutosave(path)) {
      state.recoveryOffer = path;
      return;
    }
    OpenPickedProject(state, path);
  }
}

void SaveProjectDialog(AppState& state) {
  if (!state.document)
    return;
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"Designer project", "dsgn,json"}};
  if (NFD_SaveDialogU8(&outPath, filters, 1, nullptr, "project.dsgn") ==
      NFD_OKAY) {
    std::string path(outPath);
    NFD_FreePathU8(outPath);
    if (std::filesystem::path(path).extension().empty()) {
      path += ".dsgn"; // directory format is the default for new saves (F3)
    }
    // SaveProject, not DocumentIO::Save: a multi-document project writes the
    // container form; a lone part still lands in the flat form.
    std::string error;
    if (!SaveProject(state, path, &error)) {
      LOG_ERROR("Failed to save project: {}", error);
    } else {
      MarkProjectSaved(state, path);
    }
  }
}

void ExportDialog(AppState& state) {
  if (!NativeDialogsSafe())
    return;
  // A drawing tab exports to vector sheet formats (SVG / PDF), not the mesh /
  // B-Rep writers below.
  if (state.drawing) {
    if (state.drawing->GetViews().empty())
      return; // nothing projected to export yet
    nfdu8char_t* out = nullptr;
    const nfdu8filteritem_t sheetFilters[] = {{"SVG", "svg"}, {"PDF", "pdf"}};
    if (NFD_SaveDialogU8(&out, sheetFilters, 2, nullptr, "sheet.pdf") !=
        NFD_OKAY) {
      return;
    }
    std::string path(out);
    NFD_FreePathU8(out);
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".svg" && ext != ".pdf")
      path += ".pdf"; // default
    std::string error;
    if (ExportActiveDrawing(state, path, &error))
      LOG_INFO("Exported drawing to {}", path);
    else
      LOG_ERROR("Drawing export failed: {}", error);
    return;
  }

  const bool haveAssembly =
      state.assembly && state.assembly->OccurrenceCount() > 0;
  const bool havePart = state.document && !state.document->GetBodies().empty();
  if (!haveAssembly && !havePart)
    return;
  // Assemblies export only through the visual (tessellation) writers; a part
  // additionally offers the B-Rep CAD formats.
  const nfdu8filteritem_t asmFilters[] = {{"Visual mesh", "glb,gltf,obj,ply"}};
  const nfdu8filteritem_t partFilters[] = {
      {"CAD / mesh", "step,stp,stl,brep,glb,gltf,obj,ply"}};
  nfdu8char_t* outPath = nullptr;
  if (NFD_SaveDialogU8(&outPath, haveAssembly ? asmFilters : partFilters, 1,
                       nullptr, haveAssembly ? "assembly.glb" : "part.step") !=
      NFD_OKAY) {
    return;
  }
  std::string path(outPath);
  NFD_FreePathU8(outPath);
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
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"CSV", "csv"}};
  if (NFD_SaveDialogU8(&outPath, filters, 1, nullptr, "bom.csv") != NFD_OKAY)
    return;
  std::string path(outPath);
  NFD_FreePathU8(outPath);
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

#else // no nativefiledialog: the ImGui browser is the only picker

void OpenStepFileDialog(AppState& state) {
  OpenBrowserFallback(state, /*assemblyMode=*/false);
}

void OpenStepAssemblyFileDialog(AppState& state) {
  OpenBrowserFallback(state, /*assemblyMode=*/true);
}

void OpenReferenceImageDialog(AppState& state) {
  state.sketchEdit.actionMsg =
      "Reference-image import needs the native file dialog.";
}

void OpenProjectFolderDialog(AppState&) {
  LOG_WARN("Project open requires nativefiledialog");
}

void OpenProjectDialog(AppState&) {
  LOG_WARN("Open Project requires nativefiledialog");
}

void SaveProjectDialog(AppState&) {
  LOG_WARN("Save Project requires nativefiledialog");
}

void ExportDialog(AppState&) { LOG_WARN("Export requires nativefiledialog"); }

void ExportBomDialog(AppState&) {
  LOG_WARN("BOM export requires nativefiledialog");
}

#endif

void SaveProjectAction(AppState& state) {
  if (!state.document)
    return;
  if (state.projectPath.empty()) {
    SaveProjectDialog(state); // first save: pick a home
    return;
  }
  std::string error;
  if (SaveProject(state, state.projectPath, &error))
    MarkProjectSaved(state, state.projectPath);
  else
    LOG_ERROR("Failed to save project: {}", error);
}

} // namespace ui
