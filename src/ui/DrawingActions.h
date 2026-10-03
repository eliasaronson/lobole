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
#include <set>
#include <string>
#include <utility>
#include <vector>

class SketchPlane;
class SketchFeature;
class SplitFeature;
class FilletFeature; // defined in core/features/FilletFeature.h
struct RTStillDesc;  // defined in RayTracingRenderer.h
struct Ray;          // defined in FaceSelector.h

// Drawing views: projecting, placing, dimensioning and exporting them.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

struct DrawingViewWork; // ui/DrawingJob.h

drawing::ResolvedNote ResolveDrawingNote(AppState& state,
                                         const DrawingNote& note);

// Bringing the active drawing's views up to date
// (docs/drawings/README.md#projection-off-the-frame). A view is projected
// (the hidden-line removal, the dear half) and then assembled onto the sheet
// (placed, scaled, turned, its curves named; cheap). Each view keeps its
// projection (state.drawingProjections), so anything that only moves a view
// on the sheet assembles it again and projects nothing.
//
// RequestDrawingUpdate is the GUI's: it assembles every view whose inputs
// have not moved since its projection, and hands the rest to a `drawing`
// job, so the frame never waits for HLR; a view projecting shows its last
// geometry, or a placeholder at its source's bounds, until it lands. With
// `state.drawingDeferProjection` off (the GUI suite) it is
// UpdateDrawingRendersNow. `only` limits the projecting to those views (the
// rest are assembled as they are, stale or not).
void RequestDrawingUpdate(AppState& state, double budgetSeconds = 0.0,
                          const std::set<uint64_t>* only = nullptr);

// The same, waiting: joins a `drawing` job in flight (taking its caches
// back), then projects whatever is owed inline. For the callers that may
// wait -- an RPC in a `batch` or on --headless, a test -- and never the
// frame: test_conventions.cpp fails on a call to it from any file under
// src/ui but this one.
//
// `budgetSeconds` > 0 bounds the WHOLE update: a view still running when the
// budget is gone stops where it is, keeps its last-good geometry and reports
// that it ran out (OCC's Hide() has no cancel hook of its own — the pairwise
// drive in DrawingProjection.cpp is what makes stopping possible at all).
//
// The budget exists because exact HLR is priced by SURFACE complexity and can
// therefore surprise you: 16 bored boxes project in 3.1 ms, but one four-turn
// helical spring takes 6.4 s and the single-cylinder engine took 85 s that
// fell to 0.0 when its two valve springs were hidden. A three-view sheet of it
// had not finished after 21 minutes, with no way to interrupt it and the RPC
// server blocked behind it (docs/drawings/README.md#projection-hlr).
void UpdateDrawingRendersNow(AppState& state, double budgetSeconds = 0.0);

// Projects `view` -- one about to be added or changed, not yet on the sheet --
// on a `drawing` job into the projection cache, so the synchronous update
// that adds it finds it there: what a parked RPC does before it lets the
// handler run (remote/RemoteDrawings.h). False when nothing needed
// projecting, a run is in flight, or projection is not deferred.
bool PrefetchDrawingView(AppState& state, const DrawingView& view);

// A view moved on the sheet (its scale, turn or place) or one was deleted:
// every view assembled again from its kept projection, stale ones as stale,
// and only a view that has none requested.
void ReassembleDrawingViews(AppState& state);

// Whether a `drawing` job is projecting `viewId` of the active drawing (or
// the view a detail is of).
[[nodiscard]] bool DrawingViewProjecting(const AppState& state,
                                         uint64_t viewId);

// Where the sheet draws a projecting view that has no geometry yet: its
// source's bounds seen along the view, at the view's scale and place
// (sheet mm). False when it has none.
bool DrawingPlaceholderBox(const AppState& state, const DrawingView& view,
                           glm::dvec2& min, glm::dvec2& max);

