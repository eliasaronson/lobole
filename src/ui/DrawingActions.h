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

// Drawing views: projecting, placing, dimensioning and exporting them.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture-review-2026-09-06.md, stage 4.

namespace ui {

drawing::ResolvedNote ResolveDrawingNote(AppState& state,
                                         const DrawingNote& note);

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

// The units the active sheet is written in: its first view's SOURCE document,
// not the reader's preference — a drawing of an inch part is an inch drawing
// wherever it is opened. One accessor, because the title block on screen, the
// title block in the exported file and the dimension text itself all have to
// agree about it; they did not, and the numbers were mm regardless.
units::System ActiveDrawingUnits(AppState& state);

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

} // namespace ui
