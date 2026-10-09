// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/fem/FemStudy.h"
#include "render/SketchRenderer.h"
#include "ui/AppState.h"

#include <string>
#include <vector>

// The simulation panel, its result field legend, and the
// viewport interaction for placing and dragging loads.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture.md#the-ui-actions-layer.
namespace ui {

// The Studies panel drawn in the Model dock while in simulation mode on a
// PART tab. Defined in StudyPanel.cpp.
void DrawStudyPanel(AppState& state);

// The same studies on an ASSEMBLY tab (S1), as the floating tool window
// beside explode and motion — the Model dock there is the occurrence panel.
// Adds the participants table (each occurrence's role) and a clamp at each
// rigid support's contact.
void DrawSimulationToolWindow(AppState& state);

// The result-field colorbar legend overlaid on the viewport while a field is
// displayed. Defined in StudyPanel.cpp.
void DrawFieldLegend(AppState& state);

// Assigns the armed boundary condition (state.simulation.tool) to the face
// under `ray`, as an undoable study edit. No-op if no tool is armed or the
// pick misses a named face. Defined in StudyPanel.cpp.
// Applies the armed BC/keep tool to a picked face (U1c: the selection
// pipeline resolves the pick; this consumes it).
void SimulationApplyToFace(AppState& state, const ElementRef& face);
// The same with the occurrence the face was picked on (an assembly tab):
// only the MESHED occurrence takes a load, and a click elsewhere is refused
// in the panel rather than silently dropped.
void SimulationApplyToFace(AppState& state, uint64_t occurrenceId,
                           const ElementRef& face);

// Boundary-condition glyphs while simulation mode is active. A pressure is
// a FIELD of arrows over its face (one per grid cell, into the local
// surface), a force one resultant arrow at the face centre, a fixed support
// an anchor hatch repeated over the face, a keep face a diamond, gravity one
// arrow at the centre of mass. The grid spacing derives from the view-scaled
// glyph length, so a face gets more arrows as you zoom in and never fewer
// than one.
void AppendSimulationBCGlyphs(AppState& state, std::vector<PrimVertex>& out);

// The glyphs that must be seen THROUGH the part, for the on-top channel the
// measurements and handles draw in: gravity's arrow leaves the centre of
// mass, which is inside the solid, where the depth-tested channel hides it.
void AppendSimulationOverlayGlyphs(AppState& state,
                                   std::vector<PrimVertex>& out);

// Tints every boundary-condition face of the active study through the mesh
// renderer's highlight channel (fixtures, loads and keep faces each in their
// own colour). Part of the per-frame highlight projection: called by
// SyncSelectionHighlights AFTER the selection, since that clears first.
void AppendSimulationFaceTints(AppState& state);

// Value-and-unit labels beside the glyphs, in the viewport draw list. Gated
// by SimulationState::showLabels.
void DrawSimulationLabels(AppState& state);

// The label text for one boundary condition: "1.2 kN", "0.5 MPa", "fixed",
// "keep". Pure, so a test can pin the formatting.
std::string SimulationLoadLabel(const fem::Load& load);
// "g 9.81 m/s^2" for a study's gravity vector.
std::string SimulationGravityLabel(const glm::dvec3& gravity);

// The world-space sample points and local normals a face's distributed
// glyphs stand on: at most `maxCount`, roughly one per `spacing` x `spacing`
// of area, and always at least the face centre. Deterministic (a Halton
// sequence over the triangles' area), so the picture is stable frame to
// frame. Exposed for the GUI tests.
struct FaceSample {
  glm::vec3 point;
  glm::vec3 normal;
};
std::vector<FaceSample> SampleFace(AppState& state, uint64_t occurrenceId,
                                   const ElementRef& face, float spacing,
                                   int maxCount);

// Dragging a force arrow to change its magnitude. Its own grab payload
// (SimulationState::loadDrag), like the gizmo's and the section arrow's —
// the shared layer answers WHICH handle, not what happens to it
// (docs/ui/viewport-handles.md). Returns true when it consumed the event.
bool HandleLoadDragInput(AppState& state, const Ray& ray, bool clicked,
                         bool down, bool released);

} // namespace ui
