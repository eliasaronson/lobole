#pragma once

#include "cad/MeshExport.h" // MeshExport::Item
#include "core/AssemblySolve.h"
#include "core/ElementMap.h"               // ElementRef
#include "core/drawing/DrawingDimension.h" // drawing::ResolvedDim
#include "core/io/Autosaver.h"
#include "render/SketchRenderer.h" // Vertex3D, PrimVertex
#include "ui/AppState.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
class SketchFeature;
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
// Every selected occurrence, in selection order. The single-id form above is
// the head of this and stays the right question for the editor rows, which
// show ONE occurrence's transform.
[[nodiscard]] std::vector<uint64_t>
SelectedOccurrenceIds(const AppState& state);
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
size_t AddJob(AppState& state, const std::string& name);

// The bodies the VIEWPORT is showing: a part tab's document bodies, or a job
// tab's parts laid out. Anything that turns a picked mesh back into geometry
// has to ask this rather than `state.document`, which is null on a job tab —
// asking the document there returned nothing, so a click on a job selected
// nothing at all and said nothing about it.
const std::vector<Body>& ViewportBodies(AppState& state);
// Appends a copy of the tab at `index` — same kind, same contents, a new
// stable id and a name nothing else has taken ("Part 1 copy") — and switches
// to it. Returns the new tab's index, or SIZE_MAX if `index` names no tab.
//
// The copy is independent: a part's features are cloned and regenerated, and
// its undo history is NOT copied (that history describes the original). What
// the copy still POINTS at is unchanged — a copied assembly places the same
// parts, a copied drawing draws the same source document — and nothing
// pointing at the original is repointed at the copy.
size_t DuplicateDocument(AppState& state, size_t index);
void SetActivePart(AppState& state, size_t index);
// Closes a tab. Refused (with the reason in `why`) for the last tab, or
// while any assembly still places this part or assembly — a dangling
// reference is not something to create on purpose.
bool DeletePart(AppState& state, size_t index, std::string* why = nullptr);
// Why that tab cannot be deleted, or empty when it can. The menu asks BEFORE
// offering, so a refusal is a disabled item with a reason on it rather than a
// live one that does nothing.
[[nodiscard]] std::string WhyPartUndeletable(AppState& state, size_t index);

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

// AssemblyActions: see ui/AssemblyActions.h

// Drawing-edit undo (snapshot-based, the assembly pattern; workstream 13).
// Call SnapshotDrawing before mutating the active drawing's sheet/views/dims.
void SnapshotDrawing(AppState& state);
bool UndoDrawing(AppState& state);
bool RedoDrawing(AppState& state);

// Job undo (docs/cam/router-round.md, R7): a full snapshot, the drawing's
// pattern. SnapshotJob BEFORE a mutation; PushJobUndo when the pre-edit copy
// is already in hand (the guard below). Undo restores the copy, re-generates
// the toolpaths and re-verifies, and bumps the version past the current one
// so every cache keyed on it drops.
void SnapshotJob(AppState& state);
void PushJobUndo(AppState& state, const cam::JobDocument& before);
bool UndoJob(AppState& state);
bool RedoJob(AppState& state);

// The one way the CAM panel's forty-nine edit sites and every mutating RPC
// get onto the undo stack without each saying so: hold a copy and the job's
// fingerprint at construction; at destruction -- every return path -- push
// the copy if the fingerprint moved. `coalesce`: while an ImGui item is
// still active (a drag), hold the copy back and push it once when the item
// lets go, so a drag is one undo step and not sixty.
class JobUndoGuard {
public:
  explicit JobUndoGuard(AppState& state, bool coalesce = false);
  ~JobUndoGuard();
  JobUndoGuard(const JobUndoGuard&) = delete;
  JobUndoGuard& operator=(const JobUndoGuard&) = delete;

private:
  AppState& m_state;
  bool m_coalesce;
  std::optional<cam::JobDocument> m_before;
  std::string m_fingerprint;
};

// The tab index of the slot with a stable id (nullopt when gone) — the
// in-context banner's Return button and RPCs resolve slots through this.
std::optional<size_t> SlotIndexById(const AppState& state, uint64_t slotId);

// OccurrencePanel: see ui/OccurrencePanel.h

// The placement a mesh renders at (solved ∘ explode), exposed for tests and
// for anything that has to reproduce the live-drag composition.
glm::mat4 DisplayMatrixOf(const AppState& state, const Occurrence& o);

