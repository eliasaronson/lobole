#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingRender.h"
#include "core/Occurrence.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchInference.h"
#include "core/sketch/SketchSolver.h"
#include "ui/TopoJob.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <map>
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
  // Dimension-label interaction (CAD-style placement): the pill the cursor
  // is over (refreshed each frame by DrawDimensions, read by the canvas
  // input handler one frame behind) and the one being dragged. While either
  // is live the sketch canvas ignores the mouse, so moving a label never
  // draws or selects through it.
  int dimHot = -1;
  int dimDrag = -1;

  // Solver status of the active sketch, refreshed each frame: remaining DOF
  // (0 = fully constrained, -1 = failed) and the constraint indices flagged as
  // conflicting/redundant.
  int dof = -1;
  std::vector<size_t> conflictingConstraints;
  // Entities with free coordinates (PlaneGCS dependent-parameter diagnosis):
  // drawn in the base color; fully-defined geometry draws green.
  std::vector<uint32_t> underDefined;
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

// An armed feature-dialog pick slot (U3): a reference field of the feature
// being edited collects viewport picks. The selection pipeline filters
// candidates to `kinds` and routes the resolved item to SlotPickClicked;
// single-value slots disarm after one pick, multi slots (face lists) stay
// armed until toggled off / Escape / the feature resolves.
struct SlotPickState {
  uint64_t featureId = 0; // feature being edited (0 = inactive)
  int slot = -1;          // PickSlot id (FeatureEditor.h)
  uint32_t kinds = 0;     // SelectionItem::Kind bitmask accepted
  bool multi = false;
  std::string prompt;     // status pill while armed
  bool Active() const { return featureId != 0; }
};

// Measure tool (inspection I1 GUI, C3): a viewport mode that reads geometry
// off picked elements. Pick one element → its own measure (face area / edge
// length + radius); pick a second → the distance between them. Faces and
// edges both pick; the readout string is shown in an overlay + the Properties
// panel. Part-tab only.
struct MeasureState {
  bool active = false; // the tool is armed (toolbar toggle)
  // Every measured element stays picked (highlighted in the viewport) until
  // clicked again, removed in the panel, or Escape clears the tool: one
  // element reads out its own measure, two their distance.
  struct Element {
    ElementRef ref;
    bool isEdge = false;
    // Assembly tabs: the occurrence the element was picked on (0 = part
    // context). Part of the element's identity — the same face on two
    // occurrences of one part is two distinct measurables.
    uint64_t occurrenceId = 0;
  };
  std::vector<Element> picked;
  std::string readout; // formatted result ("" = nothing measured yet)
  // Nearest-distance line between two picked elements, drawn in the
  // viewport (set alongside the two-element readout).
  bool hasDistanceLine = false;
  glm::vec3 distA{0.0f};
  glm::vec3 distB{0.0f};
  // Hover feedback comes from the selection pipeline (U1c).
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
  // Keep (topology) freezes the picked face solid during optimization.
  enum class Tool { None, Fixed, Force, Pressure, Keep };
  Tool tool = Tool::None;
  // Params applied to the load placed by the next face pick.
  double pendingMagnitude = 0.0;
  glm::dvec3 pendingDirection{0.0, 0.0, -1.0};

  // Result overlay display. VonMises/Displacement are static/modal fields;
  // Shape (the optimized iso-surface as a grey body) and Density (a colormap
  // of the density field) are topology views.
  enum class Field { None, VonMises, Displacement, Shape, Density };
  Field field = Field::None;
  float deformScale = 0.0f; // shader-side warp multiplier (0 = undeformed)
  int activeMode = 0;       // modal: which mode shape the overlay displays
  // Topology iso-surface threshold in [0,1]; 0 = auto (volume-matched).
  float topoThreshold = 0.0f;
  // The MeshRenderer instance holding the extracted iso-surface (0 = none), and
  // the signature that gates its re-extraction.
  uint32_t topoIsoInstance = 0;
  uint64_t topoIsoKey = 0;
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
// The kind of document a project tab holds (A2/drawings). A part is a feature
// list that builds bodies; an assembly is an occurrence list that places other
// parts; a drawing projects a part or assembly onto a 2D sheet.
enum class DocKind { Part, Assembly, Drawing };

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

// Snapshot-based undo for a drawing tab (the AssemblyHistory pattern): a
// drawing is a small setup object (sheet + a few views/dims), so each edit
// pushes a full copy rather than generalizing the Document-only Command stack.
struct DrawingHistory {
  std::vector<DrawingDocument> undo;
  std::vector<DrawingDocument> redo;
};

struct PartSlot {
  uint64_t id = 0;   // stable per-document id (A2): what occurrences reference
  std::string name;
  DocKind kind = DocKind::Part;
  std::unique_ptr<Document> doc;          // part: null while this tab is active
  std::unique_ptr<AssemblyDocument> assembly; // assembly: null while active
  std::unique_ptr<DrawingDocument> drawing;   // drawing: null while active
  std::unique_ptr<CommandStack> commands; // null while active (parts only)
  AssemblyHistory history;                // assembly undo/redo (stashed here)
  DrawingHistory drawingHistory;          // drawing undo/redo (stashed here)
  // Projected view geometry for a stashed drawing tab (D2). Kept across tab
  // switches so re-entering a drawing doesn't silently re-project — staleness
  // is only cleared by an explicit update (the FEM rule).
  std::map<uint64_t, drawing::SheetView> drawingRenders;
  uint64_t lastSavedVersion = 0;      // per-document dirty tracking
  // Assembly tabs only (A4): referenced parts' Document::Version() at this
  // assembly's last solve, stashed while inactive. See AppState field.
  std::unordered_map<uint64_t, uint64_t> solvedPartVersions;
};

// THE selection (U1, docs/ui/u1-selection.md): one typed, ordered set that
// every context reads and writes. Items are stable across regeneration —
// features/occurrences/mates by id, geometry by stable element name — never
// by mesh/face index (mesh ids change on every regen). Renderer highlights
// are a per-frame projection of this set, not parallel state.
struct SelectionItem {
  enum class Kind : uint8_t {
    Face, Edge, Body, Feature, Occurrence, Mate,
    SketchRegion // U2
  };
  Kind kind = Kind::Feature;
  uint64_t id = 0;    // Feature/Occurrence/Mate id; Body: its createdBy
  ElementRef element; // Face/Edge/SketchRegion: {featureId, stable name}

