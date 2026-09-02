#pragma once

#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/cam/CamJob.h"
#include "core/cam/CamSim.h"      // the cutting simulation cursor + stock
#include "core/cam/CamToolpath.h" // cam::FaceRegion (the ghost/hover cache)
#include "core/cam/CamVerify.h"   // cam::VerdictLine (the G4 verdict, in words)
#include "core/cam/JobDocument.h" // the job tab: parts placed + setups
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingRender.h"
#include "core/features/SketchFeature.h"
#include "core/io/GitVault.h"
#include "core/io/ProjectMerge.h"
#include "core/io/ProjectStamp.h"
#include "core/io/Settings.h"
#include "core/sketch/SketchInference.h"
#include "core/sketch/SketchSolver.h"
#include "render/ClipPlanes.h"
#include "render/FrameTimer.h"
#include "ui/ImportJob.h"
#include "ui/SyncJob.h"
#include "ui/TopoJob.h"
#include "ui/ViewportHandle.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

class Camera;
class MeshRenderer;
class OutlineRenderer;
class RayTracingRenderer;
class SketchRenderer;
class UnderlayRenderer;
class FieldRenderer;
class SceneRenderer;
class StockRenderer;
class FaceSelector;
class IBLTextures;
struct GLFWwindow;

namespace ui {

// Per-part tessellation + GPU asset cache; see AppState::partRenderCache.
// Defined in AppActionsInternal.h, where Vertex3D is complete.
struct PartRenderCacheStore;

struct GeometryChange {
  bool valid = false;
  double added = 0.0;   // volume in the new that was not in the old
  double removed = 0.0; // volume in the old that is not in the new
  double oldVolume = 0.0, newVolume = 0.0;
  std::string summary; // one line, already in plain language
};

struct TerminalState; // ui/TerminalPanel.cpp — keeps libvterm out of here

// A document body mirrored into the renderers. For a part tab, one per body.
// For an assembly tab, one per (occurrence × referenced-part body): meshId is
// the shared asset's instance, occurrenceId ties the hit back to the placement.
struct DocumentRenderMesh {
  uint32_t meshId;
  uint32_t outlineId;
  size_t bodyIndex; // index into the source part's GetBodies() (element names)
  uint64_t occurrenceId = 0; // assembly: the occurrence; 0 for a part body
  // Where this mesh sits INSIDE its occurrence (A5). Identity when the
  // occurrence places a part — the mesh is the part. For a leaf inside a
  // sub-assembly it is the chain from the unit's frame down to that leaf, so
  // a live drag can move the unit without collapsing its contents onto the
  // unit's origin (which is exactly what it did before this existed).
  glm::mat4 localXf{1.0f};
  // Which PART this mesh's geometry came from, and where that leaf sits in
  // the occurrence's tree. For a direct part occurrence: the occurrence's own
  // part and an empty path. For a leaf inside a sub-assembly these are the
  // only way a viewport pick can name what it hit — the occurrence resolves
  // to an assembly, which has no bodies and no faces of its own.
  uint64_t leafPartId = 0;
  std::vector<uint64_t> innerPath;
  // B-Rep edge polylines (world space, edgeIndex order — the ElementMap /
  // PickEdge convention). Cached here for edge highlighting.
  std::vector<std::vector<glm::vec3>> edgePolylines;
};

enum class ViewportDrag { None, Orbit, Pan, Zoom, Roll };

// Why a sketch session exists. Hole = the session is collecting hole centres
// for the Hole tool, so ✓/Enter hand off to BeginSketchHole instead of just
// exiting, and Escape discards the sketch the tool created if the user drew
// nothing in it. Freeform = an ordinary sketch (never auto-discarded).
enum class SketchPurpose { Freeform, Hole };

enum class SketchTool {
  Select,
  Point,
  Line,
  Circle,
  Rectangle,
  Trim,
  Arc,             // 3-click center → start → end
  ArcThreePoint,   // 3-click start → end → through-point
  Ellipse,         // 3-click center → major-axis end → minor radius
  Spline,          // click to add control poles; Enter/double-click finishes
  Mirror,          // reflect the current selection across a clicked axis line
  Fillet,          // round the clicked corner where two lines meet
  Pattern,         // linear/circular array of the current selection
  Offset,          // offset the selected closed loop by a distance
  Dimension,       // clicks accumulate targets; the tool infers the dim kind
  RectangleCenter, // 2-click rectangle: centre, then a corner
  Project,         // click a body edge/face -> live projected construction geom
  CalibrateImage,  // click two points on a reference image, then type the real
                   // distance between them to scale the image (v49)
  EllipseArc,      // 5-click: centre, major-axis end, minor radius, start, end
  Image            // U8 polish: reference-image session (insert/opacity/
                   // calibrate/delete for selectedImageId) in the Tool window
};

// Transient state for the active sketch-editing session. The sketch itself
// lives in the Document (as a SketchFeature); this only holds which sketch is
// being edited (by stable id, so it survives undo reindexing), the current
// tool, and the in-progress two-click drawing state.
struct SketchEditState {
  bool pickingPlane = false;   // "New Sketch" armed, waiting for a plane/face
  uint64_t activeSketchId = 0; // 0 = not editing a sketch
  SketchTool tool = SketchTool::Select;
  // What this session is for. NOTE: BeginSketch() resets this whole struct,
  // so the purpose is passed to BeginSketchOn*() as a PARAMETER and applied
  // by BeginSketch AFTER its own reset — never stashed across the call.
  // Every other reset site (tab switch, undo of the sketch, opening another
  // sketch, Shift+E, File/New) SHOULD clear it, and does, for free.
  SketchPurpose purpose = SketchPurpose::Freeform;
  bool hasPendingPoint = false; // first click of a two-click entity placed
  glm::vec2 pendingPoint{0.0f};
  bool hasPendingPoint2 = false; // second click of a 3-click tool (arcs)
  glm::vec2 pendingPoint2{0.0f};
  // Third/fourth clicks — only the 5-click elliptic-arc tool uses these
  // (centre, major-axis end, minor radius, start; the 5th click commits).
  bool hasPendingPoint3 = false;
  glm::vec2 pendingPoint3{0.0f};
  bool hasPendingPoint4 = false;
  glm::vec2 pendingPoint4{0.0f};
  glm::vec2 hoverPoint{0.0f};
  // Spline tool: control poles placed so far. Committed as a BSpline on Enter
  // or a double-click (>= 2 poles); Escape or a tool switch clears them.
  std::vector<glm::vec2> splinePoles;

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
  // Second grid direction (v50): count2 = 1 keeps the pattern 1D.
  int patternCount2 = 1;
  glm::vec2 patternStep2{0.0f, 2.0f};
  glm::vec2 patternCenter{0.0f}; // circular centre
  float patternAngleDeg = 60.0f; // circular step angle
  // Circular centre picking (v51): armed by the session's Pick button — the
  // next canvas click sets the centre; hitting an entity point stores its
  // REFERENCE so the pattern tracks it.
  bool patternPickCenter = false;
  sketch::PointRef patternCenterRef{0, 0};

  // Offset tool params (applied to the selected closed loop).
  float offsetDistance = 0.5f;
  bool offsetJoinArc = true; // arcs at convex corners (else extend/trim)

  // Transient feedback shown in the toolbar (e.g. why a fillet no-op'd), so
  // operations that can silently fail tell the user what happened.
  std::string actionMsg;
  int hoveredPlane = -1;       // 0 Front / 1 Top / 2 Right while picking
  uint64_t hoveredDatumId = 0; // datum plane under the cursor while picking
  // When non-zero while pickingPlane: re-pick the attached face of this sketch
  // (its faceRef) instead of starting a new sketch. Set by "Reattach face…".
  uint64_t reattachSketchId = 0;

  // Sketch-entity selection (for adding constraints), ordered.
  std::vector<sketch::PointRef> selected;

  // Same-spot click cycling in the Select tool (the U1 model-space
  // convention): the last click position (screen px) and the candidate index
  // a repeated click at that spot advances.
  glm::vec2 lastClickPos{-1e9f, -1e9f};
  int pickCycle = 0;

