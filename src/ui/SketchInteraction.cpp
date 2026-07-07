#include "ui/SketchInteraction.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h" // Vertex3D
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchInference.h"
#include "core/SketchProfile.h"
#include "core/SketchSolver.h"

#include "imgui.h"

#include <map>
#include <optional>
#include <vector>

namespace ui {

namespace {

constexpr glm::vec3 kPreviewColor{0.95f, 0.85f, 0.35f};

Vertex3D LineVertex(const glm::vec3& p) {
  Vertex3D v;
  v.position = p;
  v.normal = glm::vec3(0.0f);
  v.color = kPreviewColor;
  return v;
}

// Intersects a screen ray with a plane; returns UV on the plane, or nullopt
// if parallel.
std::optional<glm::vec2> RayToPlaneUV(const Ray& ray, const SketchPlane& plane) {
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

// Commits an entity to the active sketch as one undoable ModifyFeatureCommand.
void CommitEntity(AppState& state, SketchFeature& sketch, size_t index,
                  const sketch::Entity& entity) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch.AddEntity(entity); // stamps a stable id
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
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
    const bool keep = r2.ok && r2.conflicting.empty() &&
                      (dof < 0 || r2.dof < dof);
    if (keep) {
      dof = r2.dof;
      s2.WriteBack(sketch.entities); // settle with this constraint
    } else {
      sketch.constraints.pop_back(); // redundant or conflicting — drop it
    }
  }
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
  if (infer) {
    const float ppu = PixelsPerUV(state, sketch.plane, b);
    sketch::Snap sa = sketch::InferPoint(sketch.entities, a, std::nullopt, 0, ppu);
    sketch::Snap sb = sketch::InferPoint(sketch.entities, b, sa.uv, 0, ppu);
    pa = sa.uv;
    pb = sb.uv;
    const uint32_t id = sketch.AddEntity(sketch::Line{pa, pb});
    SnapPointConstraints(sa, id, 0, candidates);
    SnapPointConstraints(sb, id, 1, candidates);
    SnapSegmentConstraints(sb, id, candidates);
    ApplyInferred(sketch, candidates);
  } else {
    sketch.AddEntity(sketch::Line{pa, pb});
  }
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
}

// Commits a rectangle (4 lines + corner/H/V constraints) as one undo record,
// then solves so the shape settles.
void CommitRectangle(AppState& state, SketchFeature& sketch, size_t index,
                     glm::vec2 c1, glm::vec2 c2) {
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch.AddRectangle(c1, c2);
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch.entities);
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
}

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
    drag.mode = SketchDragSession::Mode::LineBody;
    const auto& l = std::get<sketch::Line>(e);
    drag.points = {{pick.ref.entity, 0}, {pick.ref.entity, 1}};
    drag.initialPos = {l.a, l.b};
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

// Per-frame while dragging: retarget from the cursor delta, re-solve, write back.
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
    state.commands->Execute(
        std::make_unique<ModifyFeatureCommand>(drag.featureIndex,
                                               state.editBaseline, after),
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
  Feature* f =
      state.document->FindFeatureById(state.sketchEdit.activeSketchId, outIndex);
  auto* sketch = dynamic_cast<SketchFeature*>(f);
  if (!sketch) {
    // The sketch was removed (undo); leave edit mode cleanly.
    state.sketchEdit = SketchEditState{};
  }
  return sketch;
}

uint64_t BeginSketchOnPlane(AppState& state, const SketchPlane& plane,
                            const std::string& name) {
  auto sketch = std::make_unique<SketchFeature>(name, plane);
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(sketch)),
                          *state.document);
  const size_t index = state.document->FeatureCount() - 1;
  const uint64_t id = state.document->GetFeature(index)->GetId();

  state.sketchEdit = SketchEditState{};
  state.sketchEdit.activeSketchId = id;
  state.selectedFeature = static_cast<int>(index);
  if (state.camera) {
    state.camera->AlignToFace(plane.GetOrigin(), plane.GetNormal());
  }
  LOG_INFO("Started {} (id {})", name, id);
  return id;
}

