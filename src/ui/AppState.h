#pragma once

#include "core/Command.h"
#include "core/Document.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class Scene;
class Camera;
class MeshRenderer;
class OutlineRenderer;
class RayTracingRenderer;
class SketchManager;
class SketchRenderer;
class FaceSelector;
struct GLFWwindow;

namespace ui {

// A document body mirrored into the renderers.
struct DocumentRenderMesh {
  uint32_t meshId;
  uint32_t outlineId;
};

enum class ViewportDrag { None, Orbit, Pan, Zoom, Roll };

// All state the UI reads and writes, replacing main.cpp's globals. The
// document and undo stack are owned here (they are swapped by File > New /
// Open); everything else is a non-owning pointer to objects whose lifetime
// main.cpp manages alongside the Vulkan context.
struct AppState {
  // Owned: the model
  std::unique_ptr<Document> document;
  std::unique_ptr<CommandStack> commands;

  // Non-owning
  Scene* scene = nullptr;
  Camera* camera = nullptr;
  MeshRenderer* meshRenderer = nullptr;
  OutlineRenderer* outlineRenderer = nullptr;
  RayTracingRenderer* rayTracer = nullptr;
  SketchManager* sketchManager = nullptr;
  SketchRenderer* sketchRenderer = nullptr;
  FaceSelector* faceSelector = nullptr;
  GLFWwindow* window = nullptr;

  // UI state
  int selectedFeature = -1;
  ViewportDrag viewportDrag = ViewportDrag::None;
  bool showPlanes = true;
  bool buildDefaultDockLayout = false;
  bool runGuiTests = false;
  nlohmann::json editBaseline; // parameter-edit snapshot while a widget is active

  // Document -> renderer sync bookkeeping
  uint64_t syncedDocumentVersion = 0;
  std::vector<DocumentRenderMesh> documentMeshes;

#ifndef HAVE_NATIVEFILEDIALOG
  // Fallback file-browser state
  bool showFileBrowser = false;
  std::string currentPath = "../CAD_files";
  std::vector<std::string> directoryContents;
#endif
};

} // namespace ui
