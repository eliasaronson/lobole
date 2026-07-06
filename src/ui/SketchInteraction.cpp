#include "ui/SketchInteraction.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h" // Vertex3D
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "core/SketchProfile.h"

#include "imgui.h"

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

  if (edit.tool == SketchTool::Select)
    return true;

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
      edit.hasPendingPoint = false;
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

} // namespace ui
