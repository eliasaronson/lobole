// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The Section tool's "To sketch"
// (docs/sketching/README.md#a-section-into-a-sketch): a new sketch on the
// active cut's plane with the cut drawn in it, the same section project_section
// adds -- locked construction geometry refitted from the bodies on every
// regeneration -- and the sketch opened for editing, so the outline can be
// offset into a profile.

#include "ui/AppState.h"

#include <cstdint>

namespace ui {

// The sketch plane a cut lies in: its normal and distance as the section
// draws them, its origin the visible scene's centre put onto it, its U axis
// the base plane's own (projected) so an untilted cut on a base plane draws
// as a sketch on that plane does.
SketchPlane SectionSketchPlane(AppState& state,
                               const AppState::SectionView& section);

// Starts that sketch for `sectionId` with `state.sectionSketchFit`. Returns
// its id; 0 when refused (no part tab, a cut whose source is gone, a cut
// that crosses no body), the reason in `state.sectionSketchMsg`.
uint64_t SectionToSketch(AppState& state, uint64_t sectionId);

// The tool window's row: the fit and the button, for the active cut.
void DrawSectionToSketchRow(AppState& state);

} // namespace ui
