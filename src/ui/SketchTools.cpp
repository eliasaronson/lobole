// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/DerivedFeature.h"
#include "ui/DerivedActions.h"
#include "ui/Overlays.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"

#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/ReadContext.h" // CheckFeatureRoom
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

// Starting and ending a sketch, and what each drawing tool COMMITS: a line, a
// rectangle, an arc, a drag of an existing entity. Copy and paste of a
// selection is here too -- pasting is committing entities someone else drew.
// Split out of SketchInteraction.cpp
// (docs/architecture.md#the-ui-actions-layer). The 843-line viewport router
// stays there; these are the blocks it calls into, and
// SketchInteractionInternal.h is that interface written down.

namespace ui {

namespace {

constexpr glm::vec3 kPreviewColor{0.95f, 0.85f, 0.35f};
// Point-level constraints implied by a Snap for endpoint `which` of `id`.
void SnapPointConstraints(const sketch::Snap& snap, uint32_t id, uint8_t which,
                          std::vector<sketch::Constraint>& out) {
  using K = sketch::ConstraintKind;
  if (snap.coincident)
    out.push_back({K::Coincident, {id, which}, *snap.coincident, 0.0});
  else if (snap.pointOnLine)
    out.push_back({K::PointOnLine, {id, which}, {*snap.pointOnLine, 0}, 0.0});
  // Axis alignment with another vertex (two-point form): shared X = Vertical,
  // shared Y = Horizontal. Independent of coincident/point-on-line and of each
  // other; ApplyInferred keeps each only if it further constrains the sketch.
  if (snap.alignShareX)
    out.push_back({K::Vertical, {id, which}, *snap.alignShareX, 0.0});
  if (snap.alignShareY)
    out.push_back({K::Horizontal, {id, which}, *snap.alignShareY, 0.0});
}

// Segment-level constraints implied by a line's end Snap (whole line = id).
void SnapSegmentConstraints(const sketch::Snap& snap, uint32_t id,
                            std::vector<sketch::Constraint>& out) {
  using K = sketch::ConstraintKind;
  if (snap.horizontal)
    out.push_back({K::Horizontal, {id, 0}, {0, 0}, 0.0});
  else if (snap.vertical)
    out.push_back({K::Vertical, {id, 0}, {0, 0}, 0.0});
  if (snap.parallelTo)
    out.push_back({K::Parallel, {id, 0}, {*snap.parallelTo, 0}, 0.0});
  else if (snap.perpendicularTo)
    out.push_back({K::Perpendicular, {id, 0}, {*snap.perpendicularTo, 0}, 0.0});
}

// Adds each candidate constraint only if it further constrains the sketch
// (sketch::JudgeAddition, the test every add goes through) — the safety net
// that keeps inference from over-constraining. Settles geometry with the
// kept constraints.
void ApplyInferred(SketchFeature& sketch,
                   const std::vector<sketch::Constraint>& candidates) {
  sketch::Solver solver;
  solver.Load(sketch);
  sketch::SolveResult r = solver.Solve();
  std::vector<bool> driving;
  for (const sketch::Constraint& k : sketch.constraints)
    driving.push_back(!k.driven);
  for (const auto& c : candidates) {
    sketch.constraints.push_back(c);
    sketch::Solver s2;
    const sketch::AddVerdict v = sketch::JudgeAddition(r, driving, sketch, &s2);
    if (v.accepted) {
      r = v.after;
      driving.push_back(!c.driven);
      if (v.after.ok)
        s2.WriteBack(sketch.entities); // settle with this constraint
    } else {
      sketch.constraints.pop_back(); // redundant or conflicting — drop it
    }
  }
}

// Defining points of an entity eligible for snap inference — the subset of
// its vertices that are real solver points: Point / Circle centre / Ellipse
// centre = {0}, Arc = centre/start/end. Lines go through CommitLine instead.
std::vector<uint8_t> InferrablePoints(const sketch::Entity& entity) {
  if (std::holds_alternative<sketch::Arc>(entity))
    return {0, 1, 2};
  if (std::holds_alternative<sketch::Line>(entity))
    return {};
  return {0};
}
bool LineHasDrivingDimension(const SketchFeature& sketch, uint32_t lineId) {
  return std::any_of(sketch.constraints.begin(), sketch.constraints.end(),
                     [lineId](const sketch::Constraint& c) {
                       return sketch::IsDimension(c.kind) &&
                              (c.a.entity == lineId || c.b.entity == lineId);
                     });
}

void BeginSketchTranslate(SketchDragSession& drag,
                          const SketchFeature& sketch) {
  drag.mode = SketchDragSession::Mode::SketchTranslate;
  for (const sketch::Entity& entity : sketch.entities) {
    const uint32_t id = sketch::EntityId(entity);
    for (const auto& [which, point] : EntityPoints(entity)) {
      drag.points.push_back({id, which});
      drag.initialPos.push_back(point);
    }
  }
}
} // namespace

Ray ScreenRay(AppState& state, const glm::vec2& mousePos) {
  // Map through the central-node viewport rect (see Pick in ViewportInput)
  return state.faceSelector->ScreenToWorldRay(
      mousePos.x - state.viewportPos.x, mousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
}

// Starts a drag on mouse-down: builds the solver once, pins the dragged element
// (endpoint / line body / circle rim) with a weak temporary constraint, and
// captures the mouse-down baseline. Leaves the session inactive if the sketch
// is conflicting (BeginDrag refused).
void BeginDragSession(AppState& state, SketchFeature& sketch, size_t index,
                      const SketchPick& pick, glm::dvec2 cursorUV) {
  SketchDragSession& drag = state.sketchDrag;
  drag = SketchDragSession{}; // reset (drops any prior solver)
  const size_t eidx = sketch.IndexOfEntity(pick.ref.entity);
  if (eidx == SIZE_MAX)
    return;
  const sketch::Entity& e = sketch.entities[eidx];

  drag.featureIndex = index;
  drag.initialCursor = cursorUV;
  drag.solver = std::make_unique<sketch::Solver>();
  drag.solver->Load(sketch);
  state.editBaseline = DocumentIO::FeatureToJson(sketch);

  bool ok = false;
  if (pick.body && std::holds_alternative<sketch::Circle>(e)) {
    drag.mode = SketchDragSession::Mode::CircleRim;
    drag.circleId = pick.ref.entity;
    drag.circleCenter = std::get<sketch::Circle>(e).center;
    ok = drag.solver->BeginRadiusDrag(drag.circleId);
  } else if (pick.body && std::holds_alternative<sketch::Line>(e)) {
    const auto& l = std::get<sketch::Line>(e);
    if (LineHasDrivingDimension(sketch, pick.ref.entity)) {
      // A driving dimension defines the line's size. Moving its body should
      // therefore relocate the unconstrained sketch, not fight that value.
      BeginSketchTranslate(drag, sketch);
    } else {
      drag.mode = SketchDragSession::Mode::LineBody;
      drag.points = {{pick.ref.entity, 0}, {pick.ref.entity, 1}};
      drag.initialPos = {l.a, l.b};
    }
    ok = drag.solver->BeginDrag(drag.points);
  } else if (pick.body && (std::holds_alternative<sketch::Ellipse>(e) ||
                           std::holds_alternative<sketch::EllipseArc>(e))) {
    // An ellipse by its body moves by its centre: the solver holds its
    // size, shape, turn and an arc's ends softly, so it translates whole
    // (docs/sketching/README.md#ellipses-and-splines). Its handles -- the
    // axis and arc ends -- reshape it.
    drag.mode = SketchDragSession::Mode::Point;
    drag.points = {{pick.ref.entity, 0}};
    drag.initialPos = {sketch::EntityPointUV(e, 0)};
    ok = drag.solver->BeginDrag(drag.points);
  } else if (pick.body && std::holds_alternative<sketch::BSpline>(e)) {
    // A spline by its body moves whole: every pole follows the cursor by the
    // same delta, instead of one pulled pole reshaping it.
    drag.mode = SketchDragSession::Mode::LineBody;
    for (const auto& [which, uv] : EntityPoints(e)) {
      drag.points.push_back({pick.ref.entity, which});
      drag.initialPos.push_back(uv);
    }
    ok = drag.solver->BeginDrag(drag.points);
  } else {
    drag.mode = SketchDragSession::Mode::Point;
    // From the point itself, not the cursor: the pick tolerance is pixels,
    // and a handle that jumped to the cursor would move before the drag did.
    glm::dvec2 pos = cursorUV;
    if (!pick.body && pick.ref.which != sketch::kWholeEntity)
      pos = sketch::EntityPointUV(e, pick.ref.which);
    else if (auto* c = std::get_if<sketch::Circle>(&e))
      pos = c->center;
    drag.points = {pick.ref};
    drag.initialPos = {pos};
    ok = drag.solver->BeginDrag(drag.points);
  }
  drag.active = ok;
  if (!ok)
    drag.solver.reset();
}

// Commits an entity to the active sketch as one undoable ModifyFeatureCommand.
// With `infer`, each defining point is re-inferred against the existing
// geometry (the clicks were already cursor-snapped, so this re-finds the same
// targets) and the implied coincident / point-on-line / align constraints are
// applied — the non-line twin of CommitLine's inference.
uint32_t CommitEntity(AppState& state, SketchFeature& sketch, size_t index,
                      const sketch::Entity& entity, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  std::vector<std::pair<uint8_t, sketch::Snap>> snaps;
  if (infer) {
    for (uint8_t which : InferrablePoints(entity)) {
      const glm::dvec2 p = sketch::EntityPointUV(entity, which);
      const float ppu = PixelsPerUV(state, sketch.plane, p);
      snaps.emplace_back(
          which, sketch::InferPoint(sketch.entities, p, std::nullopt, 0, ppu));
    }
  }
  const uint32_t id = sketch.AddEntity(entity); // stamps a stable id
  if (!snaps.empty()) {
    std::vector<sketch::Constraint> candidates;
    for (const auto& [which, snap] : snaps)
      SnapPointConstraints(snap, id, which, candidates);
    ApplyInferred(sketch, candidates);
  }
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  // Dimension-on-create target: circles/arcs take a typed radius.
  state.sketchEdit.lastDrawnStage = 0;
  state.sketchEdit.lastDrawn.clear();
  if (std::holds_alternative<sketch::Circle>(entity) ||
      std::holds_alternative<sketch::Arc>(entity))
    state.sketchEdit.lastDrawn = {id};
  return id;
}

// Per-frame while dragging: retarget from the cursor delta, re-solve, write
// back.
void UpdateDragSession(AppState& state, SketchFeature& sketch,
                       glm::dvec2 cursorUV) {
  SketchDragSession& drag = state.sketchDrag;
  if (!drag.active || !drag.solver)
    return;
  if (drag.mode == SketchDragSession::Mode::CircleRim) {
    drag.solver->SetRadiusTarget(glm::length(cursorUV - drag.circleCenter));
  } else {
    const glm::dvec2 delta = cursorUV - drag.initialCursor;
    std::vector<glm::dvec2> targets;
    targets.reserve(drag.initialPos.size());
    for (const glm::dvec2& ip : drag.initialPos)
      targets.push_back(ip + delta);
    drag.solver->SetDragTargets(targets);
  }
  if (drag.solver->SolveDrag().ok) {
    drag.solver->WriteBack(sketch.entities);
    sketch.RegenerateDerived();
    state.document->Regenerate();
  }
}

// Commits a rectangle (4 lines + corner/H/V constraints) as one undo record,
// then solves so the shape settles. With `infer`, the two clicked corners are
// inferred against the pre-existing geometry (before the four sides are added,
// so the rectangle can't snap to itself) and the implied constraints applied.
void CommitRectangle(AppState& state, SketchFeature& sketch, size_t index,
                     glm::dvec2 c1, glm::dvec2 c2, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  std::optional<sketch::Snap> s1, s2;
  if (infer) {
    s1 = sketch::InferPoint(sketch.entities, c1, std::nullopt, 0,
                            PixelsPerUV(state, sketch.plane, c1));
    s2 = sketch::InferPoint(sketch.entities, c2, std::nullopt, 0,
                            PixelsPerUV(state, sketch.plane, c2));
  }
  const auto ids = sketch.AddRectangle(c1, c2);
  if (infer) {
    // Clicked corners: c1 = bottom line's start (l0,0), c2 = top line's start
    // (l2,0) — see AddRectangle's corner layout.
    std::vector<sketch::Constraint> candidates;
    SnapPointConstraints(*s1, ids[0], 0, candidates);
    SnapPointConstraints(*s2, ids[2], 0, candidates);
    ApplyInferred(sketch, candidates);
  }
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch.entities);
  sketch.RegenerateDerived();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  // Dimension-on-create targets: typed numbers become width, then height.
  state.sketchEdit.lastDrawn = {ids[0], ids[1], ids[2], ids[3]};
  state.sketchEdit.lastDrawnStage = 0;
}

// Commits a centre + corner rectangle (AddRectangleCentered: 4 lines + the
// anchored centre point) as one undo record. With `infer`, the CENTRE point
// and the clicked corner are inferred against the pre-existing geometry.
void CommitCenterRectangle(AppState& state, SketchFeature& sketch, size_t index,
                           glm::dvec2 center, glm::dvec2 corner, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  std::optional<sketch::Snap> s1, s2;
  if (infer) {
    s1 = sketch::InferPoint(sketch.entities, center, std::nullopt, 0,
                            PixelsPerUV(state, sketch.plane, center));
    s2 = sketch::InferPoint(sketch.entities, corner, std::nullopt, 0,
                            PixelsPerUV(state, sketch.plane, corner));
    center = s1->uv;
    corner = s2->uv;
  }
  const auto ids = sketch.AddRectangleCentered(center, corner);
  if (infer) {
    std::vector<sketch::Constraint> candidates;
    SnapPointConstraints(*s1, ids[4], 0, candidates); // the centre point
    SnapPointConstraints(*s2, ids[2], 0, candidates); // the clicked corner
    ApplyInferred(sketch, candidates);
  }
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch.entities);
  sketch.RegenerateDerived();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  state.sketchEdit.lastDrawn = {ids[0], ids[1], ids[2], ids[3]};
  state.sketchEdit.lastDrawnStage = 0;
}

