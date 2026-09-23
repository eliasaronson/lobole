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

// The sketch as vertices, and the edge-pick highlight.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

namespace ui {

// The stable ElementRef for a picked B-Rep edge (the body's createdBy + the
// edge's MappedName); nullopt when the edge has no stable name.
std::optional<ElementRef> EdgeElementRef(AppState& state, uint32_t meshId,
                                         uint32_t edgeIndex);

// Blend edge collection (fillet/chamfer): toggles the clicked edge on `fil`,
// remapping the displayed (already blended) name onto the blend's input body
// first. meshId/edgeIndex are the displayed handles the geometric remap
// anchors on. Commits internally (preview: mutate+regen; committed: one undo
// step); sets an actionMsg for a blend-created edge instead. Seeding passes
// regenerate=false: a regen between batched toggles would blend the body
// mid-seed and strand the remaining pre-picked names (only the FIRST
// pre-selected edge survived).
void ApplyBlendEdgePick(AppState& state, FilletFeature& fil,
                        const ElementRef& displayed, uint32_t meshId,
                        uint32_t edgeIndex, bool regenerate = true);

// Appends edge highlight primitives while edge picking is active: every body
// edge dimmed, the hovered edge and the blend's picked edges emphasized.
void AppendEdgePickPrimitives(AppState& state, std::vector<PrimVertex>& out);

// A thick screen-space line segment (two triangles) for overlay primitives.
void AppendPrimLine(glm::vec3 a, glm::vec3 b, float halfPx,
                    const glm::vec4& color, std::vector<PrimVertex>& out);

// A constant-size round dot, for marking points on the model (CAM leftover).
void AppendPrimDot(glm::vec3 p, float radiusPx, const glm::vec4& color,
                   std::vector<PrimVertex>& out);

// Highlighted line geometry for the sketch currently being edited (empty if
// none). Rendered as a per-frame overlay on top of the static sketch lines.
// Holds the rubber-band preview + gizmo/grid; entity lines and vertex dots go
// through BuildActiveSketchPrimitives instead.
std::vector<LineVertex3D> BuildActiveSketchOverlay(AppState& state,
                                                   uint64_t activeSketchId);

// Thick anti-aliased entity lines + constant-size vertex dots for the active
// sketch, with hover/selection emphasis. Empty if not editing a sketch.
std::vector<PrimVertex> BuildActiveSketchPrimitives(AppState& state,
                                                    uint64_t activeSketchId);

// Base-color thin lines for every sketch except the active one (which renders
// through BuildActiveSketchPrimitives). Rebuilt per-frame.
std::vector<LineVertex3D> BuildStaticSketchLines(AppState& state,
                                                 uint64_t activeSketchId);

} // namespace ui
