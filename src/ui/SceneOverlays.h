// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

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

// The line geometry the 3D scene draws that is not a body.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Appends translucent quads for every DatumPlaneFeature in the document,
// tinted distinctly from the base planes; the hovered/selected/pending datum
// is brightened. Visible while picking a plane, or when showPlanes is on
// (datums are user reference geometry, so they don't auto-hide behind bodies).
void AppendDatumPlaneQuads(AppState& state, std::vector<LineVertex3D>& quads);

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
// default has to come from what it acts on
// (docs/ui/tool-sessions.md#defaults-and-refusals) — 8 mm planes are a speck
// against a 1 m speaker panel, which is exactly the part you most need a plane
// to sketch on. Cached against the document's version: the bbox under it is an
// OCC pass per body, and this is asked for every frame the planes are up.
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

} // namespace ui
