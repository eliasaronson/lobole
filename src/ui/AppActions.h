#pragma once

#include "cad/MeshExport.h" // MeshExport::Item
#include "core/AssemblySolve.h"
#include "core/ElementMap.h"               // ElementRef
#include "core/drawing/DrawingDimension.h" // drawing::ResolvedDim
#include "render/SketchRenderer.h"         // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
class SplitFeature;
class FilletFeature; // defined in core/features/FilletFeature.h
struct RTStillDesc;  // defined in RayTracingRenderer.h
struct Ray;          // defined in FaceSelector.h

namespace ui {

// Feature selection through the one Selection set (U1a). Stored by stable
// feature id, so it survives regeneration, reordering and sibling deletes;
// callers that need the index resolve it fresh each time.
uint64_t SelectedFeatureId(const AppState& state); // 0 = none
Feature* SelectedFeature(AppState& state, size_t* indexOut = nullptr);
int SelectedFeatureIndexOrMinus1(AppState& state);       // RPC back-compat
void SelectFeature(AppState& state, uint64_t featureId); // replaces the set
void SelectFeatureByIndex(AppState& state, size_t index);
// Opens the committed-feature editor session in the Tool window (U7e —
// tree double-click / context-menu Edit). No-op for unknown ids.
void OpenFeatureEditor(AppState& state, uint64_t featureId);
// Assembly-tab selection through the same set (U1d). Occurrence and mate
// items coexist (the panel lists select independently, as the old separate
// fields did): each setter replaces only its own kind; 0 removes it.
uint64_t SelectedOccurrenceId(const AppState& state);
uint64_t SelectedMateId(const AppState& state);
void SelectMate(AppState& state, uint64_t mateId);
// Drops items whose referent no longer exists (deleted feature, undone
// creation, File > New). Called once per frame before the panels draw.
void PruneSelection(AppState& state);

// Swaps in a new document (File > New / Open) with a fresh undo stack and
// forces the renderers to resync.
void ReplaceDocument(AppState& state, std::unique_ptr<Document> doc);

// Multi-document projects (assemblies A1/A2). EnsureParts makes the parts list
// non-empty (one slot for the active document) — call once after the initial
// document is created. AddPart/AddAssembly append a new empty tab of that kind
// and switch to it. SetActivePart swaps the active document out to its slot
// and the target in. DeletePart removes a tab (never the last). All resync the
// renderers.
void EnsureParts(AppState& state, const std::string& firstName = "Part 1");
size_t AddPart(AppState& state, const std::string& name);
size_t AddAssembly(AppState& state, const std::string& name);
size_t AddDrawing(AppState& state, const std::string& name);
void SetActivePart(AppState& state, size_t index);
// Closes a tab. Refused (with the reason in `why`) for the last tab, or
// while any assembly still places this part or assembly — a dangling
// reference is not something to create on purpose.
bool DeletePart(AppState& state, size_t index, std::string* why = nullptr);

// Tab-bar folders (display grouping only — storage stays flat, ids and
// references are unaffected). Folders live in state.tabFolders and may be
// empty; CreateFolder registers a name, MoveToFolder reassigns one tab
// ("" = top level, registering the folder as needed), RenameFolder renames
// the registry entry and every member (renaming onto an existing folder
// merges them), DissolveFolder unregisters and moves members to the top
// level. All mark the project meta-dirty.
void CreateFolder(AppState& state, const std::string& folder);
void MoveToFolder(AppState& state, size_t index, const std::string& folder);
void RenameFolder(AppState& state, const std::string& from,
                  const std::string& to);
void DissolveFolder(AppState& state, const std::string& folder);

// The document id (PartSlot::id) for a part name, or 0 if no such part.
// Occurrences reference parts by this stable id, never by name.
uint64_t PartIdByName(const AppState& state, const std::string& name);

// The part document an occurrence (or a cross-part query) references, by
// stable id — active or stashed; never switches the active tab. Null when the
// id names no part slot.
Document* PartDocById(AppState& state, uint64_t partId);

// An occurrence's solved placement as a model matrix (translate ∘ rotate) —
// the transform assembly measure / interference place part bodies with.
glm::mat4 OccurrenceMatrix(const Occurrence& o);

// A5 resolution: an occurrence's stable id names a part OR an assembly slot,
// per its refKind.
AssemblyDocument* AssemblyDocById(AppState& state, uint64_t slotId);
assembly::PartResolver PartResolverFor(AppState& state);
assembly::AssemblyResolver AssemblyResolverFor(AppState& state);

// The active assembly's leaf parts, placed in world space — the one walk
// every geometry consumer uses (render, BOM, mass, contact, export, drawings,
// ray tracing, section caps). Empty when the tree cannot be walked, with the
// reason logged.
std::vector<assembly::FlatInstance> FlattenActiveAssembly(AppState& state);

// Assembly occurrence-edit undo (snapshot-based; A2 phase 3). Call
// SnapshotAssembly *before* mutating the active assembly's occurrences; Undo/
// Redo restore a snapshot and force a renderer resync. No-ops without an active
// assembly / empty stack (Undo/Redo return whether they did anything).
void SnapshotAssembly(AppState& state);
bool UndoAssembly(AppState& state);
bool RedoAssembly(AppState& state);

// Drawing-edit undo (snapshot-based, the assembly pattern; workstream 13).
// Call SnapshotDrawing before mutating the active drawing's sheet/views/dims.
void SnapshotDrawing(AppState& state);
bool UndoDrawing(AppState& state);
bool RedoDrawing(AppState& state);

// Selects an occurrence in the active assembly (0 = clear): records it and
// highlights all its rendered instances. Safe with null renderers.
void SelectOccurrence(AppState& state, uint64_t occId);

// Re-solves the active assembly's mates (A3): places every mated, non-fixed
// occurrence by direct frame alignment, then bumps the version so the renderer
// resyncs. No-op without an active assembly. Call after any mate/occurrence
// edit and after loading. Returns the number of mates left in an error state.
int SolveActiveAssembly(AppState& state);

// Part→assembly change propagation (A4): true when any part referenced by the
// active assembly has regenerated since the assembly's last solve (compared
// against the version stamps SolveActiveAssembly records). SetActivePart and
// OpenProject use it to re-solve automatically; in-context editing re-checks
// after every part regeneration. False without an active assembly.
bool AssemblyPartsStale(AppState& state);

// In-context editing (A4): switch to the part tab of `occurrenceId` (in the
// active assembly) with the assembly ghosted around it — the part renders at
// identity and every other occurrence at OccMatrix(edited)⁻¹·OccMatrix(other),
// alpha-blended, unpickable, no outlines. The context re-solves after each
// part regeneration so ghosts track mates live. Exit = activating any other
// tab (SetActivePart clears the mode). False when the occurrence or its part
// can't be resolved.
bool EnterInContextEdit(AppState& state, uint64_t occurrenceId);
// The tab index of the slot with a stable id (nullopt when gone) — the
// in-context banner's Return button and RPCs resolve slots through this.
std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId);