// DrawingPanel: see ui/DrawingPanel.h

// Re-projects every view of the active drawing into state.drawingRenders (the
// HLR-per-view work). Explicit (D2): called on drawing-tab entry and by the
// update_drawing RPC / panel button — never per frame, since HLR is expensive.
// Failed views keep their last-good geometry with an error string.
// Re-project every view of the active drawing whose source has moved.
//
// `budgetSeconds` > 0 bounds the WHOLE update: a view still running when the
// budget is gone stops where it is, keeps its last-good geometry and reports
// that it ran out (OCC's Hide() has no cancel hook of its own — the pairwise
// drive in DrawingProjection.cpp is what makes stopping possible at all).
// 0 = run to completion, which is what every existing caller wants and gets.
//
// The budget exists because exact HLR is priced by SURFACE complexity and can
// therefore surprise you: 16 bored boxes project in 3.1 ms, but one four-turn
// helical spring takes 6.4 s and the single-cylinder engine took 85 s that
// fell to 0.0 when its two valve springs were hidden. A three-view sheet of it
// had not finished after 21 minutes, with no way to interrupt it and the RPC
// server blocked behind it (docs/agent-scale-followup-plan.md, W3).
void UpdateDrawingRenders(AppState& state, double budgetSeconds = 0.0);

// Adds a view to the active drawing, choosing the two things a human should
// not have to compute: a standard scale that makes the part legible on this
// sheet (only when `v.scale` is <= 0), and a spot clear of the views already
// placed. Snapshots for undo and re-projects. Shared by the drawing
// toolbar's Add-view menu and the panel's Add view button.
uint64_t AddDrawingViewPlaced(AppState& state, DrawingView v);

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

// Auto-place a new dim's label clear of every view, the title block and the
// labels already on the sheet (drawing::PlaceDimOffset with live context).
// The {0,-12} fallback covers an unprojected view.
glm::dvec2 AutoDimOffset(AppState& state, const DrawingDim& dim);

// Re-projects and writes the active drawing to `path` (.svg or .pdf by
// extension). The shared export path for the export_drawing RPC and the GUI
// Export dialog. Returns false and fills *error on failure.
bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error);

// ---- Simulation mode (FEM) ------------------------------------------------

// SimulationActions: see ui/SimulationActions.h

// ---- Topology optimization runs (TO2, async) ------------------------------
// A topology study's SIMP solve is minutes long, so it runs on a worker thread
// over an immutable snapshot; the main thread polls and stores the result at
// the single join point. One run at a time.

// StudyPanel: see ui/StudyPanel.h

// Save/open a project directory. A multi-part project writes a project.json
// index + one `parts/<name>/` directory each; a single-part project stays
// the flat `.dsgn` (index.json + features/) form for backward compatibility.
// Open detects project.json to load all parts, else loads one part.
// Save the project. Refuses when the project on disk changed since we last
// read or wrote it (F8) — the write would replace someone else's work with
// no way back — unless `overwriteExternalChanges` says to do it anyway.
// Every save funnels through here: Ctrl+S, Save As, naming a version, and
// the save_project RPC, so this is the one guard that covers them all.
bool SaveProject(AppState& state, const std::string& path, std::string* error,
                 bool overwriteExternalChanges = false);

// Open `path` and lay every autosaved document over it (F10). The project
// itself comes from disk; each document with a sidecar newer than its saved
// form is replaced by the sidecar. All-or-nothing on purpose: a project half
// from one session and half from another is a state nobody can reason about.
//
// Leaves the project DIRTY — the recovered work has not been saved, and
// pretending otherwise would invite closing without a prompt.
bool RecoverProject(AppState& state, const std::string& path,
                    std::string* error = nullptr);

// What the GEOMETRY did between a version and now, for one part document
// (F11 layer 3). The recipe as it was is extracted from the revision,
// regenerated, and boolean-diffed against the part in front of you.
//
// This is where compare_geometry's machinery belongs and a merge's does not:
// a diff has two operands by construction. `Measure::CompareBodySets` does
// the booleans; this only finds the other operand.
//
// Also MATERIALIZES the result (SetGeometryDelta): the honest answer to
// "what changed?" is a shape, not a number, so the diff solids go straight
// on the model — red where material went away, green where it arrived.
GeometryChange CompareVersionGeometry(AppState& state,
                                      const std::string& revision,
                                      const std::string& documentDir);

