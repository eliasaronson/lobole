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
#include "ui/AppActions.h"       // SelectFeature
#include "ui/SketchToolbar.h"    // BeginSketchExtrude/Revolve (Shift+E/R)

#include "imgui.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

namespace ui {

namespace {

constexpr glm::vec3 kPreviewColor{0.95f, 0.85f, 0.35f};

// Body hit tolerance for the interactive sketch tools (endpoints get +4 px on
// top — see PickSketchCandidates). Was 12; lines were hard to hit.
constexpr float kSketchBodyTolPx = 14.0f;

LineVertex3D LineVertex(const glm::vec3& p) {
  LineVertex3D v;
  v.position = p;
  v.color = kPreviewColor;
  return v;
}

// Intersects a screen ray with a plane; returns UV on the plane, or nullopt
// if parallel.
std::optional<glm::vec2> RayToPlaneUV(const Ray& ray,
                                      const SketchPlane& plane) {
  const glm::vec3 n = plane.GetNormal();
  const float denom = glm::dot(ray.direction, n);
  if (std::abs(denom) < 1e-6f)
    return std::nullopt;
  const float t = glm::dot(plane.GetOrigin() - ray.origin, n) / denom;
  if (t < 0.0f)
    return std::nullopt;
  glm::vec3 world = ray.origin + t * ray.direction;
  return plane.WorldToPlane(world);
}

// Approximate pixels per UV unit at `uv` (from the current zoom), for turning
// the inference's pixel tolerances into UV distances.
float PixelsPerUV(AppState& state, const SketchPlane& plane, glm::vec2 uv) {
  glm::vec2 s0 = SketchUVToScreen(state, plane, uv);
  glm::vec2 s1 = SketchUVToScreen(state, plane, uv + glm::vec2(1.0f, 0.0f));
  return glm::length(s1 - s0);
}

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
// (DOF drops) without conflicting — the add-then-diagnose safety net that keeps
// inference from over-constraining. Settles geometry with the kept constraints.
void ApplyInferred(SketchFeature& sketch,
                   const std::vector<sketch::Constraint>& candidates) {
  sketch::Solver solver;
  solver.Load(sketch);
  sketch::SolveResult r = solver.Solve();
  int dof = r.ok ? r.dof : -1;
  for (const auto& c : candidates) {
    sketch.constraints.push_back(c);
    sketch::Solver s2;
    s2.Load(sketch);
    sketch::SolveResult r2 = s2.Solve();
    const bool keep =
        r2.ok && r2.conflicting.empty() && (dof < 0 || r2.dof < dof);
    if (keep) {
      dof = r2.dof;
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

// Commits an entity to the active sketch as one undoable ModifyFeatureCommand.
// With `infer`, each defining point is re-inferred against the existing
// geometry (the clicks were already cursor-snapped, so this re-finds the same
// targets) and the implied coincident / point-on-line / align constraints are
// applied — the non-line twin of CommitLine's inference.
void CommitEntity(AppState& state, SketchFeature& sketch, size_t index,
                  const sketch::Entity& entity, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  std::vector<std::pair<uint8_t, sketch::Snap>> snaps;
  if (infer) {
    for (uint8_t which : InferrablePoints(entity)) {
      const glm::vec2 p = sketch::EntityPointUV(entity, which);
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
}

// Commits a line a->b with auto-constraint inference: snaps each endpoint to
// nearby geometry and applies the implied coincident / point-on-line / H-V /
// parallel-perpendicular constraints (each kept only if it bites). `infer`
// off (auto-constrain disabled or Ctrl held) places the raw line.
void CommitLine(AppState& state, SketchFeature& sketch, size_t index,
                glm::vec2 a, glm::vec2 b, bool infer) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  glm::vec2 pa = a, pb = b;
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

// Commits a rectangle (4 lines + corner/H/V constraints) as one undo record,
// then solves so the shape settles. With `infer`, the two clicked corners are
// inferred against the pre-existing geometry (before the four sides are added,
// so the rectangle can't snap to itself) and the implied constraints applied.
void CommitRectangle(AppState& state, SketchFeature& sketch, size_t index,
                     glm::vec2 c1, glm::vec2 c2, bool infer) {
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
                           glm::vec2 center, glm::vec2 corner, bool infer) {
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

Ray ScreenRay(AppState& state, const glm::vec2& mousePos) {
  // Map through the central-node viewport rect (see Pick in ViewportInput)
  return state.faceSelector->ScreenToWorldRay(
      mousePos.x - state.viewportPos.x, mousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
}

// Defined with sketch picking below. A forward declaration lets body-drag
// setup use the same complete set of solver points.
std::vector<std::pair<uint8_t, glm::vec2>>
EntityPoints(const sketch::Entity& e);

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

// Starts a drag on mouse-down: builds the solver once, pins the dragged element
// (endpoint / line body / circle rim) with a weak temporary constraint, and
// captures the mouse-down baseline. Leaves the session inactive if the sketch
// is conflicting (BeginDrag refused).
void BeginDragSession(AppState& state, SketchFeature& sketch, size_t index,
                      const SketchPick& pick, glm::vec2 cursorUV) {
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
  } else {
    drag.mode = SketchDragSession::Mode::Point;
    glm::vec2 pos = cursorUV;
    if (auto* p = std::get_if<sketch::Point>(&e))
      pos = p->p;
    else if (auto* l = std::get_if<sketch::Line>(&e))
      pos = pick.ref.which ? l->b : l->a;
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

// Per-frame while dragging: retarget from the cursor delta, re-solve, write
// back.
void UpdateDragSession(AppState& state, SketchFeature& sketch,
                       glm::vec2 cursorUV) {
  SketchDragSession& drag = state.sketchDrag;
  if (!drag.active || !drag.solver)
    return;
  if (drag.mode == SketchDragSession::Mode::CircleRim) {
    drag.solver->SetRadiusTarget(glm::length(cursorUV - drag.circleCenter));
  } else {
    const glm::vec2 delta = cursorUV - drag.initialCursor;
    std::vector<glm::vec2> targets;
    targets.reserve(drag.initialPos.size());
    for (const glm::vec2& ip : drag.initialPos)
      targets.push_back(ip + delta);
    drag.solver->SetDragTargets(targets);
  }
  if (drag.solver->SolveDrag().ok) {
    drag.solver->WriteBack(sketch.entities);
    sketch.RegenerateDerived();
    state.document->Regenerate();
  }
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

} // namespace

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
                            const SketchPlane& plane, const std::string& name) {
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(sk)),
                          *state.document);
  const size_t index = state.document->FeatureCount() - 1;
  const uint64_t id = state.document->GetFeature(index)->GetId();

  state.sketchEdit = SketchEditState{};
  state.sketchEdit.activeSketchId = id;
  state.measure = MeasureState{}; // sketch mode replaces the measure tool
  SelectFeature(state, id);
  if (state.camera) {
    state.camera->AlignToFace(plane.GetOrigin(), plane.GetNormal());
  }
  LOG_INFO("Started {} (id {})", name, id);
  return id;
}

uint64_t BeginSketchOnPlane(AppState& state, const SketchPlane& plane,
                            const std::string& name) {
  return BeginSketch(state, std::make_unique<SketchFeature>(name, plane), plane,
                     name);
}

uint64_t BeginSketchOnDatum(AppState& state, const DatumPlaneFeature& datum,
                            const std::string& name) {
  // Bake the datum's current plane as the initial value and record the
  // reference so the sketch re-derives its plane on every regeneration.
  auto sketch = std::make_unique<SketchFeature>(name, datum.OutputPlane());
  sketch->planeRef = datum.GetId();
  return BeginSketch(state, std::move(sketch), datum.OutputPlane(), name);
}

std::optional<ElementRef> FaceElementRef(AppState& state, uint32_t meshId,
                                         uint32_t faceIndex) {
  const auto& meshes = state.documentMeshes;
  const auto it = std::find_if(
      meshes.begin(), meshes.end(),
      [&](const DocumentRenderMesh& m) { return m.meshId == meshId; });
  if (it == meshes.end() || it->bodyIndex >= state.document->GetBodies().size())
    return std::nullopt;
  const Body& body = state.document->GetBodies()[it->bodyIndex];
  const std::string* name = body.elementMap.NameOf(static_cast<int>(faceIndex));
  if (name == nullptr)
    return std::nullopt; // face isn't named (only extrude NewBody faces are)
  return ElementRef{body.createdBy, *name};
}

uint64_t BeginSketchOnFace(AppState& state, const ElementRef& ref,
                           const SketchPlane& plane, const std::string& name) {
  // Bake the face's current plane and record the ElementRef so the sketch
  // re-derives its plane from the (possibly moved) face on every regeneration.
  auto sketch = std::make_unique<SketchFeature>(name, plane);
  sketch->faceRef = ref;
  // The host face's outline comes in as LIVE external geometry (locked
  // construction projected onto the plane) — snappable/dimensionable
  // references that now track the face when the body changes, instead of the
  // old one-shot Fixed-constraint bake. Materialises on the first regen.
  sketch->AddExternalGeometry(ref, sketch::ExternalKind::Face);
  return BeginSketch(state, std::move(sketch), plane, name);
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

// Sketch-context keyboard shortcuts (Onshape keymap). Called each frame while a
// sketch is being edited. Guarded so keys never fire while typing in a field or
// during a Ctrl/Alt chord (undo/redo).
// Copies the selected entities (deduplicated) into the app clipboard.
static void CopySketchSelection(AppState& state, SketchFeature& sketch) {
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
  state.sketchEdit.actionMsg =
      std::to_string(state.sketchClipboard.size()) +
      " entities copied (Ctrl+V paste, Ctrl+Shift+V paste linked)";
}

// Pastes the clipboard into the active sketch as UNLINKED copies with fresh
// ids, offset so the paste is visible beside the source. (Linked copies that
// track the source are a separate design — docs/sketching/copy-links.md.)
static void PasteSketchClipboard(AppState& state, SketchFeature& sketch,
                                 size_t index) {
  if (state.sketchClipboard.empty())
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  const glm::vec2 d(0.5f, 0.5f);
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
            for (glm::vec2& pole : ent.poles)
              pole += d;
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

// Pastes the clipboard as a LINKED copy (docs/sketching/copy-links.md):
// outputs re-sync from the source until the link is broken. Cross-sketch
// requires the source EARLIER in the history; cross-part (source not in
// this document) falls back to a plain paste with an explanation.
static void PasteSketchClipboardLinked(AppState& state, SketchFeature& sketch,
                                       size_t index) {
  if (state.sketchClipboardIds.empty())
    return;
  Document& doc = *state.document;
  const uint64_t srcId = state.sketchClipboardSourceSketch;
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
          "Source sketch is in another part - pasted unlinked";
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
  op.vec = glm::vec2(0.5f, 0.5f); // paste offset
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

static void HandleSketchKeys(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  ImGuiIO& io = ImGui::GetIO();
  // Copy/paste run on Ctrl chords, ahead of the chord early-out below.
  if (!io.WantTextInput && io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
    size_t index = 0;
    if (SketchFeature* sk = ResolveActiveSketch(state, &index)) {
      if (ImGui::IsKeyPressed(ImGuiKey_C) && !edit.selected.empty())
        CopySketchSelection(state, *sk);
      if (ImGui::IsKeyPressed(ImGuiKey_V)) {
        if (io.KeyShift)
          PasteSketchClipboardLinked(state, *sk, index);
        else
          PasteSketchClipboard(state, *sk, index);
      }
    }
  }
  if (io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper)
    return;
  const bool shift = io.KeyShift;

  auto setTool = [&](SketchTool t) {
    edit.tool = t;
    edit.hasPendingPoint = edit.hasPendingPoint2 = false;
    edit.hasPendingPoint3 = edit.hasPendingPoint4 = false;
    edit.splinePoles.clear();
    edit.mirrorAxisPhase = false;
    edit.dimPickBody.clear();
    edit.dimPlacing = false;
    edit.lastDrawn.clear(); // dimension-on-create doesn't cross tools
    edit.dimOnCreateOpen = false;
    if (t == SketchTool::Dimension)
      edit.selected.clear(); // dimension targets accumulate from scratch
    edit.actionMsg.clear();
  };

  if (shift) {
    if (ImGui::IsKeyPressed(ImGuiKey_F))
      setTool(SketchTool::Fillet); // Shift+F: Fillet
    else if (ImGui::IsKeyPressed(ImGuiKey_S))
      edit.autoConstrain = !edit.autoConstrain; // Shift+S: suppress inferencing
    else if (ImGui::IsKeyPressed(ImGuiKey_E))
      BeginSketchExtrude(state); // Shift+E: extrude the sketch
    else if (ImGui::IsKeyPressed(ImGuiKey_R))
      BeginSketchRevolve(state); // Shift+R: revolve the sketch
    else if (ImGui::IsKeyPressed(ImGuiKey_G))
      setTool(SketchTool::RectangleCenter); // Shift+G: centre rectangle
    return;
  }

  // Bare-key tools.
  if (ImGui::IsKeyPressed(ImGuiKey_L))
    return setTool(SketchTool::Line);
  if (ImGui::IsKeyPressed(ImGuiKey_C))
    return setTool(SketchTool::Circle);
  if (ImGui::IsKeyPressed(ImGuiKey_G))
    return setTool(SketchTool::Rectangle);
  if (ImGui::IsKeyPressed(ImGuiKey_A))
    return setTool(SketchTool::ArcThreePoint);
  if (ImGui::IsKeyPressed(ImGuiKey_S))
    return setTool(SketchTool::Spline);
  if (ImGui::IsKeyPressed(ImGuiKey_X))
    return setTool(SketchTool::Project);
  if (ImGui::IsKeyPressed(ImGuiKey_M))
    return setTool(SketchTool::Trim);
  if (ImGui::IsKeyPressed(ImGuiKey_O))
    return setTool(SketchTool::Offset);
  if (ImGui::IsKeyPressed(ImGuiKey_D))
    return setTool(SketchTool::Dimension);

  if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
    edit.selected.clear();
    return;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Q)) { // toggle construction on the selection
    ToggleConstruction(state);
    return;
  }

  // Constraints on the current selection.
  if (edit.selected.empty())
    return;
  using K = sketch::ConstraintKind;
  if (ImGui::IsKeyPressed(ImGuiKey_H))
    return (void)ApplySketchConstraint(state, K::Horizontal, 0.0);
  if (ImGui::IsKeyPressed(ImGuiKey_V))
    return (void)ApplySketchConstraint(state, K::Vertical, 0.0);
  if (ImGui::IsKeyPressed(ImGuiKey_N))
    return (void)ApplySketchConstraint(state, K::Coincident, 0.0);
  if (ImGui::IsKeyPressed(ImGuiKey_B))
    return (void)ApplySketchConstraint(state, K::Parallel, 0.0);
  if (ImGui::IsKeyPressed(ImGuiKey_T))
    return (void)ApplySketchConstraint(state, K::Tangent, 0.0);
  if (ImGui::IsKeyPressed(ImGuiKey_E))
    return (void)ApplySketchConstraint(state, K::Equal, 0.0);
}

namespace {

// UV of a point ref against the live sketch (datum origin/axes included).
std::optional<glm::vec2> TargetUV(const SketchFeature& sk,
                                  const sketch::PointRef& r) {
  if (sketch::IsDatum(r.entity))
    return sketch::DatumPointUV(r.entity, r.which);
  const size_t idx = sk.IndexOfEntity(r.entity);
  if (idx == SIZE_MAX)
    return std::nullopt;
  return sketch::EntityPointUV(sk.entities[idx], r.which);
}

// Direction of a line entity or datum axis (unnormalized), nullopt otherwise.
std::optional<glm::vec2> LineDirOf(const SketchFeature& sk, uint32_t id) {
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
double PointToLineDistance(const SketchFeature& sk, glm::vec2 p,
                           uint32_t lineId) {
  const auto a = TargetUV(sk, {lineId, 0});
  const auto dir = LineDirOf(sk, lineId);
  if (!a || !dir || glm::length(*dir) < 1e-9f)
    return 0.0;
  const glm::vec2 d = glm::normalize(*dir);
  const glm::vec2 r = p - *a;
  return std::abs(d.x * r.y - d.y * r.x);
}

// Creates the inferred dimension from the ordered targets, then opens the
// inline value editor on it so the user can type the value immediately (the
// Dimension tool's commit tail). Clears the accumulated targets either way.
void CommitInferredDimension(AppState& state, sketch::ConstraintKind kind,
                             double value, std::vector<sketch::PointRef> refs,
                             const glm::vec2* labelOffset = nullptr) {
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

// The resolved commit for a placed point-PAIR dimension (Phase 3.7): the
// kind switches by the cursor's zone relative to the span A→B, Onshape
// style. Cursor offset from the midpoint within ~22.5° of the span's
// perpendicular = pulled straight off the pair → ALIGNED (Distance); else
// mostly up/down → HORIZONTAL dimension (DistanceX, its line is horizontal);
// mostly sideways → VERTICAL (DistanceY). Axis-parallel spans skip the
// choice — aligned IS the axis dimension there.
struct PairPlacement {
  sketch::ConstraintKind kind = sketch::ConstraintKind::Distance;
  sketch::PointRef a, b; // ordered so the solver's signed value is positive
  double value = 0.0;
  glm::vec2 labelOffset{0.0f};
};
std::optional<PairPlacement> ClassifyPairPlacement(const SketchFeature& sk,
                                                   sketch::PointRef A,
                                                   sketch::PointRef B,
                                                   glm::vec2 cursor) {
  const auto paOpt = TargetUV(sk, A), pbOpt = TargetUV(sk, B);
  if (!paOpt || !pbOpt)
    return std::nullopt;
  glm::vec2 pa = *paOpt, pb = *pbOpt;
  const glm::vec2 span = pb - pa;
  const float len = glm::length(span);
  if (len < 1e-6f)
    return std::nullopt;
  const glm::vec2 d = span / len;
  const glm::vec2 n(-d.y, d.x); // the aligned dimension's offset direction
  const glm::vec2 mid = 0.5f * (pa + pb);
  const glm::vec2 off = cursor - mid;
  const glm::vec2 u = glm::length(off) > 1e-6f ? glm::normalize(off) : n;

  PairPlacement out;
  out.a = A;
  out.b = B;
  const bool axisParallel = std::abs(d.x) < 0.01f || std::abs(d.y) < 0.01f;
  if (axisParallel || std::abs(glm::dot(u, n)) > 0.924f) {
    out.kind = sketch::ConstraintKind::Distance;
    out.value = len;
    out.labelOffset = n * glm::dot(off, n);
    if (glm::length(out.labelOffset) < 1e-3f)
      out.labelOffset = n * 0.05f; // a zero offset would re-seed on render
  } else if (std::abs(u.y) > std::abs(u.x)) {
    out.kind = sketch::ConstraintKind::DistanceX;
    if (pb.x < pa.x) { // the constraint is signed b.x - a.x: keep it positive
      std::swap(out.a, out.b);
      std::swap(pa, pb);
    }
    out.value = std::abs(span.x);
    // x stays 0 — the item-5 drag axis for DistanceX is vertical-only.
    out.labelOffset = {0.0f, cursor.y - std::min(pa.y, pb.y)};
  } else {
    out.kind = sketch::ConstraintKind::DistanceY;
    if (pb.y < pa.y) {
      std::swap(out.a, out.b);
      std::swap(pa, pb);
    }
    out.value = std::abs(span.y);
    out.labelOffset = {cursor.x - std::min(pa.x, pb.x), 0.0f};
  }
  return out;
}

// Label offset that puts a fixed-kind placement's label at (or nearest to)
// the cursor, per that kind's rendering geometry.
glm::vec2 FixedPlacementLabelOffset(const SketchFeature& sk,
                                    sketch::ConstraintKind kind,
                                    const sketch::PointRef& a,
                                    const sketch::PointRef& b,
                                    glm::vec2 cursor) {
  using K = sketch::ConstraintKind;
  if (kind == K::Radius || kind == K::Diameter) {
    const size_t ci = sk.IndexOfEntity(a.entity);
    if (ci == SIZE_MAX)
      return {0.7f, 0.7f};
    glm::vec2 center{0.0f};
    float radius = 0.0f;
    glm::vec2 dir(0.7071f, 0.7071f); // the renderer's circle leader anchor
    if (const auto* c = std::get_if<sketch::Circle>(&sk.entities[ci])) {
      center = c->center;
      radius = static_cast<float>(c->radius);
    } else if (const auto* arc = std::get_if<sketch::Arc>(&sk.entities[ci])) {
      center = arc->center;
      radius = static_cast<float>(arc->radius);
      const double midA = 0.5 * (arc->startAngle + arc->endAngle);
      dir = {static_cast<float>(std::cos(midA)),
             static_cast<float>(std::sin(midA))};
    }
    return cursor - center - dir * radius; // label lands exactly at cursor
  }
  if (kind == K::DistancePL) {
    // The label slides along the line's direction (the item-5 axis).
    const auto p = TargetUV(sk, a);
    const auto la = TargetUV(sk, {b.entity, 0});
    const auto dirO = LineDirOf(sk, b.entity);
    if (!p || !la || !dirO || glm::length(*dirO) < 1e-6f)
      return {0.0f, 0.8f};
    const glm::vec2 d = glm::normalize(*dirO);
    const glm::vec2 foot = *la + d * glm::dot(*p - *la, d);
    glm::vec2 off = d * glm::dot(cursor - 0.5f * (*p + foot), d);
    if (glm::length(off) < 1e-3f)
      off = d * 0.05f;
    return off;
  }
  return {0.7f, 0.7f};
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
    const glm::vec2 u = glm::normalize(*da), v = glm::normalize(*db);
    const float cross = u.x * v.y - u.y * v.x;
    // Keep the accumulation in sync so an Esc drops back to the two targets.
    edit.selected = {{lineId, 0}, {pick.ref.entity, 0}};
    edit.dimPickBody = {true, true};
    if (std::abs(cross) < 0.01f) {
      const auto p = TargetUV(sketch, {lineId, 0});
      const double dist =
          p ? PointToLineDistance(sketch, *p, pick.ref.entity) : 0.0;
      StartDimensionPlacement(state, /*pair=*/false, K::DistancePL, {lineId, 0},
                              {pick.ref.entity, 0}, dist);
    } else {
      edit.dimPlacing = false; // angle commits immediately (as ever)
      const double ang = std::atan2(std::abs(cross), glm::dot(u, v));
      sketch::PointRef refA{lineId, 0}, refB{pick.ref.entity, 0};
      if (cross < 0.0f)
        std::swap(refA, refB);
      CommitInferredDimension(state, K::Angle, ang, {refA, refB});
    }
    return true;
  }
  if (!pick.body) { // a point target → perpendicular point-line distance
    const auto p = TargetUV(sketch, pick.ref);
    const double dist = p ? PointToLineDistance(sketch, *p, lineId) : 0.0;
    edit.selected = {pick.ref, {lineId, 0}};
    edit.dimPickBody = {false, true};
    StartDimensionPlacement(state, /*pair=*/false, K::DistancePL, pick.ref,
                            {lineId, 0}, dist);
    return true;
  }
  return false; // circle/arc body: combos out of scope (deferred)
}

// The placement click: derive kind/order/value/label from the cursor and
// commit through the shared tail (which opens the inline editor).
void CommitPlacedDimension(AppState& state, const SketchFeature& sketch,
                           glm::vec2 cursor) {
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
  const glm::vec2 off = FixedPlacementLabelOffset(
      sketch, edit.dimPlaceKind, edit.dimPlaceA, edit.dimPlaceB, cursor);
  std::vector<sketch::PointRef> refs = {edit.dimPlaceA};
  if (edit.dimPlaceKind == sketch::ConstraintKind::DistancePL)
    refs.push_back(edit.dimPlaceB);
  CommitInferredDimension(state, edit.dimPlaceKind, edit.dimPlaceValue, refs,
                          &off);
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
    const glm::vec2 u = glm::normalize(*da), v = glm::normalize(*db);
    const float cross = u.x * v.y - u.y * v.x;
    if (std::abs(cross) < 0.01f) {
      // Parallel (within tolerance): the perpendicular distance, dimensioned
      // from an endpoint of the first line to the second (the SolidWorks
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
      if (cross < 0.0f)
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

  edit.selected.push_back(lineTarget ? sketch::PointRef{pick.ref.entity, 0}
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

// Project tool: click a body's edge or face to add it as LIVE external
// (projected) construction geometry — it tracks the source when it moves.
// Prefers an edge under the cursor, else the face. Returns true (the viewport
// click is consumed).
bool HandleProjectTool(AppState& state, SketchFeature& sketch, size_t index,
                       const Ray& ray) {
  SketchEditState& edit = state.sketchEdit;
  const EdgeHit edge = state.faceSelector->PickEdge(ray);
  const RayHit face = state.faceSelector->PickFace(ray);
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

} // namespace

// Commits a mirror of the selected entities across `axisEntity` (a line or
// datum axis id): solve, regenerate, one undo step, back to Select. Shared
// by the axis-phase click and mirror-on-activate seeding. Returns false
// (with an actionMsg) when the selection holds nothing besides the axis.
static bool CommitMirrorAcross(AppState& state, SketchFeature& sketch,
                               size_t index, uint32_t axisEntity) {
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

bool HandleSketchViewport(AppState& state, const glm::vec2& mousePos,
                          bool overViewport) {
  SketchEditState& edit = state.sketchEdit;

  // --- Plane picking -------------------------------------------------------
  if (edit.pickingPlane) {
    edit.hoveredPlane = -1;
    edit.hoveredDatumId = 0;
    if (overViewport) {
      Ray ray = ScreenRay(state, mousePos);
      // A model face wins whenever the ray hits one (faces are solid; you're
      // pointing at the body). The default/datum planes are only candidates
      // where no face is under the cursor — and only within their finite quad,
      // since the planes are infinite and would otherwise be "hit" everywhere.
      RayHit hit = state.faceSelector->PickFace(ray);
      int plane = -1;
      uint64_t datumId = 0;
      if (!hit.hit) {
        const float halfExtent = kDefaultPlaneHalfExtent;
        float bestDist = 1e30f;
        for (int i = 0; i < 3; i++) {
          SketchPlane p = DefaultPlane(i);
          if (auto uv = RayToPlaneUV(ray, p)) {
            if (std::abs(uv->x) > halfExtent || std::abs(uv->y) > halfExtent)
              continue; // ray misses the visible quad
            float dist = glm::length(p.PlaneToWorld(*uv) - ray.origin);
            if (dist < bestDist) {
              bestDist = dist;
              plane = i;
            }
          }
        }
        // Datum planes compete on the same nearest-hit basis as base planes.
        for (size_t i = 0; i < state.document->FeatureCount(); i++) {
          auto* dp =
              dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i));
          if (!dp)
            continue;
          if (auto uv = RayToPlaneUV(ray, dp->OutputPlane())) {
            if (std::abs(uv->x) > halfExtent || std::abs(uv->y) > halfExtent)
              continue;
            float dist =
                glm::length(dp->OutputPlane().PlaneToWorld(*uv) - ray.origin);
            if (dist < bestDist) {
              bestDist = dist;
              plane = -1; // a datum beat the base planes
              datumId = dp->GetId();
            }
          }
        }
      }
      edit.hoveredPlane = plane;
      edit.hoveredDatumId = datumId;

      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int n = static_cast<int>(state.document->FeatureCount()) + 1;
        const std::string name = "Sketch " + std::to_string(n);
        // Reattach mode: a named face re-points an existing sketch's faceRef.
        if (edit.reattachSketchId != 0) {
          if (hit.hit) {
            if (auto ref = FaceElementRef(state, hit.meshId, hit.faceIndex))
              ReattachSketchFace(state, edit.reattachSketchId, *ref);
          }
          edit.pickingPlane = false;
          edit.reattachSketchId = 0;
        } else if (hit.hit) {
          glm::vec3 c =
              state.faceSelector->GetFaceCenter(hit.meshId, hit.faceIndex);
          glm::vec3 nrm =
              state.faceSelector->GetFaceNormal(hit.meshId, hit.faceIndex);
          glm::vec3 u =
              std::abs(nrm.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
          u = glm::normalize(u - glm::dot(u, nrm) * nrm);
          const SketchPlane facePlane(c, nrm, u);
          // Attach to the face by its stable name when it has one; else bake.
          if (auto ref = FaceElementRef(state, hit.meshId, hit.faceIndex)) {
            BeginSketchOnFace(state, *ref, facePlane, name);
          } else {
            BeginSketchOnPlane(state, facePlane, name);
          }
        } else if (datumId != 0) {
          if (auto* dp = dynamic_cast<DatumPlaneFeature*>(
                  state.document->FindFeatureById(datumId)))
            BeginSketchOnDatum(state, *dp, name);
        } else if (plane >= 0) {
          BeginSketchOnPlane(state, DefaultPlane(plane), name);
        }
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      edit.pickingPlane = false;
    }
    return true;
  }

  if (!edit.IsEditing())
    return false;

  // --- Drawing on the active sketch ----------------------------------------
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return false;

  // Cleared each frame; re-set below only while hovering in the Select/Trim
  // tools or snapping in a draw tool.
  edit.hasHover = false;
  edit.hasTrimPreview = false;
  edit.hasCurrentSnap = false;

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    // Back out one level per press: close the inline dimension editor →
    // cancel an in-progress action → return to the Select tool → clear the
    // selection → leave sketch edit.
    if (edit.editingDim >= 0 || edit.dimOnCreateOpen) {
      // The inline editor / dimension-on-create field (drawn later this
      // frame) consumes it.
    } else if (edit.tool == SketchTool::Dimension && edit.dimPlacing) {
      edit.dimPlacing = false; // back to target accumulation (targets kept)
    } else if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
      edit.splinePoles.clear(); // cancel the in-progress spline
    } else if (edit.hasPendingPoint || edit.hasPendingPoint2 ||
               edit.hasPendingPoint3 || edit.hasPendingPoint4 ||
               edit.boxActive) {
      edit.hasPendingPoint = false;
      edit.hasPendingPoint2 = false;
      edit.hasPendingPoint3 = false;
      edit.hasPendingPoint4 = false;
      edit.boxActive = false;
    } else if (edit.tool == SketchTool::Mirror && edit.mirrorAxisPhase) {
      edit.mirrorAxisPhase = false; // axis-pick → back to collecting geometry
    } else if (edit.tool == SketchTool::Dimension && !edit.selected.empty()) {
      // Clear the accumulated targets first; a second Esc disarms the tool.
      edit.selected.clear();
      edit.dimPickBody.clear();
    } else if (edit.tool != SketchTool::Select) {
      edit.tool = SketchTool::Select;
      edit.mirrorAxisPhase = false;
      edit.actionMsg.clear();
    } else if (edit.selectedConstraint >= 0) {
      edit.selectedConstraint = -1; // deselect the constraint first
    } else if (!edit.selected.empty()) {
      edit.selected.clear();
    } else {
      edit = SketchEditState{}; // leave sketch edit
    }
    return true;
  }

  HandleSketchKeys(state); // Onshape sketch-context shortcuts
  // A key shortcut can apply a constraint, which REPLACES the sketch feature
  // (ModifyFeatureCommand swaps the object) — the pointer above is then a
  // dangling free. Re-resolve before any further use. (Latent use-after-free;
  // it started crashing once R3's regeneration allocations reused the freed
  // block.)
  sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return true; // the shortcut ended sketch edit; viewport was consumed

  if (!overViewport)
    return true;

  Ray ray = ScreenRay(state, mousePos);

  // Project tool picks MODEL geometry (a body edge/face), not a plane point.
  if (edit.tool == SketchTool::Project)
    return HandleProjectTool(state, *sketch, index, ray);

  auto uvOpt = RayToPlaneUV(ray, sketch->plane);
  if (!uvOpt)
    return true;
  glm::vec2 uv = *uvOpt;

  // Auto-constraint inference for the drawing tools: snap the cursor to nearby
  // geometry so the preview and the click both use the snapped point, and stash
  // the result for the ghost-glyph overlay. Ctrl or the toggle suppresses it.
  // Segment-level snaps (H/V/parallel/perp against the previous point) apply to
  // the Line tool and to Spline pole placement (relative to the last pole); the
  // other tools snap point-by-point.
  const bool drawTool =
      edit.tool == SketchTool::Point || edit.tool == SketchTool::Line ||
      edit.tool == SketchTool::Circle || edit.tool == SketchTool::Rectangle ||
      edit.tool == SketchTool::RectangleCenter ||
      edit.tool == SketchTool::Arc || edit.tool == SketchTool::ArcThreePoint ||
      edit.tool == SketchTool::Ellipse || edit.tool == SketchTool::EllipseArc ||
      edit.tool == SketchTool::Spline;
  const bool infer = drawTool && edit.autoConstrain && !ImGui::GetIO().KeyCtrl;
  if (infer) {
    const float ppu = PixelsPerUV(state, sketch->plane, uv);
    // The fixed "other end" for segment-level H/V/parallel/perp snapping: the
    // pending Line start, or the last placed Spline pole.
    std::optional<glm::vec2> other;
    if (edit.tool == SketchTool::Line && edit.hasPendingPoint)
      other = edit.pendingPoint;
    else if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty())
      other = edit.splinePoles.back();
    // While placing a spline, the poles dropped so far this session aren't in
    // the sketch yet, so InferPoint can't see them. Expose them as Point
    // snap-targets so a new pole can go coincident with / H/V-aligned to an
    // earlier pole (and snap onto the first pole to close the loop). Synthetic
    // ids in a reserved range below kDatumBase; never committed as refs (the
    // spline commits with inference off).
    sketch::Snap snap;
    if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
      std::vector<sketch::Entity> withPoles = sketch->entities;
      uint32_t synthId = 0xFFF00000u;
      for (const glm::vec2& pole : edit.splinePoles) {
        sketch::Point pt;
        pt.p = pole;
        pt.id = synthId++;
        withPoles.push_back(pt);
      }
      snap = sketch::InferPoint(withPoles, uv, other, 0, ppu);
    } else {
      snap = sketch::InferPoint(sketch->entities, uv, other, 0, ppu);
    }
    uv = snap.uv;
    edit.currentSnap = snap;
    edit.hasCurrentSnap = !snap.glyphs.empty();
  }
  edit.hoverPoint = uv;

  // A click-selected constraint (dimension pill / glyph) takes Delete
  // priority over entities — selection is mutually exclusive, so this is
  // unambiguous. Handled BEFORE the pill-hover gate below (the cursor is
  // usually still on the pill that was just clicked). Guarded against text
  // fields (Backspace edits text, not the sketch).
  if ((ImGui::IsKeyPressed(ImGuiKey_Delete) ||
       ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
      !ImGui::GetIO().WantTextInput && edit.selectedConstraint >= 0) {
    const size_t ci = static_cast<size_t>(edit.selectedConstraint);
    edit.selectedConstraint = -1;
    edit.editingDim = -1; // indices shift; don't leave the editor dangling
    DeleteConstraint(state, ci); // replaces the sketch feature
    return true;
  }

  // A dimension pill under the cursor (or mid-drag) owns the mouse —
  // DrawDimensions runs the drag/inline-edit; don't also draw/select under it.
  if (edit.dimHot >= 0 || edit.dimDrag >= 0)
    return true;

  if (edit.tool == SketchTool::Select) {
    // Track what the cursor is over for hover highlighting (endpoint-first,
    // then entity body).
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    edit.hoveredBody = hover.body;
    edit.hoveredRef = hover.ref;

    // A constraint glyph under the cursor owns clicks within its rect (it
    // may overlap an entity — the glyph wins there).
    if (edit.glyphHot >= 0) {
      edit.hasHover = false;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        edit.selectedConstraint = edit.glyphHot;
        edit.selected.clear(); // constraint vs entity Delete stays unambiguous
      }
      return true;
    }

    // Delete removes the selected entities (and cleans up their constraints).
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::GetIO().WantTextInput &&
        !edit.selected.empty()) {
      nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
      size_t dropped = 0;
      std::vector<uint32_t> ids;
      for (const auto& r : edit.selected)
        ids.push_back(r.entity);
      for (uint32_t id : ids)
        dropped += sketch->DeleteEntity(id);
      edit.selected.clear();
      state.sketchDrag = SketchDragSession{}; // cancel any drag
      sketch::Solver solver;
      solver.Load(*sketch);
      if (solver.Solve().ok)
        solver.WriteBack(sketch->entities);
      sketch->RegenerateDerived();
      state.document->Regenerate();
      nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
      if (after != before)
        state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *state.document);
      if (dropped > 0)
        LOG_INFO("Deleted {} dependent constraint(s)", dropped);
      return true;
    }
    // Click selects an element (for constraints) and begins a drag: an endpoint
    // moves that point, a line body translates the whole line, a circle rim
    // resizes it. Empty click clears the selection.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const glm::vec2 pos(mousePos.x, mousePos.y);
      const std::vector<SketchPick> candidates =
          PickSketchCandidates(state, *sketch, pos, kSketchBodyTolPx);
      const bool samePos = glm::length(pos - edit.lastClickPos) < 6.0f;
      edit.lastClickPos = pos;
      if (!candidates.empty()) {
        // Same-spot repeat cycles the stacked candidates (endpoint → line
        // body → …, the U1 model-space convention); a fresh spot takes the
        // best one — points outrank bodies.
        edit.pickCycle =
            samePos ? (edit.pickCycle + 1) % static_cast<int>(candidates.size())
                    : 0;
        const SketchPick pick = candidates[static_cast<size_t>(edit.pickCycle)];
        if (!ImGui::GetIO().KeyCtrl)
          edit.selected.clear();
        edit.selectedConstraint = -1; // entity selection replaces it
        edit.selected.push_back(pick.ref);
        BeginDragSession(state, *sketch, index, pick, uv);
      } else {
        edit.pickCycle = 0;
        // Missed geometry: start a rubber-band box (don't clear yet — a plain
        // click clears on release).
        edit.boxActive = true;
        edit.boxStart = edit.boxEnd = mousePos;
      }
    }
    if (edit.boxActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      edit.boxEnd = mousePos;
    if (edit.boxActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const bool add = ImGui::GetIO().KeyCtrl;
      if (glm::length(edit.boxEnd - edit.boxStart) < 4.0f) {
        if (!add) {
          edit.selected.clear(); // tiny box == a plain empty click
          edit.selectedConstraint = -1;
        }
      } else {
        const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
        auto project = [&](glm::vec2 p) {
          return SketchUVToScreen(state, sketch->plane, p);
        };
        auto ids = sketch::BoxSelect(sketch->entities, edit.boxStart,
                                     edit.boxEnd, crossing, project);
        if (!add)
          edit.selected.clear();
        for (uint32_t id : ids)
          edit.selected.push_back({id, 0});
      }
      edit.boxActive = false;
    }
    if (state.sketchDrag.active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      UpdateDragSession(state, *sketch, uv);
    if (state.sketchDrag.active &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left))
      EndDragSession(state, *sketch);
    return true;
  }

  if (edit.tool == SketchTool::Trim) {
    // Click a line segment to remove the piece under the cursor (bounded by
    // its crossings). Preview highlights just that sub-segment (not the whole
    // line) so it's clear what the click deletes.
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = false; // whole-entity highlight suppressed in Trim
    edit.hasTrimPreview = false;
    if (hover.hit) {
      if (auto span = sketch->TrimSpanAt(hover.ref.entity, uv)) {
        edit.hasTrimPreview = true;
        edit.trimPreviewA = span->first;
        edit.trimPreviewB = span->second;
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover.hit) {
      size_t eidx = sketch->IndexOfEntity(hover.ref.entity);
      const bool curve =
          eidx != SIZE_MAX &&
          (std::holds_alternative<sketch::Circle>(sketch->entities[eidx]) ||
           std::holds_alternative<sketch::Arc>(sketch->entities[eidx]));
      nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
      const bool changed = curve ? sketch->TrimCurveAt(hover.ref.entity, uv)
                                 : sketch->TrimLineAt(hover.ref.entity, uv);
      if (changed) {
        sketch::Solver solver;
        solver.Load(*sketch);
        if (solver.Solve().ok)
          solver.WriteBack(sketch->entities);
        sketch->RegenerateDerived();
        state.document->Regenerate();
        nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
        state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *state.document);
      }
    }
    return true;
  }

  // Load → Solve → WriteBack → Regenerate → one command, given the pre-edit
  // snapshot. Shared by the generative tools (mirror/fillet).
  auto commitMutation = [&](const nlohmann::json& before) {
    sketch::Solver solver;
    solver.Load(*sketch);
    if (solver.Solve().ok)
      solver.WriteBack(sketch->entities);
    sketch->RegenerateDerived();
    state.document->Regenerate();
    nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(index, before, after),
        *state.document);
  };

  if (edit.tool == SketchTool::Mirror) {
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit)
      edit.hoveredRef = hover.ref;

    if (!edit.mirrorAxisPhase) {
      // Step 1 — collect the geometry to mirror. A click toggles the whole
      // entity; a DRAG in empty space box-selects (adds); a TAP in empty
      // space — or Enter, or the session window's Axis row — advances to
      // the axis pick.
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hover.hit) {
          bool removed = false;
          for (size_t i = edit.selected.size(); i-- > 0;)
            if (edit.selected[i].entity == hover.ref.entity) {
              edit.selected.erase(edit.selected.begin() + i);
              removed = true;
            }
          if (!removed)
            edit.selected.push_back({hover.ref.entity, 0});
        } else {
          edit.boxActive = true;
          edit.boxStart = edit.boxEnd = mousePos;
        }
      }
      if (edit.boxActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        edit.boxEnd = mousePos;
      if (edit.boxActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (glm::length(edit.boxEnd - edit.boxStart) < 4.0f) {
          // Tap in empty space: continue to the axis step (Onshape-style).
          if (!edit.selected.empty())
            edit.mirrorAxisPhase = true;
        } else {
          const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
          auto project = [&](glm::vec2 p) {
            return SketchUVToScreen(state, sketch->plane, p);
          };
          for (uint32_t id :
               sketch::BoxSelect(sketch->entities, edit.boxStart, edit.boxEnd,
                                 crossing, project)) {
            bool seen = false;
            for (const auto& r : edit.selected)
              seen |= (r.entity == id);
            if (!seen)
              edit.selected.push_back({id, 0});
          }
        }
        edit.boxActive = false;
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !edit.selected.empty())
        edit.mirrorAxisPhase = true;
      return true;
    }

    // Step 2 — click a line (drawn, construction, or an X/Y datum axis).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover.hit) {
      const size_t ei = sketch->IndexOfEntity(hover.ref.entity);
      const bool isLine =
          sketch::IsDatum(hover.ref.entity) ||
          (ei != SIZE_MAX &&
           std::holds_alternative<sketch::Line>(sketch->entities[ei]));
      if (!isLine) {
        edit.actionMsg = "Mirror: the axis must be a line or an X/Y axis.";
      } else {
        CommitMirrorAcross(state, *sketch, index, hover.ref.entity);
      }
    }
    return true;
  }

  if (edit.tool == SketchTool::CalibrateImage) {
    // Capture two plane-UV points on the reference image (e.g. the two ends of
    // a printed dimension line). The Sketch panel then prompts for the real
    // distance between them and applies the scale (stage 2). Uses the raw plane
    // point — no entity snapping, since the target is the image, not geometry.
    if (edit.calibImageId == 0) {
      edit.actionMsg =
          "Select a reference image and press Calibrate in the Sketch panel.";
      return true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (edit.calibStage == 0) {
        edit.calibP1 = uv;
        edit.calibStage = 1;
      } else if (edit.calibStage == 1) {
        edit.calibP2 = uv;
        edit.calibStage = 2; // the panel now shows the distance field
      }
    }
    return true;
  }

  if (edit.tool == SketchTool::Fillet) {
    // Click a corner: fillet the two lines sharing the nearest endpoint.
    SketchPick vtx = PickSketchPoint(state, *sketch,
                                     glm::vec2(mousePos.x, mousePos.y), 16.0f);
    edit.hasHover = vtx.hit;
    if (vtx.hit)
      edit.hoveredRef = vtx.ref;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && vtx.hit) {
      const size_t vi = sketch->IndexOfEntity(vtx.ref.entity);
      if (vi != SIZE_MAX) {
        const glm::vec2 cornerUV =
            sketch::EntityPointUV(sketch->entities[vi], vtx.ref.which);
        std::vector<uint32_t> lines; // lines with an endpoint at the corner
        for (const auto& e : sketch->entities)
          if (auto* l = std::get_if<sketch::Line>(&e))
            if (glm::length(l->a - cornerUV) < 1e-3f ||
                glm::length(l->b - cornerUV) < 1e-3f)
              lines.push_back(sketch::EntityId(e));
        if (lines.size() >= 2) {
          nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
          if (sketch->FilletCorner(lines[0], lines[1], edit.filletRadius)) {
            commitMutation(before);
            edit.actionMsg.clear();
          } else {
            edit.actionMsg = "Fillet: radius " +
                             std::to_string(edit.filletRadius) +
                             " doesn't fit this corner — try a smaller radius.";
          }
        } else {
          edit.actionMsg = "Fillet: click a corner where two lines meet.";
        }
      }
    }
    return true;
  }

  if (edit.tool == SketchTool::Dimension) {
    if (edit.dimPlacing) {
      // Placement stage: the ghost (DrawDimensions) follows the cursor; a
      // click commits the kind + label position. Single-line length
      // placement first offers the click to refinement (B): landing on
      // another line/point switches the dimension kind instead of placing.
      // Highlight the pickable candidate under the cursor so the user sees
      // what a refine click would take (same hover as first-pick).
      if (edit.dimPlaceFromLine != 0) {
        SketchPick hover =
            PickSketchEntity(state, *sketch, glm::vec2(mousePos.x, mousePos.y),
                             kSketchBodyTolPx);
        // The measured line itself can't refine (it just places); don't
        // highlight it as a target.
        if (hover.hit && hover.ref.entity != edit.dimPlaceFromLine) {
          edit.hasHover = true;
          edit.hoveredBody = hover.body;
          edit.hoveredRef = hover.ref;
        }
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (edit.dimPlaceFromLine != 0) {
          const SketchPick pick = PickSketchEntity(
              state, *sketch, glm::vec2(mousePos.x, mousePos.y),
              kSketchBodyTolPx);
          if (pick.hit && RefineLinePlacement(state, *sketch, pick))
            return true;
        }
        // May replace the sketch feature — the local pointer dies here.
        CommitPlacedDimension(state, *sketch, uv);
      }
      return true;
    }
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit) {
      edit.hoveredBody = hover.body;
      edit.hoveredRef = hover.ref;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      // May replace the sketch feature through the command system — the
      // local pointer is dead after this call.
      HandleDimensionClick(state, *sketch, hover);
    }
    return true;
  }

