// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Compare to scan (docs/inspect/README.md#how-far-a-body-is-from-a-scan):
// how far a body stands from a scan, as a map on the scan and a list of the
// places past the tolerance. The tool opens on the newest scan (an Import
// read as a mesh body) and the newest other body, compares them on a
// `deviation` job with its progress and a cancel, and shows the map with
// its legend; a hotspot's row frames the camera on it. The map is the
// FieldRenderer's (render/FieldRenderer.h) on a diverging colour map
// centred on zero, and stays when the tool closes with Done, until Clear,
// set_scan_deviation {clear}, or any edit of the part. Nothing it does
// changes the document.

#include "ui/AppState.h"

#include <memory>
#include <vector>

struct FieldVertex;
struct FieldStyle;

namespace scancompare {
struct Input;
struct Outcome;
} // namespace scancompare

namespace ui {

struct ShownField;

// Whether the part has a scan (a mesh body) and another body to compare it
// with: what gates the command.
[[nodiscard]] bool HasScanToCompare(const AppState& state);
// Why not, for the disabled button's tooltip; empty when it can.
[[nodiscard]] std::string ScanCompareRefusal(const AppState& state);

// Opens on the newest scan and the newest other body (a B-Rep before a
// mesh), the scan's own noise the tolerance, and starts the comparison.
void OpenScanCompareTool(AppState& state);
// Closes, keeping the map (Done).
void CloseScanCompareTool(AppState& state);
// Closes and removes the map (Clear, Escape, a tab switch).
void CancelScanCompareTool(AppState& state);
void DrawScanCompareToolWindow(AppState& state);

// The map: shown for `input` and its outcome (replacing any), or removed.
void ShowScanDeviation(AppState& state,
                       std::shared_ptr<const scancompare::Input> input,
                       std::shared_ptr<const scancompare::Outcome> outcome);
void ClearScanDeviation(AppState& state);
// The bar's half span: the one set, else derived (twice the tolerance, or
// the 99th percentile of the distances when that is more, rounded up to a
// 1-2-5 step).
[[nodiscard]] double ScanDeviationRangeMm(const AppState& state);

// The map's overlay for the field renderer: the scan's triangles coloured
// by their vertices' distances over +-range, the bodies it stands in for
// (the scan's and the body's, by index: the two lie on each other) in
// `hide`. False (and the map dropped) when it no
// longer matches the document: another tab, or an edit since.
bool BuildScanDeviationOverlay(AppState& state,
                               std::vector<FieldVertex>& vertices,
                               std::vector<uint32_t>& indices,
                               FieldStyle& style, ShownField& shown,
                               std::vector<int>& hide);

// Frames the camera on hotspot `index` of the map, looking at it along the
// scan's normal there.
void FrameScanHotspot(AppState& state, size_t index);

// The diverging map's colour at `t` (0-1), as ImGui packs one: the legend's
// bar and the tool's swatches; field.frag's diverging() is its twin.
[[nodiscard]] unsigned int DivergingColour(float t);

// The legend's gear rows for the map (ui/StudyPanel.cpp draws the legend).
void DrawScanDeviationLegendRows(AppState& state);

} // namespace ui
