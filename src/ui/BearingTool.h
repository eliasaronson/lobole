// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

// The bearing session: put a standard bearing on a seat by clicking the seat
// (docs/parts/README.md#bearings).
//
// Like the fastener session it is not a form. The diameter comes from the
// seat, the position from its shoulder and the axis from the cylinder; what
// is left to choose is WHICH of the bearings that diameter takes — a Ø10
// journal takes five series — and those are offered as buttons once the
// seat is picked, so the choice is always among bearings that fit.
namespace ui {

void OpenBearingTool(AppState& state);
void CloseBearingTool(AppState& state);

// Arm the seat pick: a cylindrical face or a circular edge. The session opens
// armed and re-arms after each placement.
void ArmBearingSeat(AppState& state);

// Routed from SlotPickClicked for kBearingToolSlot.
void BearingSeatPicked(AppState& state, const SelectionItem& item);

// Re-derive what Place would do (a dry-run fit_bearing). Called when the pick
// or the designation changes, never per frame.
void RefreshBearingFit(AppState& state);

void DrawBearingToolWindow(AppState& state);

// The translucent bearing about to be placed, pushed at the renderer. Called
// every frame for the reason SyncFastenerGhost is: what it follows (a pick, a
// designation button) bumps no document version.
void SyncBearingGhost(AppState& state);

} // namespace ui