// Intersects a screen ray with a plane; returns UV on the plane, or nullopt
// if parallel.
std::optional<glm::dvec2> RayToPlaneUV(const Ray& ray,
                                       const SketchPlane& plane) {
  const glm::dvec3 n = plane.GetNormal();
  const glm::dvec3 o(ray.origin), dir(ray.direction);
  const double denom = glm::dot(dir, n);
  if (std::abs(denom) < 1e-6)
    return std::nullopt;
  const double t = glm::dot(plane.GetOrigin() - o, n) / denom;
  if (t < 0.0)
    return std::nullopt;
  return plane.WorldToPlane(o + t * dir);
}

// Mouse-up: end the drag and commit one undoable edit from the baseline.
void EndDragSession(AppState& state, SketchFeature& sketch) {
  SketchDragSession& drag = state.sketchDrag;
  if (!drag.active)
    return;
  drag.active = false;
  drag.solver.reset();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  if (after != state.editBaseline)
    state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                drag.featureIndex, state.editBaseline, after),
                            *state.document);
}

// Commits a line a->b with auto-constraint inference: snaps each endpoint to
// nearby geometry and applies the implied coincident / point-on-line / H-V /
// parallel-perpendicular constraints (each kept only if it bites). `infer`
// off (auto-constrain disabled or Ctrl held) places the raw line.
void CommitLine(AppState& state, SketchFeature& sketch, size_t index,
                glm::dvec2 a, glm::dvec2 b, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  glm::dvec2 pa = a, pb = b;
  std::vector<sketch::Constraint> candidates;
  uint32_t id = 0;
  if (infer) {
    const float ppu = PixelsPerUV(state, sketch.plane, b);
    sketch::Snap sa =
        sketch::InferPoint(sketch.entities, a, std::nullopt, 0, ppu);
    sketch::Snap sb = sketch::InferPoint(sketch.entities, b, sa.uv, 0, ppu);
    pa = sa.uv;
    pb = sb.uv;
    id = sketch.AddEntity(sketch::Line{pa, pb});
    SnapPointConstraints(sa, id, 0, candidates);
    SnapPointConstraints(sb, id, 1, candidates);
    SnapSegmentConstraints(sb, id, candidates);
    ApplyInferred(sketch, candidates);
  } else {
    id = sketch.AddEntity(sketch::Line{pa, pb});
  }
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  // Dimension-on-create target: a typed number becomes the line's length.
  state.sketchEdit.lastDrawn = {id};
  state.sketchEdit.lastDrawnStage = 0;
}