  // Dimension tool: parallel to `selected` while it is armed — true when the
  // accumulated target was picked as a whole line (body), false for a point.
  std::vector<bool> dimPickBody;

  // Dimension placement stage (Phase 3.7): armed once the tool's targets
  // resolve; a ghost dimension follows the cursor until a click commits the
  // kind + labelOffset. For a point PAIR (`dimPlacePair`) the kind switches
  // live by cursor zone (horizontal / vertical / aligned, the mainstream CAD
  // behavior); fixed-kind targets (radius, point-line) place the label only.
  bool dimPlacing = false;
  bool dimPlacePair = false;
  sketch::ConstraintKind dimPlaceKind = sketch::ConstraintKind::Distance;
  sketch::PointRef dimPlaceA, dimPlaceB;
  double dimPlaceValue = 0.0; // fixed-kind value (pairs recompute at commit)
  // Non-zero: the placement was entered from a SINGLE line (its length) —
  // a placement click on another line/point REFINES the kind instead of
  // placing (parallel line → DistancePL, non-parallel → angle, point → PL).
  uint32_t dimPlaceFromLine = 0;

  // Click-selected constraint (dimension pill or constraint glyph; -1 =
  // none). Delete/Backspace removes it. Selecting a constraint clears the
  // entity selection and vice versa, so Delete is never ambiguous.
  int selectedConstraint = -1;
  // Constraint glyph under the cursor (constraint index, -1 = none),
  // refreshed each frame by DrawConstraintGlyphs and read by the canvas one
  // frame behind (the dimHot convention). The glyph owns clicks in its rect.
  int glyphHot = -1;

  // Dimension-on-create: the entities the last draw-tool commit produced (a
  // rectangle keeps its 4 sides, everything else one id). Typing a digit
  // while the tool is live opens a value field that applies a driving
  // dimension to them; rectangles take two numbers (stage 0 = width next,
  // 1 = height). Cleared on tool switch.
  std::vector<uint32_t> lastDrawn;
  int lastDrawnStage = 0;
  bool dimOnCreateOpen = false; // the value field is up

  // Reference-image tools (v49). `selectedImageId` is the image the Images
  // panel acts on (opacity/delete/calibrate). The CalibrateImage tool captures
  // two plane-UV points, then the panel shows a distance field:
  //   calibStage 0 = awaiting the first point, 1 = awaiting the second,
  //   2 = both picked, awaiting the typed distance (Apply).
  uint32_t selectedImageId = 0;
  // Derived-op edit session (U8 polish): index into the sketch's `derived`
  // list whose editor is open in the Tool window (-1 = none). Opened by
  // double-clicking an Operations row in the Model panel.
  int editOpIndex = -1;
  // Re-pick modes within the op edit session: sources (collect-phase toggle/
  // box, tap/Enter commits) and — mirror only — the axis (one click).
  bool opPickSources = false;
  bool opPickAxis = false;
  // Entity whose collapsed constraint-badge stack is hover-expanded this
  // frame (0 = none). Entities with >2 badges draw one "xN" summary; hovering
  // it fans the individual badges out (they stay while the cursor is over
  // the fan).
  uint32_t glyphExpandedEntity = 0;
  uint32_t calibImageId = 0;
  glm::vec2 calibP1{0.0f, 0.0f};
  glm::vec2 calibP2{0.0f, 0.0f};
  // Drag-to-move for the selected reference image (SketchTool::Image): the
  // grab point (plane UV) and the image's origin at grab. Commits like the
  // opacity slider: baseline at grab, one command on release.
  bool imageDragActive = false;
  glm::vec2 imageDragGrabUV{0.0f, 0.0f};
  glm::vec2 imageDragStartOrigin{0.0f, 0.0f};
  int calibStage = 0;
  bool calibAlignAxis = false; // also rotate so p1->p2 lands on the U axis
  float calibDistance = 10.0f; // the typed real-world distance
  // The image feature as it was when an opacity drag STARTED. Opacity was a
  // live cosmetic edit with no undo entry at all — which is wrong for a
  // persisted field, and worse than it sounds: the slider writes every frame,
  // so a naive fix pushes one command per frame of the drag. Baseline at
  // grab, one ModifyFeatureCommand on release (the HandleEdit recipe from
  // docs/ui/direct-manipulation.md). Empty when no drag is in progress.
  nlohmann::json imageEditBaseline;

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
  enum class Mode { Point, LineBody, SketchTranslate, CircleRim };
  bool active = false;
  Mode mode = Mode::Point;
  std::unique_ptr<sketch::Solver> solver;
  // Dragged points (Point / LineBody / SketchTranslate).
  std::vector<sketch::PointRef> points;
  std::vector<glm::vec2> initialPos; // their UV positions at mouse-down
  glm::vec2 initialCursor{0.0f};
  uint32_t circleId = 0;        // CircleRim
  glm::vec2 circleCenter{0.0f}; // CircleRim: radius = |cursor - centre|
  size_t featureIndex = 0;      // sketch index for the commit
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
  std::string prompt; // status pill while armed
  // Why the last pick did not land (empty = nothing to report). An armed
  // tool used to swallow a click on empty space or the wrong kind of thing
  // in complete silence; the tool window shows this under the slot rows and
  // the next good pick clears it (U10).
  std::string miss;
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
    // WHAT was picked. Faces and edges are named elements of a body; a Body
    // or an Occurrence is a whole placed shape, and "how far apart are these
    // two parts" is the most natural question in an assembly — one the
    // engine has answered since I4 while the tool refused the pick.
    enum class Kind : uint8_t { Face, Edge, Body, Occurrence, Corner };
    Kind kind = Kind::Face;
    // Face/Edge: the stable element name. Body: {createdBy, ""}. Occurrence:
    // {0, ""} — the occurrence id below is its whole identity.
    ElementRef ref;
    // Corner only: the SECOND edge. A corner has no stable name of its own —
    // the ElementMap names faces and edges, not vertices — so it is named by
    // the two edges meeting there, both of which do have one.
    ElementRef ref2;
    // Assembly tabs: the occurrence the element was picked on (0 = part
    // context). Part of the element's identity — the same face on two
    // occurrences of one part is two distinct measurables.
    uint64_t occurrenceId = 0;

    bool IsEdge() const { return kind == Kind::Edge; }
    bool IsCorner() const { return kind == Kind::Corner; }
    // A whole placed shape rather than one named element of one.
    bool Whole() const {
      return kind == Kind::Body || kind == Kind::Occurrence;
    }
    bool operator==(const Element& o) const {
      return kind == o.kind && ref.featureId == o.ref.featureId &&
             ref.name == o.ref.name && ref2.name == o.ref2.name &&
             occurrenceId == o.occurrenceId;
    }

    // Named constructors, because the four kinds fill different fields and a
    // brace-initialised {ref, bool, id} said nothing about which.
    static Element OfFace(const ElementRef& r, uint64_t occ = 0) {
      return {Kind::Face, r, ElementRef{}, occ};
    }
    static Element OfEdge(const ElementRef& r, uint64_t occ = 0) {
      return {Kind::Edge, r, ElementRef{}, occ};
    }
    static Element OfBody(uint64_t createdBy) {
      return {Kind::Body, ElementRef{createdBy, ""}, ElementRef{}, 0};
    }
    static Element OfOccurrence(uint64_t id) {
      return {Kind::Occurrence, ElementRef{0, ""}, ElementRef{}, id};
    }
    static Element OfCorner(const ElementRef& a, const ElementRef& b,
                            uint64_t occ = 0) {
      return {Kind::Corner, a, b, occ};
    }
  };
  std::vector<Element> picked;
  std::string readout; // formatted result ("" = nothing measured yet)
  // Nearest-distance line between two picked elements, drawn in the
  // viewport (set alongside the two-element readout).
  bool hasDistanceLine = false;
  glm::vec3 distA{0.0f};
  glm::vec3 distB{0.0f};