// Lands one view a `drawing` job finished (ui/DrawingJob.cpp): on the active
// drawing, assembled now; on a stashed one, kept and assembled when its tab
// is shown; dropped when the tab is gone. `why` is "" (it ran), "cancelled"
// or "budget".
void LandDrawingViewWork(AppState& state, uint64_t slotId,
                         DrawingViewWork& work, const std::string& why);

// The run for `slotId` has ended: whatever waited on it (an export, an
// update asked for meanwhile) goes now.
void DrawingRunEnded(AppState& state, uint64_t slotId, bool cancelled);

// A drawing tab was just shown: views a job landed while it was stashed are
// assembled, an update owed is requested, and a tab never projected this
// session asks for its first (design §2).
void DrawingTabShown(AppState& state);

// The project is going away: a run in flight is cancelled and joined, and
// the session's projection state let go.
void ForgetDrawingProjections(AppState& state);

// Adds a view to the active drawing, choosing the two things a human should
// not have to compute: a standard scale that makes the part legible on this
// sheet (only when `v.scale` is <= 0), and a spot clear of the views already
// placed. Snapshots for undo and re-projects. Shared by the drawing
// toolbar's Add-view menu and the panel's Add view button.
//
// Placing needs the view's size, which its projection gives. Deferred (the
// GUI's default, or `defer` true), the view is added at once at a place and
// scale estimated from its source's bounds, and placed again by its real
// bounds when its projection lands -- one undo step either way.
uint64_t AddDrawingViewPlaced(AppState& state, DrawingView v,
                              std::optional<bool> defer = std::nullopt);

// The current Document::Version() / AssemblyDocument::Version() of a source
// document (part or assembly) by stable id, or 0 if absent. A drawing view is
// stale when this differs from the version stamped at its last projection.
uint64_t DrawingSourceVersion(AppState& state, uint64_t sourceDocId);

// The part Document behind a drawing view's sourceDocId; null for assembly
// sources (occurrence resolution is a follow-up) or a missing slot. Lets
// list_drawing_geometry name the feature a projected curve came from.
Document* DrawingSourceDocument(AppState& state, uint64_t sourceDocId);

// A part evaluated in `configuration`: the live document when that is 0 or
// already active, else a copy regenerated under it and cached in
// AppState::configuredSources until the part or its configurations move.
// Null, with `error` (may be null) saying why, for a slot that is not a part,
// a configuration it does not have (naming the ones it has), or one in which
// a feature fails. Drawing views and Derived copies both read through this.
const Document* ConfiguredPartDocument(AppState& state, uint64_t partId,
                                       uint64_t configuration,
                                       std::string* error = nullptr);

// The part a VIEW draws: its source evaluated in the view's configuration
// (the live document when that is 0 or already active, else a cached copy).
// Null for assembly sources, a missing slot, or a configuration the part no
// longer has — and then `error` (may be null) says which.
const Document* DrawingViewDocument(AppState& state, const DrawingView& view,
                                    std::string* error = nullptr);

// The units the active sheet is written in: its first view's SOURCE document,
// not the reader's preference — a drawing of an inch part is an inch drawing
// wherever it is opened. One accessor, because the title block on screen, the
// title block in the exported file and the dimension text itself all have to
// agree about it; they did not, and the numbers were mm regardless.
units::System ActiveDrawingUnits(AppState& state);

// The active drawing's sheet as it goes out: the title block's blank
// MATERIAL and SCALE filled from the first view (its part's material, its
// scale). What the viewport shows and what an export writes, so they agree.
Sheet DrawingSheetForOutput(AppState& state);

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

// Writes the active drawing to `path` (.svg or .pdf by extension), bringing
// its views up to date first when `update`. The export_drawing RPC's path,
// and the GUI Export dialog's through ExportActiveDrawingWhenProjected. Returns
// false and fills *error on failure. `update` false writes the views as they
// are (the caller has just brought them up to date, or a parked RPC's job has).
bool ExportActiveDrawing(AppState& state, const std::string& path,
                         std::string* error, bool update = true);