// Show the user a changed item: switch to the document that holds it and
// select it, so the feature tree highlights the row (F11 G2 follow-on).
// `frame` also fits the camera to it, which is the double-click gesture —
// a single click should not move the view under someone reading a list.
//
// Returns false when there is nothing to show: the item was REMOVED by the
// change, or its document is gone. Saying so beats a click that silently
// does nothing.
bool RevealItem(AppState& state, const std::string& document,
                const std::string& documentKind, uint64_t itemId, bool frame);

// Autosave every DIRTY document of the project, one atomic file each (F10).
//
// Lives here rather than in Autosaver because only this layer knows what
// documents a project has. That is the bug it fixes: autosave was handed
// `state.document` — the ACTIVE TAB — so a multi-document project autosaved
// one part, and an assembly or drawing tab (where that pointer is null)
// autosaved nothing at all, silently.
//
// A project that IS one document keeps the single-file sidecar unchanged.
// Returns the number of documents written.
int AutosaveProject(AppState& state, Autosaver& autosaver, double now);

// Give the project a remote (F9 C2), and stop it committing built geometry
// (F9 C4) — for one person the baked bodies make opening a load instead of a
// rebuild, but on a shared project they are megabytes of binary pushed on
// every save, conflicting on every concurrent edit, and discarded either way
// because the chain hash decides at load. Saves and commits that change, so
// the remote's first push does not carry them.
bool AddProjectRemote(AppState& state, const std::string& name,
                      const std::string& url, std::string* error = nullptr);

// Record what the project directory at `path` looks like NOW, as the thing
// later saves compare against. Called wherever the app and the disk are known
// to agree: after LOADING one (we just read it), and after saving one
// (including the commit that save makes).
void StampDiskState(AppState& state, const std::string& path);

// Has someone else written to the project since StampDiskState? Walks the
// directory, so it is a save-time / poll-time question, not a per-frame one.
// False when there is no stamp to compare (a project never saved, or a Save
// As to a path we have not seen).
bool ProjectChangedOnDisk(AppState& state);
bool OpenProject(AppState& state, const std::string& path, std::string* error);

// ---- Project-wide variables (Phase V, docs/dogfooding.md, caster round)
// --------- The project parameter table lives in AppState and is shared by
// every part via Document::SetProjectSymbols; set_variable defaults to this
// scope.

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
// The cut plane {n, d} a section produces: its source plane (base, datum or
// picked face — the latter two resolved live against the document), offset
// along that normal, then tilted about the offset point (P2).
glm::vec4 SectionPlaneOf(const AppState& state, const AppState::SectionView& s);

// False when the cut names a datum plane or a face that does not resolve —
// deleted, replaced, or (for either) an assembly tab, where a part's plane
// has no meaning. Such a cut stops cutting and the tool row says why.
bool SectionSourceResolved(const AppState& state,
                           const AppState::SectionView& s);

// Arms the Section tool's face slot for `sectionId` — the next planar face
// picked in the viewport becomes that cut's source plane (P2).
void ArmSectionFacePick(AppState& state, uint64_t sectionId);
// Routed here by SlotPickClicked when that slot is armed.
void SectionFacePicked(AppState& state, const SelectionItem& item);
ClipPlaneArray SectionPlanes(const AppState& state); // enabled ones, padded

// A world section plane in ONE occurrence's part space. Caps are computed on
// the part's own bodies (which is where its geometry lives) and placed by the
// occurrence transform afterwards, so the plane has to make the trip inward
// first. Returns false for a disabled (all-zero) plane. Shared by the raster
// assembly sync and the ray-traced still — one copy of n' = Rᵀn, d' = d - n·t.
bool SectionPlaneInPartSpace(const glm::vec4& world, const glm::mat4& xf,
                             glm::vec3& localOrigin, glm::vec3& localNormal);
bool SectionActive(const AppState& state);

// The cut the sketch slice wants, or nullopt when it is off, no sketch is
// open, or the sketch does not resolve. Oriented to keep the half AWAY from
// the camera, so it follows the view rather than blanking the model when you
// orbit past the plane.
std::optional<glm::vec4> SketchSlicePlane(const AppState& state);
// Is there a clip-plane slot left for the slice to use? The renderers carry
// four (render/ClipPlanes.h) and the section list can claim all of them.
bool CanSketchSlice(const AppState& state);
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