  bool operator==(const SelectionItem& o) const {
    return kind == o.kind && id == o.id &&
           element.featureId == o.element.featureId &&
           element.name == o.element.name;
  }
};

struct Selection {
  std::vector<SelectionItem> items; // ordered: pick order matters to tools

  bool Empty() const { return items.empty(); }
  void Clear() { items.clear(); }
  void Replace(const SelectionItem& it) { items.assign(1, it); }
  // Ctrl+click semantics: present → remove, absent → append.
  void Toggle(const SelectionItem& it) {
    const auto f = std::find(items.begin(), items.end(), it);
    if (f != items.end())
      items.erase(f);
    else
      items.push_back(it);
  }
  bool Has(const SelectionItem& it) const {
    return std::find(items.begin(), items.end(), it) != items.end();
  }
  const SelectionItem* First(SelectionItem::Kind k) const {
    for (const auto& it : items)
      if (it.kind == k)
        return &it;
    return nullptr;
  }
};

struct AppState {
  // Owned: the ACTIVE tab's model. For a part tab, `document`/`commands` are
  // live and `assembly` is null; for an assembly tab, `assembly` is live and
  // `document`/`commands` are null. They are mutually exclusive.
  std::unique_ptr<Document> document;
  std::unique_ptr<CommandStack> commands;
  std::unique_ptr<AssemblyDocument> assembly;
  AssemblyHistory assemblyHistory; // active assembly's undo/redo (A2 phase 3)
  // Active drawing tab (workstream 13): exactly one of document/assembly/
  // drawing is live, per the active PartSlot's kind. A drawing projects other
  // documents onto a 2D sheet — no feature list, no occurrences.
  std::unique_ptr<DrawingDocument> drawing;
  DrawingHistory drawingHistory;
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