bool HandleSketchViewport(AppState& state, const glm::vec2& mousePos,
                          bool overViewport) {
  SketchEditState& edit = state.sketchEdit;

  // --- Plane picking -------------------------------------------------------
  if (edit.pickingPlane) {
    edit.hoveredPlane = -1;
    if (overViewport) {
      Ray ray = ScreenRay(state, mousePos);
      // Prefer a model face if the ray hits one; else a default plane.
      RayHit hit = state.faceSelector->PickFace(ray);
      float bestDist = hit.hit ? hit.distance : 1e30f;
      int plane = -1;
      for (int i = 0; i < 3; i++) {
        SketchPlane p = DefaultPlane(i);
        if (auto uv = RayToPlaneUV(ray, p)) {
          float dist = glm::length(p.PlaneToWorld(*uv) - ray.origin);
          if (dist < bestDist) {
            bestDist = dist;
            plane = i;
          }
        }
      }
      edit.hoveredPlane = plane;

      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int n = static_cast<int>(state.document->FeatureCount()) + 1;
        if (plane >= 0) {
          BeginSketchOnPlane(state, DefaultPlane(plane),
                             "Sketch " + std::to_string(n));
        } else if (hit.hit) {
          // Bake the selected face's plane (no persistent face ref yet).
          glm::vec3 c =
              state.faceSelector->GetFaceCenter(hit.meshId, hit.faceIndex);
          glm::vec3 nrm =
              state.faceSelector->GetFaceNormal(hit.meshId, hit.faceIndex);
          glm::vec3 u = std::abs(nrm.y) < 0.9f ? glm::vec3(0, 1, 0)
                                               : glm::vec3(1, 0, 0);
          u = glm::normalize(u - glm::dot(u, nrm) * nrm);
          BeginSketchOnPlane(state, SketchPlane(c, nrm, u),
                             "Sketch " + std::to_string(n));
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
    if (edit.hasPendingPoint || edit.hasPendingPoint2) {
      edit.hasPendingPoint = false;
      edit.hasPendingPoint2 = false;
    } else
      edit = SketchEditState{}; // leave sketch edit
    return true;
  }

  if (!overViewport)
    return true;

  Ray ray = ScreenRay(state, mousePos);
  auto uvOpt = RayToPlaneUV(ray, sketch->plane);
  if (!uvOpt)
    return true;
  glm::vec2 uv = *uvOpt;

  // Auto-constraint inference for the Line tool: snap the cursor to nearby
  // geometry so the preview and the click both use the snapped point, and stash
  // the result for the ghost-glyph overlay. Ctrl or the toggle suppresses it.
  if (edit.tool == SketchTool::Line && edit.autoConstrain &&
      !ImGui::GetIO().KeyCtrl) {
    const float ppu = PixelsPerUV(state, sketch->plane, uv);
    std::optional<glm::vec2> other =
        edit.hasPendingPoint ? std::optional<glm::vec2>(edit.pendingPoint)
                             : std::nullopt;
    sketch::Snap snap =
        sketch::InferPoint(sketch->entities, uv, other, 0, ppu);
    uv = snap.uv;
    edit.currentSnap = snap;
    edit.hasCurrentSnap = !snap.glyphs.empty();
  }
  edit.hoverPoint = uv;

  if (edit.tool == SketchTool::Select) {
    // Track what the cursor is over for hover highlighting (endpoint-first,
    // then entity body).
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), 12.0f);
    edit.hasHover = hover.hit;
    edit.hoveredBody = hover.body;
    edit.hoveredRef = hover.ref;

    // Delete removes the selected entities (and cleans up their constraints).
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !edit.selected.empty()) {
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
      state.document->Regenerate();
      nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
      if (after != before)
        state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                    index, before, after),
                                *state.document);
      if (dropped > 0)
        LOG_INFO("Deleted {} dependent constraint(s)", dropped);
      return true;
    }
    // Click selects an element (for constraints) and begins a drag: an endpoint
    // moves that point, a line body translates the whole line, a circle rim
    // resizes it. Empty click clears the selection.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      SketchPick pick =
          PickSketchEntity(state, *sketch, glm::vec2(mousePos.x, mousePos.y),
                           12.0f);
      if (pick.hit) {
        if (!ImGui::GetIO().KeyCtrl)
          edit.selected.clear();
        edit.selected.push_back(pick.ref);
        BeginDragSession(state, *sketch, index, pick, uv);
      } else {
        edit.selected.clear();
      }
    }
    if (state.sketchDrag.active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      UpdateDragSession(state, *sketch, uv);
    if (state.sketchDrag.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
      EndDragSession(state, *sketch);
    return true;
  }

  if (edit.tool == SketchTool::Trim) {
    // Click a line segment to remove the piece under the cursor (bounded by
    // its crossings). Preview highlights just that sub-segment (not the whole
    // line) so it's clear what the click deletes.
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), 12.0f);
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
        state.document->Regenerate();
        nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
        state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *state.document);
      }
    }
    return true;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (edit.tool == SketchTool::Point) {
      CommitEntity(state, *sketch, index, sketch::Point{uv});
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
          arc = sketch::ArcCenterStartEnd(edit.pendingPoint, edit.pendingPoint2, uv);
        else
          arc = sketch::ArcFromThreePoints(edit.pendingPoint, uv, edit.pendingPoint2);
        if (arc)
          CommitEntity(state, *sketch, index, *arc);
        edit.hasPendingPoint = false;
        edit.hasPendingPoint2 = false;
      }
    } else if (!edit.hasPendingPoint) {
      edit.pendingPoint = uv;
      edit.hasPendingPoint = true;
    } else {
      const glm::vec2 a = edit.pendingPoint;
      switch (edit.tool) {
      case SketchTool::Line:
        CommitLine(state, *sketch, index, a, uv,
                   edit.autoConstrain && !ImGui::GetIO().KeyCtrl);
        break;
      case SketchTool::Circle:
        CommitEntity(state, *sketch, index,
                     sketch::Circle{a, glm::length(uv - a)});
        break;
      case SketchTool::Rectangle:
        CommitRectangle(state, *sketch, index, a, uv);
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

void BuildSketchOverlay(AppState& state, std::vector<Vertex3D>& out) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.hasPendingPoint)
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
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
  case SketchTool::Arc:
    if (!edit.hasPendingPoint2) // center placed: preview the radius ring
      poly = sketch::TessellateEntity(sketch::Circle{a, glm::length(b - a)});
    else // center + start placed: live arc to the cursor
      poly = sketch::TessellateEntity(sketch::ArcCenterStartEnd(a, edit.pendingPoint2, b));
    break;
  case SketchTool::ArcThreePoint:
    if (!edit.hasPendingPoint2) // start placed: preview the chord
      poly = {a, b};
    else if (auto arc = sketch::ArcFromThreePoints(a, b, edit.pendingPoint2))
      poly = sketch::TessellateEntity(*arc);
    else
      poly = {a, edit.pendingPoint2}; // collinear: fall back to the chord
    break;
  default:
    return;
  }
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
  }
}

void DrawSnapGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.hasCurrentSnap)
    return;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;

  const glm::vec2 p = SketchUVToScreen(state, sketch->plane, edit.currentSnap.uv);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 accent = IM_COL32(255, 220, 120, 255);
  dl->AddCircle(ImVec2(p.x, p.y), 7.0f, accent, 0, 2.0f); // "grabbed" ring

  float gx = p.x + 12.0f, gy = p.y - 24.0f;
  for (sketch::GlyphKind k : edit.currentSnap.glyphs) {
    const char* label = "";
    switch (k) {
    case sketch::GlyphKind::Coincident: label = "coincident"; break;
    case sketch::GlyphKind::PointOnLine: label = "on line"; break;
    case sketch::GlyphKind::Horizontal: label = "H"; break;
    case sketch::GlyphKind::Vertical: label = "V"; break;
    case sketch::GlyphKind::Parallel: label = "parallel"; break;
    case sketch::GlyphKind::Perpendicular: label = "perpendicular"; break;
    }
    const ImVec2 sz = ImGui::CalcTextSize(label);
    dl->AddRectFilled(ImVec2(gx - 3, gy - 1), ImVec2(gx + sz.x + 3, gy + sz.y + 1),
                      IM_COL32(25, 28, 34, 210), 3.0f);
    dl->AddText(ImVec2(gx, gy), accent, label);
    gy += sz.y + 4.0f;
  }
}

