#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/SketchFeature.h"
#include "core/SketchInference.h"
#include "core/SketchSolver.h"

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

enum class SketchTool {
  Select,
  Point,
  Line,
  Circle,
  Rectangle,
  Trim,
  Arc,           // 3-click center → start → end
  ArcThreePoint, // 3-click start → end → through-point
  Mirror,        // reflect the current selection across a clicked axis line
  Fillet         // round the clicked corner where two lines meet
};

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
  bool hasPendingPoint2 = false; // second click of a 3-click tool (arcs)
  glm::vec2 pendingPoint2{0.0f};
  glm::vec2 hoverPoint{0.0f};

  // Generative ops (Mirror/Fillet): "linked" adds constraints tying the result
  // to its source so it tracks edits; unchecked bakes an unlinked copy.
  bool linkGenerative = true;
  float filletRadius = 0.5f;

  // Transient feedback shown in the toolbar (e.g. why a fillet no-op'd), so
  // operations that can silently fail tell the user what happened.
  std::string actionMsg;
  int hoveredPlane = -1; // 0 Front / 1 Top / 2 Right while picking

  // Sketch-entity selection (for adding constraints), ordered.
  std::vector<sketch::PointRef> selected;

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

  // Index of the dimension constraint being edited inline (-1 = none).
  int editingDim = -1;

  // Solver status of the active sketch, refreshed each frame: remaining DOF
  // (0 = fully constrained, -1 = failed) and the constraint indices flagged as
  // conflicting/redundant.
  int dof = -1;
  std::vector<size_t> conflictingConstraints;
  bool showConstraintGlyphs = true;

  bool IsEditing() const { return activeSketchId != 0; }
};

// A live whole-entity drag (FreeCAD moveGeometry recipe): the solver is built
// once on mouse-down and reused each frame. Lives here (not SketchEditState) so
// that stays cheaply copyable.
struct SketchDragSession {
  enum class Mode { Point, LineBody, CircleRim };
  bool active = false;
  Mode mode = Mode::Point;
  std::unique_ptr<sketch::Solver> solver;
  std::vector<sketch::PointRef> points; // dragged points (Point / LineBody)
  std::vector<glm::vec2> initialPos;    // their UV positions at mouse-down
  glm::vec2 initialCursor{0.0f};
  uint32_t circleId = 0;                // CircleRim
  glm::vec2 circleCenter{0.0f};         // CircleRim: radius = |cursor - centre|
  size_t featureIndex = 0;              // sketch index for the commit
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
  SketchDragSession sketchDrag;

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