// Resolves a viewport ray to a face of an occurrence in the active assembly:
// the occurrence hit and a persistent ElementRef naming the face (for building
// a mate connector). Returns false if the ray misses, hits an occurrence whose
// part isn't loaded, or lands on an unnamed face (e.g. a pattern output). The
// `ray` is in the same space FaceSelector::PickFace expects.
struct AssemblyFacePick {
  enum class SnapType { FaceCenter, HoleCenter, EdgeMidpoint };

  uint64_t occurrenceId = 0;
  ElementRef face;
  // A persistently named edge when the nearest candidate is a circular-edge
  // center or a straight/curved edge midpoint; unset means face center.
  ElementRef snapEdge;
  SnapType snapType = SnapType::FaceCenter;
  // A5: when the face belongs to a part INSIDE the picked occurrence, the
  // ids down to it. A connector built from this pick carries it, so a mate
  // can anchor on geometry inside a placed unit.
  std::vector<uint64_t> innerPath;
};
bool PickAssemblyFace(AppState& state, const Ray& ray, AssemblyFacePick& out);

// Mate creation tool (A3 phase 3, U7c slots). StartMateTool opens the
// session and arms connector slot A; the selection pipeline resolves
// Connector candidates and routes picks to MateConnectorPicked (slot A →
// advance to B; slot B → live preview mate; edit session → rewrite the
// mate's connector and re-solve). Accept keeps the preview / closes the
// session; Cancel removes it. All no-op without an active assembly.
void StartMateTool(AppState& state, MateType type);
// Re-pick session for an existing mate: opens the mate session with both
// connectors filled from the mate; clicking a connector row re-arms it and
// the next pick rewrites that side (one undo step per re-pick).
void StartMateEdit(AppState& state, uint64_t mateId);
// The armed connector slot consumed a pipeline pick (SlotPickClicked routes
// kMateSessionSlot here).
void MateConnectorPicked(AppState& state, const SelectionItem& item);
void AcceptMateTool(AppState& state);
void CancelMateTool(AppState& state);
// Arms connector slot A or B on the shared armed-slot state (the mate
// session's slot rows re-arm through this too).
void ArmMateConnector(AppState& state, bool second);
// The mate session's Tool-window contents (connector slot rows, flip/error,
// accept/cancel). Lives with the other mate UI in OccurrencePanel.cpp;
// called from DrawToolWindow.
void DrawMateToolWindow(AppState& state);
// The world frame of a connector (occurrence placement ∘ its part-local frame),
// or nullopt if it doesn't resolve. For drawing the connector triad.
std::optional<glm::mat4> ConnectorWorldFrame(AppState& state,
                                             const Connector& c);
