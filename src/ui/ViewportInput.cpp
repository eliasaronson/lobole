#include "ui/ViewportInput.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "ui/SketchInteraction.h"

#include "imgui.h"

namespace ui {

namespace {

void Pick(AppState& state, const ImVec2& pos, bool select, bool alignCamera) {
  // Map the cursor through the central-node viewport rect (not the full
  // window), matching where the 3D scene is actually drawn.
  Ray ray = state.faceSelector->ScreenToWorldRay(
      pos.x - state.viewportPos.x, pos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
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
            state.faceSelector->GetFaceNormal(hit.meshId, hit.faceIndex));
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

  // SolidWorks-style middle-button drags: plain = rotate, Ctrl = pan,
  // Shift = zoom, Alt = roll. The drag starts over the viewport and keeps
  // tracking until release even if the cursor crosses a panel; modifiers
  // are re-read every frame so pressing Ctrl mid-drag switches to pan.
  if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    state.viewportDrag = ViewportDrag::Orbit; // actual mode resolved below
    state.camera->StopAnimation();
  }
  if (ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) {
    state.viewportDrag = ViewportDrag::None;
  }
  if (state.viewportDrag != ViewportDrag::None) {
    if (io.KeyCtrl) {
      state.viewportDrag = ViewportDrag::Pan;
    } else if (io.KeyShift) {
      state.viewportDrag = ViewportDrag::Zoom;
    } else if (io.KeyAlt) {
      state.viewportDrag = ViewportDrag::Roll;
    } else {
      state.viewportDrag = ViewportDrag::Orbit;
    }
  }

  if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
    switch (state.viewportDrag) {
    case ViewportDrag::Orbit:
      state.camera->Orbit(io.MouseDelta.x, io.MouseDelta.y);
      break;
    case ViewportDrag::Pan:
      state.camera->Pan(io.MouseDelta.x, io.MouseDelta.y);
      break;
    case ViewportDrag::Zoom:
      state.camera->Zoom(io.MouseDelta.y * 0.05); // drag down = zoom out
      break;
    case ViewportDrag::Roll:
      state.camera->Roll(io.MouseDelta.x);
      break;
    case ViewportDrag::None:
      break;
    }
  }

  // SolidWorks keyboard navigation (when the UI isn't capturing keys):
  // arrows rotate (Shift = 90 degree steps), Ctrl+arrows pan, Alt+left/right
  // roll, Z / Shift+Z zoom out/in
  if (!io.WantCaptureKeyboard) {
    // 15 degrees per press (90 with Shift); orbit is 0.005 rad per "pixel"
    const double step = (io.KeyShift ? 1.5707963 : 0.2617994) / 0.005;
    const double panStep = 40.0;

    auto arrow = [&](ImGuiKey key, double dx, double dy) {
      if (!ImGui::IsKeyPressed(key))
        return;
      if (io.KeyCtrl) {
        state.camera->Pan(dx * panStep, dy * panStep);
      } else if (io.KeyAlt) {
        if (dx != 0.0) state.camera->Roll(dx * 26.0);
      } else {
        state.camera->Orbit(dx * step, dy * step);
      }
    };
    arrow(ImGuiKey_LeftArrow, -1.0, 0.0);
    arrow(ImGuiKey_RightArrow, 1.0, 0.0);
    arrow(ImGuiKey_UpArrow, 0.0, -1.0);
    arrow(ImGuiKey_DownArrow, 0.0, 1.0);

    if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
      state.camera->Zoom(io.KeyShift ? -5.0 : 5.0);
    }
  }

  // Sketch plane-picking / drawing consumes the viewport when active
  if (HandleSketchViewport(state, glm::vec2(io.MousePos.x, io.MousePos.y),
                           overViewport)) {
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
