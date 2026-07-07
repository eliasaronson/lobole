#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/SketchFeature.h"
#include "core/SketchInference.h"

#include <glm/glm.hpp>
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

enum class SketchTool { Select, Point, Line, Circle, Rectangle, Trim };

// Transient state for the active sketch-editing session. The sketch itself
// lives in the Document (as a SketchFeature); this only holds which sketch is
// being edited (by stable id, so it survives undo reindexing), the current
// tool, and the in-progress two-click drawing state.
struct SketchEditState {
  bool pickingPlane = false;   // "New Sketch" armed, waiting for a plane/face
  uint64_t activeSketchId = 0; // 0 = not editing a sketch
  SketchTool tool = SketchTool::Line;
  bool hasPendingPoint = false; // first click of a two-click entity placed
  glm::vec2 pendingPoint{0.0f};
  glm::vec2 hoverPoint{0.0f};
  int hoveredPlane = -1; // 0 Front / 1 Top / 2 Right while picking

  // Sketch-entity selection (for adding constraints), ordered.
  std::vector<sketch::PointRef> selected;
  bool dragging = false;    // dragging a selected point (Select tool)
  sketch::PointRef dragRef; // the point being dragged

  // What the cursor is over in the Select tool (for hover highlighting),
  // refreshed each frame. `hoveredBody` distinguishes an entity body from a
  // vertex.
  bool hasHover = false;
  bool hoveredBody = false;
  sketch::PointRef hoveredRef;

  // Trim tool: the sub-segment the cursor would remove, highlighted so it's
  // clear what a click deletes. In plane UV.
  bool hasTrimPreview = false;
  glm::vec2 trimPreviewA{0.0f};
  glm::vec2 trimPreviewB{0.0f};

  // Auto-constraint inference (snap + constrain while drawing). Toggle in the
  // toolbar; hold Ctrl to suppress for one click.
  bool autoConstrain = true;
  // The inference for the point currently being placed (glyphs + snap preview),
  // refreshed each frame while drawing.
  bool hasCurrentSnap = false;
  sketch::Snap currentSnap;

  bool IsEditing() const { return activeSketchId != 0; }
};

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
  SketchRenderer* sketchRenderer = nullptr;
  FaceSelector* faceSelector = nullptr;
  GLFWwindow* window = nullptr;

  // UI state
  int selectedFeature = -1;
  // Feature being previewed (in the document + rendered, but NOT on the undo
  // stack until accepted). 0 = none. See PreviewFeature.
  uint64_t pendingFeatureId = 0;
  ViewportDrag viewportDrag = ViewportDrag::None;
  bool showPlanes = true; // visible by default; auto-hidden once bodies exist
  SketchEditState sketchEdit;

  // The 3D viewport = the central dock node (between the side panels), in
  // ImGui display coords. The renderers draw only here and picking maps
  // clicks through it, so the model centers on the visible area.
  glm::vec2 viewportPos{0.0f, 0.0f};
  glm::vec2 viewportSize{1280.0f, 720.0f};
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