  // Spline: commit the accumulated poles as a BSpline (>= 2 needed). May
  // replace the sketch feature, so the local pointer dies — return after.
  auto finishSpline = [&]() {
    if (edit.splinePoles.size() >= 2) {
      sketch::BSpline bs;
      bs.poles = edit.splinePoles;
      CommitEntity(state, *sketch, index, bs, false);
    }
    edit.splinePoles.clear();
  };
  if (edit.tool == SketchTool::Spline && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    finishSpline();
    return true;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (edit.tool == SketchTool::Spline) {
      // A double-click finishes; a single click adds another pole. On the
      // double-click frame the first click already placed its pole, so just
      // finish without appending a duplicate.
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        finishSpline();
      else
        edit.splinePoles.push_back(uv);
    } else if (edit.tool == SketchTool::Point) {
      CommitEntity(state, *sketch, index, sketch::Point{uv}, infer);
    } else if (edit.tool == SketchTool::Arc ||
               edit.tool == SketchTool::ArcThreePoint) {
      // Both arc tools are 3-click: two pending points, then commit on the
      // third. Point 1/2 semantics differ per tool (see ArcCenterStartEnd /
      // ArcFromThreePoints); the CCW-always Arc is normalized on build.
      if (!edit.hasPendingPoint) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else if (!edit.hasPendingPoint2) {
        edit.pendingPoint2 = uv;
        edit.hasPendingPoint2 = true;
      } else {
        std::optional<sketch::Arc> arc;
        if (edit.tool == SketchTool::Arc)
          arc = sketch::ArcCenterStartEnd(edit.pendingPoint, edit.pendingPoint2,
                                          uv);
        else
          arc = sketch::ArcFromThreePoints(edit.pendingPoint, uv,
                                           edit.pendingPoint2);
        if (arc)
          CommitEntity(state, *sketch, index, *arc, infer);
        edit.hasPendingPoint = false;
        edit.hasPendingPoint2 = false;
      }
    } else if (edit.tool == SketchTool::Ellipse) {
      // 3-click: centre → major-axis end → minor radius.
      if (!edit.hasPendingPoint) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else if (!edit.hasPendingPoint2) {
        edit.pendingPoint2 = uv;
        edit.hasPendingPoint2 = true;
      } else {
        sketch::Ellipse el = sketch::EllipseFromClicks(edit.pendingPoint,
                                                       edit.pendingPoint2, uv);
        if (el.rMajor > 1e-6 && el.rMinor > 1e-6)
          CommitEntity(state, *sketch, index, el, infer);
        edit.hasPendingPoint = false;
        edit.hasPendingPoint2 = false;
      }
    } else if (edit.tool == SketchTool::EllipseArc) {
      // 5-click: centre → major-axis end → minor radius → start → end. The
      // first three define the ellipse; the last two pick the swept params.
      if (!edit.hasPendingPoint) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else if (!edit.hasPendingPoint2) {
        edit.pendingPoint2 = uv;
        edit.hasPendingPoint2 = true;
      } else if (!edit.hasPendingPoint3) {
        edit.pendingPoint3 = uv;
        edit.hasPendingPoint3 = true;
      } else if (!edit.hasPendingPoint4) {
        edit.pendingPoint4 = uv;
        edit.hasPendingPoint4 = true;
      } else {
        const sketch::Ellipse el = sketch::EllipseFromClicks(
            edit.pendingPoint, edit.pendingPoint2, edit.pendingPoint3);
        if (el.rMajor > 1e-6 && el.rMinor > 1e-6) {
          double sp = sketch::EllipseParamOfPoint(
              el.center, el.rMajor, el.rMinor, el.rotation, edit.pendingPoint4);
          double ep = sketch::EllipseParamOfPoint(el.center, el.rMajor,
                                                  el.rMinor, el.rotation, uv);
          // CCW-always: keep the swept range (ep - sp) in (0, 2π].
          while (ep <= sp)
            ep += glm::two_pi<double>();
          CommitEntity(state, *sketch, index,
                       sketch::EllipseArc{el.center, el.rMajor, el.rMinor,
                                          el.rotation, sp, ep},
                       infer);
        }
        edit.hasPendingPoint = edit.hasPendingPoint2 = false;
        edit.hasPendingPoint3 = edit.hasPendingPoint4 = false;
      }
    } else if (!edit.hasPendingPoint) {
      edit.pendingPoint = uv;
      edit.hasPendingPoint = true;
    } else {
      const glm::vec2 a = edit.pendingPoint;
      switch (edit.tool) {
      case SketchTool::Line:
        CommitLine(state, *sketch, index, a, uv, infer);
        break;
      case SketchTool::Circle:
        CommitEntity(state, *sketch, index,
                     sketch::Circle{a, glm::length(uv - a)}, infer);
        break;
      case SketchTool::Rectangle:
        CommitRectangle(state, *sketch, index, a, uv, infer);
        break;
      case SketchTool::RectangleCenter:
        CommitCenterRectangle(state, *sketch, index, a, uv, infer);
        break;
      default:
        break;
      }
      // The line tool chains: the endpoint becomes the next segment's start so
      // continuous polylines can be drawn without reselecting the tool. Escape
      // or switching tools breaks the chain. Other tools finish after one span.
      if (edit.tool == SketchTool::Line) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else {
        edit.hasPendingPoint = false;
      }
    }
  }
  return true;
}

void BuildSketchOverlay(AppState& state, std::vector<LineVertex3D>& out) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  // Mirror preview: in the axis-pick step, ghost each selected source reflected
  // across the hovered line so the result is visible before committing.
  if (edit.tool == SketchTool::Mirror && edit.mirrorAxisPhase &&
      edit.hasHover) {
    const size_t ax = sketch->IndexOfEntity(edit.hoveredRef.entity);
    if (ax != SIZE_MAX) {
      if (auto* axl = std::get_if<sketch::Line>(&sketch->entities[ax])) {
        for (const auto& r : edit.selected) {
          if (r.entity == edit.hoveredRef.entity)
            continue;
          const size_t si = sketch->IndexOfEntity(r.entity);
          if (si == SIZE_MAX)
            continue;
          const sketch::Entity refl =
              sketch::ReflectEntity(sketch->entities[si], axl->a, axl->b);
          std::vector<glm::vec2> poly = sketch::TessellateEntity(refl);
          for (size_t i = 0; i + 1 < poly.size(); i++) {
            out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
            out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
          }
        }
      }
    }
    return; // no pending-point preview while picking a mirror axis
  }

