#include "ui/FileDialogs.h"
#include "ui/AssemblyActions.h"
#include "ui/ImportJob.h"
#include "ui/MotionActions.h"
#include "ui/OpenJob.h"
#include "ui/StepExport.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "common/Watchdog.h"
#include "core/io/Autosaver.h"
#include "core/io/Serialization.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/HistoryPanel.h"
#include "ui/ImportActions.h"
#include "ui/ProjectActions.h"
#include "ui/ViewActions.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef HAVE_NATIVEFILEDIALOG
#include <nfd.h>

namespace {
// A native dialog blocks the main thread for as long as the user keeps it
// open. The watchdog names that wait instead of reporting it as a hang with a
// stack; the label covers the dialog call only, not what is done with the
// answer afterwards.
template <class F> auto InDialog(F&& call) {
  const watchdog::Label label("file dialog", /*wantStack=*/false);
  return call();
}
} // namespace
#endif

namespace ui {

namespace {

// ---------------------------------------------------------------------------
// Shared pieces, compiled in every build: the recovery modal and the ImGui
// fallback browser. The NFD build ALSO needs the browser — on a system
// without GSettings schemas (bare NixOS env), GLib calls abort() the moment
// a GTK file chooser opens, so native dialogs must be probed before use.
// ---------------------------------------------------------------------------

// Whether there is anything at all to write.
//
// NOT `!state.document`, which is what Save and Save As guarded on until
// 2026-09-01. A project is more than its active tab, and `state.document` is
// NULL on an assembly or a drawing tab — AddAssembly and AddDrawing null it,
// because those tabs hold a different kind of document. So the guard meant
// Save and Save As did nothing, silently, anywhere except a part tab: you
// could build a whole assembly, press Ctrl+S, and get no dialog, no file and
// no message. Open Project was unaffected, which is why it looked like a
// dialog bug rather than a save bug.
//
// `SaveProject` itself never needed this — it walks the slots and writes every
// document, and works perfectly well from any tab.
bool NothingToSave(const AppState& state) {
  return state.parts.empty() && !state.document && !state.assembly &&
         !state.drawing;
}

// Load `path` (a project or a recovery sidecar) into the app.
void OpenProjectFile(AppState& state, const std::string& path) {
  if (auto loaded = DocumentIO::Load(path)) {
    ReplaceDocument(state, std::move(*loaded));
    FitCameraToDocument(state);
  } else {
    LOG_ERROR("Failed to open project: {}", loaded.error());
  }
}

// Map any pick to the project root the loaders expect: the project.json
// inside a container project, or any file/folder inside a .dsgn, normalizes
// up to the owning project folder.
std::string NormalizeProjectPick(const std::string& path) {
  namespace fs = std::filesystem;
  fs::path p(path);
  if (p.filename() == "project.json")
    p = p.parent_path();
  for (fs::path a = p; !a.empty() && a != a.root_path(); a = a.parent_path())
    if (a.extension() == ".dsgn")
      return a.string();
  return p.string();
}

// Container-aware GUI open: ui::OpenProject handles both project forms
// (directory with project.json, flat single-document file) — unlike
// OpenProjectFile above, which predates multi-document projects and is now
// only right for recovery sidecars (always flat).
// Off the frame, with a progress bar when it is slow (ui/OpenJob.h); the job
// marks the project opened once it has been adopted.
void OpenPickedProject(AppState& state, const std::string& path) {
  std::string error;
  if (!StartOpenProject(state, path, &error)) {
    state.openError = "Could not open " + path + ": " + error;
    LOG_ERROR("{}", state.openError);
  }
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

          if (ext == ".step" || ext == ".stp" || ext == ".stl" ||
              ext == ".brep") {
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

void OpenBrowserFallback(AppState& state) {
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
    const size_t documents =
        Autosaver::RecoverableDocuments(state.recoveryOffer).size();
    if (documents > 0) {
      // A project with several documents autosaves one file each (F10), so
      // say how much is on offer rather than implying a single file.
      ImGui::TextWrapped("%zu document%s in\n%s\nhave unsaved changes from a "
                         "previous session (probably a crash). Recover them?",
                         documents, documents == 1 ? "" : "s",
                         state.recoveryOffer.c_str());
    } else {
      ImGui::TextWrapped(
          "An autosave newer than\n%s\nexists (probably from a crash). "
          "Recover the autosaved changes?",
          state.recoveryOffer.c_str());
    }
    if (ImGui::Button("Recover autosave")) {
      if (documents > 0) {
        // Open the project and lay the autosaved documents over it — NOT
        // open a sidecar as if it were the project, which on a multi-part
        // project would replace it with one part.
        std::string error;
        if (!RecoverProject(state, state.recoveryOffer, &error))
          LOG_ERROR("Could not recover the project: {}", error);
      } else {
        OpenProjectFile(state, Autosaver::SidecarPath(state.recoveryOffer));
        MarkProjectRecovered(state, state.recoveryOffer);
      }
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

// F8: a save was refused because the project moved on disk. The user pressed
// Ctrl+S and something has to happen, so this is modal — and it names what
// each way out costs, because both of them lose something.
void DrawDiskChangeModal(AppState& state) {
  if (!state.diskChangeBlockedSave)
    return;
  if (!ImGui::IsPopupOpen("Project changed on disk"))
    ImGui::OpenPopup("Project changed on disk");
  if (!ImGui::BeginPopupModal("Project changed on disk", nullptr,
                              ImGuiWindowFlags_AlwaysAutoResize))
    return;

  const std::string name =
      std::filesystem::path(state.projectPath).filename().string();
  ImGui::TextWrapped(
      "Something else wrote to %s — a command in the terminal, another "
      "editor, or a second Designer window.",
      name.c_str());
  ImGui::Spacing();
  ImGui::TextWrapped("Saving now would replace it with what is open here.");
  // Only a git-backed project can promise this, so only it says so.
  const bool recoverable = ProjectHasHistory(state);
  if (recoverable) {
    ImGui::TextWrapped("The version on disk is already in your History, so "
                       "that is recoverable.");
  }
  ImGui::Spacing();

  if (ImGui::Button("Reload from disk###disk_reload")) {
    std::string error;
    if (!OpenProject(state, state.projectPath, &error))
      LOG_ERROR("Could not reload the project: {}", error);
    state.diskChangeBlockedSave = false;
    state.diskChanged = false;
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Throw away what is open here and open what is on disk.");
  ImGui::SameLine();
  if (ImGui::Button("Save anyway###disk_overwrite")) {
    std::string error;
    if (SaveProject(state, state.projectPath, &error, /*overwrite=*/true))
      MarkProjectSaved(state, state.projectPath);
    else
      LOG_ERROR("Failed to save project: {}", error);
    state.diskChangeBlockedSave = false;
    ImGui::CloseCurrentPopup();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(recoverable
                          ? "Replace what is on disk. The History keeps it."
                          : "Replace what is on disk. It is not kept.");
  ImGui::SameLine();
  if (ImGui::Button("Cancel###disk_cancel")) {
    // The banner stays: the project HAS moved, and dismissing the save does
    // not un-move it.
    state.diskChangeBlockedSave = false;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

void DrawBrowserWindow(AppState& state) {
  if (!state.showFileBrowser)
    return;

  ImGui::Begin("Import CAD File", &state.showFileBrowser,
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
            std::string error;
            // Background + progress modal (ui/ImportJob.h): a real assembly
            // STEP takes tens of seconds, and doing it inline froze the
            // window for all of them.
            if (!StartImport(state, fullPath, &error))
              LOG_ERROR("Import failed: {}", error);
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
  DrawDiskChangeModal(state);
  DrawBrowserWindow(state);
}

#ifdef HAVE_NATIVEFILEDIALOG
bool FileDialogsAvailable() { return NativeDialogsSafe(); }
#else
bool FileDialogsAvailable() { return false; }
#endif

#ifdef HAVE_NATIVEFILEDIALOG

void ImportFileDialog(AppState& state) {
  if (!NativeDialogsSafe()) {
    OpenBrowserFallback(state);
    return;
  }
  nfdu8char_t* outPath = nullptr;
  // No IGES: ImportFeature reads STEP/BREP/STL only (listing .iges here
  // used to just defer the failure to regeneration).
  const nfdu8filteritem_t filters[] = {{"CAD files", "step,stp,stl,brep"}};

  nfdresult_t result =
      InDialog([&] { return NFD_OpenDialogU8(&outPath, filters, 1, nullptr); });
  if (result == NFD_OKAY) {
    std::string filePath(outPath);
    NFD_FreePathU8(outPath);
    LOG_INFO("Selected file: {}", filePath);
    std::string error;
    if (!StartImport(state, filePath, &error))
      LOG_ERROR("Import failed: {}", error);
  } else if (result == NFD_CANCEL) {
    LOG_DEBUG("User cancelled file dialog");
  } else {
    LOG_ERROR("Error opening file dialog: {}", NFD_GetError());
    OpenBrowserFallback(state);
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
  nfdresult_t result =
      InDialog([&] { return NFD_OpenDialogU8(&outPath, filters, 1, nullptr); });
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

void OpenProjectDialog(AppState& state) {
  // Projects are folders (.dsgn), so this is a FOLDER picker — select the
  // project itself and hit Open. A pick inside a project (or of the
  // project.json) normalizes up to the project root.
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  if (InDialog([&] { return NFD_PickFolderU8(&outPath, nullptr); }) ==
      NFD_OKAY) {
    const std::string path = NormalizeProjectPick(outPath);
    NFD_FreePathU8(outPath);
    if (Autosaver::HasNewerAutosave(path) ||
        !Autosaver::RecoverableDocuments(path).empty()) {
      // Defer to the recovery modal (DrawFileBrowser) — the user picks
      // between the saved file and the newer autosave.
      state.recoveryOffer = path;
      return;
    }
    OpenPickedProject(state, path);
  }
}

void SaveProjectDialog(AppState& state) {
  if (NothingToSave(state)) {
    LOG_WARN("Save As: there is no document to save");
    return;
  }
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  // `.dsgn` ONLY — a project saved from the GUI is always the directory form.
  // The flat single-file form is still loaded (legacy files, the RPC, tests),
  // but offering it here created projects that File > Open Project could
  // never return: it is a FOLDER picker, so a flat `foo.json` was reachable
  // only through Open Recent. ProjectSaveTarget enforces it for a name typed
  // by hand, which the filter alone does not.
  const nfdu8filteritem_t filters[] = {{"Designer project", "dsgn"}};
  if (InDialog([&] {
        return NFD_SaveDialogU8(&outPath, filters, 1, nullptr, "project.dsgn");
      }) == NFD_OKAY) {
    std::string path = DocumentIO::ProjectSaveTarget(outPath);
    NFD_FreePathU8(outPath);
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
    if (InDialog([&] {
          return NFD_SaveDialogU8(&out, sheetFilters, 2, nullptr, "sheet.pdf");
        }) != NFD_OKAY) {
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
  // STEP first on both: it is the format another CAD system opens. An
  // assembly has no single-solid formats (STL/BREP fuse ONE part).
  const nfdu8filteritem_t asmFilters[] = {{"STEP", "step,stp"},
                                          {"Visual mesh", "glb,gltf,obj,ply"}};
  const nfdu8filteritem_t partFilters[] = {
      {"CAD / mesh", "step,stp,stl,brep,glb,gltf,obj,ply"}};
  nfdu8char_t* outPath = nullptr;
  if (InDialog([&] {
        return NFD_SaveDialogU8(&outPath,
                                haveAssembly ? asmFilters : partFilters,
                                haveAssembly ? 2 : 1, nullptr,
                                haveAssembly ? "assembly.step" : "part.step");
      }) != NFD_OKAY) {
    return;
  }
  std::string path(outPath);
  NFD_FreePathU8(outPath);
  if (std::filesystem::path(path).extension().empty())
    path += ".step";
  std::string error;
  if (ExportActiveDocument(state, path, &error))
    LOG_INFO("Exported to {}", path);
  else
    LOG_ERROR("Export failed: {}", error);
}

void ExportBomDialog(AppState& state) {
  if (!state.assembly)
    return;
  if (!NativeDialogsSafe())
    return;
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"CSV", "csv"}};
  if (InDialog([&] {
        return NFD_SaveDialogU8(&outPath, filters, 1, nullptr, "bom.csv");
      }) != NFD_OKAY)
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

void ExportMotionDialog(AppState& state, uint64_t studyId) {
  if (!state.assembly || !NativeDialogsSafe())
    return;
  const MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s)
    return;
  // A name from the study, filesystem-safe: "camshaft, one turn" is a
  // perfectly good study name and a poor file name.
  std::string name = s->name.empty() ? "motion" : s->name;
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
      c = '_';
  nfdu8char_t* outPath = nullptr;
  const nfdu8filteritem_t filters[] = {{"glTF binary", "glb"}};
  if (InDialog([&] {
        return NFD_SaveDialogU8(&outPath, filters, 1, nullptr,
                                (name + ".glb").c_str());
      }) != NFD_OKAY)
    return;
  std::string path(outPath);
  NFD_FreePathU8(outPath);
  if (!path.ends_with(".glb"))
    path += ".glb";
  std::string error;
  size_t nodes = 0, frames = 0;
  if (ExportMotionGlb(state, studyId, path, &error, &nodes, &frames)) {
    state.motionAddRefusal.clear();
    LOG_INFO("Motion exported to {} ({} nodes, {} frames)", path, nodes,
             frames);
  } else {
    // Shown in the panel, not only logged: a click that silently did
    // nothing is the thing this button replaces.
    state.motionAddRefusal = "Export failed: " + error;
  }
}

#else // no nativefiledialog: the ImGui browser is the only picker

void ImportFileDialog(AppState& state) { OpenBrowserFallback(state); }

void OpenReferenceImageDialog(AppState& state) {
  state.sketchEdit.actionMsg =
      "Reference-image import needs the native file dialog.";
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

void ExportMotionDialog(AppState&, uint64_t) {
  LOG_WARN("Motion export requires nativefiledialog");
}

#endif

void ExportMotionVideoDialog(AppState& state, uint64_t studyId) {
  if (!state.assembly)
    return;
  const MotionStudy* s = state.assembly->FindMotionStudyById(studyId);
  if (!s)
    return;
  std::string path;
  if (state.saveDialogAnswer) {
    path = *state.saveDialogAnswer;
    state.saveDialogAnswer.reset();
  } else {
#ifdef HAVE_NATIVEFILEDIALOG
    if (!NativeDialogsSafe())
      return;
    std::string name = s->name.empty() ? "motion" : s->name;
    for (char& c : name)
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-')
        c = '_';
    // The file type IS the format: the first is the default.
    nfdu8char_t* outPath = nullptr;
    const nfdu8filteritem_t filters[] = {{"MP4 video (H.264)", "mp4"},
                                         {"WebM video (VP9)", "webm"},
                                         {"GIF animation", "gif"},
                                         {"PNG frames (a folder)", "png"}};
    if (InDialog([&] {
          return NFD_SaveDialogU8(&outPath, filters, 4, nullptr,
                                  (name + ".mp4").c_str());
        }) != NFD_OKAY)
      return;
    path = outPath;
    NFD_FreePathU8(outPath);
#else
    return;
#endif
  }
  MotionVideoRequest req;
  req.studyId = studyId;
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext == ".png") {
    // Frames need a folder, and a save dialog names a file: the folder is
    // the file's name without its extension, beside where it was chosen.
    req.format = "png";
    req.path = std::filesystem::path(path).replace_extension().string();
  } else {
    req.path = path; // the extension says the format, or it is refused
  }
  std::string error;
  if (StartMotionVideo(state, req, &error))
    state.motionAddRefusal.clear();
  else
    state.motionAddRefusal = "Export failed: " + error;
}

bool ExportActiveDocument(AppState& state, const std::string& path,
                          std::string* error) {
  const auto fail = [&](std::string why) {
    if (error != nullptr)
      *error = std::move(why);
    return false;
  };
  std::string ext = std::filesystem::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

  // Visual mesh formats (glTF/OBJ/PLY): tessellation of the part or the whole
  // assembly, materials carried through. Works on either tab.
  if (ext == ".glb" || ext == ".gltf" || ext == ".obj" || ext == ".ply") {
    const std::vector<MeshExport::Item> items = BuildVisualExportItems(state);
    if (items.empty())
      return fail("nothing to export");
    std::string why;
    return MeshExport::SaveVisual(path, items, 0.1, &why) || fail(why);
  }
  // STEP: the part or the assembly, with names, colours and structure (X4).
  if (ext == ".step" || ext == ".stp") {
    if (!state.assembly && !state.document)
      return fail("STEP export needs a part or an assembly tab");
    const auto counts = ExportStep(
        state, state.assembly ? nullptr : state.document.get(), -1, path);
    return counts.has_value() || fail(counts.error());
  }
  if (ext != ".stl" && ext != ".brep")
    return fail("unknown export format \"" + ext +
                "\" — use .step/.stp, .stl, .brep, .glb, .gltf, .obj or .ply");
  // Single-solid formats: the part tab only, the document fused into one.
  if (!state.document)
    return fail("STL/BREP export needs a part tab; use STEP or glTF/OBJ/PLY "
                "for an assembly");
  // Derived reference bodies are other parts' geometry: never exported.
  std::vector<Body> bodies;
  for (const Body& b : state.document->GetBodies())
    if (!b.reference)
      bodies.push_back(b);
  if (bodies.empty())
    return fail("the part has only reference bodies, nothing to export");
  CADShape shape = bodies[0].shape;
  for (size_t i = 1; i < bodies.size(); i++)
    shape = shape.Union(bodies[i].shape);
  auto& kernel = CADKernel::Instance();
  const bool ok = ext == ".stl" ? kernel.SaveSTL(path, shape)
                                : kernel.SaveBREP(path, shape);
  return ok || fail("could not write " + path);
}

void SaveProjectAction(AppState& state) {
  if (NothingToSave(state)) {
    LOG_WARN("Save: there is no document to save");
    return;
  }
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