LineVertex3D LineVertex(const glm::vec3& p) {
  LineVertex3D v;
  v.position = p;
  v.color = kPreviewColor;
  return v;
}

// Approximate pixels per UV unit at `uv` (from the current zoom), for turning
// the inference's pixel tolerances into UV distances.
float PixelsPerUV(AppState& state, const SketchPlane& plane, glm::dvec2 uv) {
  glm::vec2 s0 = SketchUVToScreen(state, plane, uv);
  glm::vec2 s1 = SketchUVToScreen(state, plane, uv + glm::dvec2(1.0, 0.0));
  return glm::length(s1 - s0);
}

SketchPlane DefaultPlane(int index) {
  switch (index) {
  case 1:
    return SketchPlane(PlaneType::XZ);
  case 2:
    return SketchPlane(PlaneType::YZ);
  default:
    return SketchPlane(PlaneType::XY);
  }
}

SketchFeature* ResolveActiveSketch(AppState& state, size_t* outIndex) {
  if (state.sketchEdit.activeSketchId == 0 || !state.document)
    return nullptr;
  Feature* f = state.document->FindFeatureById(state.sketchEdit.activeSketchId,
                                               outIndex);
  auto* sketch = dynamic_cast<SketchFeature*>(f);
  if (!sketch) {
    // The sketch was removed (undo); leave edit mode cleanly.
    state.sketchEdit = SketchEditState{};
  }
  return sketch;
}