  // Unique selected entity ids (used by the pattern/offset previews below).
  auto uniqueSources = [&]() {
    std::vector<uint32_t> ids;
    for (const auto& r : edit.selected) {
      bool seen = false;
      for (uint32_t s : ids)
        seen |= (s == r.entity);
      if (!seen)
        ids.push_back(r.entity);
    }
    return ids;
  };
  auto emitPoly = [&](const std::vector<glm::vec2>& poly) {
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
      out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
    }
  };

  // Pattern preview: ghost each generated instance for the current params.
  if (edit.tool == SketchTool::Pattern && !edit.selected.empty()) {
    for (uint32_t sid : uniqueSources()) {
      const size_t si = sketch->IndexOfEntity(sid);
      if (si == SIZE_MAX)
        continue;
      for (int k = 1; k < edit.patternCount; k++) {
        const sketch::Entity inst =
            edit.patternCircular
                ? sketch::RotateEntity(sketch->entities[si], edit.patternCenter,
                                       glm::radians(edit.patternAngleDeg) * k)
                : sketch::TranslateEntity(sketch->entities[si],
                                          static_cast<float>(k) *
                                              edit.patternStep);
        emitPoly(sketch::TessellateEntity(inst));
      }
    }
    return;
  }

  // Offset preview: ghost the offset of the selected closed loop.
  if (edit.tool == SketchTool::Offset && !edit.selected.empty()) {
    std::vector<sketch::Entity> chain;
    for (uint32_t sid : uniqueSources()) {
      const size_t si = sketch->IndexOfEntity(sid);
      if (si != SIZE_MAX)
        chain.push_back(sketch->entities[si]);
    }
    for (const auto& e : sketch::OffsetLoop(
             sketch->plane, chain, edit.offsetDistance, edit.offsetJoinArc))
      emitPoly(sketch::TessellateEntity(e));
    return;
  }

  // Spline preview: the curve through the placed poles plus the cursor (the
  // pole the next click would add), and the straight control polygon.
  if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
    sketch::BSpline bs;
    bs.poles = edit.splinePoles;
    bs.poles.push_back(edit.hoverPoint);
    if (bs.poles.size() >= 2)
      emitPoly(sketch::BSplineSampleUV(bs));
    emitPoly(bs.poles); // control polygon
    return;
  }

  if (!edit.hasPendingPoint)
    return;

  const glm::vec2 a = edit.pendingPoint;
  const glm::vec2 b = edit.hoverPoint;
  std::vector<glm::vec2> poly;
  switch (edit.tool) {
  case SketchTool::Line:
    poly = sketch::TessellateEntity(sketch::Line{a, b});
    break;
  case SketchTool::Circle:
    poly = sketch::TessellateEntity(sketch::Circle{a, glm::length(b - a)});
    break;
  case SketchTool::Rectangle:
    // Rectangles aren't an entity anymore; draw the outline directly.
    poly = {a, {b.x, a.y}, b, {a.x, b.y}, a};
    break;
  case SketchTool::RectangleCenter: {
    // Centre placed: ghost the rectangle mirrored about it, cursor = corner.
    const glm::vec2 c1 = 2.0f * a - b;
    poly = {c1, {b.x, c1.y}, b, {c1.x, b.y}, c1};
    break;
  }
  case SketchTool::Arc:
    if (!edit.hasPendingPoint2) // center placed: preview the radius ring
      poly = sketch::TessellateEntity(sketch::Circle{a, glm::length(b - a)});
    else // center + start placed: live arc to the cursor
      poly = sketch::TessellateEntity(
          sketch::ArcCenterStartEnd(a, edit.pendingPoint2, b));
    break;
  case SketchTool::ArcThreePoint:
    if (!edit.hasPendingPoint2) // start placed: preview the chord
      poly = {a, b};
    else if (auto arc = sketch::ArcFromThreePoints(a, b, edit.pendingPoint2))
      poly = sketch::TessellateEntity(*arc);
    else
      poly = {a, edit.pendingPoint2}; // collinear: fall back to the chord
    break;
  case SketchTool::Ellipse:
    if (!edit.hasPendingPoint2) // centre placed: preview the major axis
      poly = {a, b};
    else // major axis placed: live ellipse to the cursor's minor radius
      poly = sketch::TessellateEntity(
          sketch::EllipseFromClicks(a, edit.pendingPoint2, b));
    break;
  case SketchTool::EllipseArc:
    if (!edit.hasPendingPoint2) {
      poly = {a, b}; // centre placed: preview the major axis
    } else if (!edit.hasPendingPoint3) {
      poly = sketch::TessellateEntity( // major placed: live ellipse (minor)
          sketch::EllipseFromClicks(a, edit.pendingPoint2, b));
    } else {
      // Ellipse is fixed; show the whole ellipse until the start is picked,
      // then the live start→cursor arc.
      const sketch::Ellipse el =
          sketch::EllipseFromClicks(a, edit.pendingPoint2, edit.pendingPoint3);
      if (!edit.hasPendingPoint4) {
        poly = sketch::TessellateEntity(el);
      } else {
        double sp = sketch::EllipseParamOfPoint(
            el.center, el.rMajor, el.rMinor, el.rotation, edit.pendingPoint4);
        double ep = sketch::EllipseParamOfPoint(el.center, el.rMajor, el.rMinor,
                                                el.rotation, b);
        while (ep <= sp)
          ep += glm::two_pi<double>();
        poly = sketch::TessellateEntity(sketch::EllipseArc{
            el.center, el.rMajor, el.rMinor, el.rotation, sp, ep});
      }
    }
    break;
  default:
    return;
  }
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
  }
}