// Appends the mate tool's connector triads (picked connectors) to `lines`.
void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines);

// The 3D drag gizmo (A2.5). BuildGizmoOverlay appends the translate/rotate
// handles for the selected non-fixed occurrence; HandleGizmoInput runs the drag
// state machine (grab → live per-frame transform → release snapshots+solves)
// and returns true when it consumed the viewport (so the caller skips
// pick/orbit).
void BuildGizmoOverlay(AppState& state, std::vector<LineVertex3D>& lines);
// Pushes an occurrence's current placement to the renderers WITHOUT a resync
// — the live-drag path. Composes each mesh's place inside the occurrence, so
// dragging a sub-assembly moves the unit rather than collapsing its parts
// onto its origin.
void PushOccurrenceLive(AppState& state, uint64_t occId);

// The placement a mesh renders at (solved ∘ explode), exposed for tests and
// for anything that has to reproduce the live-drag composition.
glm::mat4 DisplayMatrixOf(const AppState& state, const Occurrence& o);

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released);

// The occurrence list + properties editor for an active assembly tab (drawn
// where the feature tree is for a part). Defined in OccurrencePanel.cpp.
void DrawOccurrencePanel(AppState& state);

// The sheet settings + title-block editor for an active drawing tab (drawn
// where the feature tree is for a part). Defined in DrawingPanel.cpp. The view
// list and dimension tools arrive in later drawings milestones.
void DrawSheetPanel(AppState& state);
// Renders the active drawing's sheet (border, title block, and later views/
// dims) into the viewport through the 2D sheet camera. Defined in
// DrawingPanel.cpp; called from the frame loop when a drawing tab is active.
void DrawSheet(AppState& state);
// 2D sheet pan/zoom (scroll-to-cursor zoom, middle-drag pan) — the drawing-tab
// replacement for the orbit-camera viewport input. Defined in DrawingPanel.cpp.
void HandleSheetInput(AppState& state);

// Re-projects every view of the active drawing into state.drawingRenders (the
// HLR-per-view work). Explicit (D2): called on drawing-tab entry and by the
// update_drawing RPC / panel button — never per frame, since HLR is expensive.
// Failed views keep their last-good geometry with an error string.
void UpdateDrawingRenders(AppState& state);

// Adds a view to the active drawing, choosing the two things a human should
// not have to compute: a standard scale that makes the part legible on this
// sheet (only when `v.scale` is <= 0), and a spot clear of the views already
// placed. Snapshots for undo and re-projects. Shared by the drawing
// toolbar's Add-view menu and the panel's Add view button.
void AddDrawingViewPlaced(AppState& state, DrawingView v);

// The current Document::Version() / AssemblyDocument::Version() of a source
// document (part or assembly) by stable id, or 0 if absent. A drawing view is
// stale when this differs from the version stamped at its last projection.
uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId);

// The part Document behind a drawing view's sourceDocId; null for assembly
// sources (occurrence resolution is a follow-up) or a missing slot. Lets
// list_drawing_geometry name the feature a projected curve came from.
Document* DrawingSourceDocument(AppState& state, uint64_t sourceDocId);

// Where a view actually lands on the sheet: a folded child's shared axis is
// locked to its base (design §2), so the stored position's x and/or y may be
// overridden at projection time. The one fold computation — projection and
// the set_drawing_view RPC's "your x was overridden" note both use it.
struct FoldedPosition {
  glm::dvec2 pos{0.0};
  bool xLocked = false, yLocked = false;
};
FoldedPosition DrawingFoldedPosition(AppState& state, const DrawingView& view);

// Resolves one dimension against the active drawing's projected geometry (its
// view's cached SheetView). Returns an unresolved ResolvedDim if the view is
// not projected or a ref is dangling. Defined in AppActions.cpp.
drawing::ResolvedDim ResolveDrawingDim(AppState& state, const DrawingDim& dim);

// Re-projects and writes the active drawing to `path` (.svg or .pdf by
// extension). The shared export path for the export_drawing RPC and the GUI
// Export dialog. Returns false and fills *error on failure.
bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error);

// ---- Simulation mode (FEM) ------------------------------------------------

// Enter/exit the FEM simulation workspace (a mode, like sketch mode). Enter
// arms `state.simulation.active` and selects (creating if none) a study; exit
// restores any hidden body instances and clears the mode.
// Arms a boundary-condition tool (the panel and the toolbar share this so
// they cannot drift): seeds an unset Force/Pressure magnitude from the part's
// own weight, so the tool never places a load of 0 that silently does
// nothing.
void SimulationArmTool(AppState& state, SimulationState::Tool tool);
void EnterSimulation(AppState& state);
void ExitSimulation(AppState& state);

