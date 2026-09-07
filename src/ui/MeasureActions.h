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

// What a measurement means: resolving a pick, computing it, pinning it.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture-review-2026-09-06.md, stage 4.

namespace ui {

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

} // namespace ui