// File > Export: writes now when nothing needs projecting, else when the
// last view lands (the progress shown on the drawing panel); an export never
// writes a stale or half-projected sheet. False with `error` when it cannot
// even start.
bool ExportActiveDrawingWhenProjected(AppState& state, const std::string& path,
                                      std::string* error);

// A balloon on every row of the parts list, laid out round the views that
// draw them (docs/drawings/README.md#balloon-layout): each row on the view
// in `viewIds` that draws the most of it, its leader on a visible edge of
// the part, the balloons on a ring round that view clear of the rest of the
// sheet, and no two leaders crossing. The balloons already on those views
// are replaced, so it can be run again after the model moves. One undo
// step. False with `error` when the views are not of one assembly or a
// view has no room.
struct BalloonLayoutReport {
  struct Placed {
    uint64_t note = 0, view = 0, occurrence = 0;
    int item = 0;
    std::string part;
    glm::dvec2 position{0.0};
  };
  std::vector<Placed> placed;
  std::vector<std::string> unplaced; // rows nothing on these views draws
  int crossings = 0;
};
//
// `update` brings the views up to date first, waiting (the RPC); without it
// (the GUI) a view still projecting refuses the layout, saying so.
bool LayoutDrawingBalloons(AppState& state,
                           const std::vector<uint64_t>& viewIds,
                           BalloonLayoutReport& report, std::string* error,
                           bool update = true);

// The next free letter for a section or detail view on the active sheet
// (A, B, ... skipping I, O and Q, and those its sections and details use).
std::string NextViewLetter(AppState& state);

// The next datum letter the active sheet does not use yet (A, B, ...
// skipping I, O and Q), or "" when all 23 are taken.
std::string NextDatumLetter(AppState& state);

// Why the active sheet would not take `note`, in the words add_drawing_note
// refuses it with -- a datum letter that is not one or is already used, a
// frame ISO 1101 cannot write or naming a datum the sheet lacks, a leader
// that resolves to nothing -- or "" when it would. The RPC and the GUI's
// annotation sessions both ask this, so a click is refused exactly what a
// script is (docs/drawings/guide.md#in-the-gui).
std::string WhyDrawingNoteRefused(AppState& state, const DrawingNote& note);

// Where a new symbol (a datum, a frame) stands by default: near the point its
// leader names, outward from its view, on the first spot whose box clears the
// views, the title block, the labels and the border.
glm::dvec2 AutoNotePosition(AppState& state, const DrawingNote& note);

// A geometric tolerance to start from, derived from the feature it is on:
// ISO 2768-2 class K for that length (straightness/flatness for form,
// position and profile; perpendicularity for orientation; the runout and
// symmetry values for those). `featureMM` is the toleranced feature's
// length (or a circle's diameter), model mm.
double DefaultGeoTolerance(GeoSymbol symbol, double featureMM);

// Why `detail` cannot be added to the active sheet (no such parent, a detail
// of a detail, no radius, a letter that is not one), as add_drawing_view
// refuses it; "" when it can.
std::string WhyDetailRefused(AppState& state, const DetailSpec& detail);

// The world point (the source's frame, mm) a sheet point on `view` shows,
// on the plane through the origin square to the view's direction -- which
// is all a detail's centre needs, since only its projection matters.
glm::dvec3 SheetPointToWorld(AppState& state, const DrawingView& view,
                             const glm::dvec2& sheet);

// A detail's scale to start from: a standard scale at least twice its
// parent's, as large as lets the circle stay within a third of the sheet's
// smaller side.
double DefaultDetailScale(AppState& state, const DrawingView& parent,
                          double radiusMM);

// Where a detail's centre lands by default: the free spot nearest its
// circle on the parent where the detail (circle and caption) clears the
// views, the title block, the labels and the border.
glm::dvec2 AutoDetailPosition(AppState& state, const DrawingView& detail);

} // namespace ui
