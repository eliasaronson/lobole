#pragma once

#include "ui/AppState.h"

// The fastener session: place a standard part by clicking a hole's rim
// (docs/parts/gui-plan.md).
//
// The panel side of standard parts is the BOM — what ended up in the model.
// This is the way in, and it is deliberately not a form. F1 established that
// three of the four fields a fastener dialog would ask for should not be
// asked: the size comes from the bore read backwards out of the clearance
// table, the length from the stack the axis passes through, and the head and
// the recess from the standard (an ISO 4762 IS a socket head cap screw; a
// torx one is an ISO 14579, a different catalogue item). What is left to
// choose is which standard, and whether the joint has washers and a nut —
// the one thing geometry cannot imply, since plenty of joints have neither.
//
// A tool session rather than a mode, following CAM: the viewport is not taken
// over, a picked edge means that edge, and modes have a known exit-bug class
// in this codebase.
namespace ui {

void OpenFastenerTool(AppState& state);
void CloseFastenerTool(AppState& state);

// Arm the rim pick. The session opens armed, and re-arms after each
// placement, so a pattern of holes is a row of clicks.
void ArmFastenerRim(AppState& state);

// Arm the OPTIONAL second pick: where the joint ends. One pick derives; a
// second overrides the far end, for an axis that carries on past the joint
// into material the fastener was never meant to span (docs/parts/plan.md).
void ArmFastenerFarEnd(AppState& state);

// Routed from SlotPickClicked for kFastenerToolSlot — for BOTH of the tool's
// slots, which the armed slot tells apart. An assembly Edge item carries the
// occurrence in `id`, which is exactly what tells two placements of one part
// apart — the thing an ElementRef alone cannot do.
void FastenerRimPicked(AppState& state, const SelectionItem& item);

// Re-derive what Place would do. Walks the assembly along the rim's axis, so
// it is called when the pick or an option changes, never per frame.
void RefreshFastenerFit(AppState& state);

void DrawFastenerToolWindow(AppState& state);

// The translucent joint about to be placed, pushed at the renderer.
//
// **Called every frame**, and that is the point rather than an accident. The
// document and assembly resyncs are VERSION-GATED, and what this preview
// depends on — a rim picked, a checkbox toggled, a standard chosen — bumps no
// version at all. Hung inside a resync the ghost would appear only when
// something else happened to change, which reads as a flaky preview rather
// than as a misplaced call. So it follows SyncCamPlayback: cheap, idempotent,
// per frame, and it rebuilds only when the plan it is drawing changes.
void SyncFastenerGhost(AppState& state);

} // namespace ui
