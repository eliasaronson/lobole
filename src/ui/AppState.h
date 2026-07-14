#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/SketchFeature.h"
#include "core/SketchInference.h"
#include "core/SketchSolver.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Scene;
class Camera;
class MeshRenderer;
class OutlineRenderer;
class RayTracingRenderer;
class SketchRenderer;
class FieldRenderer;
class FaceSelector;
class IBLTextures;
struct GLFWwindow;

namespace ui {

// A document body mirrored into the renderers. For a part tab, one per body.
// For an assembly tab, one per (occurrence × referenced-part body): meshId is
// the shared asset's instance, occurrenceId ties the hit back to the placement.
struct DocumentRenderMesh {
  uint32_t meshId;
  uint32_t outlineId;
  size_t bodyIndex; // index into the source part's GetBodies() (element names)
  uint64_t occurrenceId = 0; // assembly: the occurrence; 0 for a part body
  // B-Rep edge polylines (world space, edgeIndex order — the ElementMap /
  // PickEdge convention). Cached here for edge highlighting.
  std::vector<std::vector<glm::vec3>> edgePolylines;
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
  Ellipse,       // 3-click center → major-axis end → minor radius
  Mirror,        // reflect the current selection across a clicked axis line
  Fillet,        // round the clicked corner where two lines meet
  Pattern,       // linear/circular array of the current selection
  Offset         // offset the selected closed loop by a distance
};

// Transient state for the active sketch-editing session. The sketch itself
// lives in the Document (as a SketchFeature); this only holds which sketch is
// being edited (by stable id, so it survives undo reindexing), the current
// tool, and the in-progress two-click drawing state.
struct SketchEditState {
  bool pickingPlane = false;   // "New Sketch" armed, waiting for a plane/face
  uint64_t activeSketchId = 0; // 0 = not editing a sketch
  SketchTool tool = SketchTool::Select;
  bool hasPendingPoint = false; // first click of a two-click entity placed
  glm::vec2 pendingPoint{0.0f};
  bool hasPendingPoint2 = false; // second click of a 3-click tool (arcs)
  glm::vec2 pendingPoint2{0.0f};
  glm::vec2 hoverPoint{0.0f};

  // Box (rubber-band) selection in progress (Select tool). Screen px; the
  // window/crossing sense is boxEnd.x < boxStart.x (right→left = crossing).
  bool boxActive = false;
  glm::vec2 boxStart{0.0f};
  glm::vec2 boxEnd{0.0f};

  // Generative ops (Mirror/Fillet): "linked" adds constraints tying the result
  // to its source so it tracks edits; unchecked bakes an unlinked copy.
  bool linkGenerative = true;
  float filletRadius = 0.5f;

  // Mirror tool is two-step: collect geometry (false), then pick the axis
  // (true). Enter advances; Escape backs out. Selection carries the sources.
  bool mirrorAxisPhase = false;

  // Pattern tool params (applied to the current selection).
  bool patternCircular = false;
  int patternCount = 3;
  glm::vec2 patternStep{2.0f, 0.0f}; // linear per-step translation
  glm::vec2 patternCenter{0.0f};     // circular centre
  float patternAngleDeg = 60.0f;     // circular step angle

  // Offset tool params (applied to the selected closed loop).
  float offsetDistance = 0.5f;
  bool offsetJoinArc = true; // arcs at convex corners (else extend/trim)