  // The DRAWN measurement (M2). Everything below is derived in
  // UpdateMeasureReadout alongside the text, so the viewport and the panel
  // cannot disagree about what was measured — the number and the picture are
  // one computation, not two.
  //
  // Before this the whole visualization was a 2 px line between OCC's extrema
  // pair, with the value in a panel across the screen: for two parallel
  // 600 mm edges 45 mm apart that draws a stub at one far corner, and for a
  // plate's thickness it draws INSIDE the solid where the depth test hides
  // it.
  struct Dimension {
    bool valid = false;
    glm::vec3 a{0.0f}, b{0.0f}; // the dimension line's ends (arrow tips)
    // Where the elements actually are, when `a`/`b` were re-anchored off the
    // extrema pair: witness lines run from these to the arrow tips.
    glm::vec3 witnessA{0.0f}, witnessB{0.0f};
    bool hasWitness = false;
    std::string label; // "45 mm" — drawn at the dimension's midpoint

    // The angle, as an arc from `arcFrom` to `arcTo` about `arcAt`. Drawn
    // between the DIRECTIONS the angle was actually taken between (face
    // normals for two planar faces, the edges themselves for two lines), so
    // the picture shows the same basis the readout names.
    bool hasArc = false;
    glm::vec3 arcAt{0.0f};
    glm::vec3 arcFrom{0.0f}, arcTo{0.0f}; // unit directions
    std::string arcLabel;
  };
  Dimension dimension;

  // M3 — measuring before you commit to it. The element under the cursor is
  // measured live against what is already picked and drawn dimmed, so you
  // can sweep a face and read the gap without a click, and see what a click
  // is about to give you. With nothing picked yet it reads out the hovered
  // element's own measure, which is the cheapest possible "what is this".
  struct Hover {
    bool has = false;    // something measurable is under the cursor
    Element element;     // what it is (set by the selection pipeline)
    std::string readout; // one line, drawn at the cursor
    Dimension dimension; // the same picture, dimmed
    // The measurement is an OCC extrema call. Recomputing it every frame for
    // a stationary cursor is pure waste, so the result is cached against
    // everything it depends on: the hovered element, what it is being
    // measured against, and the document version.
    std::string cacheKey;
  };
  Hover hover;

  // WHICH distance the live pair reports (M4). Minimum is what every CAD
  // system defaults to and the only one this used to offer; maximum answers
  // the clearance question and centres answers hole spacing.
  Measure::DistanceKind distanceKind = Measure::DistanceKind::Minimum;

  // Pinned measurements. Comparing three gaps used to mean writing two of
  // them down: the tool held one pair and replaced it. A pin moves the
  // current measurement into this list and clears the picks for the next
  // one; pinned measurements keep drawing until dismissed.
  //
  // They are SESSION state, like the picks — not saved with the document.
  // A measurement is a question you asked, not a property of the part.
  struct Pinned {
    uint64_t id = 0;
    std::vector<Element> elements; // what it measures (1 or 2)
    std::string readout;
    Dimension dimension;
    Measure::DistanceKind kind = Measure::DistanceKind::Minimum;
  };
  std::vector<Pinned> pinned;
  uint64_t nextPinnedId = 1;
  // The document version the pinned list was last recomputed against. Each
  // pin is an OCC extrema call; redoing them every frame for a model nobody
  // touched would be sixty of those a second per pin.
  uint64_t pinnedVersion = UINT64_MAX;
  // The pinned row under the cursor in the tool window, so hovering a row
  // says which measurement in the viewport it is. 0 = none.
  uint64_t hoveredPinned = 0;
};

// Name field for "make a variable from this measurement" (M6). In AppState
// rather than a static in the draw function so the between-test reset clears
// it, like every other tool-window buffer.

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

  // Dragging a force arrow in the viewport to change its MAGNITUDE
  // (docs/ui/viewport-handles.md — this owner keeps its own grab payload,
  // like the gizmo and the section arrow). Snapshot at grab, mutate live,
  // one ModifyStudyCommand on release.
  struct LoadDrag {
    bool active = false;
    uint64_t loadId = 0;
    double startMagnitude = 0.0;
    float startParam = 0.0f;
    std::string beforeJson; // the study at grab, for the undo entry
  };
  LoadDrag loadDrag;
  // Params applied to the load placed by the next face pick. The direction
  // is only used when `directionOverridden` — otherwise the load takes the
  // inward normal of the face that is clicked (fem::DefaultForceDirection),
  // because a direction is a default with a unit and belongs to the face, not
  // to the world. The magnitude is seeded from the part's own weight when the
  // tool is armed (SimulationArmTool): a 0 N force is a load that silently
  // does nothing, which is the class docs/ui/u10-defaults-and-refusals.md
  // exists to stop.
  double pendingMagnitude = 0.0;
  // ...and the expression that produced it, when one was typed, so a load
  // placed from "part_weight*2" stays driven by the variable instead of
  // collapsing to the newtons it happened to evaluate to (u14).
  std::string pendingMagnitudeExpr;
  glm::dvec3 pendingDirection{0.0, -1.0, 0.0};
  bool directionOverridden = false;

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
  bool active = false;  // a render is in flight
  bool started = false; // BeginStill has run (internal)
  bool cancel = false;  // set by the UI to abort
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t totalSamples = 128;
  uint32_t done = 0; // samples traced so far
  uint32_t seed = 1;
  bool groundPlane = true; // shadow-catcher plane under the scene
  // A neutral sweep behind the model rather than the HDRI photograph. The
  // HDRI still lights the model and still fills its reflections.
  bool studioBackdrop = true;
  std::string outPath;
  // Result (valid once !active after having been started)
  bool finished = false; // a render completed (success or failure)
  bool success = false;
  std::string error;
  std::string resultPath;
  // Set when the finished picture is nearly one flat colour — which for a
  // still means the camera saw no geometry. A render that framed nothing
  // used to report a confident "Saved".
  std::string warning;
};

// All state the UI reads and writes, replacing main.cpp's globals. The
// document and undo stack are owned here (they are swapped by File > New /
// Open); everything else is a non-owning pointer to objects whose lifetime
// main.cpp manages alongside the Vulkan context.
// The kind of document a project tab holds (A2/drawings/CAM jobs). A part is
// a feature list that builds bodies; an assembly is an occurrence list that
// places other parts; a drawing projects a part or assembly onto a 2D sheet;
// a job says how parts are cut out of a piece of stock
// (docs/cam/job-document.md).
enum class DocKind { Part, Assembly, Drawing, Job };

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
  // Action-clock stamps parallel to undo/redo (Phase V), so the global Undo
  // can compare this tab's most recent edit against a project-variable edit.
  std::vector<uint64_t> undoStamps;
  std::vector<uint64_t> redoStamps;
};

// Snapshot-based undo for a drawing tab (the AssemblyHistory pattern): a
// drawing is a small setup object (sheet + a few views/dims), so each edit
// pushes a full copy rather than generalizing the Document-only Command stack.
struct JobHistory {
  std::vector<cam::JobDocument> undo;
  std::vector<cam::JobDocument> redo;
};

struct DrawingHistory {
  std::vector<DrawingDocument> undo;
  std::vector<DrawingDocument> redo;
  std::vector<uint64_t> undoStamps; // action-clock stamps (Phase V)
  std::vector<uint64_t> redoStamps;
};

// Project-wide variable undo (Phase V). A project-variable edit spans every
// tab (it re-evaluates the shared project symbols and regenerates all parts),
// so it can't live on any one document's CommandStack. Each entry snapshots the
// project parameter table to restore when applied, plus — for a rename, which
// also rewrote expressions inside every part — the reference rewrite to replay
// across all documents. Stamped from the shared action clock so the global
// Undo picks the most recent mutation. See AppActions project-var helpers.
struct ProjectVarSnapshot {
  std::vector<Parameter> params; // the project table to restore when applied
  uint64_t stamp = 0;
  // When set, applying this entry first rewrites `docRefFrom` -> `docRefTo` in
  // every part's feature/parameter expressions (a project-var rename's ref
  // rewrite; empty for set/delete).
  std::string docRefFrom, docRefTo;
};
struct ProjectVarHistory {
  std::vector<ProjectVarSnapshot> undo;
  std::vector<ProjectVarSnapshot> redo;
};