namespace {
// UV position of a point ref (line endpoint / point / circle centre).
std::optional<glm::vec2> RefUV(const SketchFeature& sk,
                               const sketch::PointRef& r) {
  size_t idx = sk.IndexOfEntity(r.entity);
  if (idx == SIZE_MAX)
    return std::nullopt;
  return sketch::EntityPointUV(sk.entities[idx], r.which);
}

// A filled arrowhead at `tip` pointing along `dir` (screen space).
void Arrow(ImDrawList* dl, ImVec2 tip, glm::vec2 dir, ImU32 col) {
  glm::vec2 n{-dir.y, dir.x};
  const float len = 9.0f, w = 3.5f;
  ImVec2 base(tip.x - dir.x * len, tip.y - dir.y * len);
  dl->AddTriangleFilled(tip, ImVec2(base.x + n.x * w, base.y + n.y * w),
                        ImVec2(base.x - n.x * w, base.y - n.y * w), col);
}

// Commits an edited dimension value: sets it, re-solves, one undo record.
void ApplyDimensionEdit(AppState& state, SketchFeature& sketch, size_t index,
                        int dimIdx, double value) {
  if (dimIdx < 0 || dimIdx >= (int)sketch.constraints.size())
    return;
  nlohmann::json before = DocumentIO::FeatureToJson(sketch);
  sketch.constraints[dimIdx].value = value;
  sketch::Solver solver;
  solver.Load(sketch);
  if (solver.Solve().ok)
    solver.WriteBack(sketch.entities);
  state.document->Regenerate();
  nlohmann::json after = DocumentIO::FeatureToJson(sketch);
  state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(index, before, after),
      *state.document);
}
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
  const ImU32 col = IM_COL32(120, 200, 255, 235);
  auto scr = [&](glm::vec2 uv) {
    glm::vec2 s = SketchUVToScreen(state, sketch->plane, uv);
    return ImVec2(s.x, s.y);
  };

  for (int i = 0; i < (int)sketch->constraints.size(); i++) {
    const sketch::Constraint& con = sketch->constraints[i];
    if (!sketch::IsDimension(con.kind))
      continue;

    char label[48] = "";
    ImVec2 labelPos;
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
        double mid = 0.5 * (arc->startAngle + arc->endAngle); // within the sweep
        dir = {static_cast<float>(std::cos(mid)),
               static_cast<float>(std::sin(mid))};
      } else {
        continue;
      }
      glm::vec2 rimUV = center + dir * radius;
      ImVec2 c0 = scr(center), c1 = scr(rimUV + con.labelOffset);
      dl->AddLine(c0, c1, col, 1.4f); // leader from centre out
      snprintf(label, sizeof(label),
               con.kind == K::Diameter ? "\xC3\x98%.3g" : "R%.3g", con.value);
      labelPos = c1;
    } else if (con.kind == K::Angle) {
      auto a = RefUV(*sketch, {con.a.entity, 0});
      if (!a)
        continue;
      labelPos = scr(*a + con.labelOffset + glm::vec2(0.3f, 0.3f));
      snprintf(label, sizeof(label), "%.1f\xC2\xB0", con.value * 57.2957795f);
    } else { // Distance / DistanceX / DistanceY (points a, b)
      auto a = RefUV(*sketch, con.a), b = RefUV(*sketch, con.b);
      if (!a || !b)
        continue;
      glm::vec2 da = *a + con.labelOffset, db = *b + con.labelOffset;
      if (con.kind == K::DistanceX)
        da.y = db.y = std::min(a->y, b->y) + con.labelOffset.y;
      else if (con.kind == K::DistanceY)
        da.x = db.x = std::min(a->x, b->x) + con.labelOffset.x;
      ImVec2 sa = scr(*a), sb = scr(*b), sda = scr(da), sdb = scr(db);
      dl->AddLine(sa, sda, col, 1.0f); // extension lines
      dl->AddLine(sb, sdb, col, 1.0f);
      dl->AddLine(sda, sdb, col, 1.4f); // dimension line
      glm::vec2 d{sdb.x - sda.x, sdb.y - sda.y};
      float L = std::sqrt(d.x * d.x + d.y * d.y);
      if (L > 1e-3f) {
        d /= L;
        Arrow(dl, sda, d, col);
        Arrow(dl, sdb, {-d.x, -d.y}, col);
      }
      double v = con.kind == K::DistanceX  ? std::abs(b->x - a->x)
                 : con.kind == K::DistanceY ? std::abs(b->y - a->y)
                                            : glm::length(*b - *a);
      snprintf(label, sizeof(label), "%.3g", v);
      labelPos = ImVec2((sda.x + sdb.x) * 0.5f, (sda.y + sdb.y) * 0.5f);
    }

    // Value text with a small backing box; double-click opens an inline editor.
    const ImVec2 tsz = ImGui::CalcTextSize(label);
    ImVec2 tp(labelPos.x - tsz.x * 0.5f, labelPos.y - tsz.y * 0.5f);
    dl->AddRectFilled(ImVec2(tp.x - 3, tp.y - 1), ImVec2(tp.x + tsz.x + 3, tp.y + tsz.y + 1),
                      IM_COL32(20, 24, 30, 220), 3.0f);
    dl->AddText(tp, col, label);

    ImGuiIO& io = ImGui::GetIO();
    const bool overLabel = io.MousePos.x >= tp.x - 3 && io.MousePos.x <= tp.x + tsz.x + 3 &&
                           io.MousePos.y >= tp.y - 1 && io.MousePos.y <= tp.y + tsz.y + 1;
    if (overLabel && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      edit.editingDim = i;

    if (edit.editingDim == i) {
      static char buf[48];
      static int lastEditing = -1;
      if (lastEditing != i) {
        snprintf(buf, sizeof(buf), "%.4g", con.value);
        lastEditing = i;
      }
      ImGui::SetNextWindowPos(ImVec2(tp.x - 4, tp.y - 4));
      ImGui::SetNextWindowBgAlpha(0.95f);
      ImGui::Begin("##dimedit", nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
                       ImGuiWindowFlags_NoSavedSettings);
      ImGui::SetNextItemWidth(70);
      ImGui::SetKeyboardFocusHere();
      bool commit = ImGui::InputText("##v", buf, sizeof(buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::End();
      if (commit) {
        double v = con.kind == K::Angle ? glm::radians(std::atof(buf))
                                        : std::atof(buf);
        ApplyDimensionEdit(state, *sketch, index, i, v);
        edit.editingDim = -1;
        lastEditing = -1;
      } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        edit.editingDim = -1;
        lastEditing = -1;
      }
    }
  }
}