void DrawSelectionBox(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.boxActive)
    return;
  if (glm::length(edit.boxEnd - edit.boxStart) < 2.0f)
    return;
  const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImVec2 a(std::min(edit.boxStart.x, edit.boxEnd.x),
                 std::min(edit.boxStart.y, edit.boxEnd.y));
  const ImVec2 b(std::max(edit.boxStart.x, edit.boxEnd.x),
                 std::max(edit.boxStart.y, edit.boxEnd.y));
  const ImU32 edge = crossing ? IM_COL32(110, 210, 130, 230) // crossing: green
                              : IM_COL32(90, 160, 255, 230); // window: blue
  const ImU32 fill =
      crossing ? IM_COL32(110, 210, 130, 40) : IM_COL32(90, 160, 255, 40);
  dl->AddRectFilled(a, b, fill);
  if (crossing) { // dashed outline for crossing (CAD convention)
    const float dash = 6.0f;
    for (float x = a.x; x < b.x; x += 2 * dash) {
      dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + dash, b.x), a.y), edge);
      dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + dash, b.x), b.y), edge);
    }
    for (float y = a.y; y < b.y; y += 2 * dash) {
      dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + dash, b.y)), edge);
      dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + dash, b.y)), edge);
    }
  } else {
    dl->AddRect(a, b, edge);
  }
}

void DrawSnapGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.hasCurrentSnap)
    return;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;

  const glm::vec2 p =
      SketchUVToScreen(state, sketch->plane, edit.currentSnap.uv);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 accent = IM_COL32(255, 220, 120, 255);

  // Alignment guides: a dashed inference line from the shared vertex through
  // the snapped point, extended a little past it (Onshape-style).
  auto guide = [&](const std::optional<sketch::PointRef>& ref) {
    if (!ref)
      return;
    glm::vec2 targetUV;
    if (sketch::IsDatum(ref->entity)) {
      targetUV = sketch::DatumPointUV(ref->entity, ref->which);
    } else {
      const size_t idx = sketch->IndexOfEntity(ref->entity);
      if (idx == SIZE_MAX)
        return;
      targetUV = sketch::EntityPointUV(sketch->entities[idx], ref->which);
    }
    const glm::vec2 v = SketchUVToScreen(state, sketch->plane, targetUV);
    const glm::vec2 d = p - v;
    const float len = glm::length(d);
    if (len < 1e-3f)
      return;
    const glm::vec2 dir = d / len;
    const float full = len + 24.0f;
    const ImU32 col = IM_COL32(150, 190, 255, 200);
    for (float t = 0.0f; t < full; t += 12.0f) {
      const float t1 = std::min(t + 6.0f, full);
      dl->AddLine(ImVec2(v.x + dir.x * t, v.y + dir.y * t),
                  ImVec2(v.x + dir.x * t1, v.y + dir.y * t1), col, 1.5f);
    }
  };
  guide(edit.currentSnap.alignShareX);
  guide(edit.currentSnap.alignShareY);

  dl->AddCircle(ImVec2(p.x, p.y), 7.0f, accent, 0, 2.0f); // "grabbed" ring

  float gx = p.x + 12.0f, gy = p.y - 24.0f;
  for (sketch::GlyphKind k : edit.currentSnap.glyphs) {
    const char* label = "";
    switch (k) {
    case sketch::GlyphKind::Coincident:
      label = "coincident";
      break;
    case sketch::GlyphKind::PointOnLine:
      label = "on line";
      break;
    case sketch::GlyphKind::Horizontal:
      label = "H";
      break;
    case sketch::GlyphKind::Vertical:
      label = "V";
      break;
    case sketch::GlyphKind::Parallel:
      label = "parallel";
      break;
    case sketch::GlyphKind::Perpendicular:
      label = "perpendicular";
      break;
    }
    const ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddRectFilled(ImVec2(gx - 3, gy - 1),
                      ImVec2(gx + sz.x + 3, gy + sz.y + 1),
                      IM_COL32(25, 28, 34, 210), 3.0f);
    dl->AddText(ImVec2(gx, gy), accent, label);
    gy += sz.y + 4.0f;
  }
}

ToolPrompt SketchToolPrompt(const SketchEditState& edit) {
  using T = SketchTool;
  ToolPrompt p;
  auto set = [&](const char* name, int step, int count, const char* what) {
    p.step = step;
    p.stepCount = count;
    p.text = name;
    if (count > 0)
      p.text +=
          " (Step " + std::to_string(step) + "/" + std::to_string(count) + ")";
    p.text += std::string(": ") + what;
  };
  // For 3-click tools (arcs / ellipse), the current step from the pending
  // state.
  const int click3 = edit.hasPendingPoint2 ? 3 : (edit.hasPendingPoint ? 2 : 1);
  switch (edit.tool) {
  case T::Select:
    break; // default tool: no prompt
  case T::Point:
    set("Point", 0, 0, "click to place a point");
    break;
  case T::Line:
    set("Line", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click the next point (Esc to finish)"
                             : "click the start point");
    break;
  case T::Circle:
    set("Circle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click to set the radius" : "click the centre");
    break;
  case T::Rectangle:
    set("Rectangle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click the opposite corner"
                             : "click the first corner");
    break;
  case T::RectangleCenter:
    set("Center Rectangle", edit.hasPendingPoint ? 2 : 1, 2,
        edit.hasPendingPoint ? "click a corner" : "click the centre");
    break;
  case T::Arc:
    set("Arc", click3, 3,
        click3 == 1   ? "click the centre"
        : click3 == 2 ? "click the start point"
                      : "click the end point");
    break;
  case T::ArcThreePoint:
    set("3-Point Arc", click3, 3,
        click3 == 1   ? "click the start point"
        : click3 == 2 ? "click the end point"
                      : "click a point on the arc");
    break;
  case T::Ellipse:
    set("Ellipse", click3, 3,
        click3 == 1   ? "click the centre"
        : click3 == 2 ? "click the major-axis end"
                      : "click to set the minor radius");
    break;
  case T::Spline:
    set("Spline", 0, 0,
        edit.splinePoles.empty()
            ? "click to place control poles"
            : "click for more poles (Enter or double-click to finish)");
    break;
  case T::Trim:
    set("Trim", 0, 0, "click a segment to remove it");
    break;
  case T::Mirror:
    set("Mirror", edit.mirrorAxisPhase ? 2 : 1, 2,
        edit.mirrorAxisPhase
            ? "click a line to mirror across"
            : "click or box-select geometry (tap empty space to continue)");
    break;
  case T::Fillet:
    set("Fillet", 0, 0, "click a corner where two lines meet");
    break;
  case T::Pattern:
    set("Pattern", edit.selected.empty() ? 1 : 2, 2,
        edit.selected.empty() ? "select the geometry to pattern"
                              : "set count/spacing in the panel, then Apply");
    break;
  case T::Offset:
    set("Offset", edit.selected.empty() ? 1 : 2, 2,
        edit.selected.empty() ? "select a closed loop (its edges)"
                              : "set the distance in the panel, then Apply");
    break;
  case T::Dimension:
    if (edit.dimPlacing)
      set("Dimension", 3, 3,
          !edit.dimPlacePair ? "click to place the label"
          : edit.dimPlaceFromLine != 0
              ? "place the dimension (position picks horizontal/vertical/"
                "aligned), or pick a second target"
              : "place the dimension — position picks horizontal/vertical/"
                "aligned");
    else
      set("Dimension", edit.selected.empty() ? 1 : 2, 3,
          edit.selected.empty()
              ? "pick a point, line, or circle"
              : "pick a second target, or click empty space for length");
    break;
  case T::Project:
    set("Project", 0, 0, "click a body edge or face to project it");
    break;
  case T::CalibrateImage:
    set("Calibrate Image", edit.calibStage + 1, 3,
        edit.calibStage == 0 ? "click the first point on the image"
        : edit.calibStage == 1
            ? "click the second point (a known distance apart)"
            : "enter the real distance in the Sketch panel, then Apply");
    break;
  case T::EllipseArc: {
    const int st = edit.hasPendingPoint4   ? 5
                   : edit.hasPendingPoint3 ? 4
                   : edit.hasPendingPoint2 ? 3
                   : edit.hasPendingPoint  ? 2
                                           : 1;
    set("Elliptic Arc", st, 5,
        st == 1   ? "click the centre"
        : st == 2 ? "click the major-axis end"
        : st == 3 ? "click to set the minor radius"
        : st == 4 ? "click the arc's start point"
                  : "click the arc's end point");
    break;
  }
  }
  return p;
}

void DrawToolPrompt(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  const ToolPrompt tp = SketchToolPrompt(edit);
  float y = state.viewportPos.y + 34.0f; // clear of the docked panel tab bar
  // Constraint status first — always visible while sketching, the "am I
  // done constraining?" answer at a glance. One shared pill renderer with
  // the model-space prompt bar (U8b).
  if (edit.dof == 0)
    DrawPromptPill(state, y, "Fully defined", PromptPill::StatusGood);
  else if (edit.dof > 0)
    DrawPromptPill(state, y, std::to_string(edit.dof) + " degrees of freedom",
                   PromptPill::Status);
  DrawPromptPill(state, y, tp.text, PromptPill::Prompt);
  DrawPromptPill(state, y, edit.actionMsg, PromptPill::Warning);
}

namespace {
// UV position of a point ref (line endpoint / point / circle centre / datum).
std::optional<glm::vec2> RefUV(const SketchFeature& sk,
                               const sketch::PointRef& r) {
  if (sketch::IsDatum(r.entity))
    return sketch::DatumPointUV(r.entity, r.which);
  size_t idx = sk.IndexOfEntity(r.entity);
  if (idx == SIZE_MAX)
    return std::nullopt;
  return sketch::EntityPointUV(sk.entities[idx], r.which);
}

// Constrains a dragged dimension-label offset to the dimension's own axis
// (Phase 3.5) so the indicator stays aligned with what it measures: distance
// dims keep only the component that moves the dimension line AWAY from the
// span (the along-span slide shears the line past its extension lines);
// angle dims are handled radially at the drag site; radius/diameter leaders
// stay free (the leader legitimately follows the label anywhere).
glm::vec2 ConstrainLabelOffset(const SketchFeature& sk,
                               const sketch::Constraint& con, glm::vec2 off) {
  using K = sketch::ConstraintKind;
  switch (con.kind) {
  case K::Distance: {
    const auto a = RefUV(sk, con.a), b = RefUV(sk, con.b);
    if (!a || !b || glm::length(*b - *a) < 1e-6f)
      return off;
    const glm::vec2 d = glm::normalize(*b - *a);
    return off - d * glm::dot(off, d); // perpendicular-to-span only
  }
  case K::DistancePL: {
    // The measured span runs point → foot, perpendicular to the line; the
    // allowed offset is therefore along the LINE's direction.
    const auto la = RefUV(sk, {con.b.entity, 0});
    const auto lb = RefUV(sk, {con.b.entity, 1});
    if (!la || !lb || glm::length(*lb - *la) < 1e-6f)
      return off;
    const glm::vec2 d = glm::normalize(*lb - *la);
    return d * glm::dot(off, d);
  }
  case K::DistanceX:
    return {0.0f, off.y}; // the dimension line only moves vertically
  case K::DistanceY:
    return {off.x, 0.0f};
  default:
    return off;
  }
}

// A filled arrowhead at `tip` pointing along `dir` (screen space).
void Arrow(ImDrawList* dl, ImVec2 tip, glm::vec2 dir, ImU32 col) {
  glm::vec2 n{-dir.y, dir.x};
  const float len = 9.0f, w = 3.5f;
  ImVec2 base(tip.x - dir.x * len, tip.y - dir.y * len);
  dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * w, base.y + n.y * w),
                        ImVec2(base.x - n.x * w, base.y - n.y * w), col);
}

// Reflects a DistancePL constraint's point across its line, so the next
// solve's side pin (derived from geometry at Load) lands on the OPPOSITE
// side. Supports point / line-endpoint / circle-centre refs; arcs and datum
// refs return false (no flip).
bool ReflectPLPoint(SketchFeature& sk, const sketch::Constraint& con) {
  const auto p = RefUV(sk, con.a);
  const auto la = RefUV(sk, {con.b.entity, 0});
  const auto lb = RefUV(sk, {con.b.entity, 1});
  if (!p || !la || !lb)
    return false;
  const glm::vec2 dv = *lb - *la;
  const float len = glm::length(dv);
  if (len < 1e-6f)
    return false;
  const glm::vec2 d = dv / len;
  const glm::vec2 n(-d.y, d.x);
  const glm::vec2 target = *p - 2.0f * n * glm::dot(*p - *la, n);
  const size_t idx = sk.IndexOfEntity(con.a.entity);
  if (idx == SIZE_MAX)
    return false; // datum refs can't move
  sketch::Entity& e = sk.entities[idx];
  if (auto* pt = std::get_if<sketch::Point>(&e))
    pt->p = target;
  else if (auto* l = std::get_if<sketch::Line>(&e))
    (con.a.which != 0 ? l->b : l->a) = target;
  else if (auto* c = std::get_if<sketch::Circle>(&e))
    c->center = target;
  else
    return false; // arc points imply angles — flip unsupported there
  return true;
}

// Commits an edited dimension value: sets it (plus the optional driving
// expression, empty = literal), re-solves, one undo record.
//
// A NEGATIVE value flips the dimension's direction (the SolidWorks/Onshape
// convention) instead of being stored: signed kinds (DistanceX/Y, Angle)
// swap their refs for literals — the stored value stays positive — while a
// negative-EVALUATING expression keeps its raw signed value, because
// regeneration re-evaluates it (a ref swap would flip back every regen; the
// sign itself must carry the direction, and the displays show |v|).
// DistancePL's side pin re-derives from geometry each solve, so the flip
// seeds the solve on the other side by reflecting the point. Unsigned kinds
// (Distance, Radius, Diameter) have no direction: |v|, silently.
void ApplyDimensionEdit(AppState& state, SketchFeature& sketch, size_t index,
                        int dimIdx, double value,
                        const std::string& expr = {}) {
  if (dimIdx < 0 || dimIdx >= (int)sketch.constraints.size())
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch::Constraint& con = sketch.constraints[dimIdx];
  using K = sketch::ConstraintKind;
  std::vector<sketch::Entity> preSolve; // reflection seed rollback (below)
  bool reflected = false;
  if (value < 0.0) {
    switch (con.kind) {
    case K::DistanceX:
    case K::DistanceY:
    case K::Angle:
      if (expr.empty()) {
        std::swap(con.a, con.b); // the other side / the mirrored wedge
        value = -value;          // keep the positive-value invariant
      }
      break; // expression: raw signed value (see above)
    case K::DistancePL:
      preSolve = sketch.entities;
      reflected = ReflectPLPoint(sketch, con);
      if (expr.empty())
        value = -value; // GCS abs()es the magnitude; store it positive
      break;
    default:
      if (expr.empty())
        value = -value; // unsigned kinds: |v|, no direction to flip
      break; // expression: raw value; the solver clamps unsigned kinds
    }
  }
  con.value = value;
  con.valueExpr = expr;
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok) {
    solver.WriteBack(sketch.entities);
  } else if (reflected) {
    // The flip couldn't solve (over-constrained): drop the reflection seed
    // so the geometry isn't left at the hand-placed mirror position.
    sketch.entities = std::move(preSolve);
  }
  sketch.RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  // Already applied to the live object — record only. Execute would
  // ReplaceFeature with an identical copy, dangling the caller's pointer
  // (DrawDimensions keeps iterating this sketch's constraints).
  state.commands->PushApplied(
      std::make_unique<ModifyFeatureCommand>(index, before, after));
}
} // namespace

namespace {
glm::vec2 SeedLabelOffset(const SketchFeature& sk, const sketch::Constraint& c);
} // namespace

