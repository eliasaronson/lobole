#pragma once

#include "MeshExport.h"      // MeshExport::Item
#include "SketchRenderer.h" // Vertex3D, PrimVertex
#include "core/DrawingDimension.h" // drawing::ResolvedDim
#include "core/ElementMap.h" // ElementRef
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
struct RTStillDesc; // defined in RayTracingRenderer.h
struct Ray;         // defined in FaceSelector.h

namespace ui {

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
bool DeletePart(AppState& state, size_t index);

// The document id (PartSlot::id) for a part name, or 0 if no such part.
// Occurrences reference parts by this stable id, never by name.
uint64_t PartIdByName(const AppState& state, const std::string& name);

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
  uint64_t occurrenceId = 0;
  ElementRef face;
  ElementRef snapEdge; // inference snap: nearest circular-edge (hole) center, if any
};
bool PickAssemblyFace(AppState& state, const Ray& ray, AssemblyFacePick& out);

// Mate creation tool (A3 phase 3). StartMateTool arms it with a type;
// MateToolPick feeds it a viewport ray (picks the two connector faces, then
// adds a live preview mate); Accept keeps the preview, Cancel removes it.
// All no-op without an active assembly.
void StartMateTool(AppState& state, MateType type);
void MateToolPick(AppState& state, const Ray& ray);
void AcceptMateTool(AppState& state);
void CancelMateTool(AppState& state);
// The world frame of a connector (occurrence placement ∘ its part-local frame),
// or nullopt if it doesn't resolve. For drawing the connector triad.
std::optional<glm::mat4> ConnectorWorldFrame(AppState& state, const Connector& c);
// Appends the mate tool's connector triads (picked connectors) to `lines`.
void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines);

// The 3D drag gizmo (A2.5). BuildGizmoOverlay appends the translate/rotate
// handles for the selected non-fixed occurrence; HandleGizmoInput runs the drag
// state machine (grab → live per-frame transform → release snapshots+solves) and
// returns true when it consumed the viewport (so the caller skips pick/orbit).
void BuildGizmoOverlay(AppState& state, std::vector<LineVertex3D>& lines);
bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked,
                      bool down, bool released);

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

// The current Document::Version() / AssemblyDocument::Version() of a source
// document (part or assembly) by stable id, or 0 if absent. A drawing view is
// stale when this differs from the version stamped at its last projection.
uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId);

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
// each frame and before reporting study status. Defined in SimulationActions.cpp.
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
};
TopoRunStatus GetTopologyStatus(const AppState& state, uint64_t studyId);

// The Studies panel drawn in the Model dock while in simulation mode. Defined
// in StudyPanel.cpp.
void DrawStudyPanel(AppState& state);

// The result-field colorbar legend overlaid on the viewport while a field is
// displayed. Defined in StudyPanel.cpp.
void DrawFieldLegend(AppState& state);

// Assigns the armed boundary condition (state.simulation.tool) to the face
// under `ray`, as an undoable study edit. No-op if no tool is armed or the
// pick misses a named face. Defined in StudyPanel.cpp.
void SimulationPick(AppState& state, const Ray& ray);

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
  std::string partName;      // "(missing part)" when the reference dangles
  int quantity = 0;
  std::string material;      // slug; "" = none, "mixed" when bodies differ
  double unitVolume = 0.0;   // mm^3, all bodies of the part
  bool massKnown = false;
  double unitMassG = 0.0;    // valid when massKnown
};
struct BomTable {
  std::vector<BomRow> rows;
  bool massKnown = true;     // false when any row's mass is unknown
  double totalMassG = 0.0;   // valid when massKnown
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

// Track the on-disk identity for dirty-state + autosave: call after every
// clean save (also clears the autosave sidecar) / open / new. "" = unsaved.
void MarkProjectSaved(AppState& state, const std::string& path,
                      const std::string& message = "save");
void MarkProjectOpened(AppState& state, const std::string& path);
// After loading a recovery sidecar: the document belongs at `path` but is
// deliberately dirty (unsaved recovered changes), so autosave keeps running
// and a save prompt is meaningful.
void MarkProjectRecovered(AppState& state, const std::string& path);

// Frames the camera on the document's combined bounding box.
void FitCameraToDocument(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry + materials)
// and (re)builds its acceleration structures. On-demand for the offline render;
// returns false if RT is unavailable or there's no geometry. `groundPlane`
// adds a shadow-catcher quad under the scene (offline renders only).
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

// Imports a STEP file as an undoable ImportFeature and fits the camera.
void LoadSTEPFile(AppState& state, const std::string& filename);

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
// Starts a preview of a boolean combining the two most-recent body producers.
void CreateBoolean(AppState& state);
// Starts a preview of a mirror / linear pattern of the most-recent body.
void CreateMirror(AppState& state);
void CreatePattern(AppState& state);
// Starts a fillet/chamfer preview and arms edge picking: viewport clicks
// toggle B-Rep edges on the pending feature (M7).
void CreateFillet(AppState& state);
void CreateChamfer(AppState& state);
// Arms plane-picking for a new sketch.
void StartNewSketch(AppState& state);

// The stable ElementRef for a picked B-Rep edge (the body's createdBy + the
// edge's MappedName); nullopt when the edge has no stable name.
std::optional<ElementRef> EdgeElementRef(AppState& state, uint32_t meshId,
                                         uint32_t edgeIndex);

// Routes viewport hover/click to the Measure tool while state.measure.active
// (C3): picks a face or edge, describes one element or the distance between
// two, into state.measure.readout. Returns true when it consumed the input.
bool HandleMeasureInput(AppState& state, bool overViewport);

// Routes viewport hover/click to B-Rep edge picking while state.edgePick is
// active (fillet/chamfer edge collection). Returns true when it consumed the
// viewport input for this frame.
bool HandleEdgePickInput(AppState& state, bool overViewport);

// Appends edge highlight primitives while edge picking is active: every body
// edge dimmed, the hovered edge and the blend's picked edges emphasized.
void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out);

// The features in [0, beforeIndex) that produce a body (primitives, imports,
// extrude/revolve in NewBody mode, and earlier booleans), as {featureId,
// name}. Backs the boolean operand combos and the "Boolean" menu gate.
std::vector<std::pair<uint64_t, std::string>> BodyProducers(
    const Document& doc, size_t beforeIndex);

// Appends translucent quads for every DatumPlaneFeature in the document,
// tinted distinctly from the base planes; the hovered/selected/pending datum
// is brightened. Visible while picking a plane, or when showPlanes is on
// (datums are user reference geometry, so they don't auto-hide behind bodies).
void AppendDatumPlaneQuads(AppState& state, std::vector<LineVertex3D>& quads);

// Appends a line segment for every DatumAxisFeature (same visibility rule as
// the datum planes), tinted distinctly; selected/pending is brightened.
void AppendDatumAxisLines(AppState& state, std::vector<LineVertex3D>& lines);

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

// Appends grid lines for a sketch's work plane (minor/major cells + colored
// U/V axes), so it is clear what surface you are drawing on.
void AppendSketchGrid(const SketchPlane& plane, std::vector<LineVertex3D>& lines);

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