  // Direct-manipulation handle on the pending preview feature (docs/ui/
  // direct-manipulation.md): drags the feature's headline scalar (extrude
  // distance / blend radius / revolve angle) along a grab-time axis. Preview
  // only, so no undo bookkeeping — accept commits the whole feature.
  struct HandleDrag {
    bool active = false;
    uint64_t featureId = 0;   // the pending feature at grab (undo-safe check)
    bool rotate = false;      // revolve arc vs linear arrow
    glm::vec3 axisOrigin{0};  // drag line / arc-plane center at grab
    glm::vec3 axisDir{0, 0, 1};
    double startValue = 0.0;  // the parameter at grab (Esc restores this)
    float startParam = 0.0f;  // axis coordinate / arc angle at grab
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
  Selection selection; // U1: the one selection set (see SelectionItem)
  // U1b: transient pick-pipeline state (SelectionPipeline.cpp). The hover
  // fields cache render-side handles for this frame's preview; the click
  // fields drive same-spot disambiguation cycling.
  struct PickState {
    bool edgeHover = false; // hovered edge preview (faces hover via FaceSelector)
    uint32_t edgeHoverMesh = 0;
    uint32_t edgeHoverIndex = 0;
    // U2: hovered sketch region while a Profiles slot is armed — the outer
    // loop's entity ids (empty = none).
    std::vector<uint32_t> regionHoverIds;
    glm::vec2 lastClickPos{-1e9f, -1e9f};
    int cycle = 0;
  };
  PickState pick;
  MateToolState mateTool;
  GizmoDrag gizmoDrag;
  HandleDrag handleDrag;
  // Feature being previewed (in the document + rendered, but NOT on the undo
  // stack until accepted). 0 = none. See PreviewFeature.
  uint64_t pendingFeatureId = 0;
  ViewportDrag viewportDrag = ViewportDrag::None;
  // True when the drag was started with Alt+left (trackpad-friendly) rather
  // than the middle button; the drag then owns the left button, so the click
  // must not leak into picking.
  bool viewportDragLeft = false;
  bool showPlanes = true; // visible by default; auto-hidden once bodies exist
  // Key-light shadow map (S1) — View > Shadows; session state only (no file
  // format / golden impact).
  bool showShadows = true;
  // SSAO (S2) — View > Ambient Occlusion; session state only, like shadows.
  bool showAmbientOcclusion = true;
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
  // Topology shape-guide ghost (TO4): a translucent render of a topology
  // study's optimized iso-surface, shown in the normal modeling workspace so
  // the user can re-model against it. `topoGhostStudyId` selects the study
  // (0 = off); `topoGhostInstance` is its MeshRenderer instance (rebuilt on
  // resync, like the section caps).
  uint64_t topoGhostStudyId = 0;
  uint32_t topoGhostInstance = 0;
  bool showElementNames = false; // debug: element name of the hovered face
  // Viewport-controls cheat sheet overlay (View > Viewport Controls) — U4
  // moved it out of the Properties panel, where it permanently ate space.
  bool showControlsHelp = false;
  // User font scale (View > Font Size, U5), multiplied onto the monitor
  // content scale. Setting fontRebuildRequested makes the main loop re-apply
  // the theme + rebuild the font atlas between frames (the U4 DPI path).
  float uiFontScale = 1.0f;
  bool fontRebuildRequested = false;
  bool paletteOpen = false; // Ctrl+K command palette (U6)
  // Hidden part bodies by creating-feature id (transient UI state, like the
  // camera): hidden bodies are skipped at render sync — unpickable and
  // outline-free, the assembly occurrence recipe. Cleared on tab switch.
  std::set<uint64_t> hiddenBodies;
  // Sketch-entity clipboard (Ctrl+C/V in sketch mode). Plain geometry by
  // value — pastes are unlinked copies with fresh ids; it survives tab
  // switches, so pasting into another part's sketch works.
  std::vector<sketch::Entity> sketchClipboard;
  // Where the clipboard came from (paste-LINKED needs it): the source
  // sketch's feature id and the copied entity ids, in clipboard order.
  uint64_t sketchClipboardSourceSketch = 0;
  std::vector<uint32_t> sketchClipboardIds;

  // 2D sheet viewport (workstream 13): a drawing tab pans/zooms an
  // orthographic sheet, never the orbit camera (there is no 2D camera today —
  // sketch mode aligns the perspective camera to a plane; a sheet wants none
  // of that). `offset` is the sheet-mm point drawn at the viewport center;
  // `scale` is pixels-per-mm (scroll-to-cursor zoom, middle-drag pan). Zeroed
  // scale means "fit on first paint".
  struct Sheet2DView {
    glm::dvec2 offset{0.0};
    double scale = 0.0;
    bool panning = false;
  } sheetView;

  // Projected view geometry for the active drawing tab, keyed by DrawingView
  // id (workstream 13, D2). Derived and transient — recomputed by
  // UpdateDrawingRenders on tab entry and explicit update_drawing, never
  // serialized. Cleared on tab switch (ResetActiveTabUiState).
  std::map<uint64_t, drawing::SheetView> drawingRenders;

  // Dimension-placement tool (D4): armed from the panel with a kind, then the
  // user clicks projected edges/circles on the sheet to pick refs; the dim is
  // created once enough are picked. All refs must share one view.
  struct DimTool {
    bool active = false;
    DrawingDimKind kind = DrawingDimKind::Distance;
    uint64_t viewId = 0;        // the view the first pick fixed
    std::vector<ProjRef> refs;  // picked so far
  } dimTool;
  uint64_t dimDrag = 0;  // id of the dimension whose label is being dragged

  SketchEditState sketchEdit;
  SketchDragSession sketchDrag;
  EdgePickState edgePick;
  SlotPickState slotPick; // U3: armed dialog pick slot
  MeasureState measure;
  SimulationState simulation;
  // The active topology-optimization run (TO2), or null when none is running.
  // A minutes-long SIMP solve on a worker thread; the main thread polls it and
  // stores the result at completion. One run at a time. See ui::TopoJob and the
  // Start/Poll/Cancel TopologyRun helpers.
  std::unique_ptr<TopoJob> topoJob;
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
  // One-shot launch check that re-docks panels a stale imgui.ini (saved
  // before the panel existed) left floating — see BeginDockspace.
  bool dockHomesEnforced = false;
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