void DrawConstraintGlyphs(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  if (!edit.IsEditing() || !edit.showConstraintGlyphs)
    return;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const ImU32 col = IM_COL32(150, 210, 170, 220);
  using K = sketch::ConstraintKind;

  // Fan out multiple badges on the same entity so they don't overlap.
  std::map<uint32_t, int> stack;
  auto badge = [&](uint32_t entity, const char* text) {
    auto uv = RefUV(*sketch, {entity, 0});
    if (!uv)
      return;
    glm::vec2 s = SketchUVToScreen(state, sketch->plane, *uv);
    float dx = 10.0f + 13.0f * stack[entity]++;
    ImVec2 p(s.x + dx, s.y - 16.0f);
    ImVec2 sz = ImGui::CalcTextSize(text);
    dl->AddRectFilled(ImVec2(p.x - 2, p.y - 1), ImVec2(p.x + sz.x + 2, p.y + sz.y + 1),
                      IM_COL32(25, 32, 28, 200), 2.0f);
    dl->AddText(p, col, text);
  };

  for (const auto& c : sketch->constraints) {
    switch (c.kind) {
    case K::Horizontal: badge(c.a.entity, "H"); break;
    case K::Vertical: badge(c.a.entity, "V"); break;
    case K::Parallel: badge(c.a.entity, "//"); badge(c.b.entity, "//"); break;
    case K::Perpendicular: badge(c.a.entity, "|_"); break;
    case K::Equal: badge(c.a.entity, "="); badge(c.b.entity, "="); break;
    case K::Coincident: badge(c.a.entity, "co"); break;
    case K::PointOnLine: badge(c.b.entity, "on"); break;
    case K::Tangent: badge(c.a.entity, "T"); break;
    case K::Symmetric: badge(c.c.entity, "sym"); break;
    case K::Midpoint: badge(c.b.entity, "mid"); break;
    case K::Fixed: badge(c.a.entity, "fix"); break;
    case K::Concentric: badge(c.a.entity, "con"); break;
    case K::Collinear: badge(c.a.entity, "L"); badge(c.b.entity, "L"); break;
    default: break; // dimensions render via DrawDimensions
    }
  }
}

glm::vec2 SketchUVToScreen(AppState& state, const SketchPlane& plane,
                           glm::vec2 uv) {
  glm::vec3 world = plane.PlaneToWorld(uv);
  glm::vec4 clip = state.camera->GetProjectionMatrix() *
                   state.camera->GetViewMatrix() * glm::vec4(world, 1.0f);
  glm::vec3 ndc = glm::vec3(clip) / clip.w;
  return state.viewportPos +
         glm::vec2((ndc.x + 1.0f) * 0.5f * state.viewportSize.x,
                   (ndc.y + 1.0f) * 0.5f * state.viewportSize.y);
}