void DrawDimensions(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const ImU32 col = IM_COL32(120, 200, 255, 235);
  const ImU32 colHot = IM_COL32(255, 220, 130, 255);
  auto scr = [&](glm::vec2 uv) {
    glm::vec2 s = SketchUVToScreen(state, sketch->plane, uv);
    return ImVec2(s.x, s.y);
  };
  // Screen-pixel delta → sketch-UV delta around `at` (the plane can sit
  // rotated/foreshortened on screen): invert the local 2x2 screen basis.
  auto uvDelta = [&](const ImVec2& d, glm::vec2 at) -> glm::vec2 {
    const ImVec2 o = scr(at);
    const ImVec2 px = scr(at + glm::vec2(1.0f, 0.0f));
    const ImVec2 py = scr(at + glm::vec2(0.0f, 1.0f));
    const glm::vec2 bx(px.x - o.x, px.y - o.y), by(py.x - o.x, py.y - o.y);
    const float det = bx.x * by.y - bx.y * by.x;
    if (std::abs(det) < 1e-9f)
      return {0.0f, 0.0f};
    return {(d.x * by.y - d.y * by.x) / det, (bx.x * d.y - bx.y * d.x) / det};
  };

  edit.dimHot = -1; // refreshed below; the canvas reads it next frame
  if (edit.selectedConstraint >= (int)sketch->constraints.size())
    edit.selectedConstraint = -1; // the list changed under it (undo etc.)

  // Placement ghost (Phase 3.7): while the Dimension tool is placing, a
  // transient constraint rides the list for THIS draw only — rendered like
  // any dimension, popped before return, never serialized (StartPlacement
  // closes the inline editor, the only in-loop committer). Pill interaction
  // is disabled for every dim meanwhile so the placement click reaches the
  // viewport.
  bool ghostPushed = false;
  if (edit.dimPlacing) {
    std::optional<sketch::Constraint> ghost;
    if (edit.dimPlacePair) {
      if (const auto p = ClassifyPairPlacement(
              *sketch, edit.dimPlaceA, edit.dimPlaceB, edit.hoverPoint)) {
        sketch::Constraint g;
        g.kind = p->kind;
        g.a = p->a;
        g.b = p->b;
        g.value = p->value;
        g.labelOffset = p->labelOffset;
        ghost = g;
      }
    } else {
      sketch::Constraint g;
      g.kind = edit.dimPlaceKind;
      g.a = edit.dimPlaceA;
      g.b = edit.dimPlaceB;
      g.value = edit.dimPlaceValue;
      g.labelOffset =
          FixedPlacementLabelOffset(*sketch, g.kind, g.a, g.b, edit.hoverPoint);
      ghost = g;
    }
    if (ghost) {
      sketch->constraints.push_back(*ghost);
      ghostPushed = true;
    } else {
      edit.dimPlacing = false; // a target vanished (undo) — drop the stage
    }
  }

  for (int i = 0; i < (int)sketch->constraints.size(); i++) {
    sketch::Constraint& con = sketch->constraints[i]; // drag writes labelOffset
    if (!sketch::IsDimension(con.kind))
      continue;

    char label[48] = "";
    ImVec2 labelPos;
    // The offset actually drawn: the stored one, or — while it is still
    // zero (files from before offsets were written) — a per-kind legible
    // default. A drag materializes the drawn offset into the constraint
    // first, so the label never jumps when grabbed.
    glm::vec2 effOff = con.labelOffset;
    using K = sketch::ConstraintKind;

    if (con.kind == K::Radius || con.kind == K::Diameter) {
      size_t ci = sketch->IndexOfEntity(con.a.entity);
      if (ci == SIZE_MAX)
        continue;
      const auto& ent = sketch->entities[ci];
      glm::vec2 center;
      float radius;
      glm::vec2 dir(0.7071f, 0.7071f); // circle: 45° leader
      if (auto* circ = std::get_if<sketch::Circle>(&ent)) {
        center = circ->center;
        radius = static_cast<float>(circ->radius);
      } else if (auto* arc = std::get_if<sketch::Arc>(&ent)) {
        center = arc->center;
        radius = static_cast<float>(arc->radius);
        double mid =
            0.5 * (arc->startAngle + arc->endAngle); // within the sweep
        dir = {static_cast<float>(std::cos(mid)),
               static_cast<float>(std::sin(mid))};
      } else {
        continue;
      }
      if (glm::length(effOff) < 1e-4f)
        effOff = SeedLabelOffset(*sketch, con);
      // The label sits at rim + offset; the rim anchor follows the label
      // around the circle so the leader is always radial, with the arrowhead
      // touching the rim from the label's side.
      glm::vec2 labelUV = center + dir * radius + effOff;
      const glm::vec2 fromCenter = labelUV - center;
      const glm::vec2 rdir =
          glm::length(fromCenter) > 1e-5f ? glm::normalize(fromCenter) : dir;
      const glm::vec2 rimUV = center + rdir * radius;
      const ImVec2 rimS = scr(rimUV), labS = scr(labelUV);
      dl->AddLine(rimS, labS, col, 1.4f);
      const glm::vec2 lead(rimS.x - labS.x, rimS.y - labS.y);
      if (glm::length(lead) > 1e-3f)
        Arrow(dl, rimS, glm::normalize(lead), col);
      // |value|: a negative-evaluating expr means a flip, never a "-R".
      snprintf(label, sizeof(label),
               con.kind == K::Diameter ? "\xC3\x98%.3g" : "R%.3g",
               std::abs(con.value));
      labelPos = labS;
    } else if (con.kind == K::Angle) {
      snprintf(label, sizeof(label), "%.1f\xC2\xB0",
               std::abs(con.value) * 57.2957795f);
      const auto a0 = RefUV(*sketch, {con.a.entity, 0});
      const auto a1 = RefUV(*sketch, {con.a.entity, 1});
      const auto b0 = RefUV(*sketch, {con.b.entity, 0});
      const auto b1 = RefUV(*sketch, {con.b.entity, 1});
      if (!a0 || !a1 || !b0 || !b1)
        continue;
      const glm::vec2 da = *a1 - *a0, db = *b1 - *b0;
      const float cross = da.x * db.y - da.y * db.x;
      if (std::abs(cross) < 1e-9f || glm::length(da) < 1e-6f ||
          glm::length(db) < 1e-6f) {
        // (Near-)parallel lines: no usable vertex — plain label fallback.
        labelPos = scr(*a0 + effOff + glm::vec2(0.3f, 0.3f));
      } else {
        // Vertex = the infinite lines' intersection.
        const float t =
            ((b0->x - a0->x) * db.y - (b0->y - a0->y) * db.x) / cross;
        const glm::vec2 inter = *a0 + da * t;
        if (glm::length(effOff) < 1e-4f)
          effOff = SeedLabelOffset(*sketch, con);
        const float rUV = std::max(glm::length(effOff), 0.35f);
        // Pick the direction signs whose wedge best matches the stored
        // angle, preferring the side the label offset points to — dragging
        // the label across the vertex flips the measured wedge, the way
        // other CAD flips dimension quadrants.
        const glm::vec2 offDir = glm::normalize(effOff);
        glm::vec2 u = glm::normalize(da), v = glm::normalize(db);
        float bestScore = 1e30f;
        glm::vec2 bu = u, bv = v;
        for (int su = -1; su <= 1; su += 2)
          for (int sv = -1; sv <= 1; sv += 2) {
            const glm::vec2 cu = u * (float)su, cv = v * (float)sv;
            const float ang =
                std::acos(std::clamp(glm::dot(cu, cv), -1.0f, 1.0f));
            const glm::vec2 mid = cu + cv;
            const float midDot = glm::length(mid) > 1e-4f
                                     ? glm::dot(glm::normalize(mid), offDir)
                                     : 0.0f;
            const float score =
                std::abs(ang - std::abs((float)con.value)) * 4.0f - midDot;
            if (score < bestScore) {
              bestScore = score;
              bu = cu;
              bv = cv;
            }
          }
        // Sweep from bu to bv the short way; sample the arc in UV so the
        // curve stays correct under the plane's screen projection.
        const float aStart = std::atan2(bu.y, bu.x);
        float sweep = std::atan2(bv.y, bv.x) - aStart;
        while (sweep > glm::pi<float>())
          sweep -= 2.0f * glm::pi<float>();
        while (sweep < -glm::pi<float>())
          sweep += 2.0f * glm::pi<float>();
        const int kSegs = 24;
        ImVec2 prev = scr(inter + bu * rUV);
        for (int s = 1; s <= kSegs; s++) {
          const float ang = aStart + sweep * (float)s / kSegs;
          const ImVec2 p =
              scr(inter + glm::vec2(std::cos(ang), std::sin(ang)) * rUV);
          dl->AddLine(prev, p, col, 1.4f);
          prev = p;
        }
        // Arrowheads at the arc ends, tangent to the arc and pointing away
        // from its body; thin extension rays from the vertex so short lines
        // still meet their dimension.
        const float sgn = sweep >= 0.0f ? 1.0f : -1.0f;
        const auto arcArrow = [&](float ang, float outSign) {
          const glm::vec2 p =
              inter + glm::vec2(std::cos(ang), std::sin(ang)) * rUV;
          const glm::vec2 tUV =
              glm::vec2(-std::sin(ang), std::cos(ang)) * sgn * outSign;
          const ImVec2 ps = scr(p), qs = scr(p + tUV * 0.1f);
          const glm::vec2 d(qs.x - ps.x, qs.y - ps.y);
          if (glm::length(d) > 1e-3f)
            Arrow(dl, ps, glm::normalize(d), col);
        };
        arcArrow(aStart, -1.0f);
        arcArrow(aStart + sweep, 1.0f);
        dl->AddLine(scr(inter), scr(inter + bu * rUV * 1.08f), col, 0.8f);
        dl->AddLine(scr(inter), scr(inter + bv * rUV * 1.08f), col, 0.8f);
        // Label on the arc's midpoint.
        const float aMid = aStart + sweep * 0.5f;
        labelPos = scr(inter + glm::vec2(std::cos(aMid), std::sin(aMid)) * rUV);
      }
    } else { // Distance / DistanceX / DistanceY / DistancePL (points a, b)
      auto a = RefUV(*sketch, con.a), b = RefUV(*sketch, con.b);
      if (con.kind == K::DistancePL && a) {
        // Renders like a distance dim between the point and its FOOT on the
        // line (the perpendicular projection onto the infinite line).
        const auto la = RefUV(*sketch, {con.b.entity, 0});
        const auto lb = RefUV(*sketch, {con.b.entity, 1});
        b = std::nullopt;
        if (la && lb) {
          const glm::vec2 d = *lb - *la;
          const float len2 = glm::dot(d, d);
          if (len2 > 1e-12f)
            b = *la + d * (glm::dot(*a - *la, d) / len2);
        }
      }
      if (!a || !b)
        continue;
      if (glm::length(effOff) < 1e-4f)
        effOff = SeedLabelOffset(*sketch, con);
      glm::vec2 da = *a + effOff, db = *b + effOff;
      if (con.kind == K::DistanceX)
        da.y = db.y = std::min(a->y, b->y) + effOff.y;
      else if (con.kind == K::DistanceY)
        da.x = db.x = std::min(a->x, b->x) + effOff.x;
      ImVec2 sa = scr(*a), sb = scr(*b), sda = scr(da), sdb = scr(db);
      // Extension lines overshoot the dimension line a touch (drafting
      // convention); they start just off the measured points.
      const auto extend = [&](ImVec2 from, ImVec2 to) {
        glm::vec2 d(to.x - from.x, to.y - from.y);
        const float len = glm::length(d);
        if (len < 1e-3f)
          return to;
        d /= len;
        return ImVec2(to.x + d.x * 4.0f, to.y + d.y * 4.0f);
      };
      dl->AddLine(sa, extend(sa, sda), col, 1.0f);
      dl->AddLine(sb, extend(sb, sdb), col, 1.0f);
      dl->AddLine(sda, sdb, col, 1.4f); // dimension line
      double v = con.kind == K::DistanceX   ? std::abs(b->x - a->x)
                 : con.kind == K::DistanceY ? std::abs(b->y - a->y)
                                            : glm::length(*b - *a);
      snprintf(label, sizeof(label), "%.3g", v);
      glm::vec2 d{sdb.x - sda.x, sdb.y - sda.y};
      float L = std::sqrt(d.x * d.x + d.y * d.y);
      if (L > 1e-3f) {
        d /= L;
        // Arrows inside when the value + heads fit between the extension
        // lines; flipped outside (with short tails) when the span is tight.
        const float need = ImGui::CalcTextSize(label).x + 2.0f * 9.0f + 14.0f;
        if (L >= need) {
          Arrow(dl, sda, d, col);
          Arrow(dl, sdb, {-d.x, -d.y}, col);
        } else {
          Arrow(dl, sda, {-d.x, -d.y}, col);
          Arrow(dl, sdb, d, col);
          dl->AddLine(sda, ImVec2(sda.x - d.x * 16.0f, sda.y - d.y * 16.0f),
                      col, 1.4f);
          dl->AddLine(sdb, ImVec2(sdb.x + d.x * 16.0f, sdb.y + d.y * 16.0f),
                      col, 1.4f);
        }
      }
      labelPos = ImVec2((sda.x + sdb.x) * 0.5f, (sda.y + sdb.y) * 0.5f);
    }

    // Value pill: rounded, highlighted under the cursor — the affordance for
    // dragging the label into place and double-click editing the value.
    // Expression-driven dims show the formula next to the value ("h/2 = 11")
    // so a driving expression is visible at a glance.
    std::string shown = label;
    if (!con.valueExpr.empty())
      shown = con.valueExpr + " = " + shown;
    const ImVec2 tsz = ImGui::CalcTextSize(shown.c_str());
    ImVec2 tp(labelPos.x - tsz.x * 0.5f, labelPos.y - tsz.y * 0.5f);
    const ImVec2 pmin(tp.x - 5.0f, tp.y - 2.0f);
    const ImVec2 pmax(tp.x + tsz.x + 5.0f, tp.y + tsz.y + 2.0f);
    const bool overLabel = io.MousePos.x >= pmin.x && io.MousePos.x <= pmax.x &&
                           io.MousePos.y >= pmin.y && io.MousePos.y <= pmax.y;
    // Pills are inert during placement — the ghost must not latch dimHot
    // (which would eat the placement click) and real dims can't drag.
    const bool interactive = !edit.dimPlacing;
    const bool dragging = edit.dimDrag == i;
    if (interactive && (overLabel || dragging))
      edit.dimHot = i;
    const bool hot = interactive && (overLabel || dragging);
    // Selected pill: accent fill + a heavier ring, distinct from hover.
    const bool selCon = edit.selectedConstraint == i;
    const float rounding = (pmax.y - pmin.y) * 0.5f;
    dl->AddRectFilled(pmin, pmax,
                      selCon ? IM_COL32(84, 66, 26, 250)
                      : hot  ? IM_COL32(45, 62, 82, 245)
                             : IM_COL32(20, 24, 30, 220),
                      rounding);
    dl->AddRect(pmin, pmax,
                (hot || selCon) ? colHot : IM_COL32(96, 150, 200, 150),
                rounding, 0, selCon ? 2.2f : 1.2f);
    dl->AddText(tp, (hot || selCon) ? IM_COL32(255, 240, 200, 255) : col,
                shown.c_str());

    // Drag places the label (writes the serialized labelOffset; one undo
    // record on release). The pre-drag json lives across frames.
    static nlohmann::json s_dragBefore;
    static bool s_dragMoved = false;
    if (interactive && overLabel && edit.editingDim != i && edit.dimDrag < 0 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      edit.dimDrag = i;
      s_dragBefore = DocumentIO::FeatureToJson(*sketch);
      s_dragMoved = false;
      con.labelOffset = effOff; // materialize a seeded default before moving
    }
    if (dragging) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (std::abs(io.MouseDelta.x) + std::abs(io.MouseDelta.y) > 0.0f) {
          const glm::vec2 prev = con.labelOffset;
          glm::vec2 next = prev + uvDelta(io.MouseDelta, glm::vec2(0.0f));
          if (con.kind == K::Angle) {
            // Radial only: the label slides along its current direction
            // (changing the arc radius), never around the vertex.
            const float len = glm::length(prev);
            if (len > 1e-4f) {
              const glm::vec2 dir = prev / len;
              next = dir * std::max(glm::dot(next, dir), 0.15f);
            }
          } else {
            next = ConstrainLabelOffset(*sketch, con, next);
          }
          con.labelOffset = next;
          s_dragMoved = true;
        }
      } else {
        edit.dimDrag = -1;
        if (s_dragMoved) {
          const nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
          if (after != s_dragBefore)
            state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
                index, s_dragBefore, after));
        } else {
          // Released without moving: the click SELECTS the dimension
          // (Delete/Backspace removes it); entity selection clears so
          // Delete stays unambiguous.
          edit.selectedConstraint = i;
          edit.selected.clear();
        }
      }
    }

    if (interactive && overLabel &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      edit.editingDim = i;
      edit.dimDrag = -1; // the second press began a drag; editing wins
    }

    if (edit.editingDim == i) {
      static char buf[128];
      static int lastEditing = -1;
      static std::string editError;
      if (lastEditing != i) {
        // Seed with the driving expression when there is one, else the value
        // (degrees in the UI for angles, as always).
        if (!con.valueExpr.empty())
          snprintf(buf, sizeof(buf), "%s", con.valueExpr.c_str());
        else if (con.kind == K::Angle)
          snprintf(buf, sizeof(buf), "%.4g", glm::degrees(con.value));
        else
          snprintf(buf, sizeof(buf), "%.4g", con.value);
        lastEditing = i;
        editError.clear();
      }
      ImGui::SetNextWindowPos(ImVec2(tp.x - 4, tp.y - 4));
      ImGui::SetNextWindowBgAlpha(0.95f);
      ImGui::Begin("##dimedit", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings);
      ImGui::SetNextItemWidth(120);
      ImGui::SetKeyboardFocusHere();
      bool commit = ImGui::InputText("##v", buf, sizeof(buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
      if (!editError.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           editError.c_str());
      ImGui::End();
      if (commit) {
        // A plain number stays a literal; anything else is an expression
        // over the document's variables (angles speak degrees, stored as
        // radians). Invalid expressions keep the editor open with the error.
        char* end = nullptr;
        double v = std::strtod(buf, &end);
        while (end != nullptr && *end == ' ')
          end++;
        const bool isNumber = end != buf && end != nullptr && *end == '\0';
        if (isNumber) {
          if (con.kind == K::Angle)
            v = glm::radians(v);
          ApplyDimensionEdit(state, *sketch, index, i, v);
        } else {
          const auto evaluated = ExpressionEngine::Default().EvaluateText(
              buf, state.document->Symbols());
          if (!evaluated.has_value()) {
            editError = evaluated.error().message;
            commit = false; // stay open, show the error
          } else {
            v = con.kind == K::Angle ? glm::radians(*evaluated) : *evaluated;
            ApplyDimensionEdit(state, *sketch, index, i, v, buf);
          }
        }
        if (commit) {
          edit.editingDim = -1;
          lastEditing = -1;
        }
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        edit.editingDim = -1;
        lastEditing = -1;
      }
    }
  }

  if (ghostPushed)
    sketch->constraints.pop_back(); // the placement ghost was render-only
}