struct PartSlot {
  uint64_t id = 0; // stable per-document id (A2): what occurrences reference
  std::string name;
  // Display grouping for the bottom tab bar ("" = top level). Assembly
  // import puts its extracted parts in a folder named after the assembly so
  // a 100-part import is one flyout, not 100 tabs. Cosmetic only — storage
  // stays flat, ids/references are unaffected.
  std::string folder;
  DocKind kind = DocKind::Part;
  // Has this tab ever been framed? A camera is global, not per-tab, so
  // switching from a 20 mm bracket to a 5 m dock used to leave you INSIDE the
  // dock — an assembly full of parts looking like an empty scene. The first
  // time a tab is shown it gets a fit; after that its framing is yours.
  bool everFramed = false;
  std::unique_ptr<Document> doc; // part: null while this tab is active
  std::unique_ptr<AssemblyDocument> assembly; // assembly: null while active
  std::unique_ptr<DrawingDocument> drawing;   // drawing: null while active
  std::unique_ptr<cam::JobDocument> job;      // job: null while active
  std::unique_ptr<CommandStack> commands;     // null while active (parts only)
  AssemblyHistory history;       // assembly undo/redo (stashed here)
  DrawingHistory drawingHistory; // drawing undo/redo (stashed here)
  JobHistory jobHistory;         // job undo/redo (stashed here)
  // Projected view geometry for a stashed drawing tab (D2). Kept across tab
  // switches so re-entering a drawing doesn't silently re-project — staleness
  // is only cleared by an explicit update (the FEM rule).
  std::map<uint64_t, drawing::SheetView> drawingRenders;
  // The drawing document's version the renders above were built at, so
  // UpdateDrawingRenders can skip views that cannot have changed (U11).
  uint64_t drawingRendersVersion = 0;
  uint64_t lastSavedVersion = 0; // per-document dirty tracking
  // Assembly tabs only (A4): referenced parts' Document::Version() at this
  // assembly's last solve, stashed while inactive. See AppState field.
  std::unordered_map<uint64_t, uint64_t> solvedPartVersions;
};

// THE selection (U1, docs/ui/u1-selection.md): one typed, ordered set that
// every context reads and writes. Items are stable across regeneration —
// features/occurrences/mates by id, geometry by stable element name — never
// by mesh/face index (mesh ids change on every regen). Renderer highlights
// are a per-frame projection of this set, not parallel state.
// One piece of a fastener joint and where it goes (docs/parts/preview-plan.md).
//
// Here rather than in AssemblyActions.h — which is where it belongs by
// subject — because the TOOL's own state holds a plan and the ghost draws it,
// and AssemblyActions.h includes this header rather than the reverse.
//
// The fit worked all of this out and then used it only at commit time, so a
// dry run reported the SPEC but not the PLACEMENT: an answer missing the half
// a preview needs, and a slightly dishonest one for an agent asking "what
// would this do".
struct PlannedPart {
  std::string standard; // "iso4762", "iso7089", "iso4032"
  std::string size;     // "M12"
  double length = 0.0;  // 0 for a nut or a washer
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
  // Which occurrence this piece RIDES ON, so it follows when that part moves
  // (docs/parts/f1-loose-ends.md). The fit decides it rather than the
  // placement guessing after the fact: the screw and the washer under its
  // head bear on the part the head sits against, the nut and its washer on
  // the LAST part in the stack. Attaching the far side to the near part would
  // stretch a rigid group across a joint that is allowed to open.
  uint64_t ridesOn = 0;
};

struct SelectionItem {
  enum class Kind : uint8_t {
    Face,
    Edge,
    Body,
    Feature,
    Occurrence,
    Mate,
    SketchRegion, // U2
    Connector     // U7c: mate connector — id = occurrence, element = face
  };
  Kind kind = Kind::Feature;
  uint64_t id = 0;    // Feature/Occurrence/Mate id; Body: its createdBy
  ElementRef element; // Face/Edge/SketchRegion: {featureId, stable name}
  // Connector only: the snapped edge (hole center / edge midpoint) refining
  // the face anchor. Empty for every other kind.
  ElementRef snapElement;
  // Connector only: the SECOND edge of a corner snap (v65) — a corner is
  // where two named edges meet, and needs both to be found again. Empty
  // unless the snap is a corner.
  ElementRef snapElement2;
  // Connector only (A5): the occurrence ids inside `id` down to the leaf
  // whose face this is. Empty when the occurrence places that part directly
  // — which is every pick that is not inside a sub-assembly.
  std::vector<uint64_t> innerPath;

  // Which JOB PART this pick landed on, when the tab is a job (v76). 0
  // everywhere else. Two parts in one job can own the same feature id AND
  // the same element name -- a job made of one part machined twice is the
  // easy case -- so the ElementRef alone names two faces, and resolving it by
  // search picks whichever comes first. That is the wrong bracket, cut for
  // real. Not part of operator==: it is where the pick HAPPENED, and the
  // same face picked twice is the same selection.
  uint64_t jobPartId = 0;

  bool operator==(const SelectionItem& o) const {
    return kind == o.kind && id == o.id &&
           element.featureId == o.element.featureId &&
           element.name == o.element.name &&
           snapElement.name == o.snapElement.name &&
           snapElement2.name == o.snapElement2.name && innerPath == o.innerPath;
  }
};