// Meshes + solves study `id` on the active document and caches the results
// (stamped with the current Version). Returns false and fills *error on any
// failure — the one source of truth shared by the panel Run button and the
// run_study RPC. Defined in AppActions.cpp.
bool RunStudyAndCache(Document& doc, uint64_t studyId, std::string* error);

// ---- Topology optimization runs (TO2, async) ------------------------------
// A topology study's SIMP solve is minutes long, so it runs on a worker thread
// over an immutable snapshot; the main thread polls and stores the result at
// the single join point. One run at a time.

// Prepares the snapshot (meshes on the calling thread) and spawns the worker.
// Returns false + *error on a prep failure or if a run is already in progress.
bool StartTopologyRun(AppState& state, Document& doc, uint64_t studyId,
                      std::string* error);

// If a run has finished (converged, capped, cancelled, or errored), joins it
// and stores the result (or the study error). Cheap no-op otherwise — call it
// each frame and before reporting study status. Defined in
// SimulationActions.cpp.
void PollTopologyRun(AppState& state, Document& doc);

// Requests cancellation of the run for `studyId` (keeps best-so-far). Returns
// false if no such run is active.
bool CancelTopologyRun(AppState& state, uint64_t studyId);

// Live status of the active run. `running` is false when no job is running for
// `studyId` (finished/never-started — read the cached result instead).
struct TopoRunStatus {
  bool running = false;
  int iteration = 0;
  int maxIterations = 0;
  double compliance = 0.0;
  double maxChange = 0.0;
  double volumeFraction = 0.0;
  std::string solver;          // "gpu: <device>" or "cpu (<why>)"
  double elapsedSeconds = 0.0; // since the run started
};
TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId);

// The async static/modal counterparts (StudyJob): prepare-on-main +
// solve-on-worker, polled at the same sites as the topology run. A static
// solve is NOT cancellable (no interruption points in the linear solver).
bool StartStudyRun(AppState& state, Document& doc, uint64_t studyId,
                   std::string* error);
void PollStudyRun(AppState& state, Document& doc);
// running=false when no static/modal job is active for `studyId`;
// elapsedSeconds since launch otherwise.
struct StudyRunStatus {
  bool running = false;
  double elapsedSeconds = 0.0;
};
StudyRunStatus GetStudyRunStatus(const AppState& state, uint64_t studyId);

// The Studies panel drawn in the Model dock while in simulation mode. Defined
// in StudyPanel.cpp.
void DrawStudyPanel(AppState& state);

// The result-field colorbar legend overlaid on the viewport while a field is
// displayed. Defined in StudyPanel.cpp.
void DrawFieldLegend(AppState& state);

// Assigns the armed boundary condition (state.simulation.tool) to the face
// under `ray`, as an undoable study edit. No-op if no tool is armed or the
// pick misses a named face. Defined in StudyPanel.cpp.
// Applies the armed BC/keep tool to a picked face (U1c: the selection
// pipeline resolves the pick; this consumes it).
void SimulationApplyToFace(AppState& state, const ElementRef& face);

// Boundary-condition glyphs while simulation mode is active: arrows on
// force/pressure faces (force along its direction, pressure into the face),
// anchor hatches on fixed faces, diamonds on keep faces.
void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out);

// A world-space cross at the peak of the displayed result field (max von Mises
// or max displacement), warped by the deform scale. Pushed into the no-depth
// gizmo channel like the CoG marker. Defined in AppActions.cpp.
void AppendStudyMarker(const AppState& state, std::vector<LineVertex3D>& lines);

// Rebuilds the FEM field overlay from the active study's cached results and
// toggles the solved bodies' normal render (hidden while a field is shown,
// restored otherwise). Cheap and idempotent — the vertex rebuild is gated on
// a change signature; the deform scale rides a push constant (no rebuild).
// Called every frame. Defined in AppActions.cpp.
void SyncFieldRenderer(AppState& state);

// Bill of materials (A4): derived from the active assembly every call, never
// stored. One row per referenced part, in first-occurrence order; masses via
// Measure::MassOfBodies (a part with any density-less body reports its mass
// unknown and taints the total). `visible=false` occurrences count —
// visibility is a display state, not suppression.
struct BomRow {
  uint64_t partId = 0;
  std::string partName; // "(missing part)" when the reference dangles
  int quantity = 0;
  std::string material;    // slug; "" = none, "mixed" when bodies differ
  double unitVolume = 0.0; // mm^3, all bodies of the part
  bool massKnown = false;
  double unitMassG = 0.0; // valid when massKnown
};
struct BomTable {
  std::vector<BomRow> rows;
  bool massKnown = true;   // false when any row's mass is unknown
  double totalMassG = 0.0; // valid when massKnown
  // V2: WHY a mass is unknown ("part 'X': body 'Y' has no material"), so a
  // null total explains itself instead of silently tainting the table.
  std::vector<std::string> warnings;
};
BomTable ComputeBom(AppState& state); // empty without an active assembly
// The table as CSV (quoted where needed); unknown masses are empty cells.
std::string BomToCsv(const BomTable& bom);