// Commits a freshly-built sketch, enters edit mode on it, and aligns the
// camera to its plane. Shared by the plane and datum entry points.
static uint64_t BeginSketch(AppState& state, std::unique_ptr<SketchFeature> sk,
                            const SketchPlane& plane, const std::string& name,
                            SketchPurpose purpose) {
  // Refused where the pick is answered: the pick stays armed and its prompt
  // says why (docs/ui/tool-sessions.md#defaults-and-refusals).
  if (const std::string why = readctx::CheckFeatureRoom(*state.document, 1);
      !why.empty()) {
    state.sketchEdit.pickingPlane = true;
    state.sketchEdit.actionMsg = "Sketch: " + why;
    return 0;
  }
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(sk)),
                          *state.document);
  const size_t index = state.document->FeatureCount() - 1;
  const uint64_t id = state.document->GetFeature(index)->GetId();

  state.sketchEdit = SketchEditState{};
  state.sketchEdit.activeSketchId = id;
  // Applied AFTER the reset above — a caller cannot set these beforehand.
  state.sketchEdit.purpose = purpose;
  if (purpose == SketchPurpose::Hole)
    state.sketchEdit.tool = SketchTool::Point; // the session's only job
  state.measure = MeasureState{}; // sketch mode replaces the measure tool
  SelectFeature(state, id);
  AlignCameraToSketch(state, plane);
  LOG_INFO("Started {} (id {})", name, id);
  return id;
}