  // Transient feedback shown in the toolbar (e.g. why a fillet no-op'd), so
  // operations that can silently fail tell the user what happened.
  std::string actionMsg;
  int hoveredPlane = -1; // 0 Front / 1 Top / 2 Right while picking
  uint64_t hoveredDatumId = 0; // datum plane under the cursor while picking
  // When non-zero while pickingPlane: re-pick the attached face of this sketch
  // (its faceRef) instead of starting a new sketch. Set by "Reattach face…".
  uint64_t reattachSketchId = 0;

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

// Edge picking for fillet/chamfer (M7): active while a blend feature is
// collecting edge refs — during its creation preview, or re-armed later from
// the inspector / tree ("Pick edges…"). Viewport clicks toggle edges on that
// feature; hovering pre-highlights the edge under the cursor.
struct EdgePickState {
  uint64_t filletId = 0; // the FilletFeature collecting edges; 0 = inactive
  bool hasHover = false;
  uint32_t hoverMeshId = 0;
  uint32_t hoverEdgeIndex = 0;
  bool Active() const { return filletId != 0; }
};

// Measure tool (inspection I1 GUI, C3): a viewport mode that reads geometry
// off picked elements. Pick one element → its own measure (face area / edge
// length + radius); pick a second → the distance between them. Faces and
// edges both pick; the readout string is shown in an overlay + the Properties
// panel. Part-tab only.
struct MeasureState {
  bool active = false;      // the tool is armed (toolbar toggle)
  bool hasFirst = false;    // the first element is picked
  ElementRef first;         // the first element (face or edge)
  bool firstIsEdge = false; // kind of `first`, for the readout label
  std::string readout;      // formatted result ("" = nothing measured yet)
  // Hover feedback (mirrors EdgePickState): the element under the cursor.
  bool hasHover = false;
  bool hoverIsEdge = false;
  uint32_t hoverMeshId = 0;
  uint32_t hoverIndex = 0; // face index or edge index per hoverIsEdge
};

// Simulation mode (FEM): a workspace the user enters and exits, like sketch
// mode — `active` is the mode gate. While active the toolbar swaps to
// simulation tools, the Model dock shows the Studies panel, and a viewport
// click assigns the armed boundary condition to the picked face. Results are
// derived data on the Document; this holds only transient UI/display state.
struct SimulationState {
  bool active = false;        // in simulation mode (the mode gate)
  uint64_t activeStudyId = 0; // study being edited/viewed (0 = none)

  // Armed boundary-condition tool: a viewport face click assigns this load.
  enum class Tool { None, Fixed, Force, Pressure };
  Tool tool = Tool::None;
  // Params applied to the load placed by the next face pick.
  double pendingMagnitude = 0.0;
  glm::dvec3 pendingDirection{0.0, 0.0, -1.0};

  // Result overlay display.
  enum class Field { None, VonMises, Displacement };
  Field field = Field::None;
  float deformScale = 0.0f; // shader-side warp multiplier (0 = undeformed)
  // Body render instances hidden while the field overlay replaces them, so
  // ExitSimulation / field=None restores exactly those.
  std::vector<uint32_t> hiddenMeshes;
  // Signature of the last-built field overlay (field, study, results epoch);
  // the vertex rebuild is skipped when unchanged. 0 = nothing built.
  uint64_t fieldBuiltKey = 0;

  bool IsActive() const { return active; }
};

// Drives an offline path-traced still across UI frames (one sample batch per
// frame) so the Render modal can show progress and stay cancellable. main.cpp
// owns the pump; the toolbar modal reads/writes the request + result fields.
struct RenderJob {
  bool active = false;        // a render is in flight
  bool started = false;       // BeginStill has run (internal)
  bool cancel = false;        // set by the UI to abort
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t totalSamples = 128;
  uint32_t done = 0;          // samples traced so far
  uint32_t seed = 1;
  bool groundPlane = true;    // shadow-catcher plane under the scene
  std::string outPath;
  // Result (valid once !active after having been started)
  bool finished = false;      // a render completed (success or failure)
  bool success = false;
  std::string error;
  std::string resultPath;
};

// All state the UI reads and writes, replacing main.cpp's globals. The
// document and undo stack are owned here (they are swapped by File > New /
// Open); everything else is a non-owning pointer to objects whose lifetime
// main.cpp manages alongside the Vulkan context.
// The kind of document a project tab holds (A2). A part is a feature list that
// builds bodies; an assembly is an occurrence list that places other parts.
enum class DocKind { Part, Assembly };

// One document of a project (assemblies A1/A2). The ACTIVE tab's
// document/assembly/commands live in AppState (so all existing single-document
// code is unchanged); an inactive tab stashes them here. Exactly one of
// `doc`/`assembly` is populated, per `kind`.
// Snapshot-based undo for an assembly tab's occurrence edits. The assembly is
// small (a list of occurrences), so each edit pushes a full copy — simpler than
// generalizing Command/CommandStack, which are Document-only.
struct AssemblyHistory {
  std::vector<AssemblyDocument> undo;
  std::vector<AssemblyDocument> redo;
};

struct PartSlot {
  uint64_t id = 0;   // stable per-document id (A2): what occurrences reference
  std::string name;
  DocKind kind = DocKind::Part;
  std::unique_ptr<Document> doc;          // part: null while this tab is active
  std::unique_ptr<AssemblyDocument> assembly; // assembly: null while active
  std::unique_ptr<CommandStack> commands; // null while active (parts only)
  AssemblyHistory history;                // assembly undo/redo (stashed here)
  uint64_t lastSavedVersion = 0;      // per-document dirty tracking
  // Assembly tabs only (A4): referenced parts' Document::Version() at this
  // assembly's last solve, stashed while inactive. See AppState field.
  std::unordered_map<uint64_t, uint64_t> solvedPartVersions;
};

struct AppState {
  // Owned: the ACTIVE tab's model. For a part tab, `document`/`commands` are
  // live and `assembly` is null; for an assembly tab, `assembly` is live and
  // `document`/`commands` are null. They are mutually exclusive.
  std::unique_ptr<Document> document;
  std::unique_ptr<CommandStack> commands;
  std::unique_ptr<AssemblyDocument> assembly;
  AssemblyHistory assemblyHistory; // active assembly's undo/redo (A2 phase 3)
  // Part→assembly change propagation (A4): {partId → Document::Version()}
  // stamped at the active assembly's last solve. AssemblyPartsStale compares
  // against these on tab activation, so a part edit re-solves mates
  // automatically. In-memory session state — never persisted (the F5
  // `partVersion` git pin is a separate, reserved concept).
  std::unordered_map<uint64_t, uint64_t> solvedPartVersions;