// The bit a Kind occupies in a selection-filter mask (SlotSpec::kinds,
// SlotPickState::kinds, the pipeline's candidate filter).
constexpr uint32_t SelectionKindBit(SelectionItem::Kind k) {
  return 1u << static_cast<int>(k);
}

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
  // V3 render_sheet capture in flight: viewport overlays (the view cube)
  // hide so the composited tiles show geometry only. Driven per-frame by
  // RemoteServer::ProcessPending.
  bool sheetCapture = false;

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
  // Active job tab: the CAM setups used to live on the part Document, which
  // made every job about one part (docs/cam/job-document.md).
  std::unique_ptr<cam::JobDocument> job;
  JobHistory jobHistory;
  // The active job's assembled bodies and the key they were built at
  // (ui::JobBodies). Derived, never serialized: this is where
  // `document->GetBodies()` used to be read from, and some of those reads are
  // per-frame.
  std::vector<Body> jobBodies;
  uint64_t jobBodiesKey = 0;
  // …and the same list laid out in the active setup's frame
  // (ui::JobPlacedBodies): what the viewport draws and what a pick resolves
  // against.
  std::vector<Body> jobPlacedBodies;
  uint64_t jobPlacedKey = 0;
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
  // Tab-bar folder registry, in display order. THE list of folders: lets a
  // folder exist empty ("+ > New folder" before anything is filed in it, or
  // after its last member moves out). Slot folders self-register on
  // load/import; only Dissolve removes an entry. Persisted in project.json.
  std::vector<std::string> tabFolders;

  // Project-wide variables (Phase V, docs/dogfooding.md, caster round). THE
  // project scope: an ordered parameter table shared by every part, and its
  // evaluated symbols. Every slot's Document borrows &projectSymbols
  // (SetProjectSymbols); a feature expression resolves local-first then here.
  // set_variable defaults to this scope. Persisted in project.json (multi-part)
  // or the lone part file (single-part = the whole project). projectSymbols is
  // derived — re-evaluated by ui::SyncProjectScope on every project-var edit.
  std::vector<Parameter> projectParameters;
  SymbolTable projectSymbols;
  ProjectVarHistory projectVarHistory; // cross-tab undo for project-var edits

  // ImGui::IsAnyItemActive() sampled once per frame by the frame loop, right
  // before SyncDocumentRenderers reads it. The field exists so the actions
  // layer can gate on "a widget is being dragged" without including ImGui —
  // see ui/Overlays.h and docs/architecture-review-2026-08-24.md.
  bool anyWidgetActive = false;

  // Non-owning
  Camera* camera = nullptr;
  MeshRenderer* meshRenderer = nullptr;
  OutlineRenderer* outlineRenderer = nullptr;
  RayTracingRenderer* rayTracer = nullptr;
  SketchRenderer* sketchRenderer = nullptr;
  UnderlayRenderer* underlayRenderer = nullptr; // sketch reference images (v49)
  FieldRenderer* fieldRenderer = nullptr;       // FEM result overlay
  StockRenderer* stockRenderer = nullptr;       // CAM cutting simulation
  // Every renderer that draws into the 3D scene, in DRAW ORDER — the same
  // list main.cpp renders from (docs/rendering/renderer-wiring.md). The typed
  // pointers above stay because the ui/ layer drives CONTENT through them
  // (AddMesh, SetInstanceGhost, SetMesh, SetStaticLines); this one exists so
  // the per-frame contract is a loop instead of a list nobody can forget to
  // add to.
  std::vector<SceneRenderer*> sceneRenderers;
  FaceSelector* faceSelector = nullptr;
  IBLTextures* ibl = nullptr;
  GLFWwindow* window = nullptr;

  // Mate creation tool (A3 phase 3): a toolbar button starts it with a mate
  // type; the user clicks two occurrence faces (connectors), then a preview
  // mate is added + solved live for accept/cancel. See AppActions mate-tool
  // helpers + OccurrencePanel.
  struct MateToolState {
    bool active = false; // picking connectors for a new mate
    MateType type = MateType::Fastened;
    bool haveFirst = false;     // the first connector is picked
    Connector first;            // occurrence + face anchor of the first pick
    uint64_t previewMateId = 0; // non-zero once both picked: the live preview
    // U7c: non-zero = the session re-picks THIS existing mate's connectors
    // (each pick rewrites a side + re-solves; no preview mate involved).
    uint64_t editMateId = 0;
    // Transient connector resolved under the cursor before a click. It uses
    // the same face/snap inference as MateToolPick but never affects a mate.
    bool hasHover = false;
    Connector hover;
    bool hoverSnapIsMidpoint = false;
    bool hoverSnapIsCorner = false;
    std::string message; // transient hint (e.g. "pick a named face")
  };

  // 3D drag gizmo for the selected occurrence (A2.5). World-aligned translate
  // arrows + rotate arcs; a drag mutates the occurrence transform live (cheap
  // per-instance update, no resync) and snapshots/solves once on release.
  struct GizmoDrag {
    bool active = false;
    int axis = -1;       // handle axis index (into the gizmo frame below)
    bool rotate = false; // false = translate along axis, true = rotate about it
    glm::vec3 startPos{0}; // occurrence position at grab (free drag)
    glm::quat startRot{1, 0, 0,
                       0}; // occurrence orientation at grab (free drag)
    float startParam =
        0.0f; // axis coordinate (translate) / angle (rotate) at grab
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
    uint64_t featureId = 0;  // the pending feature at grab (undo-safe check)
    bool rotate = false;     // revolve arc vs linear arrow
    glm::vec3 axisOrigin{0}; // drag line / arc-plane center at grab
    glm::vec3 axisDir{0, 0, 1};
    double startValue = 0.0; // the parameter at grab (Esc restores this)
    float startParam = 0.0f; // axis coordinate / arc angle at grab
    // Non-zero when the handle belongs to a SECTION rather than a pending
    // feature — sections are view state, so there is no feature to resolve
    // and no regeneration to run.
    uint64_t sectionId = 0;
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
    bool edgeHover =
        false; // hovered edge preview (faces hover via FaceSelector)
    uint32_t edgeHoverMesh = 0;
    uint32_t edgeHoverIndex = 0;
    // U2: hovered sketch region while a Profiles slot is armed — the outer
    // loop's entity ids (empty = none).
    std::vector<uint32_t> regionHoverIds;
    // The face a click would take, as a stable reference (G3): the renderer's
    // own hover is a mesh/index pair, which cannot say WHICH face of the
    // model it is, and a preview of "the region that would be cleared" has to
    // ask the B-Rep.
    ElementRef hoverFace;
    glm::vec2 lastClickPos{-1e9f, -1e9f};
    int cycle = 0;
  };
  PickState pick;
  MateToolState mateTool;
  GizmoDrag gizmoDrag;

  // Every draggable arrow/arc in the viewport this frame, from all three
  // owners (docs/ui/viewport-handles.md). Rebuilt each frame by the overlay
  // pass, then used for BOTH hover and click hit-testing — one list, so what
  // highlights is exactly what a click would grab.
  std::vector<ViewportHandle> viewportHandles;
  uint64_t hoveredHandle = 0; // 0 = nothing under the cursor
  HandleDrag handleDrag;
  // Feature being previewed (in the document + rendered, but NOT on the undo
  // stack until accepted). 0 = none. See PreviewFeature.
  uint64_t pendingFeatureId = 0;
  // Committed feature open in the Tool-window editor session (U7e: editing
  // is explicit — double-click / context-menu Edit — now that the always-on
  // Properties panel is gone). 0 = none. Transient; edits commit as usual.
  uint64_t editFeatureId = 0;
  ViewportDrag viewportDrag = ViewportDrag::None;
  // True when the drag was started with Alt+left (trackpad-friendly) rather
  // than the middle/right button; the drag then owns the left button, so the
  // click must not leak into picking.
  bool viewportDragLeft = false;
  // Which ImGuiMouseButton started the current viewport nav drag (so release
  // is detected on the same button). Meaningful only while viewportDrag !=
  // None. 0 = Left (Alt+left), 1 = Right, 2 = Middle.
  int viewportDragButton = 2;
  bool showPlanes = true; // visible by default; auto-hidden once bodies exist
  // The menu bar's top-level names, rebuilt every frame as it draws. The
  // ImGui test engine cannot enumerate menu-bar buttons (they live in an
  // internal child it does not gather), so this is what `menu_items` with no
  // path reports — recorded by the code that draws them, never guessed.
  std::vector<std::string> menuBarTop;

  // Screen rect where the floating tool window last drew (zero if it never
  // has). FitCameraToDocument keeps the model clear of it while a session is
  // open — fitting used to park geometry underneath the very window that
  // asked you to click it (U10). Whether a session IS open is asked of the
  // session state, not of this rect: the remote fit runs before the UI draws,
  // so the rect is always a frame behind.
  glm::vec4 toolWindowRect{0.0f}; // min.x, min.y, max.x, max.y

  // Help > Keyboard Shortcuts (Ctrl+/). In AppState rather than a static in
  // the draw function so the between-test reset can close it — a stray
  // floating window swallows the viewport clicks of every later GUI test.
  bool showShortcuts = false;
  // Help > About Designer. A flag rather than a bare ImGui popup id for the
  // same reason as showShortcuts, plus one specific to popups: OpenPopup()
  // called INSIDE a BeginMenu scope registers the id against the menu's
  // window, and the matching BeginPopupModal() outside it never finds it —
  // which is why About did nothing at all until 2026-08-19.
  bool showAbout = false;
  // What the renderer picked, for Help > About and bug reports. Filled in by
  // main() after device selection; empty in headless builds, which have no
  // renderer to report.
  std::string gpuName;
  std::string gpuApiVersion;
  // Key-light shadow map (S1) — View > Shadows; session state only (no file
  // format / golden impact).
  bool showShadows = true;
  // SSAO (S2) — View > Ambient Occlusion; session state only, like shadows.
  bool showAmbientOcclusion = true;
  // S3 quality settings (View > Render Quality, set_render_options RPC).
  // The main loop applies changes: target recreation behind wait-idle +
  // descriptor rebind.
  bool ssaoFullRes = false; // full-res AO target instead of half-res
  int shadowMapSize = 2048; // key-light shadow map texels (1024 | 2048)
  // Section views (I3, tool round): up to kMaxClipPlanes cuts at once, which
  // INTERSECT — a fragment survives only where every enabled section keeps
  // it, so two sections give a quarter cut.
  //
  // View state, not model state: not saved with the project (neither is the
  // camera), applies to whatever the active tab shows, and references no
  // geometry so nothing can dangle. Owned here rather than in the tool's draw
  // function so that an agent's set_section and the tool window are the same
  // thing — the menu this replaces kept its plane and offset in function
  // statics, and the two silently disagreed.
  struct SectionView {
    uint64_t id = 0;   // stable while the app runs; row identity for tests
    int basePlane = 0; // 0 = XY (cut along Z), 1 = XZ, 2 = YZ
    // The cut's SOURCE plane (P2). 0 = the base plane above; otherwise a
    // datum plane feature in the active part, resolved live — move the datum
    // and the cut follows it, which is the reason to reference one at all.
    uint64_t planeId = 0;
    // A planar face of a body, as a stable element reference (P2). Wins over
    // planeId when set. Also resolved live, so an edit that moves the face
    // moves the cut.
    ElementRef face;
    float offset = 0.0f; // signed distance along the source normal
    // Tilt, in degrees, about the source plane's own u and v axes. The plane
    // pivots about its offset point, so tilting does not also translate the
    // cut off the model.
    float tiltU = 0.0f;
    float tiltV = 0.0f;
    bool reverse = false; // keep the other side of the cut
    bool visible = true;  // unchecked = the row stays, the cut stops
  };
  std::vector<SectionView> sections;
  uint64_t nextSectionId = 1;
  // The Section tool window is open. The sections outlive it: closing the
  // tool does not undo the cut you set up.
  bool sectionToolOpen = false;
  // Which row owns the viewport drag arrow (0 = the first section).
  uint64_t sectionActiveId = 0;
  // Section cap fills (I3 v2, C4): the planes at the last cap rebuild, and
  // the cap instance ids (separate from documentMeshes so body↔index
  // alignment holds; removed on the next resync). A cap rebuild is triggered
  // when the planes change and no widget is being dragged (so caps settle on
  // release while the shader clip previews live).
  ClipPlaneArray syncedSectionPlanes = NoClipPlanes();
  std::vector<uint32_t> sectionCapMeshes;
  // Topology shape-guide ghost (TO4): a translucent render of a topology
  // study's optimized iso-surface, shown in the normal modeling workspace so
  // the user can re-model against it. `topoGhostStudyId` selects the study
  // (0 = off); `topoGhostInstance` is its MeshRenderer instance (rebuilt on
  // resync, like the section caps).
  uint64_t topoGhostStudyId = 0;
  uint32_t topoGhostInstance = 0;
  // Geometry-delta overlay (compare_geometry's `materialize`, and F11's
  // "Compare shape"): the two boolean-diff solids drawn over the part, so
  // ONE look answers "where did it change?" instead of eyeballing a number
  // or two screenshots. Red = in the reference and not here, green = here
  // and beyond the reference — the same sense as the missing/extra volumes.
  //
  // Transient by construction: never saved, never picked, not in
  // documentMeshes, and dropped as soon as it stops being TRUE — when the
  // active document changes (`deltaOwner`) or when the part is edited out
  // from under it (`deltaVersion`). A diff belongs to the exact shape it was
  // measured on; one that outlives it is a picture of something else.
  // Version() restarts at 0 on document replacement, hence both keys.
  CADShape deltaMissingShape, deltaExtraShape;
  const Document* deltaOwner = nullptr;
  uint64_t deltaVersion = 0;
  uint32_t deltaMissingInstance = 0, deltaExtraInstance = 0;
  std::string deltaSummary; // what the comparison said, for the viewport hint
  // The units the ACTIVE document is read in (core/Units.h). An assembly tab
  // has no `document` at all — and the BOM, which is where masses are shown,
  // is drawn on exactly that tab — so the assembly's own system wins there.
  units::System ActiveUnits() const {
    if (assembly)
      return assembly->Units();
    return document ? document->Units() : units::System{};
  }

  bool HasGeometryDelta() const {
    return deltaMissingShape.IsValid() || deltaExtraShape.IsValid();
  }
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
  // A popup a keyboard binding asked for, opened by the toolbar that owns it.
  // ImGui::OpenPopup builds its id from the ID stack of the CALLING window,
  // and the key dispatcher runs outside every window — calling it there opens
  // a popup no BeginPopup will ever match. The command records the name; the
  // owning toolbar takes it on its next frame (ui::TakePopupRequest).
  std::string popupRequest;
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
  // The drawing document's version those renders were built at, so
  // UpdateDrawingRenders can skip views that cannot have gone stale — HLR is
  // half a second per view, and export re-projected everything every time
  // (U11 scale round). Stashed with the renders, per tab.
  uint64_t drawingRendersVersion = 0;

  // Dimension-placement tool (D4): armed from the panel with a kind, then the
  // user clicks projected edges/circles on the sheet to pick refs; the dim is
  // created once enough are picked. All refs must share one view.
  struct DimTool {
    bool active = false;
    DrawingDimKind kind = DrawingDimKind::Distance;
    uint64_t viewId = 0;       // the view the first pick fixed
    std::vector<ProjRef> refs; // picked so far
  } dimTool;

  // The fastener session (F1 p6, docs/parts/gui-plan.md). One pick and one
  // choice: the rim says which hole and which way round, the standard says
  // what kind of screw, and the size and length derive from the geometry.
  //
  // It STAYS OPEN across placements and re-arms the rim pick, because holes
  // come in patterns and a flange of eight bolts should be eight clicks. That
  // differs from the mate session, which ends when both connectors are
  // filled — a deliberate difference, not an oversight.
  struct FastenerTool {
    bool open = false;
    // The picked rim. `element` unset means nothing is picked yet, which is
    // the state the tool opens in and the one that must refuse VISIBLY (U10).
    uint64_t rimOccurrence = 0;
    ElementRef rimElement;
    std::string standard = "iso4762";
    std::string nut; // "" = no nut
    bool washerUnderHead = false;
    bool washerUnderNut = false;
    // The last dry run: what Place would do, or why it cannot. Recomputed
    // when the pick or an option changes rather than per frame — it walks the
    // whole assembly along a ray.
    bool ok = false;
    std::string summary;
    std::string why;
    // The optional second pick: where the joint ENDS. Unset means "the axis
    // runs to the end of whatever is on it", which is the ordinary case and
    // stays one click.
    ElementRef farElement;
    uint64_t farOccurrence = 0;
    int placed = 0; // how many this session has put in, for the tally
    // How many of them JOINED two parts that nothing already held. Counted
    // apart from `placed` because the difference is the interesting number:
    // eight bolts through one flange make one joint, and a tally that said
    // "8" for both would be claiming eight constraints that do not exist.
    int joined = 0;
    // What Place would put where, from the last dry run. Drives the ghost.
    std::vector<PlannedPart> plan;
  } fastenerTool;

  // The translucent preview of the joint about to be placed, and the key it
  // was built from. Tracked APART from documentMeshes, like topoGhostInstance
  // — a ghost left in that list survives a tab switch and haunts a document
  // that cannot explain it.
  std::vector<uint32_t> fastenerGhostInstances;
  std::string fastenerGhostKey;

  // The last "Check fasteners" answer, as the panel shows it (F1 p6 phase 4).
  // Kept here rather than in a static local inside the draw function: a
  // static would be shared by every document the process opens, and it would
  // be unobservable to a test — which for a check whose whole job is to be
  // SEEN is the wrong pair of properties.
  std::vector<std::string> fastenerChecks;
  bool fastenerChecked = false;
  uint64_t dimDrag = 0; // id of the dimension whose label is being dragged

  SketchEditState sketchEdit;
  SketchDragSession sketchDrag;
  SlotPickState slotPick; // U3/U7: armed tool-session pick slot
  MeasureState measure;
  char measureVarName[64] = "";
  SimulationState simulation;
  // The active topology-optimization run (TO2), or null when none is running.
  // A minutes-long SIMP solve on a worker thread; the main thread polls it and
  // stores the result at completion. One run at a time. See ui::TopoJob and the
  // Start/Poll/Cancel TopologyRun helpers.
  std::unique_ptr<TopoJob> topoJob;

  // A running fetch/push/clone (F9 C2). One at a time; PollSync joins it and
  // applies the result on the main thread.
  std::unique_ptr<SyncJob> syncJob;
  // A background STEP import (ui/ImportJob.h) and the message left by the
  // last failed one. Non-null = the progress modal is up and File > Import
  // refuses a second one.
  std::unique_ptr<ImportJob> importJob;
  std::string importError;
  // What the last one said, kept until the next one starts, so the panel has
  // something to show instead of the message flashing past.
  std::string syncMessage;
  bool syncFailed = false;
  // Cached so the panel does not spawn `git` every frame to draw a label.
  GitVault::Divergence divergence;
  std::vector<GitVault::Remote> remotes;
  double remotesStamp = -1.0;
  // The active async static/modal solve, or null. One study run at a time
  // across both job slots (see Start{Topology,Study}Run's shared guard).
  std::unique_ptr<StudyJob> studyJob;
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
  // One-shot launch check that narrows a side panel wide enough to crowd the
  // viewport out. ImGui gives a panel beside a CENTRAL node its absolute
  // width and lets the central node absorb every resize (imgui.cpp,
  // DockNodeTreeUpdatePosSize branch 3) — so a width that was a sane share
  // on one window is the same pixel count forever, whatever window opens
  // next. See BeginDockspace.
  bool sidePanelWidthChecked = false;
  bool runGuiTests = false;

  // CAM (docs/cam/plan.md). Toolpaths are CACHED, not regenerated per frame:
  // the backplot overlay and the panel read the same vector the post
  // consumes, so the picture cannot disagree with the file.
  std::vector<cam::Toolpath> camToolpaths;
  uint64_t camActiveSetup = 0;
  bool camShowToolpath = true;
  float camSafeZEdit = 5.0f;
  std::string camLastPost;
  // The verb session (G2): a Tool window, NOT a mode — CAM does not take the
  // viewport over, and modes have a known exit-bug class here (U11).
  bool camToolOpen = false;
  // WHICH verb the window is for. The verbs are the job toolbar since
  // 2026-09-02 (one button each, like the part tools), and the window that
  // opens is that verb's: its prompt, its tool, what it made. None = the
  // plain session window (the job's operations and verdict, no verb armed).
  enum class CamVerb {
    None,
    TopFace,
    LayFaceUp,
    IndexFace,
    Pocket,
    Profile,
    Drill,
    Edge,
    Whole,
    Face,
  };
  CamVerb camVerb = CamVerb::None;
  // The operation the no-pick verbs (Whole, Face) just made, so the window
  // can re-derive it for a different tool without the user removing it and
  // pressing the verb again. 0 = none this session.
  uint64_t camVerbOp = 0;

  // Where the floating tool window was last anchored: the top-right corner of
  // the 3D viewport at the time. Kept so the window can FOLLOW the viewport —
  // anchoring once left it stranded over the model whenever the layout
  // changed (docs/cam/dogfood-round.md, round 2).
  glm::vec2 toolWindowAnchor{-1.0f, -1.0f};
  int toolWindowAnchorFrames = 0;
  int toolWindowLastFrame = -2; // the frame it last drew, to spot a reopen

  // A docked panel asked to come forward (focus_panel over the RPC). Focus
  // takes effect on the next frame, and a tab sharing a node with several
  // others needs the request repeated before it wins, so this is a countdown
  // the dock host re-applies rather than a one-shot call.
  std::string focusPanel;
  int focusPanelFrames = 0;
  // What the last verb refused to do, in words. Cleared by the next attempt.
  std::string camRefusal;
  // The operation the viewport is illustrating (G3): its removed volume, its
  // depth label. 0 = none, which is what closing the session leaves behind.
  uint64_t camGhostOp = 0;
  // The face region the ghost and the hover preview draw, cached. Both used
  // to section the picked face — through a copy of every body — on EVERY
  // FRAME the ghost was on or the cursor sat over a face, which is OCC work
  // inside the frame loop. The key is everything the answer depends on, so a
  // stale one is not expressible: a model edit bumps the version, and a
  // different pick changes the ref.
  struct CamRegionCache {
    uint64_t documentVersion = UINT64_MAX;
    uint64_t setupId = 0;
    double rotationA = 0.0;
    // The fixture frame too: laying a face up re-sections every body, so a
    // key that watched only the rotary axis served a region from the previous
    // orientation (S2).
    glm::dquat frame{1.0, 0.0, 0.0, 0.0};
    ElementRef ref;
    cam::FaceRegion region;
  };
  // What the last CAM action DID, when it did more than was asked — laid the
  // part flat to take a Z zero, re-derived depths after a re-clamp. Not a
  // refusal (those are camRefusal, in red): a consequence, stated, because the
  // alternative is a job that quietly changed under the user.
  std::string camNote;
  // Which cutter the next verb derives for. 0 = not chosen this session, so
  // the LAST TOOL USED — the one the setup's newest operation cuts with —
  // and the first tool only when nothing has been cut yet (ui::VerbTool).
  int camVerbTool = 0;
  CamRegionCache camGhostRegion;
  CamRegionCache camHoverRegion;
  bool camShowStock = true;
  // The last verification (G4), in the words the panel prints, plus the
  // points where the tool could not reach. Both cleared on any edit, because
  // a verdict about a previous toolpath is worse than no verdict.
  std::vector<cam::VerdictLine> camVerdict;
  std::vector<glm::dvec3> camLeftover;

  // Cutting simulation (docs/cam/simulation.md). Playback is SESSION state:
  // nothing here serializes, and a document edit invalidates it rather than
  // re-cutting under the cursor.
  //
  // Ends through ONE path, ui::EndCamPlayback — it hides the bodies while it
  // runs, and a mode that hides things and has several exits is the U11 bug
  // class this codebase has already paid for once.
  bool camPlaybackOpen = false;
  bool camPlaying = false;
  // What the timeline was built from, kept so a re-post can be spotted.
  std::string camPlaybackGcode;
  cam::Timeline camTimeline;
  cam::StockSim camStockSim;
  cam::SimCursor camCursor;
  // Grid for the picture. Verification runs at 0.4 mm; a visual wants finer,
  // and the cost is quadratic, so it is a setting rather than a constant.
  double camSimRes = 0.25;
  // Multiplier on real feed rates. The cursor advances by DISTANCE, so this
  // is "mm of path per second of wall clock" scaled by the job's own feeds.
  float camPlaybackSpeed = 8.0f;
  double camPlaybackLastTick = 0.0;
  // The StockSim revision the drawn mesh was built from, so a frame that
  // changed nothing re-meshes nothing.
  uint64_t camStockDrawnRevision = 0;
  // Why playback will not start, in words, where the button is.
  std::string camPlaybackRefusal;
  // The cutter, drawn as a real solid through the mesh renderer (it is static
  // geometry that MOVES, which is the one thing that renderer is good at).
  // `camToolInstanceFor` is the T number it was built for; a tool change
  // rebuilds it.
  uint32_t camToolInstance = 0;
  int camToolInstanceFor = -1;

  // The terminal panel (docs/ui/terminal-panel.md). Off by default via
  // settings.terminalPanel; the state is created the first time it draws, so
  // an app that never opens it never allocates a terminal.
  std::shared_ptr<TerminalState> terminal;
  // This window's remote port (0 = the app was started without --remote), and
  // a way to open one on demand — a Claude Code launched from the terminal is
  // useless if the app it is inside cannot be reached. Installed by main.cpp,
  // which owns the server.
  int remotePort = 0;
  std::function<int()> ensureRemotePort;

  // Frame-time accounting (docs/rendering/frame-timing.md). Always measured —
  // it costs five clock reads a frame — but the HUD is off unless asked for
  // by --perf-hud, Ctrl+Shift+F, or the frame_stats RPC. Deliberately session
  // state and not a preference: it belongs to the debugging session, and a
  // preference would have to be either visible in the Preferences window or a
  // key that window cannot edit.
  render::FrameTimer frameTimer;
  // GPU per-pass averages, mirrored from main.cpp's GpuTimer each frame
  // (pure data here so this header stays Vulkan-free). `supported` false =
  // the queue has no timestamp support; frame_stats says so instead of
  // guessing. shadow+ssao+main == total by construction (contiguous deltas).
  struct GpuPassStats {
    bool supported = false;
    size_t samples = 0;
    double shadowMs = 0.0;
    double ssaoMs = 0.0;
    double mainMs = 0.0;
    double totalMs = 0.0;
  } gpuStats;
  bool perfHudOpen = false;
  // Idle throttle (docs/rendering/idle-throttle.md). The pacer itself lives
  // in main.cpp; these are its test/observability seams: `pacerForceEnable`
  // lets the pacer's own GUI test opt back in under --gui-tests (where the
  // pacer is otherwise bypassed — the engine drives frames), and
  // `pacerIdleFrames` mirrors how many iterations ran idle-paced.
  bool pacerForceEnable = false;
  uint64_t pacerIdleFrames = 0;
  // The last wake signal's name (FramePacer::WakeName) and its age — the
  // "why is this not idling" readout for the HUD and frame_stats.
  const char* pacerWake = "none";
  double pacerWakeAgeS = -1.0;
  nlohmann::json
      editBaseline; // parameter-edit snapshot while a widget is active

  // Where the document lives on disk ("" = never saved) and the document
  // version at the last clean save/open — dirty = Version() != lastSaved.
  // Drives autosave (core/Autosaver) and its recovery sidecars.
  std::string projectPath;
  uint64_t lastSavedVersion = 0;
  // Edits that must be SAVED but must not bump a document version. Tab
  // folders and tab renames live in project.json and belong to no document
  // at all; display units (U13e) do live in the document, but bumping its
  // version for them would mark every solved study stale over a change that
  // cannot move a vertex. Either way this flag is what keeps them
  // save-prompt visible. Cleared on save/open (StampAllSaved).
  bool projectMetaDirty = false;
  // Bumped whenever the project is replaced (opened, recovered). The
  // autosaver is a loop local rather than state, so this is how it learns
  // that what it has been tracking no longer applies (F10).
  uint64_t projectGeneration = 0;
  // Non-empty: OpenProjectDialog found an autosave newer than this project
  // file; the recovery modal (DrawFileBrowser) resolves it.
  std::string recoveryOffer;

  // The bounds of what SyncRayTracingScene last handed the ray tracer, so a
  // still can size its ray limits from the scene instead of a literal.
  // Valid only when rtSceneValid: an empty scene has no bounds to speak of.
  glm::vec3 rtSceneMin{0.0f};
  glm::vec3 rtSceneMax{0.0f};
  bool rtSceneValid = false;

  // How far this document's geometry reaches from the world origin, which is
  // what every piece of reference geometry (base planes, datum quads, the
  // sketch grid) is sized from. Behind it is an OCC bounding box per body and
  // it is asked for every frame the planes are up, so it is cached against
  // the document that answered and the version it answered for.
  const Document* sceneReachDoc = nullptr;
  uint64_t sceneReachVersion = 0;
  float sceneReach = 0.0f;

  // Document -> renderer sync bookkeeping
  uint64_t syncedDocumentVersion = 0;
  std::vector<DocumentRenderMesh> documentMeshes;
  // Tessellations + GPU assets kept per referenced part ACROSS assembly
  // resyncs, keyed on (part slot id, that part document's Version()). A
  // resync removes every instance and re-adds them, so without this every
  // switch back to an assembly tab re-tessellated and re-uploaded every
  // unique part — 5.9 s on a 299-part STEP import, and the same 5.9 s for
  // hiding a single occurrence. Opaque here on purpose: the entries hold
  // Vertex3D, and AppState.h forward-declares MeshRenderer rather than
  // including it. Defined in AppActionsInternal.h, filled by
  // SyncAssemblyRenderers; a shared_ptr so an incomplete type still has a
  // usable implicit destructor here.
  std::shared_ptr<PartRenderCacheStore> partRenderCache;

  // Offline render (Render toolbar button); pumped by main.cpp.
  RenderJob renderJob;

  // Fallback file-browser state. Compiled unconditionally: the NFD build
  // also falls back to it when native (GTK) dialogs would abort the process
  // — missing GSettings schemas make GLib call abort() the moment a file
  // chooser opens (the NixOS bare-env classic).
  bool showFileBrowser = false;
  std::string currentPath = "../CAD_files";
  std::vector<std::string> directoryContents;

  // Version history (F6). Every read shells out to `git`, so the panel works
  // off this cache: refreshed when it is visible and stale (~1 Hz), and
  // immediately after any action that changes the history. `historyPath`
  // guards the cache against a project switch.
  std::vector<GitVault::LogEntry> historyLog;
  std::string historyPath;
  double historyStamp = -1.0; // ImGui time of the last refresh; <0 = never
  bool historyUncommitted = false;

  // Did the project change on disk under us? (F8) `diskStamp` is what the
  // directory looked like when we last read or wrote it; anything else is
  // somebody else's write — a command in the terminal panel, another editor,
  // a second instance. `diskChanged` is the poll's answer (the banner);
  // `diskChangeBlockedSave` means a save was refused and the modal owes the
  // user a choice.
  DocumentIO::ProjectStamp diskStamp;
  std::string diskStampPath;
  std::string diskStampHead; // git HEAD when stamped — the cheap pre-check
  bool diskChanged = false;
  bool diskChangeBlockedSave = false;
  double diskCheckStamp = -1.0; // ImGui time of the last poll; <0 = never
  bool windowFocused = true;    // main.cpp mirrors GLFW_FOCUSED here

  // Conflicts (F7). A scan spawns `git` and walks the project for the
  // duplicate ids git cannot see, so it runs on an event rather than a clock
  // (F11 G3 — see conflictsStale).
  ProjectMerge::Report conflicts;
  std::string conflictsPath;
  // Rescan when this is set, and ONLY then — no timer. A scan walks the whole
  // project (for the duplicate ids git cannot report) and spawns `git`, and
  // on a clean project the answer cannot change on its own: a conflict
  // appears when we merge (which invalidates explicitly), when a project is
  // opened, or when something else writes the directory — which is exactly
  // what F8's poll already detects. Timing it instead cost a tree walk a
  // second, forever, for an answer that was "no" every time.
  bool conflictsStale = true;
  // The version the History panel is showing a diff against, and what it
  // found (F11 G2). Empty hash = no diff open; empty `diffTo` = the working
  // tree, which is "what have I changed since then".
  std::string diffFrom, diffTo, diffLabel;
  std::vector<ProjectMerge::ItemChange> diffChanges;
  // Layer 3 (F11): what the geometry did, computed on demand — it regenerates
  // the old recipe and runs booleans, which is not something to do while
  // someone scrolls a list.
  GeometryChange diffGeometry;
  // How many scans have actually happened. Only a counter for a test to
  // watch — but the property it guards is the whole point of the change: on
  // a clean project this must stop going up.
  uint64_t conflictScans = 0;
  // Set by "Finish merge" when the resolved project does not load or rebuild:
  // the merge is NOT concluded and these say why.
  std::vector<ProjectMerge::Breakage> conflictsBreakage;
  // Local branches, for the History panel's branch controls.
  std::vector<std::string> branches;
  std::string currentBranch;

  // User preferences (docs/ui/settings.md). Exactly ONE live copy — nothing
  // else caches a field, or "it reverted when I opened a second project"
  // becomes possible. `settingsSaved` is the last state written to disk, so a
  // save only overwrites the keys THIS instance changed (another instance may
  // be running). `settingsWindowOpen` is session state, not a preference.
  settings::Settings settings;
  settings::Settings settingsSaved;
  double settingsDirtyAt = -1.0; // ImGui time of the last edit; <0 = clean
  bool settingsWindowOpen = false;
  // Preferences > Keyboard is waiting for the next chord, for this command id.
  // Session state, not a preference, and it gates the key dispatcher: while a
  // capture is armed every chord means "bind me", not "run something".
  std::string keyCaptureId;
  // Render devices this machine offers, for the Preferences combo. When the
  // preferred one could not be honoured, startup records WHY here — the
  // window must not promise that a restart will fix something a restart
  // cannot fix (a device that cannot present to a window).
  std::vector<std::string> gpuDevices;
  std::string gpuPreferenceNote;
};

} // namespace ui