// Visual-mesh export items (interchange X2) for the active tab: a part's
// bodies at identity, or an assembly's occurrences at their SOLVED placements
// (occurrences of one part share the underlying shape, so the writers emit
// instanced geometry). Each item carries the body's material color/PBR (or a
// palette color for an unassigned body). Empty when nothing is active.
// Defined in AppActions.cpp; the return type is MeshExport::Item.
std::vector<MeshExport::Item> BuildVisualExportItems(AppState& state);

// Exploded views (A4). AutoExplode replaces `viewId`'s steps with a radial
// seed (each occurrence pushed away from the assembly centroid, scaled by the
// assembly's size) — a starting point to refine by gizmo drag. Snapshots for
// undo; false without an active assembly / unknown view / fewer than two
// placed occurrences. ApplyExplodeTransforms re-applies display transforms
// (solved ∘ explode) to every occurrence's render instances WITHOUT a resync
// — the factor slider's cheap live path (picking refreshes on the next real
// resync).
bool AutoExplode(AppState& state, uint64_t viewId);
void ApplyExplodeTransforms(AppState& state);

// Save/open a project directory. A multi-part project writes a project.json
// index + one `parts/<name>/` directory each; a single-part project stays
// the flat `.dsgn` (index.json + features/) form for backward compatibility.
// Open detects project.json to load all parts, else loads one part.
bool SaveProject(AppState& state, const std::string& path, std::string* error);
bool OpenProject(AppState& state, const std::string& path, std::string* error);

// ---- Project-wide variables (Phase V, docs/plan-caster-backlog.md) ---------
// The project parameter table lives in AppState and is shared by every part via
// Document::SetProjectSymbols; set_variable defaults to this scope.

// Re-evaluate the project table into state.projectSymbols and regenerate every
// part against it (wiring each slot's Document to the project symbols first).
// Call after any project-var edit or structural change (open/new/add part).
void SyncProjectScope(AppState& state);

// Project-variable edits, undoable across tabs (projectVarHistory + regen-all).
// Set returns false only for an invalid name. Remove is refused (returns false,
// fills outRefs) while any part references it. Rename rewrites references in
// every part; false if newName is invalid/taken or oldName is absent.
bool SetProjectVariable(AppState& state, const std::string& name,
                        const std::string& expression,
                        const std::string& comment);
bool RemoveProjectVariable(AppState& state, const std::string& name,
                           std::vector<std::string>* outRefs);
bool RenameProjectVariable(AppState& state, const std::string& oldName,
                           const std::string& newName);
// Human-readable references to a project variable across ALL parts (+ project
// params), for delete-refusal messages.
std::vector<std::string> ProjectVarReferences(AppState& state,
                                              const std::string& name);
const Parameter* FindProjectParameter(const AppState& state,
                                      const std::string& name);
bool UndoProjectVars(AppState& state);
bool RedoProjectVars(AppState& state);

// Global Undo/Redo: route Ctrl+Z / Edit>Undo to whichever source — the active
// tab's stack or the cross-tab project-var stack — was most recently mutated.
// Use these at the dispatch sites instead of the per-tab stacks directly.
bool CanGlobalUndo(const AppState& state);
bool CanGlobalRedo(const AppState& state);
void GlobalUndo(AppState& state);
void GlobalRedo(AppState& state);

// Track the on-disk identity for dirty-state + autosave: call after every
// clean save (also clears the autosave sidecar) / open / new. "" = unsaved.
void MarkProjectSaved(AppState& state, const std::string& path,
                      const std::string& message = "save");
void MarkProjectOpened(AppState& state, const std::string& path);
// After loading a recovery sidecar: the document belongs at `path` but is
// deliberately dirty (unsaved recovered changes), so autosave keeps running
// and a save prompt is meaningful.
void MarkProjectRecovered(AppState& state, const std::string& path);

// True when any document in the project (every tab, not just the active one)
// has edits since its last clean save/open.
bool ProjectDirty(AppState& state);
// Window title: "<project name>[*] - Designer", * = ProjectDirty. The name is
// the project path's stem; "Untitled" before the first save.
std::string WindowTitle(AppState& state);

// Frames the camera on the document's combined bounding box.
// The world-space box around everything the active tab shows: a part's
// bodies, or an assembly's visible (unsuppressed) occurrences in place.
// False when there is nothing to bound. Camera fit and the section plane
// both need it — a view feature positioned in absolute coordinates is wrong
// for any model that is not sitting on the origin.
bool VisibleSceneBounds(AppState& state, glm::vec3& min, glm::vec3& max);