uint64_t BeginPreparedSketch(AppState& state,
                             std::unique_ptr<SketchFeature> sketch) {
  if (!state.document || !sketch)
    return 0;
  const SketchPlane plane = sketch->plane;
  const std::string name = sketch->GetName();
  return BeginSketch(state, std::move(sketch), plane, name,
                     SketchPurpose::Freeform);
}

void AlignCameraToSketch(AppState& state, const SketchPlane& plane) {
  if (!state.camera)
    return;
  const glm::vec3 n(plane.GetNormal());
  const glm::vec3 up =
      std::abs(glm::dot(glm::normalize(n), conv::kWorldUpF)) > 0.9f
          ? glm::vec3(plane.GetVAxis())
          : conv::kWorldUpF;
  state.camera->AlignToFace(plane.GetOrigin(), n, 0.0f, up);
}

uint64_t BeginSketchOnPlane(AppState& state, const SketchPlane& plane,
                            const std::string& name, SketchPurpose purpose) {
  return BeginSketch(state, std::make_unique<SketchFeature>(name, plane), plane,
                     name, purpose);
}

uint64_t BeginSketchOnDatum(AppState& state, const DatumPlaneFeature& datum,
                            const std::string& name, SketchPurpose purpose) {
  // Bake the datum's current plane as the initial value and record the
  // reference so the sketch re-derives its plane on every regeneration.
  auto sketch = std::make_unique<SketchFeature>(name, datum.OutputPlane());
  sketch->planeRef = datum.GetId();
  return BeginSketch(state, std::move(sketch), datum.OutputPlane(), name,
                     purpose);
}

