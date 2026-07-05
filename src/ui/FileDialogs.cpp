#include "ui/FileDialogs.h"

#include "Logger.h"
#include "core/Serialization.h"
#include "ui/AppActions.h"

#include "imgui.h"

#include <algorithm>
#include <filesystem>
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

void OpenProjectDialog(AppState& state) {
  nfdchar_t* outPath = nullptr;
  if (NFD_OpenDialog("json", nullptr, &outPath) == NFD_OKAY) {
    std::string path(outPath);
    free(outPath);
    std::string error;
    if (auto doc = DocumentIO::Load(path, &error)) {
      ReplaceDocument(state, std::move(doc));
      FitCameraToDocument(state);
    } else {
      LOG_ERROR("Failed to open project: {}", error);
    }
  }
}

void SaveProjectDialog(AppState& state) {
  if (!state.document)
    return;
  nfdchar_t* outPath = nullptr;
  if (NFD_SaveDialog("json", nullptr, &outPath) == NFD_OKAY) {
    std::string path(outPath);
    free(outPath);
    if (std::filesystem::path(path).extension().empty()) {
      path += ".json";
    }
    std::string error;
    if (!DocumentIO::Save(*state.document, path, &error)) {
      LOG_ERROR("Failed to save project: {}", error);
    }
  }
}

void DrawFileBrowser(AppState&) {}

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

void OpenProjectDialog(AppState&) {
  LOG_WARN("Open Project requires nativefiledialog");
}

void SaveProjectDialog(AppState&) {
  LOG_WARN("Save Project requires nativefiledialog");
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