// Section views (I3). `basePlane` is 0=XY, 1=XZ, 2=YZ; `offset` is the signed
// distance along that plane's normal; `reverse` keeps the other side. ONE
// list, shared by the tool window and the RPCs, so they cannot drift apart —
// the menu this replaces kept its own copy in function statics.
glm::vec4 SectionPlaneOf(const AppState::SectionView& s);
ClipPlaneArray SectionPlanes(const AppState& state); // enabled ones, padded
bool SectionActive(const AppState& state);
AppState::SectionView* FindSection(AppState& state, uint64_t id);
// Adds a section cutting through the middle of what the tab shows, and
// returns its id; 0 when the list is full (kMaxClipPlanes).
uint64_t AddSection(AppState& state, int basePlane);
void RemoveSection(AppState& state, uint64_t id);
void ClearSections(AppState& state);

// Emits the filled cut faces for one set of cap shapes, placed by `xf` and
// facing `worldNormal`. Shared by the part path (identity transform) and the
// assembly path (one call per occurrence) so both cuts look the same — the
// double-sided emit and the nudge onto the kept side are fiddly enough that a
// second copy would drift.
void AppendCapMeshes(AppState& state, const std::vector<CADShape>& caps,
                     const std::vector<glm::vec3>& colors, const glm::mat4& xf,
                     const glm::vec3& worldNormal);

// The offset that puts the cut through the MIDDLE of what the tab is showing,
// along `basePlane`'s normal. A section defaults to the world origin
// otherwise, which for a model built anywhere else is either a cut that
// removes everything or one that does nothing — both read as "section view is
// broken". Returns 0 when the scene is empty.
float CenteredSectionOffset(AppState& state, int basePlane);

void FitCameraToDocument(AppState& state);
// Frames the camera on an explicit world-space box (render_sheet focus).
void FitCameraToWorldBox(AppState& state, const glm::vec3& min,
                         const glm::vec3& max);
// Frames the camera on the selection's combined bounding box (faces/edges/
// bodies/features by their geometry; occurrences by their placed part).
// Falls back to the whole document when nothing is selected/resolvable.
void FitCameraToSelection(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry +
// materials) and (re)builds its acceleration structures. On-demand for the
// offline render; returns false if RT is unavailable or there's no geometry.
// `groundPlane` adds a shadow-catcher quad under the scene (offline renders
// only).
bool SyncRayTracingScene(AppState& state, bool groundPlane = false);

// Builds the offline-render descriptor from the current app state (camera +
// shared IBL maps) for a `width`x`height` still with the given RNG seed.
::RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                            uint32_t seed);

// Full synchronous offline still: sync the scene, build ASes, trace `samples`
// samples, tonemap and write a PNG to `path`. Blocks until done. Returns false
// (with *error set) on any failure. Used by the render_still RPC; the UI modal
// drives BeginStill/RenderBatch itself for progress. `seed` fixes the RNG so a
// given (scene, samples, seed) is reproducible.
bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, bool groundPlane, std::string* error);

// Advances an active AppState::renderJob by one sample batch per call (the
// non-blocking counterpart of RenderStillBlocking). Call once per UI frame on
// the main thread. No-op when no job is active. Updates job.done/finished/
// success/error so the Render modal can show progress.
void PumpRenderJob(AppState& state);

// Imports a CAD file (.step/.stp, .brep, .stl) into the ACTIVE part as an
// undoable ImportFeature and fits the camera. False (with *error set) when
// nothing imported.
bool ImportAsFeature(AppState& state, const std::string& filename,
                     std::string* error = nullptr);

// Reference-image underlays (v49). Add reads/decodes the file and centres it on
// the active sketch's plane; Calibrate scales it so p1..p2 spans `distance`
// (p1 fixed; alignAxis also rotates p1->p2 onto the U axis); Delete removes it.
// All commit one undoable ModifyFeatureCommand. See
// docs/sketch/reference-image-plan.md.
uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path);
void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis);
void DeleteReferenceImageAction(AppState& state, uint32_t imageId);

// Imports an assembly STEP as parts + an assembly (interchange I2): one part
// tab per unique product (geometry extracted as .brep into
// <project>/imported, or the staging area before the first save),
// occurrences placed from the file's instance tree. Not undoable (creates
// documents, like Open).
bool ImportStepAsAssembly(AppState& state, const std::string& stepPath,
                          std::string* error);

// THE import entry point (one Import… button): a STEP whose structure places
// several instances imports as an assembly (above); everything else —
// single-product STEP, .brep, .stl — imports as a feature into the active
// part.
bool ImportCADFile(AppState& state, const std::string& path,
                   std::string* error);