// Applies (and removes) the transient view rollback that "edit sketch" wants:
// the model stops just before the first feature built FROM the open sketch,
// so the solid the sketch produced is not sitting in front of the sketch —
// the rollback SolidWorks and Fusion do on Edit Sketch. Reconciles against
// sketchEdit.activeSketchId, so the seventeen places that enter or leave a
// sketch session (including teardown paths like a tab switch or an undo that
// removed the sketch) do not each have to remember to put the marker back.
// Call once per frame, BEFORE SyncDocumentRenderers — it can regenerate, and
// the projection that follows must see the bodies it settled on.
void SyncSketchViewRollback(AppState& state);

// Mirrors document bodies into the renderers when the document version
// changed (edit, undo, redo). Call once per frame before drawing.
void SyncDocumentRenderers(AppState& state);

// The whole document->renderer projection as ONE idempotent unit, in the
// order the steps depend on each other. Every step is version- or key-gated,
// so calling it when nothing changed is a handful of branches — which is what
// lets a caller that cannot know whether it is needed simply call it.
//
// It exists for --offscreen, which draws only when asked and therefore runs
// this LAZILY: the tick does not project, and everything that reads what the
// projection produces — a capture, and the RPCs that resolve an element to a
// render mesh — calls this first. The saving is not small: on a 200-body part
// a resync costs ~2-5 s (the tessellation cache does not survive a full
// regen), which an agent that never looks at the model used to pay on EVERY
// edit. The windowed loop draws every frame, so it still projects inline.
//
// A reader that forgets to call it does not fail loudly: `documentMeshes`
// still maps body index -> mesh id, for the PREVIOUS geometry. Ask
// RenderMeshesCurrent() rather than assuming.
void SyncRenderersToDocument(AppState& state);

// Are `documentMeshes` (and the picking data beside them) the projection of
// the CURRENT tab at its CURRENT version? False when nothing has been
// projected yet, when an edit has landed since, and always without a
// renderer. Volatile render ids must not be reported when this is false: the
// list still has the right SHAPE, so an index reads back a plausible id
// belonging to superseded geometry.
bool RenderMeshesCurrent(AppState& state);

// Drops every cached part tessellation and releases the GPU assets it holds
// (AppState::partRenderCache). MUST run while the renderers are still alive —
// the cache stores asset ids, and the release is what frees their buffers —
// so main.cpp calls it immediately before ShutdownSceneRenderers().
void ReleasePartRenderCache(AppState& state);

// Show a boolean-diff result ON the model: `missing` (in the reference and
// not in what you are looking at) in red, `extra` (beyond the reference) in
// green, both translucent. This is compare_geometry's `materialize` and the
// History panel's "Compare shape" — the answer to "where did it change?",
// which no single number can give.
//
// `summary` is the comparison's own words, shown with the overlay so it is
// never a mystery what the two colors are measured against. Setting a delta
// forces a renderer resync; passing empty shapes is the same as clearing.
void SetGeometryDelta(AppState& state, const CADShape& missing,
                      const CADShape& extra, const std::string& summary);
void ClearGeometryDelta(AppState& state);

// Rebuilds the delta overlay's instances, and drops it when it has stopped
// being true of what's on screen. Called from SyncDocumentRenderers.
void SyncGeometryDelta(AppState& state);

// Mirrors the current document bodies into the ray tracer (geometry +
// materials) and (re)builds its acceleration structures. On-demand for the
// offline render; returns false if RT is unavailable or there's no geometry.
// `groundPlane` adds a shadow-catcher quad under the scene (offline renders
// only).
bool SyncRayTracingScene(AppState& state, bool groundPlane = false);

// Builds the offline-render descriptor from the current app state (camera +
// shared IBL maps) for a `width`x`height` still with the given RNG seed.
// `outlines` is ANDed with the viewport's own outline mode, so a still draws
// edges exactly when the screen it came from does.
::RTStillDesc MakeStillDesc(AppState& state, uint32_t width, uint32_t height,
                            uint32_t seed, bool studioBackdrop = true,
                            bool outlines = true);

