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

// Editing a sketch after it is drawn: applying a constraint, the derived
// operations (pattern, offset) and their parameters, and toggling
// construction geometry.
// Split out of SketchInteraction.cpp (docs/architecture-review-2026-09-06.md,
// stage 7a). The 843-line viewport router stays there; these are the blocks
// it calls into, and SketchInteractionInternal.h is that interface written
// down.

namespace ui {

namespace {} // namespace

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
  // An EMPTY sketch is not "fully defined" -- it has nothing to define. The
  // solver answers 0 for it, honestly (no free parameters, so no degrees of
  // freedom), but 0 is also the answer that prints "Fully defined", which is
  // the app's reply to "am I done constraining?" before there is anything to
  // constrain. The first Windows tester read it as the app being wrong about
  // geometry, and it is: dof is not a status until there is geometry.
  if (sketch->entities.empty())
    return; // dof stays -1: no status
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
          ? sketch->AddCircularPattern(
                sources, edit.patternCenter, glm::radians(edit.patternAngleDeg),
                edit.patternCount, edit.linkGenerative,
                edit.patternCenterRef.entity, edit.patternCenterRef.which)
          : sketch->AddLinearPattern(sources, edit.patternStep,
                                     edit.patternCount, edit.linkGenerative,
                                     edit.patternStep2, edit.patternCount2);
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
  edit.selected.clear();
  edit.tool = SketchTool::Select; // applied: the session is done
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
  edit.selected.clear();
  edit.tool = SketchTool::Select; // applied: the session is done
  return true;
}

bool EditPatternParams(AppState& state, size_t opIndex, glm::vec2 vec,
                       double scalar, int count, glm::vec2 vec2, int count2) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  count2 = std::max(1, count2);
  if (!sketch || opIndex >= sketch->derived.size() || count < 2)
    return false;
  sketch::DerivedOp& op = sketch->derived[opIndex];
  if (op.vec == vec && op.scalar == scalar && op.count == count &&
      op.vec2 == vec2 && op.count2 == count2)
    return false; // no change
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  op.vec = vec;       // linear step / circular centre
  op.scalar = scalar; // circular step angle (radians); unused for linear
  op.count = count;
  op.vec2 = vec2; // v50: second linear grid direction (count2 = 1 → 1D)
  op.count2 = count2;
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

// Shared solve→regen→one-undo-step tail for the op edits below.
static void CommitOpEdit(AppState& state, SketchFeature& sketch, size_t index,
                         const nlohmann::json& before) {
  sketch.RegenerateDerived();
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
}

bool EditOpSources(AppState& state, size_t opIndex,
                   const std::vector<uint32_t>& ids) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || opIndex >= sketch->derived.size())
    return false;
  sketch::DerivedOp& op = sketch->derived[opIndex];
  std::vector<uint32_t> clean;
  for (uint32_t id : ids) {
    if (op.kind == sketch::DerivedKind::Mirror && id == op.axisEntity)
      continue;        // the axis can't mirror itself
    bool self = false; // the op's own outputs can't be its inputs
    for (const auto& in : op.instances)
      self |= (in.genId == id);
    if (self || sketch->IndexOfEntity(id) == SIZE_MAX)
      continue;
    bool seen = false;
    for (uint32_t s : clean)
      seen |= (s == id);
    if (!seen)
      clean.push_back(id);
  }
  if (clean.empty()) {
    state.sketchEdit.actionMsg = "Select at least one source entity.";
    return false;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  op.sourceIds = std::move(clean);
  CommitOpEdit(state, *sketch, index, before);
  return true;
}

bool EditOpAxis(AppState& state, size_t opIndex, uint32_t axisEntity) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch || opIndex >= sketch->derived.size())
    return false;
  sketch::DerivedOp& op = sketch->derived[opIndex];
  if (op.kind != sketch::DerivedKind::Mirror)
    return false;
  const size_t ai = sketch->IndexOfEntity(axisEntity);
  const bool isLine = sketch::IsDatum(axisEntity) ||
                      (ai != SIZE_MAX && std::holds_alternative<sketch::Line>(
                                             sketch->entities[ai]));
  if (!isLine) {
    state.sketchEdit.actionMsg =
        "Mirror: the axis must be a line or an X/Y axis.";
    return false;
  }
  bool self = false; // an op output can't be its own axis
  for (const auto& in : op.instances)
    self |= (in.genId == axisEntity);
  if (self) {
    state.sketchEdit.actionMsg = "Pick an axis the mirror doesn't generate.";
    return false;
  }
  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
  op.axisEntity = axisEntity;
  CommitOpEdit(state, *sketch, index, before);
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
