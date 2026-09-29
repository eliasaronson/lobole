#pragma once

// The router's interface to the sketch tool blocks.
//
// SketchInteraction.cpp was 4,278 lines, and its core is HandleSketchViewport:
// a 843-line pipeline of a shared preamble (plane picking, the Escape ladder,
// snap inference, the dimension-pill gate) followed by per-tool work. Stage 7a
// moved the per-tool work into six files and left the router where it was,
// which is why these twenty helpers stopped being file-static: thirteen are
// the router calling into a tool block, and that IS the interface -- it was
// simply never written down while everything shared one translation unit.
//
// NOT public: only the Sketch* translation units include it.

#include "render/MeshRenderer.h" // LineVertex3D
#include "ui/AppState.h"
#include "ui/RayMath.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace ui {

// The resolved commit for a placed point-PAIR dimension (Phase 3.7): the
// kind switches by the cursor's zone relative to the span A→B, the way
// commercial CAD places them. Cursor offset from the midpoint within ~22.5° of
// the span's perpendicular = pulled straight off the pair → ALIGNED (Distance);
// else mostly up/down → HORIZONTAL dimension (DistanceX, its line is
// horizontal); mostly sideways → VERTICAL (DistanceY). Axis-parallel spans skip
// the choice — aligned IS the axis dimension there.
struct PairPlacement {
  sketch::ConstraintKind kind = sketch::ConstraintKind::Distance;
  sketch::PointRef a, b; // ordered so the solver's signed value is positive
  double value = 0.0;
  glm::vec2 labelOffset{0.0f};
};

bool CommitMirrorAcross(AppState& state, SketchFeature& sketch, size_t index,
                        uint32_t axisEntity);

void BeginDragSession(AppState& state, SketchFeature& sketch, size_t index,
                      const SketchPick& pick, glm::vec2 cursorUV);

std::optional<PairPlacement> ClassifyPairPlacement(const SketchFeature& sk,
                                                   sketch::PointRef A,
                                                   sketch::PointRef B,
                                                   glm::vec2 cursor);

void CommitCenterRectangle(AppState& state, SketchFeature& sketch, size_t index,
                           glm::vec2 center, glm::vec2 corner, bool infer);

// Returns the committed entity's stable id.
uint32_t CommitEntity(AppState& state, SketchFeature& sketch, size_t index,
                      const sketch::Entity& entity, bool infer);

void CommitLine(AppState& state, SketchFeature& sketch, size_t index,
                glm::vec2 a, glm::vec2 b, bool infer);

void CommitPlacedDimension(AppState& state, const SketchFeature& sketch,
                           glm::vec2 cursor);

void CommitRectangle(AppState& state, SketchFeature& sketch, size_t index,
                     glm::vec2 c1, glm::vec2 c2, bool infer);

void EndDragSession(AppState& state, SketchFeature& sketch);

std::vector<std::pair<uint8_t, glm::vec2>>
EntityPoints(const sketch::Entity& e);

glm::vec2 FixedPlacementLabelOffset(const SketchFeature& sk,
                                    sketch::ConstraintKind kind,
                                    const sketch::PointRef& a,
                                    const sketch::PointRef& b,
                                    glm::vec2 cursor);

void HandleDimensionClick(AppState& state, const SketchFeature& sketch,
                          const SketchPick& pick);

bool HandleProjectTool(AppState& state, SketchFeature& sketch, size_t index,
                       const Ray& ray);

LineVertex3D LineVertex(const glm::vec3& p);

float PixelsPerUV(AppState& state, const SketchPlane& plane, glm::vec2 uv);

std::optional<glm::vec2> RayToPlaneUV(const Ray& ray, const SketchPlane& plane);

std::optional<glm::vec2> RefUV(const SketchFeature& sk,
                               const sketch::PointRef& r);

bool RefineLinePlacement(AppState& state, const SketchFeature& sketch,
                         const SketchPick& pick);

Ray ScreenRay(AppState& state, const glm::vec2& mousePos);

glm::vec2 SeedLabelOffset(const SketchFeature& sk, const sketch::Constraint& c);

void UpdateDragSession(AppState& state, SketchFeature& sketch,
                       glm::vec2 cursorUV);

// Body hit tolerance for the interactive sketch tools (endpoints get +4 px on
// top — see PickSketchCandidates). Was 12; lines were hard to hit.
inline constexpr float kSketchBodyTolPx = 14.0f;

} // namespace ui