  // Multi-document project (A1/A2): all tabs; `activePart` indexes the one
  // whose model is live above. One entry = today's single part.
  std::vector<PartSlot> parts;
  int activePart = 0;
  uint64_t nextPartId = 1; // stable document-id allocator (A2)

  // Non-owning
  Scene* scene = nullptr;
  Camera* camera = nullptr;
  MeshRenderer* meshRenderer = nullptr;
  OutlineRenderer* outlineRenderer = nullptr;
  RayTracingRenderer* rayTracer = nullptr;
  SketchRenderer* sketchRenderer = nullptr;
  FieldRenderer* fieldRenderer = nullptr; // FEM result overlay
  FaceSelector* faceSelector = nullptr;
  IBLTextures* ibl = nullptr;
  GLFWwindow* window = nullptr;

  // Mate creation tool (A3 phase 3): a toolbar button starts it with a mate
  // type; the user clicks two occurrence faces (connectors), then a preview
  // mate is added + solved live for accept/cancel. See AppActions mate-tool
  // helpers + OccurrencePanel.
  struct MateToolState {
    bool active = false;       // picking connectors for a new mate
    MateType type = MateType::Fastened;
    bool haveFirst = false;    // the first connector is picked
    Connector first;           // occurrence + face anchor of the first pick
    uint64_t previewMateId = 0; // non-zero once both picked: the live preview
    std::string message;       // transient hint (e.g. "pick a named face")
  };

  // 3D drag gizmo for the selected occurrence (A2.5). World-aligned translate
  // arrows + rotate arcs; a drag mutates the occurrence transform live (cheap
  // per-instance update, no resync) and snapshots/solves once on release.
  struct GizmoDrag {
    bool active = false;
    int axis = -1;         // handle axis index (into the gizmo frame below)
    bool rotate = false;   // false = translate along axis, true = rotate about it
    glm::vec3 startPos{0}; // occurrence position at grab (free drag)
    glm::quat startRot{1, 0, 0, 0}; // occurrence orientation at grab (free drag)
    float startParam = 0.0f; // axis coordinate (translate) / angle (rotate) at grab
    // The gizmo's frame: world axes at the occurrence origin for a free drag,
    // or the mate's connector-A world frame for a drag-within-mate (phase 3).
    glm::vec3 frameOrigin{0};
    glm::vec3 frameAxis[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    // Drag-within-mate: non-zero → the drag writes this mate's DOF (offset/dx/
    // dy/rotation/ballRotation) instead of the occurrence transform. Only one
    // scalar DOF drags at a time (startDof); ball uses the quaternion.
    uint64_t mateId = 0;
    float startDof = 0.0f;
    glm::quat startBall{1, 0, 0, 0};
    // Explode authoring (A4): non-zero → the drag writes the occurrence's
    // ExplodeStep.offset in this exploded view (translate only; the solved
    // transform is untouched).
    uint64_t explodeView = 0;
    glm::vec3 startOffset{0};
  };