// Shared feature-creation actions (menu + toolbar call these). Each starts a
// preview the user accepts/cancels (see PreviewFeature).
void CreateBox(AppState& state);
void CreateSphere(AppState& state);
void CreateCylinder(AppState& state);
// Starts a preview of an offset datum plane (from XY, or from the selected
// datum if one is selected) — the first of the DatumPlane variants.
void CreateDatumPlane(AppState& state);
// Starts a preview of a two-point datum axis (along X by default).
void CreateDatumAxis(AppState& state);

void CreateCurve3D(AppState& state); // 3D curve (helix / fit-spline / poly)
void CreatePipe(AppState& state);    // pipe/tube along a path
void CreateSweep(AppState& state);   // sweep a profile along a path
void CreateLoft(AppState& state);    // loft through sections (optional guide)

// Menu/palette gating queries for the curve-driven tools.
size_t SketchFeatureCount(const Document& doc);
bool HasCurveOrSketch(const Document& doc);
// Starts a preview of a boolean combining the two most-recent body producers.
void CreateBoolean(AppState& state);
// Starts a preview of a mirror / linear pattern of the most-recent body.
void CreateMirror(AppState& state);
void CreatePattern(AppState& state);
// Starts a fillet/chamfer preview and arms edge picking: viewport clicks
// toggle B-Rep edges on the pending feature (M7).
void CreateFillet(AppState& state);
void CreateChamfer(AppState& state);
void CreateRib(AppState& state);
void CreateThread(AppState& state);

// Body-modifying tools whose parameters/slots already existed but that had no
// creation path outside the script API. Each opens the normal preview with
// the most recent body pre-targeted (Draft takes its body from the first face
// pick — it has no body slot); the required slots arm themselves.
void CreateShell(AppState& state);
void CreateDraft(AppState& state);
void CreateSplit(AppState& state);

// The plane a plane-referencing feature points at (a base plane, or an
// earlier DatumPlane by id), as the UI sees it: the datum's last regenerated
// output. nullopt when the id does not resolve. The regen path has
// ResolveParentPlane for the same job; this is its caller-side twin, for
// tools that must know where a plane IS while the user is picking it.
std::optional<SketchPlane>
ResolvePlaneForTool(AppState& state, uint64_t datumId, PlaneType basePlane);

// Re-derives a split's offset so the knife lands mid-body on its CURRENT
// plane. Called when the plane changes: an offset measured from the old
// plane means something else on the new one. Leaves a user-typed expression
// alone — that is theirs.
void RecenterSplitOffset(AppState& state, SplitFeature& split);
void CreateRefine(AppState& state);
// Arms plane-picking for a new sketch.
void StartNewSketch(AppState& state);
// Arms the Hole tool: pick a FLAT face (or plane) to drill into, which opens
// a point sketch on it with the Point tool ready; ✓/Enter then converts the
// centres into a Hole preview. A single planar face already selected skips
// the pick. No-op without a body (there would be nothing to cut).
void StartHoleTool(AppState& state);

// The stable ElementRef for a picked B-Rep edge (the body's createdBy + the
// edge's MappedName); nullopt when the edge has no stable name.
std::optional<ElementRef> EdgeElementRef(AppState& state, uint32_t meshId,
                                         uint32_t edgeIndex);

// Armed-tool pick consumers (U1c): the selection pipeline resolves what was
// clicked and hands it here; the tools own what happens next.

// Measure tool (C3): toggles the clicked element in the measured set —
// one element reads out its own measure, two their distance; measured
// elements stay highlighted until re-clicked / removed / Escape. On an
// assembly tab `occurrenceId` names the occurrence the element was picked
// on (0 = part context) — part of the element's identity.
void MeasureElementClicked(AppState& state, const ElementRef& ref, bool isEdge,
                           uint64_t occurrenceId = 0);
// Recomputes the readout after the picked set changes (panel removals).
void UpdateMeasureReadout(AppState& state);

// Blend edge collection (fillet/chamfer): toggles the clicked edge on `fil`,
// remapping the displayed (already blended) name onto the blend's input body
// first. meshId/edgeIndex are the displayed handles the geometric remap
// anchors on. Commits internally (preview: mutate+regen; committed: one undo
// step); sets an actionMsg for a blend-created edge instead. Seeding passes
// regenerate=false: a regen between batched toggles would blend the body
// mid-seed and strand the remaining pre-picked names (only the FIRST
// pre-selected edge survived).
void ApplyBlendEdgePick(AppState& state, FilletFeature& fil,
                        const ElementRef& displayed, uint32_t meshId,
                        uint32_t edgeIndex, bool regenerate = true);

// Appends edge highlight primitives while edge picking is active: every body
// edge dimmed, the hovered edge and the blend's picked edges emphasized.
void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out);

// A thick screen-space line segment (two triangles) for overlay primitives.
void AppendPrimLine(glm::vec3 a, glm::vec3 b, float halfPx,
                    const glm::vec4& color, std::vector<PrimVertex>& out);

