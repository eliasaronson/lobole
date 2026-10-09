// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/Overlays.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"

#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchInference.h"
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSelection.h"
#include "core/sketch/SketchSolver.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h" // LineVertex3D
#include "ui/DocumentSlots.h"
#include "ui/ExprComplete.h" // variable completion in the dim editors
#include "ui/SceneOverlays.h"
#include "ui/SketchOverlay.h" // SelectFeature
#include "ui/SketchToolbar.h" // BeginSketchExtrude/Revolve (Shift+E/R)

#include "imgui.h"
#include "imgui_internal.h" // GetInputTextState (re-read after a completion)

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

#include "ui/SketchInteractionInternal.h"

// Placing a dimension: which pair of things a click meant, where the label
// goes, and committing it as a driving constraint. Also the two tools that
// collect a selection before they act -- project and mirror.
// Split out of SketchInteraction.cpp
// (docs/architecture.md#the-ui-actions-layer). The 843-line viewport router
// stays there; these are the blocks it calls into, and
// SketchInteractionInternal.h is that interface written down.

namespace ui {

namespace {

// UV of a point ref against the live sketch (datum origin/axes included).
std::optional<glm::dvec2> TargetUV(const SketchFeature& sk,
                                   const sketch::PointRef& r) {
  if (sketch::IsDatum(r.entity))
    return sketch::DatumPointUV(r.entity, r.which);
  const size_t idx = sk.IndexOfEntity(r.entity);
  if (idx == SIZE_MAX)
    return std::nullopt;
  return sketch::EntityPointUV(sk.entities[idx], r.which);
}

// Direction of a line entity or datum axis (unnormalized), nullopt otherwise.
std::optional<glm::dvec2> LineDirOf(const SketchFeature& sk, uint32_t id) {
  if (sketch::IsDatum(id)) {
    const auto ep = sketch::DatumAxisEndpoints(id);
    return ep.second - ep.first;
  }
  const size_t idx = sk.IndexOfEntity(id);
  if (idx == SIZE_MAX)
    return std::nullopt;
  if (const auto* l = std::get_if<sketch::Line>(&sk.entities[idx]))
    return l->b - l->a;
  return std::nullopt;
}

// Perpendicular distance from p to the INFINITE line through entity `lineId`.
double PointToLineDistance(const SketchFeature& sk, glm::dvec2 p,
                           uint32_t lineId) {
  const auto a = TargetUV(sk, {lineId, 0});
  const auto dir = LineDirOf(sk, lineId);
  if (!a || !dir || glm::length(*dir) < 1e-9)
    return 0.0;
  const glm::dvec2 d = glm::normalize(*dir);
  const glm::dvec2 r = p - *a;
  return std::abs(d.x * r.y - d.y * r.x);
}

// Creates the inferred dimension from the ordered targets, then opens the
// inline value editor on it so the user can type the value immediately (the
// Dimension tool's commit tail). Clears the accumulated targets either way.
void CommitInferredDimension(AppState& state, sketch::ConstraintKind kind,
                             double value, std::vector<sketch::PointRef> refs,
                             const glm::dvec2* labelOffset = nullptr) {
  SketchEditState& edit = state.sketchEdit;
  edit.selected = std::move(refs);
  const bool ok = ApplySketchConstraint(state, kind, value, {}, labelOffset);
  edit.selected.clear(); // ApplySketchConstraint clears on success; ensure
  edit.dimPickBody.clear();
  if (!ok)
    return;
  edit.actionMsg.clear();
  if (SketchFeature* sk = ResolveActiveSketch(state))
    edit.editingDim = static_cast<int>(sk->constraints.size()) - 1;
}
// Arms the placement stage: the ghost in DrawDimensions takes over until a
// click commits (or Esc drops back to target accumulation).
void StartDimensionPlacement(AppState& state, bool pair,
                             sketch::ConstraintKind kind, sketch::PointRef a,
                             sketch::PointRef b, double value) {
  SketchEditState& edit = state.sketchEdit;
  edit.dimPlacing = true;
  edit.dimPlacePair = pair;
  edit.dimPlaceKind = kind;
  edit.dimPlaceA = a;
  edit.dimPlaceB = b;
  edit.dimPlaceValue = value;
  edit.dimPlaceFromLine = 0; // the single-line entry points set it after
  // An open inline editor would commit with the ghost transiently in the
  // constraint list (see DrawDimensions) — close it first.
  edit.editingDim = -1;
}
// Resolves the two accumulated targets into either a placement stage or an
// immediate dimension: two parallel lines → perpendicular distance (P2L from
// an endpoint of the first — no parallel constraint implied), fixed-kind
// placement; two non-parallel lines → angle (immediate — its placement is
// deferred); point + line → perpendicular distance, fixed-kind placement;
// two points → the kind-switching pair placement.
void ResolveAccumulatedTargets(AppState& state, const SketchFeature& sketch) {
  SketchEditState& edit = state.sketchEdit;
  using K = sketch::ConstraintKind;
  const sketch::PointRef A = edit.selected[0], B = edit.selected[1];
  const bool aLine = edit.dimPickBody[0], bLine = edit.dimPickBody[1];

  if (aLine && bLine) {
    const auto da = LineDirOf(sketch, A.entity);
    const auto db = LineDirOf(sketch, B.entity);
    if (!da || !db) {
      edit.selected.clear();
      edit.dimPickBody.clear();
      return;
    }
    const glm::dvec2 u = glm::normalize(*da), v = glm::normalize(*db);
    const double cross = u.x * v.y - u.y * v.x;
    if (std::abs(cross) < 0.01) {
      // Parallel (within tolerance): the perpendicular distance, dimensioned
      // from an endpoint of the first line to the second (the standard
      // parallel-distance recipe — no parallel constraint is implied).
      const auto p = TargetUV(sketch, {A.entity, 0});
      const double dist = p ? PointToLineDistance(sketch, *p, B.entity) : 0.0;
      StartDimensionPlacement(state, /*pair=*/false, K::DistancePL,
                              {A.entity, 0}, {B.entity, 0}, dist);
    } else {
      // Angle. GCS's line-line angle is signed (l1 → l2): order the pair so
      // the current wedge is positive and pin exactly it.
      const double ang = std::atan2(std::abs(cross), glm::dot(u, v));
      sketch::PointRef refA = A, refB = B;
      if (cross < 0.0)
        std::swap(refA, refB);
      CommitInferredDimension(state, K::Angle, ang, {refA, refB});
    }
    return;
  }
  if (aLine != bLine) {
    const sketch::PointRef pt = aLine ? B : A;
    const sketch::PointRef ln = aLine ? A : B;
    if (pt.entity == ln.entity) {
      edit.actionMsg = "Dimension: pick a point off its own line.";
      edit.selected.pop_back();
      edit.dimPickBody.pop_back();
      return;
    }
    const auto p = TargetUV(sketch, pt);
    const double dist = p ? PointToLineDistance(sketch, *p, ln.entity) : 0.0;
    StartDimensionPlacement(state, /*pair=*/false, K::DistancePL, pt,
                            {ln.entity, 0}, dist);
    return;
  }
  // Two points: the kind switches by where the dimension is placed.
  StartDimensionPlacement(state, /*pair=*/true, K::Distance, A, B, 0.0);
}
} // namespace

// The placement click: derive kind/order/value/label from the cursor and
// commit through the shared tail (which opens the inline editor).
void CommitPlacedDimension(AppState& state, const SketchFeature& sketch,
                           glm::dvec2 cursor) {
  SketchEditState& edit = state.sketchEdit;
  edit.dimPlacing = false;
  if (edit.dimPlacePair) {
    const auto p =
        ClassifyPairPlacement(sketch, edit.dimPlaceA, edit.dimPlaceB, cursor);
    if (!p) {
      edit.selected.clear();
      edit.dimPickBody.clear();
      return;
    }
    CommitInferredDimension(state, p->kind, p->value, {p->a, p->b},
                            &p->labelOffset);
    return;
  }
  const glm::dvec2 off = FixedPlacementLabelOffset(
      sketch, edit.dimPlaceKind, edit.dimPlaceA, edit.dimPlaceB, cursor);
  std::vector<sketch::PointRef> refs = {edit.dimPlaceA};
  if (edit.dimPlaceKind == sketch::ConstraintKind::DistancePL)
    refs.push_back(edit.dimPlaceB);
  CommitInferredDimension(state, edit.dimPlaceKind, edit.dimPlaceValue, refs,
                          &off);
}

// One Dimension-tool click (target-accumulation stage): accumulate the pick
// and, as soon as the set matches a rule, enter the placement stage (or, for
// angles, create the dimension — Decision 1A inference):
//   circle/arc → radius;  line + empty click (or the same line again) →
//   length;  see ResolveAccumulatedTargets for the two-target rules.
// May REPLACE the sketch feature through the command system; callers must
// drop their SketchFeature pointer afterwards.
void HandleDimensionClick(AppState& state, const SketchFeature& sketch,
                          const SketchPick& pick) {
  SketchEditState& edit = state.sketchEdit;
  using K = sketch::ConstraintKind;
  if (edit.dimPickBody.size() != edit.selected.size()) {
    // Selection changed under the tool (panel click etc.) — start over.
    edit.selected.clear();
    edit.dimPickBody.clear();
  }
  // A full pair kept by an Esc'd placement: any click re-enters placement.
  if (edit.selected.size() >= 2) {
    ResolveAccumulatedTargets(state, sketch);
    return;
  }
  const auto entityOf = [&](uint32_t id) -> const sketch::Entity* {
    const size_t idx = sketch.IndexOfEntity(id);
    return idx == SIZE_MAX ? nullptr : &sketch.entities[idx];
  };
  const auto startLengthPlacement = [&](uint32_t id) {
    // A line's length is an endpoint pair — full kind-switching placement
    // (a sloped line can be dimensioned by its horizontal/vertical extent).
    // Marked as single-line so a placement click on another target REFINES
    // the kind instead of placing (B).
    StartDimensionPlacement(state, /*pair=*/true, K::Distance, {id, 0}, {id, 1},
                            0.0);
    edit.dimPlaceFromLine = id;
  };

  if (!pick.hit) {
    // Empty space sends a single picked line to LENGTH placement.
    if (edit.selected.size() == 1 && edit.dimPickBody[0] &&
        !sketch::IsDatum(edit.selected[0].entity))
      startLengthPlacement(edit.selected[0].entity);
    return;
  }

  const sketch::Entity* e = entityOf(pick.ref.entity);
  const bool isAxis = sketch::IsDatum(pick.ref.entity);

  // A circle/arc body goes straight to radius (label-only) placement.
  if (pick.body && e != nullptr &&
      (std::holds_alternative<sketch::Circle>(*e) ||
       std::holds_alternative<sketch::Arc>(*e))) {
    const double r = std::holds_alternative<sketch::Circle>(*e)
                         ? std::get<sketch::Circle>(*e).radius
                         : std::get<sketch::Arc>(*e).radius;
    edit.selected.clear();
    edit.dimPickBody.clear();
    StartDimensionPlacement(state, /*pair=*/false, K::Radius,
                            {pick.ref.entity, 0}, {}, r);
    return;
  }
  // An ellipse body: its MAJOR radius, as the distance from the centre to
  // the major-axis end (an ellipse has two radii; the minor one is the
  // centre and a minor-axis end, both pickable dots).
  if (pick.body && e != nullptr &&
      (std::holds_alternative<sketch::Ellipse>(*e) ||
       std::holds_alternative<sketch::EllipseArc>(*e))) {
    edit.selected.clear();
    edit.dimPickBody.clear();
    StartDimensionPlacement(state, /*pair=*/true, K::Distance,
                            {pick.ref.entity, 0},
                            {pick.ref.entity, sketch::kEllipseMajorPos}, 0.0);
    return;
  }

  const bool lineTarget =
      pick.body &&
      (isAxis || (e != nullptr && std::holds_alternative<sketch::Line>(*e)));

  // Clicking the picked line a second time = its length (the "double-click
  // the line" rule).
  if (lineTarget && edit.selected.size() == 1 && edit.dimPickBody[0] &&
      edit.selected[0].entity == pick.ref.entity) {
    if (!isAxis)
      startLengthPlacement(pick.ref.entity);
    return;
  }
  // Re-clicking an accumulated point is a no-op.
  if (!lineTarget && edit.selected.size() == 1 && !edit.dimPickBody[0] &&
      edit.selected[0].entity == pick.ref.entity &&
      edit.selected[0].which == pick.ref.which)
    return;

  edit.selected.push_back(
      lineTarget ? sketch::PointRef{pick.ref.entity, sketch::kWholeEntity}
                 : pick.ref);
  edit.dimPickBody.push_back(lineTarget);
  if (edit.selected.size() < 2) {
    // A single LINE goes straight to length placement (B): the ghost is up
    // immediately; a click on another line/point refines instead of placing.
    // (Axes have no length — they wait for a second target.)
    if (lineTarget && !isAxis)
      startLengthPlacement(pick.ref.entity);
    return;
  }
  ResolveAccumulatedTargets(state, sketch);
}

// Label offset that puts a fixed-kind placement's label at (or nearest to)
// the cursor, per that kind's rendering geometry.
glm::dvec2 FixedPlacementLabelOffset(const SketchFeature& sk,
                                     sketch::ConstraintKind kind,
                                     const sketch::PointRef& a,
                                     const sketch::PointRef& b,
                                     glm::dvec2 cursor) {
  using K = sketch::ConstraintKind;
  if (kind == K::Radius || kind == K::Diameter) {
    const size_t ci = sk.IndexOfEntity(a.entity);
    if (ci == SIZE_MAX)
      return {0.7, 0.7};
    glm::dvec2 center{0.0};
    double radius = 0.0;
    glm::dvec2 dir(0.7071, 0.7071); // the renderer's circle leader anchor
    if (const auto* c = std::get_if<sketch::Circle>(&sk.entities[ci])) {
      center = c->center;
      radius = c->radius;
    } else if (const auto* arc = std::get_if<sketch::Arc>(&sk.entities[ci])) {
      center = arc->center;
      radius = arc->radius;
      const double midA = 0.5 * (arc->startAngle + arc->endAngle);
      dir = {(std::cos(midA)), (std::sin(midA))};
    }
    return cursor - center - dir * radius; // label lands exactly at cursor
  }
  if (kind == K::DistancePL) {
    // The label slides along the line's direction (the item-5 axis).
    const auto p = TargetUV(sk, a);
    const auto la = TargetUV(sk, {b.entity, 0});
    const auto dirO = LineDirOf(sk, b.entity);
    if (!p || !la || !dirO || glm::length(*dirO) < 1e-6)
      return {0.0, 0.8};
    const glm::dvec2 d = glm::normalize(*dirO);
    const glm::dvec2 foot = *la + d * glm::dot(*p - *la, d);
    glm::dvec2 off = d * glm::dot(cursor - 0.5 * (*p + foot), d);
    if (glm::length(off) < 1e-3)
      off = d * 0.05;
    return off;
  }
  return {0.7, 0.7};
}

// Project tool: click a body's edge or face to add it as LIVE external
// (projected) construction geometry — it tracks the source when it moves.
// Prefers an edge under the cursor, else the face. Returns true (the viewport
// click is consumed).
bool HandleProjectTool(AppState& state, SketchFeature& sketch, size_t index,
                       const Ray& ray) {
  SketchEditState& edit = state.sketchEdit;
  const EdgeHit edge = state.faceSelector->PickEdge(ray);
  RayHit face = state.faceSelector->PickFace(ray);
  if (face.cap)
    face = RayHit{}; // a section cap has no element to project
  // Hover feedback: what a click would project.
  edit.actionMsg = edge.hit   ? "Project: click to project this edge"
                   : face.hit ? "Project: click to project this face"
                              : "Project: hover a body edge or face";
  if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    return true;

  std::optional<ElementRef> ref;
  sketch::ExternalKind kind = sketch::ExternalKind::Edge;
  if (edge.hit)
    ref = EdgeElementRef(state, edge.meshId, edge.edgeIndex);
  if (!ref && face.hit) {
    ref = FaceElementRef(state, face.meshId, face.faceIndex);
    kind = sketch::ExternalKind::Face;
  }
  if (!ref) {
    edit.actionMsg = "Project: that element has no stable name — try its face";
    return true;
  }
  for (const auto& e : sketch.external) // skip an already-projected ref
    if (e.kind == kind && e.ref.featureId == ref->featureId &&
        e.ref.name == ref->name) {
      edit.actionMsg = "already projected";
      return true;
    }

  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch.AddExternalGeometry(*ref, kind);
  state.document->Regenerate();
  if (!sketch.GetError().empty()) { // shouldn't happen for a live pick
    sketch.RemoveExternalGeometry(sketch.external.size() - 1);
    state.document->Regenerate();
    edit.actionMsg = "Project: could not project that element";
    return true;
  }
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  edit.actionMsg = kind == sketch::ExternalKind::Edge ? "projected an edge"
                                                      : "projected a face";
  return true;
}

// A placement click that lands ON another target while placing a single
// line's LENGTH refines the dimension instead of placing it (B): a parallel
// line or a point → DistancePL placement; a non-parallel line → angle
// (immediate, as ever). Returns false when the click should place instead
// (the measured line itself; circle/arc combos are out of scope).
bool RefineLinePlacement(AppState& state, const SketchFeature& sketch,
                         const SketchPick& pick) {
  SketchEditState& edit = state.sketchEdit;
  using K = sketch::ConstraintKind;
  const uint32_t lineId = edit.dimPlaceFromLine;
  if (pick.ref.entity == lineId)
    return false; // clicking the measured line again just places
  const size_t idx = sketch.IndexOfEntity(pick.ref.entity);
  const sketch::Entity* e = idx == SIZE_MAX ? nullptr : &sketch.entities[idx];
  const bool isAxis = sketch::IsDatum(pick.ref.entity);
  const bool otherLine =
      pick.body &&
      (isAxis || (e != nullptr && std::holds_alternative<sketch::Line>(*e)));

  if (otherLine) {
    const auto da = LineDirOf(sketch, lineId);
    const auto db = LineDirOf(sketch, pick.ref.entity);
    if (!da || !db)
      return false;
    const glm::dvec2 u = glm::normalize(*da), v = glm::normalize(*db);
    const double cross = u.x * v.y - u.y * v.x;
    // Keep the accumulation in sync so an Esc drops back to the two targets.
    edit.selected = {{lineId, sketch::kWholeEntity},
                     {pick.ref.entity, sketch::kWholeEntity}};
    edit.dimPickBody = {true, true};
    if (std::abs(cross) < 0.01) {
      const auto p = TargetUV(sketch, {lineId, 0});
      const double dist =
          p ? PointToLineDistance(sketch, *p, pick.ref.entity) : 0.0;
      StartDimensionPlacement(state, /*pair=*/false, K::DistancePL, {lineId, 0},
                              {pick.ref.entity, 0}, dist);
    } else {
      edit.dimPlacing = false; // angle commits immediately (as ever)
      const double ang = std::atan2(std::abs(cross), glm::dot(u, v));
      sketch::PointRef refA{lineId, 0}, refB{pick.ref.entity, 0};
      if (cross < 0.0)
        std::swap(refA, refB);
      CommitInferredDimension(state, K::Angle, ang, {refA, refB});
    }
    return true;
  }
  if (!pick.body) { // a point target → perpendicular point-line distance
    const auto p = TargetUV(sketch, pick.ref);
    const double dist = p ? PointToLineDistance(sketch, *p, lineId) : 0.0;
    edit.selected = {pick.ref, {lineId, sketch::kWholeEntity}};
    edit.dimPickBody = {false, true};
    StartDimensionPlacement(state, /*pair=*/false, K::DistancePL, pick.ref,
                            {lineId, 0}, dist);
    return true;
  }
  return false; // circle/arc body: combos out of scope (deferred)
}

// The resolved commit for a placed point-PAIR dimension (Phase 3.7): the
// kind switches by the cursor's zone relative to the span A→B, the way
// commercial CAD places them. Cursor offset from the midpoint within ~22.5° of
// the span's perpendicular = pulled straight off the pair → ALIGNED (Distance);
// else mostly up/down → HORIZONTAL dimension (DistanceX, its line is
// horizontal); mostly sideways → VERTICAL (DistanceY). Axis-parallel spans skip
// the choice — aligned IS the axis dimension there.
std::optional<PairPlacement> ClassifyPairPlacement(const SketchFeature& sk,
                                                   sketch::PointRef A,
                                                   sketch::PointRef B,
                                                   glm::dvec2 cursor) {
  const auto paOpt = TargetUV(sk, A), pbOpt = TargetUV(sk, B);
  if (!paOpt || !pbOpt)
    return std::nullopt;
  glm::dvec2 pa = *paOpt, pb = *pbOpt;
  const glm::dvec2 span = pb - pa;
  const double len = glm::length(span);
  if (len < 1e-6)
    return std::nullopt;
  const glm::dvec2 d = span / len;
  const glm::dvec2 n(-d.y, d.x); // the aligned dimension's offset direction
  const glm::dvec2 mid = 0.5 * (pa + pb);
  const glm::dvec2 off = cursor - mid;
  const glm::dvec2 u = glm::length(off) > 1e-6 ? glm::normalize(off) : n;

  PairPlacement out;
  out.a = A;
  out.b = B;
  const bool axisParallel = std::abs(d.x) < 0.01 || std::abs(d.y) < 0.01;
  if (axisParallel || std::abs(glm::dot(u, n)) > 0.924) {
    out.kind = sketch::ConstraintKind::Distance;
    out.value = len;
    out.labelOffset = n * glm::dot(off, n);
    if (glm::length(out.labelOffset) < 1e-3)
      out.labelOffset = n * 0.05; // a zero offset would re-seed on render
  } else if (std::abs(u.y) > std::abs(u.x)) {
    out.kind = sketch::ConstraintKind::DistanceX;
    if (pb.x < pa.x) { // the constraint is signed b.x - a.x: keep it positive
      std::swap(out.a, out.b);
      std::swap(pa, pb);
    }
    out.value = std::abs(span.x);
    // x stays 0 — the item-5 drag axis for DistanceX is vertical-only.
    out.labelOffset = {0.0, cursor.y - std::min(pa.y, pb.y)};
  } else {
    out.kind = sketch::ConstraintKind::DistanceY;
    if (pb.y < pa.y) {
      std::swap(out.a, out.b);
      std::swap(pa, pb);
    }
    out.value = std::abs(span.y);
    out.labelOffset = {cursor.x - std::min(pa.x, pb.x), 0.0};
  }
  return out;
}

// Commits a mirror of the selected entities across `axisEntity` (a line or
// datum axis id): solve, regenerate, one undo step, back to Select. Shared
// by the axis-phase click and mirror-on-activate seeding. Returns false
// (with an actionMsg) when the selection holds nothing besides the axis.
bool CommitMirrorAcross(AppState& state, SketchFeature& sketch, size_t index,
                        uint32_t axisEntity) {
  SketchEditState& edit = state.sketchEdit;
  std::vector<uint32_t> sources; // unique selected ids, minus the axis
  for (const auto& r : edit.selected) {
    if (r.entity == axisEntity)
      continue;
    bool seen = false;
    for (uint32_t s : sources)
      seen |= (s == r.entity);
    if (!seen)
      sources.push_back(r.entity);
  }
  if (sources.empty()) {
    edit.actionMsg = "Mirror: pick an axis other than the selected geometry.";
    return false;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch.MirrorEntities(sources, axisEntity, edit.linkGenerative);
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch.entities);
  sketch.RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  edit.selected.clear();
  edit.mirrorAxisPhase = false;
  edit.actionMsg.clear();
  edit.tool = SketchTool::Select;
  return true;
}

void MirrorToolActivated(AppState& state) {
  // Select-first shortcut (U8 polish): a pre-selection that contains exactly
  // one construction line among other entities names its own axis — mirror
  // immediately, one gesture, no phases.
  SketchEditState& edit = state.sketchEdit;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (sketch == nullptr || edit.selected.empty())
    return;
  uint32_t axis = 0;
  int constructionLines = 0;
  int others = 0;
  std::vector<uint32_t> uniq;
  for (const auto& r : edit.selected) {
    bool seen = false;
    for (uint32_t u : uniq)
      seen |= (u == r.entity);
    if (seen)
      continue;
    uniq.push_back(r.entity);
    const size_t ei = sketch->IndexOfEntity(r.entity);
    const auto* line = ei != SIZE_MAX
                           ? std::get_if<sketch::Line>(&sketch->entities[ei])
                           : nullptr;
    if (line != nullptr && line->construction) {
      axis = r.entity;
      constructionLines++;
    } else {
      others++;
    }
  }
  if (constructionLines == 1 && others > 0)
    CommitMirrorAcross(state, *sketch, index, axis);
}

} // namespace ui