std::optional<ElementRef> FaceElementRef(AppState& state, uint32_t meshId,
                                         uint32_t faceIndex) {
  const auto& meshes = state.gpu.documentMeshes;
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == meshId; });
  // Whatever the viewport is showing — a job tab has no document, and asking
  // one there dereferenced null and picked nothing.
  const std::vector<Body>& bodies = ViewportBodies(state);
  if (it == meshes.end() || it->bodyIndex >= bodies.size())
    return std::nullopt;
  const Body& body = bodies[it->bodyIndex];
  const std::string* name = body.elementMap.NameOf(static_cast<int>(faceIndex));
  if (name == nullptr)
    return std::nullopt; // face isn't named (only extrude NewBody faces are)
  return ElementRef{body.createdBy, *name};
}

uint64_t BeginSketchOnFace(AppState& state, const ElementRef& ref,
                           const SketchPlane& plane, const std::string& name,
                           SketchPurpose purpose) {
  // Bake the face's current plane and record the ElementRef so the sketch
  // re-derives its plane from the (possibly moved) face on every regeneration.
  auto sketch = std::make_unique<SketchFeature>(name, plane);
  sketch->faceRef = ref;
  // The host face's outline comes in as LIVE external geometry (locked
  // construction projected onto the plane) — snappable/dimensionable
  // references that now track the face when the body changes, instead of the
  // old one-shot Fixed-constraint bake. Materialises on the first regen.
  sketch->AddExternalGeometry(ref, sketch::ExternalKind::Face);
  return BeginSketch(state, std::move(sketch), plane, name, purpose);
}

bool ExactFacePlane(const AppState& state, uint32_t meshId, uint32_t faceIndex,
                    glm::dvec3& origin, glm::dvec3& normal, glm::dvec3& uAxis) {
  if (!state.document)
    return false;
  const auto& meshes = state.gpu.documentMeshes;
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == meshId; });
  if (it == meshes.end() || it->bodyIndex >= state.document->GetBodies().size())
    return false;
  const Body& body = state.document->GetBodies()[it->bodyIndex];
  return body.shape.FacePlane(static_cast<int>(faceIndex), origin, normal,
                              uAxis);
}

bool BeginHoleSketchOnFace(AppState& state, uint32_t meshId,
                           uint32_t faceIndex) {
  // The B-Rep planarity gate: FacePlane fails on curved faces and hands back
  // the exact frame for planar ones.
  glm::dvec3 origin, normal, uAxis;
  if (!ExactFacePlane(state, meshId, faceIndex, origin, normal, uAxis))
    return false;
  const SketchPlane plane(origin, normal, uAxis);
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const std::string name = "Hole points " + std::to_string(n);
  if (auto ref = FaceElementRef(state, meshId, faceIndex))
    BeginSketchOnFace(state, *ref, plane, name, SketchPurpose::Hole);
  else
    BeginSketchOnPlane(state, plane, name, SketchPurpose::Hole);
  return true;
}

// Re-points an existing sketch's faceRef at a new face, as one undoable edit.
void ReattachSketchFace(AppState& state, uint64_t sketchId,
                        const ElementRef& ref) {
  size_t index = 0;
  auto* sk = dynamic_cast<SketchFeature*>(
      state.document->FindFeatureById(sketchId, &index));
  if (sk == nullptr)
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  sk->faceRef = ref;
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sk);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
}