// One prompt-pill renderer (U8b) for the top-center stack — the sketch and
// model prompt bars share it. Draws `text` as a rounded pill centered over
// the viewport at stack position `y`, advancing y for the next pill.
enum class PromptPill {
  StatusGood, // green: "Fully defined"
  Status,     // blue: DOF count etc.
  Prompt,     // neutral: what the armed tool expects
  Warning     // amber: actionMsg feedback
};
void DrawPromptPill(AppState& state, float& y, const std::string& text,
                    PromptPill style);

// The features in [0, beforeIndex) that produce a body (primitives, imports,
// extrude/revolve in NewBody mode, and earlier booleans), as {featureId,
// name}. Backs the boolean operand combos and the "Boolean" menu gate.
std::vector<std::pair<uint64_t, std::string>> BodyProducers(const Document& doc,
                                                            size_t beforeIndex);

// Appends translucent quads for every DatumPlaneFeature in the document,
// tinted distinctly from the base planes; the hovered/selected/pending datum
// is brightened. Visible while picking a plane, or when showPlanes is on
// (datums are user reference geometry, so they don't auto-hide behind bodies).
void AppendDatumPlaneQuads(AppState& state, std::vector<LineVertex3D>& quads);
// Cosmetic thread overlays (M8 thread): dashed thread-depth circles at both
// ends of each threaded span + dashed crest lines — the drafting convention,
// drawn from the session overlay each ThreadFeature computes at regen.
void AppendThreadOverlays(AppState& state, std::vector<LineVertex3D>& lines);

// Appends a line segment for every DatumAxisFeature (same visibility rule as
// the datum planes), tinted distinctly; selected/pending is brightened.
void AppendDatumAxisLines(AppState& state, std::vector<LineVertex3D>& lines);
// Draws every Curve3D feature's tessellated polyline (curves-3d Phase 0).
void AppendCurve3DLines(AppState& state, std::vector<LineVertex3D>& lines);

// Draws each visible datum plane's and datum axis's name as a 2D label
// (foreground draw list). Call inside the ImGui frame, near DrawDimensions.
void DrawDatumLabels(AppState& state);

// When state.showElementNames is on, labels the hovered face with its stable
// element name (topological-naming debug overlay). No-op otherwise.
void DrawElementNameOverlay(AppState& state);

// Whether the default planes should render this frame (visible by default,
// auto-hidden once the part has bodies, always shown while picking/editing).
bool EffectivePlaneVisibility(const AppState& state);

// Translucent default-plane quads (Front/Top/Right); hoveredPlane 0/1/2 is
// brightened, -1 for none. Empty when show is false.
std::vector<LineVertex3D> BuildPlaneQuads(bool show, int hoveredPlane);

// Appends a translucent fill quad for one plane (e.g. the active sketch plane).
void AppendPlaneQuad(const SketchPlane& plane, const glm::vec3& color,
                     float half, std::vector<LineVertex3D>& quads);

// The active sketch's work-plane fill and 1-unit grid extend this far from its
// origin, covering normal part-scale sketches without a tiny visible patch.
inline constexpr float kSketchBackgroundHalfExtent = 50.0f;
// Half-size of the visible default-plane quads and their plane-picking area.
inline constexpr float kDefaultPlaneHalfExtent = 8.0f;

// Appends grid lines for a sketch's work plane (minor/major cells + colored
// U/V axes), so it is clear what surface you are drawing on.
void AppendSketchGrid(const SketchPlane& plane,
                      std::vector<LineVertex3D>& lines);

// Appends the world-origin gizmo: three arrows (X red / Y green / Z blue).
void AppendOriginGizmo(std::vector<LineVertex3D>& lines);

// Appends the center-of-gravity marker (C3): a small amber 3-axis cross at
// state.cogWorld, when state.showCog is set by the mass-properties panel.
void AppendCogMarker(const AppState& state, std::vector<LineVertex3D>& lines);

// Highlighted line geometry for the sketch currently being edited (empty if
// none). Rendered as a per-frame overlay on top of the static sketch lines.
// Holds the rubber-band preview + gizmo/grid; entity lines and vertex dots go
// through BuildActiveSketchPrimitives instead.
std::vector<LineVertex3D> BuildActiveSketchOverlay(AppState& state,
                                                   uint64_t activeSketchId);

// Thick anti-aliased entity lines + constant-size vertex dots for the active
// sketch, with hover/selection emphasis. Empty if not editing a sketch.
std::vector<PrimVertex> BuildActiveSketchPrimitives(AppState& state,
                                                    uint64_t activeSketchId);

// Base-color thin lines for every sketch except the active one (which renders
// through BuildActiveSketchPrimitives). Rebuilt per-frame.
std::vector<LineVertex3D> BuildStaticSketchLines(AppState& state,
                                                 uint64_t activeSketchId);

} // namespace ui