// Full synchronous offline still: sync the scene, build ASes, trace `samples`
// samples, tonemap and write a PNG to `path`. Blocks until done. Returns false
// (with *error set) on any failure. Used by the render_still RPC; the UI modal
// drives BeginStill/RenderBatch itself for progress. `seed` fixes the RNG so a
// given (scene, samples, seed) is reproducible.
// `warning` (optional, out): non-empty when the still is a valid picture of
// nothing — the scene's bounds fall entirely outside the camera's frustum.
// Saved is not the same as good, and this is the difference.
bool RenderStillBlocking(AppState& state, uint32_t width, uint32_t height,
                         uint32_t samples, const std::string& path,
                         uint32_t seed, bool groundPlane, std::string* error,
                         std::string* warning = nullptr,
                         bool studioBackdrop = true, bool outlines = true);

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

// Same, for a caller that has ALREADY parsed the file. The GUI import worker
// parses it to decide part-vs-assembly (ui/ImportJob.cpp); without this a
// single-product STEP is then read a SECOND time by the feature's first
// regen — the whole file twice, ~12 s each at 48 MB. `parsed` is moved from,
// and a later regen re-reads the path exactly as before.
bool ImportAsFeature(AppState& state, const std::string& filename,
                     CADKernel::ImportedModel* parsed, std::string* error);

// Reference-image underlays (v49). Add reads/decodes the file and centres it on
// the active sketch's plane; Calibrate scales it so p1..p2 spans `distance`
// (p1 fixed; alignAxis also rotates p1->p2 onto the U axis); Delete removes it.
// All commit one undoable ModifyFeatureCommand. See
// docs/sketching/reference-image-plan.md.
uint32_t AddReferenceImageToSketch(AppState& state, const std::string& path);
void CalibrateReferenceImageAction(AppState& state, uint32_t imageId,
                                   glm::vec2 p1, glm::vec2 p2, double distance,
                                   bool alignAxis);
void DeleteReferenceImageAction(AppState& state, uint32_t imageId);

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
// A named datum frame (v86, agent-scale plan G0), explicit at the part origin
// until edited; a mate anchors to it by name.
void CreateFrame(AppState& state);

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
void MeasureElementClicked(AppState& state, const MeasureState::Element& e);
// Recomputes the readout after the picked set changes (panel removals).
void UpdateMeasureReadout(AppState& state);

// M3: the live measurement of whatever the cursor is over, against what is
// already picked (or on its own when nothing is). Runs once per frame after
// the pick pipeline has said what is hovered; the OCC work is cached against
// the hovered element, the anchor and the document version, so a stationary
// cursor costs nothing.
void UpdateMeasureHover(AppState& state);

// M4: move the current measurement into the pinned list and clear the picks
// for the next one. No-op when there is nothing measured. Comparing three
// gaps used to mean writing two of them down.
// Select first, then measure (M4b): the Face/Edge items already in the
// selection become the measured set, in PICK ORDER — which the selection
// set has always preserved, and which is exactly what makes one of them `a`
// and the other `b`. More than two seeds a total rather than being dropped.
void SeedMeasureFromSelection(AppState& state);

// How a person names a measured element for display: "Bracket - top face",
// "Board - 600 mm edge", "Plate - Ø25 bore", with the occurrence prefix on an
// assembly. The tool window's rows read `Edge 1/face3;U0` before this — the
// internal stable name, which is what makes a measurement survive a regen
// and exactly what nobody should have to read. The stable name stays the
// identity and becomes the tooltip.
std::string MeasureElementLabel(AppState& state,
                                const MeasureState::Element& e);

void PinMeasurement(AppState& state);

// M6: the number the current measurement leads with, as a bare value in the
// document's units and with no label — what belongs on a clipboard.
// Empty when nothing is measured.
std::string MeasuredValueForClipboard(const AppState& state);

// ...and the same number as a VARIABLE, which is the point at which
// measuring stops being read-only. Creates (or redefines) `name` at project
// scope with the measured value as a literal expression in millimetres —
// storage units, like every other expression — and returns false with the
// reason when there is nothing to bind or the name is unusable.
bool CreateVariableFromMeasurement(AppState& state, const std::string& name,
                                   std::string* why = nullptr);
// Drop one pinned measurement by id, or all of them.
void UnpinMeasurement(AppState& state, uint64_t id);
// Recompute every pinned measurement against the current document. Their
// refs are stable names, so an edit that CHANGES a measured face moves the
// number; an edit that DESTROYS it drops the pin rather than leaving a stale
// dimension floating over geometry that no longer exists.
void RefreshPinnedMeasurements(AppState& state);

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