void DrawDimensionOnCreate(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing())
    return;
  using T = SketchTool;
  const bool drawTool = edit.tool == T::Line || edit.tool == T::Circle ||
                        edit.tool == T::Rectangle ||
                        edit.tool == T::RectangleCenter ||
                        edit.tool == T::Arc || edit.tool == T::ArcThreePoint;
  if (!drawTool || edit.lastDrawn.empty()) {
    edit.dimOnCreateOpen = false;
    return;
  }
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;
  for (uint32_t id : edit.lastDrawn) {
    if (sketch->IndexOfEntity(id) == SIZE_MAX) { // undone or deleted
      edit.lastDrawn.clear();
      edit.dimOnCreateOpen = false;
      return;
    }
  }

  // What the next typed number dimensions.
  const bool rect = edit.lastDrawn.size() == 4;
  if (rect && edit.lastDrawnStage >= 2) {
    edit.dimOnCreateOpen = false; // width and height both applied
    return;
  }
  sketch::ConstraintKind kind = sketch::ConstraintKind::Distance;
  std::vector<sketch::PointRef> refs;
  const char* what = "Length";
  if (rect) {
    // lastDrawn = {bottom, right, top, left}: width first, then height.
    const uint32_t side = edit.lastDrawn[edit.lastDrawnStage == 0 ? 0 : 1];
    refs = {{side, 0}, {side, 1}};
    what = edit.lastDrawnStage == 0 ? "Width" : "Height";
  } else {
    const uint32_t id = edit.lastDrawn[0];
    const size_t ei = sketch->IndexOfEntity(id);
    if (std::holds_alternative<sketch::Line>(sketch->entities[ei])) {
      refs = {{id, 0}, {id, 1}};
    } else { // circle / arc (the only other kinds recorded)
      kind = sketch::ConstraintKind::Radius;
      refs = {{id, 0}};
      what = "Radius";
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  static char buf[64];
  static ImVec2 s_pos;
  static std::string editError;
  if (!edit.dimOnCreateOpen) {
    if (io.WantTextInput)
      return; // another field (e.g. the inline dim editor) owns typing
    // A typed digit (or '.' / '-') opens the field, seeded with it.
    std::string seed;
    for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
      const ImWchar c = io.InputQueueCharacters[i];
      if ((c >= '0' && c <= '9') || c == '.' || c == '-')
        seed += static_cast<char>(c);
    }
    if (seed.empty())
      return;
    snprintf(buf, sizeof(buf), "%s", seed.c_str());
    s_pos = ImVec2(io.MousePos.x + 14.0f, io.MousePos.y + 14.0f);
    editError.clear();
    edit.dimOnCreateOpen = true;
    return; // draw + focus from the next frame
  }

  ImGui::SetNextWindowPos(s_pos);
  ImGui::SetNextWindowBgAlpha(0.95f);
  ImGui::Begin("##dimoncreate", nullptr,
               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                   ImGuiWindowFlags_NoSavedSettings);
  ImGui::TextUnformatted(what);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(110);
  ImGui::SetKeyboardFocusHere();
  bool commit = ImGui::InputText("##docv", buf, sizeof(buf),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
  if (!editError.empty())
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       editError.c_str());
  ImGui::End();

  if (commit) {
    // A plain number stays a literal; anything else is an expression over
    // the document's variables (the inline dim editor's convention). The
    // kinds created here (Distance, Radius) are unsigned — a negative
    // input silently means its magnitude (the flip convention has no
    // direction to act on); negative-evaluating exprs keep their raw value
    // (the solver clamps unsigned kinds).
    char* end = nullptr;
    double v = std::strtod(buf, &end);
    while (end != nullptr && *end == ' ')
      end++;
    const bool isNumber = end != buf && end != nullptr && *end == '\0';
    if (isNumber)
      v = std::abs(v);
    std::string expr;
    if (!isNumber) {
      const auto evaluated = ExpressionEngine::Default().EvaluateText(
          buf, state.document->Symbols());
      if (!evaluated.has_value()) {
        editError = evaluated.error().message;
        commit = false; // stay open, show the error
      } else {
        v = *evaluated;
        expr = buf;
      }
    }
    if (commit) {
      edit.dimOnCreateOpen = false;
      editError.clear();
      edit.selected = refs;
      // Replaces the sketch feature through the command system — the local
      // pointer is dead after this call.
      ApplySketchConstraint(state, kind, v, expr);
      if (rect)
        edit.lastDrawnStage++; // width applied → height next (then done)
      else
        edit.lastDrawn.clear(); // one dimension per entity
    }
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    // Dismiss without a dimension; the draw tool's staged state is untouched
    // (the viewport Escape cascade defers to this field while it is open).
    edit.dimOnCreateOpen = false;
    editError.clear();
  }
}

void DrawConstraintGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  edit.glyphHot = -1; // refreshed below; the canvas reads it next frame
  if (!edit.IsEditing() || !edit.showConstraintGlyphs)
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImGuiIO& io = ImGui::GetIO();
  const ImU32 col = IM_COL32(150, 210, 170, 220);
  const ImU32 colSel = IM_COL32(255, 220, 130, 255);
  using K = sketch::ConstraintKind;

  // Fan out multiple badges on the same entity so they don't overlap. A
  // badge hit-tests its rect: hovering latches glyphHot (the canvas selects
  // on click, one frame behind — the dimHot convention), the selected
  // constraint's badges show the accent.
  std::map<uint32_t, int> stack;
  auto badge = [&](int ci, uint32_t entity, const char* text) {
    auto uv = RefUV(*sketch, {entity, 0});
    if (!uv)
      return;
    glm::vec2 s = SketchUVToScreen(state, sketch->plane, *uv);
    float dx = 10.0f + 13.0f * stack[entity]++;
    ImVec2 p(s.x + dx, s.y - 16.0f);
    ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 rmin(p.x - 2, p.y - 1);
    const ImVec2 rmax(p.x + sz.x + 2, p.y + sz.y + 1);
    const bool over = io.MousePos.x >= rmin.x && io.MousePos.x <= rmax.x &&
                      io.MousePos.y >= rmin.y && io.MousePos.y <= rmax.y;
    if (over)
      edit.glyphHot = ci;
    const bool selCon = edit.selectedConstraint == ci;
    dl->AddRectFilled(rmin, rmax,
                      selCon ? IM_COL32(84, 66, 26, 250)
                      : over ? IM_COL32(45, 62, 52, 235)
                             : IM_COL32(25, 32, 28, 200),
                      2.0f);
    if (selCon)
      dl->AddRect(rmin, rmax, colSel, 2.0f);
    dl->AddText(p, selCon ? colSel : col, text);
  };

  for (int i = 0; i < (int)sketch->constraints.size(); i++) {
    const auto& c = sketch->constraints[i];
    switch (c.kind) {
    case K::Horizontal:
      badge(i, c.a.entity, "H");
      break;
    case K::Vertical:
      badge(i, c.a.entity, "V");
      break;
    case K::Parallel:
      badge(i, c.a.entity, "//");
      badge(i, c.b.entity, "//");
      break;
    case K::Perpendicular:
      badge(i, c.a.entity, "|_");
      break;
    case K::Equal:
      badge(i, c.a.entity, "=");
      badge(i, c.b.entity, "=");
      break;
    case K::Coincident:
      badge(i, c.a.entity, "co");
      break;
    case K::PointOnLine:
      badge(i, c.b.entity, "on");
      break;
    case K::Tangent:
      badge(i, c.a.entity, "T");
      break;
    case K::Symmetric:
      badge(i, c.c.entity, "sym");
      break;
    case K::Midpoint:
      badge(i, c.b.entity, "mid");
      break;
    case K::Fixed:
      badge(i, c.a.entity, "fix");
      break;
    case K::Concentric:
      badge(i, c.a.entity, "con");
      break;
    case K::Collinear:
      badge(i, c.a.entity, "L");
      badge(i, c.b.entity, "L");
      break;
    default:
      break; // dimensions render via DrawDimensions
    }
  }
}

glm::vec2 WorldToScreen(AppState& state, const glm::vec3& world) {
  glm::vec4 clip = state.camera->GetProjectionMatrix() *
                   state.camera->GetViewMatrix() * glm::vec4(world, 1.0f);
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return state.viewportPos +
         glm::vec2((ndc.x + 1.0f) * 0.5f * state.viewportSize.x,
                   (ndc.y + 1.0f) * 0.5f * state.viewportSize.y);
}

glm::vec2 SketchUVToScreen(AppState& state, const SketchPlane& plane,
                           glm::vec2 uv) {
  return WorldToScreen(state, plane.PlaneToWorld(uv));
}

namespace {
// Endpoints exposed by an entity for picking/constraints.
std::vector<std::pair<uint8_t, glm::vec2>>
EntityPoints(const sketch::Entity& e) {
  std::vector<std::pair<uint8_t, glm::vec2>> pts;
  if (auto* p = std::get_if<sketch::Point>(&e)) {
    pts = {{0, p->p}};
  } else if (auto* l = std::get_if<sketch::Line>(&e)) {
    pts = {{0, l->a}, {1, l->b}};
  } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
    pts = {{0, c->center}};
  } else if (auto* a = std::get_if<sketch::Arc>(&e)) {
    pts = {{0, a->center},
           {1, sketch::ArcPointUV(*a, 1)},
           {2, sketch::ArcPointUV(*a, 2)}};
  } else if (auto* el = std::get_if<sketch::Ellipse>(&e)) {
    pts = {{0, el->center}};
  } else if (auto* ea = std::get_if<sketch::EllipseArc>(&e)) {
    pts = {{0, ea->center},
           {1, sketch::EllipseArcPointUV(*ea, 1)},
           {2, sketch::EllipseArcPointUV(*ea, 2)}};
  } else if (auto* bs = std::get_if<sketch::BSpline>(&e)) {
    const size_t nPoles = std::min<size_t>(bs->poles.size(), 256);
    for (size_t k = 0; k < nPoles; k++)
      pts.push_back({static_cast<uint8_t>(k), bs->poles[k]});
  }
  return pts;
}
} // namespace

SketchPick PickSketchPoint(AppState& state, const SketchFeature& sketch,
                           glm::vec2 mousePos, float tolPx) {
  SketchPick best;
  float bestDist = tolPx;
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    const uint32_t id = sketch::EntityId(sketch.entities[i]);
    for (auto& [which, uv] : EntityPoints(sketch.entities[i])) {
      glm::vec2 screen = SketchUVToScreen(state, sketch.plane, uv);
      float d = glm::length(screen - mousePos);
      if (d < bestDist) {
        bestDist = d;
        best.hit = true;
        best.ref = {id, which};
      }
    }
  }
  return best;
}

namespace {
// Distance in pixels from p to segment [a,b].
float DistToSegment(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
  glm::vec2 ab = b - a;
  float len2 = glm::dot(ab, ab);
  float t =
      len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
  return glm::length(p - (a + t * ab));
}
} // namespace

std::vector<SketchPick> PickSketchCandidates(AppState& state,
                                             const SketchFeature& sketch,
                                             glm::vec2 mousePos, float tolPx) {
  using Ranked = std::pair<float, SketchPick>;
  std::vector<Ranked> points, bodies, axes;

  // Endpoints, within a slightly LARGER tolerance than bodies so a point is
  // never shadowed by the line it terminates.
  const float pointTolPx = tolPx + 4.0f;
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    const uint32_t id = sketch::EntityId(sketch.entities[i]);
    for (auto& [which, uv] : EntityPoints(sketch.entities[i])) {
      const float d =
          glm::length(SketchUVToScreen(state, sketch.plane, uv) - mousePos);
      if (d < pointTolPx)
        points.push_back({d, SketchPick{true, false, {id, which}}});
    }
  }

  // Entity bodies (line segment / circle rim / arc polyline).
  for (const auto& e : sketch.entities) {
    const uint32_t id = sketch::EntityId(e);
    float d = 1e30f;
    if (auto* l = std::get_if<sketch::Line>(&e)) {
      d = DistToSegment(mousePos, SketchUVToScreen(state, sketch.plane, l->a),
                        SketchUVToScreen(state, sketch.plane, l->b));
    } else if (auto* c = std::get_if<sketch::Circle>(&e)) {
      glm::vec2 center = SketchUVToScreen(state, sketch.plane, c->center);
      glm::vec2 rim = SketchUVToScreen(
          state, sketch.plane,
          c->center + glm::vec2(static_cast<float>(c->radius), 0.0f));
      float rPx = glm::length(rim - center);
      d = std::abs(glm::length(mousePos - center) - rPx);
    } else if (std::holds_alternative<sketch::Arc>(e)) {
      // Distance to the tessellated arc polyline — respects the sweep (only
      // the drawn span is hit) and works under any projection.
      std::vector<glm::vec2> uv = sketch::TessellateEntity(e);
      for (size_t k = 0; k + 1 < uv.size(); k++)
        d = std::min(
            d, DistToSegment(mousePos,
                             SketchUVToScreen(state, sketch.plane, uv[k]),
                             SketchUVToScreen(state, sketch.plane, uv[k + 1])));
    }
    if (d < tolPx)
      bodies.push_back({d, SketchPick{true, true, {id, 0}}});
  }

  // Datum axes are pickable (e.g. as a mirror axis) but rank below real
  // geometry, so a click only takes one when nothing user-drawn is in range
  // (or cycling walked past everything else).
  for (uint32_t axisId : {sketch::kAxisX, sketch::kAxisY}) {
    const auto ep = sketch::DatumAxisEndpoints(axisId);
    const glm::vec2 dir = ep.second - ep.first;
    const glm::vec2 a = ep.first - dir * 100.0f, b = ep.first + dir * 100.0f;
    const float d =
        DistToSegment(mousePos, SketchUVToScreen(state, sketch.plane, a),
                      SketchUVToScreen(state, sketch.plane, b));
    if (d < tolPx)
      axes.push_back({d, SketchPick{true, true, {axisId, 0}}});
  }

  const auto byDist = [](const Ranked& x, const Ranked& y) {
    return x.first < y.first;
  };
  std::sort(points.begin(), points.end(), byDist);
  std::sort(bodies.begin(), bodies.end(), byDist);
  std::sort(axes.begin(), axes.end(), byDist);

  std::vector<SketchPick> out;
  out.reserve(points.size() + bodies.size() + axes.size());
  for (const auto& [d, p] : points)
    out.push_back(p);
  for (const auto& [d, p] : bodies)
    out.push_back(p);
  for (const auto& [d, p] : axes)
    out.push_back(p);
  return out;
}