namespace {
// Endpoints exposed by an entity for picking/constraints.
std::vector<std::pair<uint8_t, glm::vec2>> EntityPoints(const sketch::Entity& e) {
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
  float t = len2 > 1e-6f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                         : 0.0f;
  return glm::length(p - (a + t * ab));
}
} // namespace

SketchPick PickSketchEntity(AppState& state, const SketchFeature& sketch,
                            glm::vec2 mousePos, float tolPx) {
  // Endpoints win first, so coincident/distance/drag are unaffected.
  SketchPick pick = PickSketchPoint(state, sketch, mousePos, tolPx);
  if (pick.hit)
    return pick;

  // Otherwise fall back to the nearest entity body (line segment / circle rim).
  SketchPick best;
  float bestDist = tolPx;
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
        d = std::min(d, DistToSegment(
                            mousePos,
                            SketchUVToScreen(state, sketch.plane, uv[k]),
                            SketchUVToScreen(state, sketch.plane, uv[k + 1])));
    }
    if (d < bestDist) {
      bestDist = d;
      best.hit = true;
      best.body = true;
      best.ref = {id, 0};
    }
  }
  return best;
}

bool ApplySketchConstraint(AppState& state, sketch::ConstraintKind kind,
                           double value) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return false;

  const auto& sel = state.sketchEdit.selected;
  using K = sketch::ConstraintKind;
  sketch::Constraint c;
  c.kind = kind;
  c.value = value;

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
    if (sel.size() < 2) return false;
    c.a = sel[0];
    c.b = sel[1];
    break;
  case K::Horizontal:
  case K::Vertical:
    if (sel.empty() || !isLine(sel[0])) return false;
    c.a = {sel[0].entity, 0};
    break;
  case K::Parallel:
  case K::Perpendicular:
  case K::Equal:
    if (sel.size() < 2) return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Radius:
  case K::Diameter:
    if (sel.empty() || !isCurve(sel[0])) return false;
    c.a = {sel[0].entity, 0};
    break;
  case K::Angle:
    if (sel.size() < 2 || !isLine(sel[0]) || !isLine(sel[1])) return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Tangent: {
    // One line + one circle/arc, either order; normalize to a=line, b=curve.
    if (sel.size() < 2) return false;
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
    if (sel.size() < 2) return false;
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
    if (pt->entity == line->entity) return false; // point is on its own line
    c.a = *pt;
    c.b = {line->entity, 0};
    break;
  }
  case K::Symmetric: {
    // Two points + a symmetry line (any order). Line → c, the two points → a/b.
    if (sel.size() < 3) return false;
    int lineIdx = -1;
    for (int i = 0; i < 3; i++)
      if (isLine(sel[i])) { lineIdx = i; break; }
    if (lineIdx < 0) return false;
    int p0 = -1, p1 = -1;
    for (int i = 0; i < 3; i++)
      if (i != lineIdx) (p0 < 0 ? p0 : p1) = i;
    c.a = sel[p0];
    c.b = sel[p1];
    c.c = {sel[lineIdx].entity, 0};
    break;
  }
  case K::Midpoint: {
    // A point + a line: the point sits at the line's midpoint.
    if (sel.size() < 2) return false;
    int lineIdx = isLine(sel[1]) ? 1 : (isLine(sel[0]) ? 0 : -1);
    if (lineIdx < 0) return false;
    c.a = sel[1 - lineIdx]; // the point (verbatim, keeps which)
    c.b = {sel[lineIdx].entity, 0};
    break;
  }
  case K::Fixed:
    if (sel.empty()) return false;
    c.a = sel[0]; // pin this point in place
    break;
  case K::Concentric:
    if (sel.size() < 2 || !isCurve(sel[0]) || !isCurve(sel[1])) return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Collinear:
    if (sel.size() < 2 || !isLine(sel[0]) || !isLine(sel[1])) return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  }

  nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
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
  solver.Load(*sketch); // solve for status only — no WriteBack, sketch unchanged
  sketch::SolveResult r = solver.Solve();
  edit.dof = r.ok ? r.dof : -1;
  edit.conflictingConstraints = r.conflictingConstraints;
  for (size_t ri : r.redundantConstraints)
    edit.conflictingConstraints.push_back(ri); // flag redundant too
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