// The calibrate-image picks and the span between them, on the sketch plane.
void AppendCalibrationMarkers(AppState& state, std::vector<PrimVertex>& out);

// A constant-size round dot, for marking points on the model (CAM leftover).
void AppendPrimDot(glm::vec3 p, float radiusPx, const glm::vec4& color,
                   std::vector<PrimVertex>& out);

// One prompt-pill renderer (U8b) for the top-center stack — the sketch and
// model prompt bars share it. Draws `text` as a rounded pill centered over
// the viewport at stack position `y`, advancing y for the next pill.
// PromptPill / DrawPromptPill moved to ui/Overlays.h.

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
// Appends an RGB triad (X red, Y green, Z blue) for every resolved
// FrameFeature, `FrameTriadLength` long — derived from the scene's extent
// like the datum planes, so a frame on a 2 mm part and one on a 2 m dock
// both read.
void AppendFrameLines(AppState& state, std::vector<LineVertex3D>& lines);
float FrameTriadLength(AppState& state);
// Draws every Curve3D feature's tessellated polyline (curves-3d Phase 0).
void AppendCurve3DLines(AppState& state, std::vector<LineVertex3D>& lines);

// Draws each visible datum plane's and datum axis's name as a 2D label
// (foreground draw list). Call inside the ImGui frame, near DrawDimensions.

// When state.showElementNames is on, labels the hovered face with its stable
// element name (topological-naming debug overlay). No-op otherwise.

// Whether the default planes should render this frame (visible by default,
// auto-hidden once the part has bodies, always shown while picking/editing).
bool EffectivePlaneVisibility(const AppState& state);

// Translucent default-plane quads (Front/Top/Right); hoveredPlane 0/1/2 is
// brightened, -1 for none. `half` is ScenePlaneHalfExtent — the same number
// the plane PICKING uses, or the quad you can see and the quad you can click
// are different rectangles. Empty when show is false.
std::vector<LineVertex3D> BuildPlaneQuads(bool show, int hoveredPlane,
                                          float half);

// Appends a translucent fill quad for one plane (e.g. the active sketch plane).
void AppendPlaneQuad(const SketchPlane& plane, const glm::vec3& color,
                     float half, std::vector<LineVertex3D>& quads);

// What reference geometry measures in an EMPTY document — the floor, not the
// answer: both are scaled up by the document's own reach below.
inline constexpr float kSketchBackgroundHalfExtent = 50.0f;
inline constexpr float kDefaultPlaneHalfExtent = 8.0f;

// How far the base planes reach from the origin, and the size everything else
// that is a REFERENCE (datum quads, the sketch background) is derived from:
// the document's own reach, floored at the empty-workspace default. A unit
// default has to come from what it acts on (docs/ui/u10-defaults-and-
// refusals.md) — 8 mm planes are a speck against a 1 m speaker panel, which
// is exactly the part you most need a plane to sketch on. Cached against the
// document's version: the bbox under it is an OCC pass per body, and this is
// asked for every frame the planes are up.
float ScenePlaneHalfExtent(AppState& state);

// The active sketch's grid: how far it reaches, and how big one cell is.
struct SketchGridMetrics {
  float half = kSketchBackgroundHalfExtent;
  float step = 1.0f;
};
// Sized from the sketch's OWN entities first (the outline of a metre-tall
// speaker is drawn before any body exists, so the document cannot answer it),
// then the part around it. The step follows a 1-2-5 decade so the cell count
// stays constant: a metre-scale sketch gets 20 mm cells instead of a thousand
// 1 mm ones that read as a solid grey sheet.
SketchGridMetrics SketchGridFor(AppState& state, const SketchFeature& sketch);

// Appends grid lines for a sketch's work plane (minor/major cells + colored
// U/V axes), so it is clear what surface you are drawing on.
void AppendSketchGrid(const SketchPlane& plane, const SketchGridMetrics& grid,
                      std::vector<LineVertex3D>& lines);

// Appends the world-origin gizmo: three arrows (X red / Y green / Z blue).
// Screen-stable like the COG marker — the origin is a MARKER, and a fixed
// 1.5 mm arrow is invisible on anything bigger than a fastener.
void AppendOriginGizmo(const AppState& state, std::vector<LineVertex3D>& lines);

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
