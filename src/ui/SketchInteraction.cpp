#include "ui/SketchInteraction.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h" // Vertex3D
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"
#include "core/SketchSolver.h"

#include "imgui.h"

#include <optional>

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
  sketch.entities.push_back(entity);
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
  const glm::vec2 uv = *uvOpt;
  edit.hoverPoint = uv;

  if (edit.tool == SketchTool::Select) {
    // Click selects an endpoint (for constraints) and begins a drag; empty
    // click clears the selection.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      SketchPick pick =
          PickSketchPoint(state, *sketch, glm::vec2(mousePos.x, mousePos.y),
                          12.0f);
      if (pick.hit) {
        if (!ImGui::GetIO().KeyCtrl)
          edit.selected.clear();
        edit.selected.push_back(pick.ref);
        edit.dragging = true;
        edit.dragRef = pick.ref;
        state.editBaseline = DocumentIO::FeatureToJson(*sketch);
      } else {
        edit.selected.clear();
      }
    }
    // Drag: re-solve with the grabbed point pinned to the cursor.
    if (edit.dragging && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      sketch::Solver solver;
      solver.Load(*sketch);
      solver.SetDragPoint(edit.dragRef, uv);
      if (solver.Solve().ok) {
        solver.WriteBack(sketch->entities);
        state.document->Regenerate();
      }
    }
    if (edit.dragging && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      edit.dragging = false;
      nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
      if (after != state.editBaseline) {
        state.commands->Execute(std::make_unique<ModifyFeatureCommand>(
                                    index, state.editBaseline, after),
                                *state.document);
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
        CommitEntity(state, *sketch, index, sketch::Line{a, uv});
        break;
      case SketchTool::Circle:
        CommitEntity(state, *sketch, index,
                     sketch::Circle{a, glm::length(uv - a)});
        break;
      case SketchTool::Rectangle:
        CommitEntity(state, *sketch, index, sketch::Rectangle{a, uv});
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
  sketch::Entity preview;
  switch (edit.tool) {
  case SketchTool::Line:
    preview = sketch::Line{a, b};
    break;
  case SketchTool::Circle:
    preview = sketch::Circle{a, glm::length(b - a)};
    break;
  case SketchTool::Rectangle:
    preview = sketch::Rectangle{a, b};
    break;
  default:
    return;
  }
  std::vector<glm::vec2> poly = sketch::TessellateEntity(preview);
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i])));
    out.push_back(LineVertex(sketch->plane.PlaneToWorld(poly[i + 1])));
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
  } else if (auto* r = std::get_if<sketch::Rectangle>(&e)) {
    pts = {{0, r->c1}, {1, r->c2}};
  }
  return pts;
}
} // namespace

SketchPick PickSketchPoint(AppState& state, const SketchFeature& sketch,
                           glm::vec2 mousePos, float tolPx) {
  SketchPick best;
  float bestDist = tolPx;
  for (uint32_t i = 0; i < sketch.entities.size(); i++) {
    for (auto& [which, uv] : EntityPoints(sketch.entities[i])) {
      glm::vec2 screen = SketchUVToScreen(state, sketch.plane, uv);
      float d = glm::length(screen - mousePos);
      if (d < bestDist) {
        bestDist = d;
        best.hit = true;
        best.ref = {i, which};
      }
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

  // Validate the selection for the constraint kind.
  auto isLine = [&](const sketch::PointRef& r) {
    return std::holds_alternative<sketch::Line>(sketch->entities[r.entity]);
  };
  auto isCircle = [&](const sketch::PointRef& r) {
    return std::holds_alternative<sketch::Circle>(sketch->entities[r.entity]);
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
