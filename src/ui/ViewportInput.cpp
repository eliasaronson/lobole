#include "ui/ViewportInput.h"

#include "Camera.h"
#include "FaceSelector.h"
#include "Logger.h"
#include "MeshRenderer.h"
#include "ui/AppActions.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"

#include "imgui.h"

namespace ui {

void HandleViewportInput(AppState& state) {
  // A drawing tab drives a 2D sheet, not the orbit camera.
  if (state.drawing) {
    HandleSheetInput(state);
    return;
  }
  if (!state.camera)
    return;

  // The pick pipeline re-sets this when it runs; consumed frames (sketch,
  // armed tools, assembly) must not leave a stale edge-hover preview behind.
  state.pick.edgeHover = false;

  ImGuiIO& io = ImGui::GetIO();
  // With the passthru central dock node, WantCaptureMouse is true over
  // panels/menus and false over the 3D viewport.
  const bool overViewport = !io.WantCaptureMouse;

  // Scroll: plain vertical = zoom; Shift = pan (trackpad two-finger pan); a
  // horizontal wheel/swipe always pans sideways (only trackpads emit it).
  if (overViewport && (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f)) {
    const double wheelPanPx = 30.0; // one wheel notch ≈ 30 px of drag
    if (io.KeyShift) {
      state.camera->Pan(io.MouseWheelH * wheelPanPx, -io.MouseWheel * wheelPanPx);
    } else {
      if (io.MouseWheelH != 0.0f)
        state.camera->Pan(io.MouseWheelH * wheelPanPx, 0.0);
      if (io.MouseWheel != 0.0f)
        state.camera->Zoom(-io.MouseWheel * 5.0);
    }
  }

  // SolidWorks-style middle-button drags: plain = rotate, Ctrl = pan,
  // Shift = zoom, Alt = roll. The drag starts over the viewport and keeps
  // tracking until release even if the cursor crosses a panel; modifiers
  // are re-read every frame so pressing Ctrl mid-drag switches to pan.
  // Trackpads without a usable middle button: Alt+left-drag navigates the
  // same way (plain = rotate, +Ctrl = pan, +Shift = zoom; no roll). Alt is
  // only needed to start the drag, and the click never reaches picking.
  if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    state.viewportDrag = ViewportDrag::Orbit; // actual mode resolved below
    state.viewportDragLeft = false;
    state.camera->StopAnimation();
  } else if (overViewport && io.KeyAlt &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             state.viewportDrag == ViewportDrag::None) {
    state.viewportDrag = ViewportDrag::Orbit;
    state.viewportDragLeft = true;
    state.camera->StopAnimation();
  }
  if (ImGui::IsMouseReleased(state.viewportDragLeft ? ImGuiMouseButton_Left
                                                    : ImGuiMouseButton_Middle)) {
    state.viewportDrag = ViewportDrag::None;
  }
  if (state.viewportDrag != ViewportDrag::None) {
    if (io.KeyCtrl) {
      state.viewportDrag = ViewportDrag::Pan;
    } else if (io.KeyShift) {
      state.viewportDrag = ViewportDrag::Zoom;
    } else if (io.KeyAlt && !state.viewportDragLeft) {
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

  // An Alt+left navigation drag owns the left button — stop here so the click
  // can't leak into the sketch / picking handlers below.
  if (state.viewportDrag != ViewportDrag::None && state.viewportDragLeft)
    return;

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

  // Onshape view/model shortcuts. Guarded only by WantTextInput (like the sketch
  // keys) so they fire whether or not a panel has focus. `f` (zoom to fit) works
  // in any context; the rest are model-context so they don't fight the sketch
  // keymap (e.g. `n` = Coincident while sketching).
  if (!io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
    if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F))
      FitCameraToDocument(state);

    const bool modelCtx = !state.sketchEdit.IsEditing();
    if (modelCtx && !io.KeyShift && !state.sketchEdit.pickingPlane &&
        ImGui::IsKeyPressed(ImGuiKey_S))
      StartNewSketch(state); // `s`: start a sketch (then pick a plane/face)
    if (modelCtx && io.KeyShift) {
      if (ImGui::IsKeyPressed(ImGuiKey_P))
        state.showPlanes = !state.showPlanes; // Shift+P: hide/show planes
      // Shift+1..7: standard views (camera aligned along the axis normal;
      // distance 0 keeps the current zoom).
      auto view = [&](glm::vec3 n) {
        state.camera->AlignToFace(glm::vec3(0.0f), n);
      };
      if (ImGui::IsKeyPressed(ImGuiKey_1)) view({0, 0, 1});        // Front
      else if (ImGui::IsKeyPressed(ImGuiKey_2)) view({0, 0, -1});  // Back
      else if (ImGui::IsKeyPressed(ImGuiKey_3)) view({-1, 0, 0});  // Left
      else if (ImGui::IsKeyPressed(ImGuiKey_4)) view({1, 0, 0});   // Right
      else if (ImGui::IsKeyPressed(ImGuiKey_5)) view({0, 1, 0});   // Top
      else if (ImGui::IsKeyPressed(ImGuiKey_6)) view({0, -1, 0});  // Bottom
      else if (ImGui::IsKeyPressed(ImGuiKey_7))
        view(glm::normalize(glm::vec3(1, 1, 1))); // Isometric
    }
  }

