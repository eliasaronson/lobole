#include "ui/ViewportInput.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "SketchManager.h"

#include "imgui.h"

namespace ui {

namespace {

void Pick(AppState& state, const ImVec2& pos, bool select, bool alignCamera) {
  ImGuiIO& io = ImGui::GetIO();
  Ray ray = state.faceSelector->ScreenToWorldRay(
      pos.x, pos.y, static_cast<int>(io.DisplaySize.x),
      static_cast<int>(io.DisplaySize.y), *state.camera);
  RayHit hit = state.faceSelector->PickFace(ray);

  uint32_t first = 0, count = 0;
  if (select) {
    if (hit.hit) {
      state.faceSelector->SetSelectedFace(hit.meshId, hit.faceIndex);
      if (state.faceSelector->GetFaceVertexRange(hit.meshId, hit.faceIndex,
                                                 first, count)) {
        state.meshRenderer->SetFaceHighlight(hit.meshId, first, count);
      }
      LOG_DEBUG("Selected face {} on mesh {}", hit.faceIndex, hit.meshId);
      if (alignCamera) {
        state.camera->AlignToFace(
            state.faceSelector->GetFaceCenter(hit.meshId, hit.faceIndex),
            state.faceSelector->GetFaceNormal(hit.meshId, hit.faceIndex),
            5.0f);
      }
    } else {
      state.faceSelector->ClearSelection();
      state.meshRenderer->ClearFaceHighlight();
    }
  } else {
    if (hit.hit) {
      state.faceSelector->SetHoveredFace(hit.meshId, hit.faceIndex);
      if (state.faceSelector->GetFaceVertexRange(hit.meshId, hit.faceIndex,
                                                 first, count)) {
        state.meshRenderer->SetFaceHover(hit.meshId, first, count);
      }
    } else {
      state.faceSelector->ClearHover();
      state.meshRenderer->ClearFaceHover();
    }
  }
}

} // namespace

void HandleViewportInput(AppState& state) {
  if (!state.camera)
    return;

  ImGuiIO& io = ImGui::GetIO();
  // With the passthru central dock node, WantCaptureMouse is true over
  // panels/menus and false over the 3D viewport.
  const bool overViewport = !io.WantCaptureMouse;

  // Zoom
  if (overViewport && io.MouseWheel != 0.0f) {
    state.camera->Zoom(-io.MouseWheel * 5.0);
  }

  // Camera drags: start only over the viewport, but keep tracking until
  // release so a drag that crosses a panel doesn't stick or stutter
  if (overViewport) {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
      state.viewportDrag = ViewportDrag::Orbit;
      state.camera->StopAnimation();
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
      state.viewportDrag = ViewportDrag::Pan;
      state.camera->StopAnimation();
    }
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      state.viewportDrag == ViewportDrag::Orbit) {
    state.viewportDrag = ViewportDrag::None;
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle) &&
      state.viewportDrag == ViewportDrag::Pan) {
    state.viewportDrag = ViewportDrag::None;
  }

  if (state.viewportDrag == ViewportDrag::Orbit) {
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      state.camera->Orbit(io.MouseDelta.x, io.MouseDelta.y);
    }
  } else if (state.viewportDrag == ViewportDrag::Pan) {
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
      state.camera->Pan(io.MouseDelta.x, io.MouseDelta.y);
    }
  }

  // Sketch mode consumes left click/move in the viewport
  if (state.sketchManager && state.sketchManager->IsSketchMode()) {
    if (overViewport) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        state.sketchManager->OnMouseClick(io.MousePos.x, io.MousePos.y,
                                          *state.camera);
      } else if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
        state.sketchManager->OnMouseMove(io.MousePos.x, io.MousePos.y,
                                         *state.camera);
      }
    }
    return;
  }

  // Face picking: click to select, otherwise hover pre-highlight
  if (!state.faceSelector || !state.meshRenderer)
    return;
  if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    Pick(state, io.MousePos, true, io.KeyCtrl);
  } else if (overViewport && state.viewportDrag == ViewportDrag::None) {
    Pick(state, io.MousePos, false, false);
  } else if (!overViewport) {
    state.faceSelector->ClearHover();
    state.meshRenderer->ClearFaceHover();
  }
}

} // namespace ui