// Sketch-context keyboard shortcuts (the sketch keymap). Called each frame
// while a
// sketch is being edited. Guarded so keys never fire while typing in a field or
// during a Ctrl/Alt chord (undo/redo).
// Copies the selected entities (deduplicated) into the app clipboard.
void CopySketchSelection(AppState& state, SketchFeature& sketch) {
  std::vector<uint32_t> ids;
  for (const auto& r : state.sketchEdit.selected)
    if (std::find(ids.begin(), ids.end(), r.entity) == ids.end())
      ids.push_back(r.entity);
  state.sketchClipboard.clear();
  state.sketchClipboardIds.clear();
  for (const auto& e : sketch.entities)
    if (std::find(ids.begin(), ids.end(), EntityId(e)) != ids.end()) {
      state.sketchClipboard.push_back(e);
      state.sketchClipboardIds.push_back(EntityId(e));
    }
  state.sketchClipboardSourceSketch = sketch.GetId();
  state.sketchClipboardSourcePart =
      state.activePart >= 0 &&
              static_cast<size_t>(state.activePart) < state.parts.size()
          ? state.parts[state.activePart].id
          : 0;
  state.sketchEdit.actionMsg =
      std::to_string(state.sketchClipboard.size()) +
      " entities copied (Ctrl+V paste, Ctrl+Shift+V paste linked)";
}

// Pastes the clipboard into the active sketch as UNLINKED copies with fresh
// ids, offset so the paste is visible beside the source. (Linked copies that
// track the source are a separate design —
// docs/sketching/README.md#linked-copies.)
void PasteSketchClipboard(AppState& state, SketchFeature& sketch,
                          size_t index) {
  if (state.sketchClipboard.empty())
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  const glm::dvec2 d(0.5, 0.5);
  for (sketch::Entity e : state.sketchClipboard) {
    std::visit(
        [&](auto& ent) {
          using T = std::decay_t<decltype(ent)>;
          if constexpr (std::is_same_v<T, sketch::Point>) {
            ent.p += d;
          } else if constexpr (std::is_same_v<T, sketch::Line>) {
            ent.a += d;
            ent.b += d;
          } else if constexpr (std::is_same_v<T, sketch::Circle> ||
                               std::is_same_v<T, sketch::Arc> ||
                               std::is_same_v<T, sketch::Ellipse> ||
                               std::is_same_v<T, sketch::EllipseArc>) {
            ent.center += d;
          } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
            for (glm::dvec2& pole : ent.poles)
              pole += d;
          } else if constexpr (std::is_same_v<T, sketch::Text>) {
            ent.anchor += d;
          }
        },
        e);
    sketch.AddEntity(std::move(e)); // fresh id
  }
  state.sketchEdit.selected.clear();
  sketch.RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  state.sketchEdit.actionMsg =
      std::to_string(state.sketchClipboard.size()) + " entities pasted";
}