SketchPick PickSketchEntity(AppState& state, const SketchFeature& sketch,
                            glm::vec2 mousePos, float tolPx) {
  const std::vector<SketchPick> candidates =
      PickSketchCandidates(state, sketch, mousePos, tolPx);
  return candidates.empty() ? SketchPick{} : candidates.front();
}

namespace {
// A newly created dimension is born with a legible label offset — pushed off
// the measured geometry the way other CAD seeds placements — instead of at
// offset 0 (on top of it). UV units (≈mm at scale 1).
glm::vec2 SeedLabelOffset(const SketchFeature& sk,
                          const sketch::Constraint& c) {
  using K = sketch::ConstraintKind;
  switch (c.kind) {
  case K::Distance: {
    const auto a = RefUV(sk, c.a), b = RefUV(sk, c.b);
    if (!a || !b || glm::length(*b - *a) < 1e-6f)
      return {0.0f, 0.8f};
    const glm::vec2 d = glm::normalize(*b - *a);
    return glm::vec2(-d.y, d.x) * 0.8f; // perpendicular to the span
  }
  case K::DistanceX:
    return {0.0f, -0.8f}; // dimension line below the pair
  case K::DistanceY:
    return {-0.8f, 0.0f}; // dimension line left of the pair
  case K::DistancePL: {
    // Along the measured line's direction so the dim line (point → foot,
    // perpendicular to it) slides clear of both.
    const auto a = RefUV(sk, {c.b.entity, 0}), b = RefUV(sk, {c.b.entity, 1});
    if (!a || !b || glm::length(*b - *a) < 1e-6f)
      return {0.0f, 0.8f};
    return glm::normalize(*b - *a) * 0.8f;
  }
  case K::Radius:
  case K::Diameter: {
    // Along the renderer's leader direction, clear of the rim.
    const size_t ci = sk.IndexOfEntity(c.a.entity);
    glm::vec2 dir(0.7071f, 0.7071f);
    float radius = 0.0f;
    if (ci != SIZE_MAX) {
      if (const auto* arc = std::get_if<sketch::Arc>(&sk.entities[ci])) {
        const double mid = 0.5 * (arc->startAngle + arc->endAngle);
        dir = {static_cast<float>(std::cos(mid)),
               static_cast<float>(std::sin(mid))};
        radius = static_cast<float>(arc->radius);
      } else if (const auto* circ =
                     std::get_if<sketch::Circle>(&sk.entities[ci])) {
        radius = static_cast<float>(circ->radius);
      }
    }
    return dir * (0.25f * radius + 0.7f);
  }
  case K::Angle: {
    // Along the lines' bisector so the arc lands between them.
    const auto a0 = RefUV(sk, {c.a.entity, 0}), a1 = RefUV(sk, {c.a.entity, 1});
    const auto b0 = RefUV(sk, {c.b.entity, 0}), b1 = RefUV(sk, {c.b.entity, 1});
    if (!a0 || !a1 || !b0 || !b1)
      return {0.9f, 0.9f};
    const glm::vec2 da = glm::normalize(*a1 - *a0);
    const glm::vec2 db = glm::normalize(*b1 - *b0);
    const glm::vec2 bis = da + db;
    return glm::length(bis) > 1e-4f ? glm::normalize(bis) * 1.2f
                                    : glm::vec2(0.9f, 0.9f);
  }
  default:
    return {0.0f, 0.0f};
  }
}
} // namespace

bool ApplySketchConstraint(AppState& state, sketch::ConstraintKind kind,
                           double value, const std::string& valueExpr,
                           const glm::vec2* labelOffset) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return false;

  const auto& sel = state.sketchEdit.selected;
  using K = sketch::ConstraintKind;
  sketch::Constraint c;
  c.kind = kind;
  c.value = value;
  c.valueExpr = valueExpr;

  // Validate the selection for the constraint kind. Refs carry entity ids, so
  // resolve to the current index first (skip if the entity is gone).
  auto entityOf = [&](const sketch::PointRef& r) -> const sketch::Entity* {
    size_t idx = sketch->IndexOfEntity(r.entity);
    return idx == SIZE_MAX ? nullptr : &sketch->entities[idx];
  };
  auto isLine = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Line>(*e);
  };
  // The datum X/Y axes act as lines for the dimensional kinds (angle-to-axis,
  // distance-to-axis) — the solver registers them as fixed lines.
  auto isLineOrAxis = [&](const sketch::PointRef& r) {
    return isLine(r) || r.entity == sketch::kAxisX ||
           r.entity == sketch::kAxisY;
  };
  auto isCircle = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Circle>(*e);
  };
  auto isArc = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Arc>(*e);
  };
  auto isCurve = [&](const sketch::PointRef& r) { // circle or arc: has a radius
    return isCircle(r) || isArc(r);
  };
  auto isPoint = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Point>(*e);
  };

  switch (kind) {
  case K::Coincident:
  case K::Distance:
  case K::DistanceX:
  case K::DistanceY:
    if (sel.size() < 2)
      return false;
    c.a = sel[0];
    c.b = sel[1];
    break;
  case K::Horizontal:
  case K::Vertical:
    if (sel.empty() || !isLine(sel[0]))
      return false;
    c.a = {sel[0].entity, 0};
    break;
  case K::Parallel:
  case K::Perpendicular:
  case K::Equal:
    if (sel.size() < 2)
      return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Radius:
  case K::Diameter:
    if (sel.empty() || !isCurve(sel[0]))
      return false;
    c.a = {sel[0].entity, 0};
    break;
  case K::Angle:
    if (sel.size() < 2 || !isLineOrAxis(sel[0]) || !isLineOrAxis(sel[1]))
      return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::DistancePL: {
    // A point (or line endpoint) + a line: perpendicular distance. Two lines
    // = the parallel-lines distance (an endpoint of the first onto the
    // second); the first ref supplies the point, like PointOnLine.
    if (sel.size() < 2)
      return false;
    const sketch::PointRef* pt = nullptr;
    const sketch::PointRef* line = nullptr;
    if (isLineOrAxis(sel[1])) {
      pt = &sel[0];
      line = &sel[1];
    } else if (isLineOrAxis(sel[0])) {
      pt = &sel[1];
      line = &sel[0];
    } else {
      return false;
    }
    if (pt->entity == line->entity)
      return false; // point is on its own line
    c.a = *pt;
    c.b = {line->entity, 0};
    break;
  }
  case K::Tangent: {
    // One line + one circle/arc, either order; normalize to a=line, b=curve.
    if (sel.size() < 2)
      return false;
    if (isLine(sel[0]) && isCurve(sel[1])) {
      c.a = {sel[0].entity, 0};
      c.b = {sel[1].entity, 0};
    } else if (isCurve(sel[0]) && isLine(sel[1])) {
      c.a = {sel[1].entity, 0};
      c.b = {sel[0].entity, 0};
    } else {
      return false;
    }
    break;
  }
  case K::PointOnLine: {
    // One point + one line, in either order. The point is stored verbatim
    // (its `which` selects the endpoint); the line entity goes in b.
    if (sel.size() < 2)
      return false;
    const sketch::PointRef* pt = nullptr;
    const sketch::PointRef* line = nullptr;
    if (isPoint(sel[0]) && isLine(sel[1])) {
      pt = &sel[0];
      line = &sel[1];
    } else if (isLine(sel[0]) && isPoint(sel[1])) {
      pt = &sel[1];
      line = &sel[0];
    } else if (isLine(sel[0]) && isLine(sel[1])) {
      // A line endpoint onto another line; first selection is the point.
      pt = &sel[0];
      line = &sel[1];
    } else {
      return false;
    }
    if (pt->entity == line->entity)
      return false; // point is on its own line
    c.a = *pt;
    c.b = {line->entity, 0};
    break;
  }
  case K::Symmetric: {
    // Two points + a symmetry line (any order). Line → c, the two points → a/b.
    if (sel.size() < 3)
      return false;
    int lineIdx = -1;
    for (int i = 0; i < 3; i++)
      if (isLine(sel[i])) {
        lineIdx = i;
        break;
      }
    if (lineIdx < 0)
      return false;
    int p0 = -1, p1 = -1;
    for (int i = 0; i < 3; i++)
      if (i != lineIdx)
        (p0 < 0 ? p0 : p1) = i;
    c.a = sel[p0];
    c.b = sel[p1];
    c.c = {sel[lineIdx].entity, 0};
    break;
  }
  case K::Midpoint: {
    // A point + a line: the point sits at the line's midpoint.
    if (sel.size() < 2)
      return false;
    int lineIdx = isLine(sel[1]) ? 1 : (isLine(sel[0]) ? 0 : -1);
    if (lineIdx < 0)
      return false;
    c.a = sel[1 - lineIdx]; // the point (verbatim, keeps which)
    c.b = {sel[lineIdx].entity, 0};
    break;
  }
  case K::Fixed:
    if (sel.empty())
      return false;
    c.a = sel[0]; // pin this point in place
    break;
  case K::Concentric:
    if (sel.size() < 2 || !isCurve(sel[0]) || !isCurve(sel[1]))
      return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Collinear:
    if (sel.size() < 2 || !isLine(sel[0]) || !isLine(sel[1]))
      return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  }

  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  if (sketch::IsDimension(kind))
    c.labelOffset =
        labelOffset != nullptr ? *labelOffset : SeedLabelOffset(*sketch, c);
  sketch->constraints.push_back(c);

  // Solve and write the result back into the sketch.
  sketch::Solver solver;
  solver.Load(*sketch);
  sketch::SolveResult r = solver.Solve();
  if (r.ok)
    solver.WriteBack(sketch->entities);

  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  state.sketchEdit.selected.clear();
  return true;
}

void UpdateSketchStatus(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  edit.dof = -1;
  edit.conflictingConstraints.clear();
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;
  sketch::Solver solver;
  solver.Load(
      *sketch); // solve for status only — no WriteBack, sketch unchanged
  sketch::SolveResult r = solver.Solve();
  edit.dof = r.ok ? r.dof : -1;
  edit.conflictingConstraints = r.conflictingConstraints;
  for (size_t ri : r.redundantConstraints)
    edit.conflictingConstraints.push_back(ri); // flag redundant too
  // Which entities still have free coordinates: they render in the base
  // color while fully-defined geometry turns green (the SW convention).
  edit.underDefined =
      edit.dof > 0 ? solver.UnderDefinedEntities() : std::vector<uint32_t>{};
}

bool DeleteConstraint(AppState& state, size_t cIndex) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || cIndex >= sketch->constraints.size())
    return false;
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  sketch->constraints.erase(sketch->constraints.begin() + cIndex);
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

bool ApplyPattern(AppState& state) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return false;
  auto& edit = state.sketchEdit;
  std::vector<uint32_t> sources; // unique selected ids
  for (const auto& r : edit.selected) {
    bool seen = false;
    for (uint32_t s : sources)
      seen |= (s == r.entity);
    if (!seen)
      sources.push_back(r.entity);
  }
  if (sources.empty()) {
    edit.actionMsg = "Pattern: select the geometry to pattern first.";
    return false;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  std::vector<uint32_t> gen =
      edit.patternCircular
          ? sketch->AddCircularPattern(sources, edit.patternCenter,
                                       glm::radians(edit.patternAngleDeg),
                                       edit.patternCount, edit.linkGenerative)
          : sketch->AddLinearPattern(sources, edit.patternStep,
                                     edit.patternCount, edit.linkGenerative);
  if (gen.empty()) {
    edit.actionMsg = "Pattern: nothing generated (count must be >= 2).";
    return false;
  }
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  edit.actionMsg.clear();
  return true;
}

bool ApplyOffset(AppState& state) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return false;
  auto& edit = state.sketchEdit;
  std::vector<uint32_t> chain; // unique selected ids
  for (const auto& r : edit.selected) {
    bool seen = false;
    for (uint32_t s : chain)
      seen |= (s == r.entity);
    if (!seen)
      chain.push_back(r.entity);
  }
  if (chain.empty()) {
    edit.actionMsg = "Offset: select a closed loop (its edges) first.";
    return false;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  auto gen =
      sketch->OffsetChain(chain, edit.offsetDistance, edit.offsetJoinArc);
  if (gen.empty()) {
    edit.actionMsg = "Offset: the selection must form a single closed loop.";
    return false;
  }
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  edit.actionMsg.clear();
  return true;
}

bool EditPatternParams(AppState& state, size_t opIndex, glm::vec2 vec,
                       double scalar, int count) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || opIndex >= sketch->derived.size() || count < 2)
    return false;
  sketch::DerivedOp& op = sketch->derived[opIndex];
  if (op.vec == vec && op.scalar == scalar && op.count == count)
    return false; // no change
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  op.vec = vec;       // linear step / circular centre
  op.scalar = scalar; // circular step angle (radians); unused for linear
  op.count = count;
  sketch->RegenerateDerived();
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

bool SetPatternCount(AppState& state, size_t opIndex, int count) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || opIndex >= sketch->derived.size())
    return false;
  const sketch::DerivedOp& op = sketch->derived[opIndex];
  return EditPatternParams(state, opIndex, op.vec, op.scalar, count);
}

bool EditDimensionValue(AppState& state, int dimIndex, double value) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || dimIndex < 0 || dimIndex >= (int)sketch->constraints.size())
    return false;
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  sketch->constraints[dimIndex].value = value;
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

bool RemovePattern(AppState& state, size_t opIndex) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || opIndex >= sketch->derived.size())
    return false;
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  sketch->RemoveDerivedOp(opIndex); // drops the op + its generated entities
  sketch->RegenerateDerived();
  sketch::Solver solver;
  solver.Load(*sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch->entities);
  sketch->RegenerateDerived();
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

bool ToggleConstruction(AppState& state) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || state.sketchEdit.selected.empty())
    return false;

  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  // Flip each selected entity; toggle relative to the first one's current state
  // so a mixed selection ends up consistent.
  bool anyConstruction = false;
  for (const auto& r : state.sketchEdit.selected) {
    size_t idx = sketch->IndexOfEntity(r.entity);
    if (idx != SIZE_MAX && sketch::IsConstruction(sketch->entities[idx]))
      anyConstruction = true;
  }
  const bool target = !anyConstruction;
  for (const auto& r : state.sketchEdit.selected) {
    size_t idx = sketch->IndexOfEntity(r.entity);
    if (idx != SIZE_MAX)
      sketch::SetConstruction(sketch->entities[idx], target);
  }

  state.document->Regenerate(); // profile changes when a line leaves/enters it
  nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
  return true;
}

} // namespace ui
