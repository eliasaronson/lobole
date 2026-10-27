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

// What the camera looks at, and what is cut away.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// Frames the camera on the document's combined bounding box.
// The world-space box around everything the active tab shows: a part's
// bodies, or an assembly's visible (unsuppressed) occurrences in place.
// False when there is nothing to bound. Camera fit and the section plane
// both need it — a view feature positioned in absolute coordinates is wrong
// for any model that is not sitting on the origin. `tight` asks for the
// shapes' exact boxes, which the camera fit takes: the conservative one
// bounds a B-spline face by its control points, and a downloaded STEP's
// came out five times its size, the part a speck in the framed view.
bool VisibleSceneBounds(AppState& state, glm::vec3& min, glm::vec3& max,
                        bool tight = false);

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
// `occurrenceId` is 0 on a part tab; `caps` is one per body, in body order,
// so its index is the body a cap belongs to — both recorded with the cap so
// selecting or hovering that body can light it.
void AppendCapMeshes(AppState& state, const std::vector<CADShape>& caps,
                     const std::vector<glm::vec3>& colors, const glm::mat4& xf,
                     const glm::vec3& worldNormal,
                     const std::vector<float>& alphas = {},
                     uint64_t occurrenceId = 0);

// The offset that puts the cut through the MIDDLE of what the tab is showing,
// along `basePlane`'s normal. A section defaults to the world origin
// otherwise, which for a model built anywhere else is either a cut that
// removes everything or one that does nothing — both read as "section view is
// broken". Returns 0 when the scene is empty.
float CenteredSectionOffset(AppState& state, int basePlane);

// Frames the camera on everything visible. `forAspect` is the aspect ratio
// of the PICTURE being framed — pass it when that is not the viewport (a
// `render_still` at its own size projects with width/height, so a fit solved
// for the viewport crops the still); 0 means the viewport's own.
void FitCameraToDocument(AppState& state, float forAspect = 0.0f);

// The largest rectangle inside `view` that overlaps none of `obstacles`
// (all min.x, min.y, max.x, max.y, in one screen space). Obstacles are
// clipped to the view first and empty ones dropped, so one hanging off the
// edge costs only the part of it that is inside. The candidates are every
// pair of x edges and every pair of y edges taken from the view and the
// obstacles; the free one with the largest area wins. With no obstacle it is
// the view itself. What Fit frames the model into
// (docs/ui/README.md#viewport).
glm::vec4 LargestFreeRect(const glm::vec4& view,
                          const std::vector<glm::vec4>& obstacles);

// Frames the camera on an explicit world-space box (render_sheet focus).
void FitCameraToWorldBox(AppState& state, const glm::vec3& min,
                         const glm::vec3& max);

// Frames the camera on the selection's combined bounding box (faces/edges/
// bodies/features by their geometry; occurrences by their placed part).
// Falls back to the whole document when nothing is selected/resolvable.
void FitCameraToSelection(AppState& state);

} // namespace ui