// Pastes the clipboard as a LINKED copy
// (docs/sketching/README.md#linked-copies): outputs re-sync from the source
// until the link is broken. Cross-sketch requires the source EARLIER in the
// history. From ANOTHER part, the source sketch is copied by a Derived placed
// before this sketch (made, or one already there reused) and the pasted
// entities project its copies, so they follow the other part
// (docs/modeling/derived.md#sketches).
void PasteSketchClipboardLinked(AppState& state, SketchFeature& sketch,
                                size_t index) {
  if (state.sketchClipboardIds.empty())
    return;
  Document& doc = *state.document;
  const uint64_t srcId = state.sketchClipboardSourceSketch;
  const uint64_t herePart =
      state.activePart >= 0 &&
              static_cast<size_t>(state.activePart) < state.parts.size()
          ? state.parts[state.activePart].id
          : 0;
  // The clipboard names its part: a feature id alone is a per-part counter,
  // and named this part's own sketch of that id.
  if (state.sketchClipboardSourcePart != 0 &&
      state.sketchClipboardSourcePart != herePart) {
    const uint64_t sketchId = sketch.GetId();
    const size_t mark = state.commands->UndoDepth();
    size_t at = index;
    uint64_t derivedId = 0;
    if (const std::string why = DerivedSketchBefore(
            state, state.sketchClipboardSourcePart, srcId, at, derivedId);
        !why.empty()) {
      state.commands->RollbackSince(mark, doc);
      state.sketchEdit.actionMsg =
          "Could not link to the other part (" + why + ") - pasted unlinked";
      if (auto* sk =
              dynamic_cast<SketchFeature*>(doc.FindFeatureById(sketchId)))
        PasteSketchClipboard(state, *sk, at);
      return;
    }
    // The commands above replaced the feature objects: find the sketch again.
    auto* target =
        dynamic_cast<SketchFeature*>(doc.FindFeatureById(sketchId, &at));
    const auto* copy =
        dynamic_cast<const DerivedFeature*>(doc.FindFeatureById(derivedId));
    if (target == nullptr || copy == nullptr)
      return;
    nlohmann::json before = DocumentIO::FeatureToJson(*target);
    for (uint32_t id : state.sketchClipboardIds)
      target->AddExternalGeometry(
          ElementRef{derivedId, copy->SketchEntityName(srcId, id)},
          sketch::ExternalKind::Edge);
    state.sketchEdit.selected.clear();
    nlohmann::json after = DocumentIO::FeatureToJson(*target);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(at, before, after), doc);
    state.commands->CoalesceSince(mark, "Paste linked from another part");
    state.sketchEdit.actionMsg =
        std::to_string(state.sketchClipboardIds.size()) +
        " linked copies pasted from '" + copy->sourceName +
        "' (violet = follows that part)";
    return;
  }
  SketchFeature* source = nullptr;
  uint64_t opSourceId = 0; // 0 = same sketch
  if (srcId == sketch.GetId()) {
    source = &sketch;
  } else {
    size_t srcIndex = 0;
    source =
        dynamic_cast<SketchFeature*>(doc.FindFeatureById(srcId, &srcIndex));
    if (source == nullptr) {
      state.sketchEdit.actionMsg =
          "The source sketch is gone - pasted unlinked";
      PasteSketchClipboard(state, sketch, index);
      return;
    }
    if (srcIndex >= index) {
      state.sketchEdit.actionMsg =
          "Linked copies need the source sketch earlier in the history - "
          "pasted unlinked";
      PasteSketchClipboard(state, sketch, index);
      return;
    }
    opSourceId = srcId;
  }
  // Validate every copied entity still exists in the source.
  for (uint32_t id : state.sketchClipboardIds) {
    if (source->IndexOfEntity(id) == std::numeric_limits<size_t>::max()) {
      state.sketchEdit.actionMsg =
          "A copied entity no longer exists in the source - pasted unlinked";
      PasteSketchClipboard(state, sketch, index);
      return;
    }
  }

  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch::DerivedOp op;
  op.kind = sketch::DerivedKind::LinkedCopy;
  op.sourceIds = state.sketchClipboardIds;
  op.vec = glm::dvec2(0.5, 0.5); // paste offset
  op.count = 2;
  op.sourceSketchId = opSourceId;
  sketch.derived.push_back(op);
  // Materialize the outputs now; regeneration keeps them in sync after.
  if (opSourceId == 0)
    sketch.RegenerateDerived();
  else
    sketch.ReplayLinkedCopyFrom(sketch.derived.back(), *source);
  state.sketchEdit.selected.clear();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->PushApplied(
      std::make_unique<ModifyFeatureCommand>(index, before, after));
  state.sketchEdit.actionMsg =
      std::to_string(state.sketchClipboardIds.size()) +
      " linked copies pasted (violet = synced; break the link in the "
      "Sketch panel)";
}

void SetSketchTool(AppState& state, SketchTool tool) {
  SketchEditState& edit = state.sketchEdit;
  edit.tool = tool;
  edit.hasPendingPoint = edit.hasPendingPoint2 = false;
  edit.hasPendingPoint3 = edit.hasPendingPoint4 = false;
  edit.splinePoles.clear();
  edit.mirrorAxisPhase = false;
  edit.dimPickBody.clear();
  edit.dimPlacing = false;
  edit.lastDrawn.clear(); // dimension-on-create doesn't cross tools
  edit.dimOnCreateOpen = false;
  if (tool == SketchTool::Dimension)
    edit.selected.clear(); // dimension targets accumulate from scratch
  edit.actionMsg.clear();
  if (tool == SketchTool::Mirror)
    MirrorToolActivated(state); // preselected construction line = axis
  edit.textEditId = 0;
  edit.textError.clear();
  if (tool == SketchTool::Text)
    TextToolActivated(state); // default height; a selected label to edit
}

} // namespace ui