  // Sketch plane-picking / drawing consumes the viewport when active
  if (HandleSketchViewport(state, glm::vec2(io.MousePos.x, io.MousePos.y),
                           overViewport)) {
    return;
  }

  // B-Rep edge picking (fillet/chamfer edge collection) consumes the
  // viewport while active.
  if (HandleEdgePickInput(state, overViewport)) {
    return;
  }

  // Measure tool (C3): picks faces/edges to read area/length/radius/distance.
  if (HandleMeasureInput(state, overViewport)) {
    return;
  }

  if (!state.faceSelector || !state.meshRenderer)
    return;

  // Simulation mode (FEM): Escape disarms the boundary-condition tool, or
  // exits the mode when nothing is armed. While a tool is armed, a viewport
  // click assigns it to the picked face.
  if (state.simulation.IsActive()) {
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      if (state.simulation.tool != SimulationState::Tool::None)
        state.simulation.tool = SimulationState::Tool::None;
      else
        ExitSimulation(state);
      return;
    }
    if (state.simulation.tool != SimulationState::Tool::None) {
      if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        Ray ray = state.faceSelector->ScreenToWorldRay(
            io.MousePos.x - state.viewportPos.x,
            io.MousePos.y - state.viewportPos.y,
            static_cast<int>(state.viewportSize.x),
            static_cast<int>(state.viewportSize.y), *state.camera);
        SimulationPick(state, ray);
      }
      return; // BC tool armed: consume viewport clicks
    }
  }

  // Assembly tab: pick whole occurrences — click selects (highlights every
  // instance), double-click enters the referenced part in context (assembly
  // ghosted, A4). While the mate tool is arming, clicks pick connector faces.
  if (state.assembly) {
    // Escape cancels an in-progress mate.
    if (state.mateTool.active && ImGui::IsKeyPressed(ImGuiKey_Escape))
      CancelMateTool(state);

    // The drag gizmo gets first shot: a live drag continues even off-viewport,
    // and a click on a handle starts one (consuming the click so it doesn't
    // also re-select). Built each frame from the current mouse ray.
    {
      const bool clicked =
          overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
      const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
      const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
      if (state.gizmoDrag.active || clicked) {
        Ray ray = state.faceSelector->ScreenToWorldRay(
            io.MousePos.x - state.viewportPos.x,
            io.MousePos.y - state.viewportPos.y,
            static_cast<int>(state.viewportSize.x),
            static_cast<int>(state.viewportSize.y), *state.camera);
        if (HandleGizmoInput(state, ray, clicked, down, released))
          return;
      }
    }

    if (state.mateTool.active && state.mateTool.previewMateId == 0 &&
        overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      Ray ray = state.faceSelector->ScreenToWorldRay(
          io.MousePos.x - state.viewportPos.x,
          io.MousePos.y - state.viewportPos.y,
          static_cast<int>(state.viewportSize.x),
          static_cast<int>(state.viewportSize.y), *state.camera);
      MateToolPick(state, ray);
      return;
    }

    if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      Ray ray = state.faceSelector->ScreenToWorldRay(
          io.MousePos.x - state.viewportPos.x,
          io.MousePos.y - state.viewportPos.y,
          static_cast<int>(state.viewportSize.x),
          static_cast<int>(state.viewportSize.y), *state.camera);
      const RayHit hit = state.faceSelector->PickFace(ray);
      uint64_t occId = 0;
      if (hit.hit) {
        for (const DocumentRenderMesh& m : state.documentMeshes)
          if (m.meshId == hit.meshId) { occId = m.occurrenceId; break; }
      }
      if (occId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        // Enter the part IN CONTEXT (A4): the part tab with the rest of the
        // assembly ghosted around it. Falls back to nothing on a dangling ref.
        EnterInContextEdit(state, occId);
      } else {
        SelectOccurrence(state, occId); // 0 clears the selection
      }
    }
    return;
  }

  // Default context: the unified pick pipeline (U1b) — edges/faces/bodies,
  // hover preview, Ctrl multi-select, same-spot disambiguation cycling.
  HandleSelectionInput(state, overViewport);
}

} // namespace ui