  // In-context editing (A4): non-zero while a part tab is being edited with
  // its context assembly ghosted around it. The part renders at identity;
  // every other occurrence renders at OccMatrix(edited)⁻¹·OccMatrix(other)
  // (the assembly brought to the part, so part tooling is untouched). Set by
  // EnterInContextEdit; cleared by any tab switch (SetActivePart).
  struct InContextEdit {
    uint64_t assemblySlotId = 0; // PartSlot::id of the context assembly
    uint64_t occurrenceId = 0;   // the occurrence being edited
  };
  InContextEdit inContext;

  // Exploded-view display state (A4): TRANSIENT, never saved — which of the
  // active assembly's exploded views is applied, and how far. Rendering (and
  // picking, which follows it) composes Translate(factor·step.offset) in
  // front of each solved occurrence transform; the document is untouched.
  uint64_t activeExplodedView = 0; // 0 = none (normal display)
  float explodeFactor = 1.0f;

  // UI state
  int selectedFeature = -1;
  uint64_t selectedOccurrence = 0; // active assembly's selected occurrence (0 = none)
  uint64_t selectedMate = 0;       // active assembly's selected mate (0 = none)
  MateToolState mateTool;
  GizmoDrag gizmoDrag;
  // Feature being previewed (in the document + rendered, but NOT on the undo
  // stack until accepted). 0 = none. See PreviewFeature.
  uint64_t pendingFeatureId = 0;
  ViewportDrag viewportDrag = ViewportDrag::None;
  bool showPlanes = true; // visible by default; auto-hidden once bodies exist
  // Section view (I3): a world-space clip plane {xyz=normal, w=dist};
  // fragments past it are discarded. Zero = disabled.
  glm::vec4 sectionPlane = glm::vec4(0.0f);
  // Section cap fills (I3 v2, C4): the section plane at the last cap rebuild,
  // and the cap instance ids (separate from documentMeshes so body↔index
  // alignment holds; removed on the next resync). A cap rebuild is triggered
  // when sectionPlane changes and no widget is being dragged (so caps settle
  // on slider release while the shader clip previews live).
  glm::vec4 syncedSectionPlane = glm::vec4(0.0f);
  std::vector<uint32_t> sectionCapMeshes;
  bool showElementNames = false; // debug: element name of the hovered face
  SketchEditState sketchEdit;
  SketchDragSession sketchDrag;
  EdgePickState edgePick;
  MeasureState measure;
  SimulationState simulation;
  // Mass-properties center-of-gravity marker (C3): drawn as a small world-space
  // cross when the Properties panel's Mass properties section is open. The
  // panel computes the CoM and stamps it here; the overlay build reads it.
  bool showCog = false;
  glm::vec3 cogWorld{0.0f};

  // The 3D viewport = the central dock node (between the side panels), in
  // ImGui display coords. The renderers draw only here and picking maps
  // clicks through it, so the model centers on the visible area.
  glm::vec2 viewportPos{0.0f, 0.0f};
  glm::vec2 viewportSize{1280.0f, 720.0f};
  bool buildDefaultDockLayout = false;
  bool runGuiTests = false;
  nlohmann::json editBaseline; // parameter-edit snapshot while a widget is active

  // Where the document lives on disk ("" = never saved) and the document
  // version at the last clean save/open — dirty = Version() != lastSaved.
  // Drives autosave (core/Autosaver) and its recovery sidecars.
  std::string projectPath;
  uint64_t lastSavedVersion = 0;
  // Non-empty: OpenProjectDialog found an autosave newer than this project
  // file; the recovery modal (DrawFileBrowser) resolves it.
  std::string recoveryOffer;

  // Document -> renderer sync bookkeeping
  uint64_t syncedDocumentVersion = 0;
  std::vector<DocumentRenderMesh> documentMeshes;

  // Offline render (Render toolbar button); pumped by main.cpp.
  RenderJob renderJob;

#ifndef HAVE_NATIVEFILEDIALOG
  // Fallback file-browser state
  bool showFileBrowser = false;
  std::string currentPath = "../CAD_files";
  std::vector<std::string> directoryContents;
#endif
};

} // namespace ui
