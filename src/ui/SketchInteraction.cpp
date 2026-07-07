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
    if (edit.hasPendingPoint)
      edit.hasPendingPoint = false;
    else
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
      if (eidx != SIZE_MAX &&
          std::holds_alternative<sketch::Circle>(sketch->entities[eidx])) {
        LOG_INFO("Trim: circle trimming needs arcs (not yet supported)");
      } else {
        nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
        if (sketch->TrimLineAt(hover.ref.entity, uv)) {
          sketch::Solver solver;
          solver.Load(*sketch);
          if (solver.Solve().ok)
            solver.WriteBack(sketch->entities);
          state.document->Regenerate();
          nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
          state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                      index, before, after),
                                  *state.document);
        }
      }
    }
    return true;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (edit.tool == SketchTool::Point) {
      CommitEntity(state, *sketch, index, sketch::Point{uv});
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
  auto isPoint = [&](const sketch::PointRef& r) {
    const auto* e = entityOf(r);
    return e && std::holds_alternative<sketch::Point>(*e);
  };

  switch (kind) {
  case K::Coincident:
  case K::Distance:
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
    if (sel.empty() || !isCircle(sel[0])) return false;
    c.a = {sel[0].entity, 0};
    break;
  case K::Angle:
    if (sel.size() < 2 || !isLine(sel[0]) || !isLine(sel[1])) return false;
    c.a = {sel[0].entity, 0};
    c.b = {sel[1].entity, 0};
    break;
  case K::Tangent: {
    // One line + one circle, in either order; normalize to a=line, b=circle.
    if (sel.size() < 2) return false;
    if (isLine(sel[0]) && isCircle(sel[1])) {
      c.a = {sel[0].entity, 0};
      c.b = {sel[1].entity, 0};
    } else if (isCircle(sel[0]) && isLine(sel[1])) {
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

} // namespace ui
