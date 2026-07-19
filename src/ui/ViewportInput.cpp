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

  // The pick pipeline re-sets these when it runs; consumed frames (sketch,
  // assembly) must not leave a stale edge-hover preview behind.
  state.pick.edgeHover = false;
  state.edgePick.hasHover = false;

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
        state.camera->Zoom(-io.MouseWheel * 2.0); // gentler per-notch step
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
    // Shift+F: frame just the selection (falls back to fit when empty).
    if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F))
      FitCameraToSelection(state);

    const bool modelCtx = !state.sketchEdit.IsEditing();
    if (modelCtx && !io.KeyShift && !state.sketchEdit.pickingPlane &&
        ImGui::IsKeyPressed(ImGuiKey_S))
      StartNewSketch(state); // `s`: start a sketch (then pick a plane/face)
    // `n`: align the camera normal to the selected face (else the hovered
    // one) — the model-space sibling of the sketcher's plane alignment.
    if (modelCtx && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N) &&
        state.faceSelector) {
      uint32_t mesh = 0, index = 0;
      bool have = false;
      if (const SelectionItem* it =
              state.selection.First(SelectionItem::Kind::Face)) {
        have = ResolveFaceItem(state, *it, mesh, index);
      }
      if (!have) {
        if (const Face* hov = state.faceSelector->GetHoveredFace()) {
          mesh = hov->meshId;
          index = hov->faceIndex;
          have = true;
        }
      }
      if (have) {
        state.camera->AlignToFace(
            state.faceSelector->GetFaceCenter(mesh, index),
            state.faceSelector->GetFaceNormal(mesh, index));
      }
    }
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

  // Armed-tool LIFECYCLE only (U1c) — picking itself flows through the
  // selection pipeline below, filtered by whichever tool is armed. Escape
  // disarms in the same priority order the tools pick in; one press feeds
  // exactly one consumer (escUsed also keeps the pipeline's Escape-clears
  // from firing on the same press).
  bool escUsed = false;
  const bool esc =
      !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (state.edgePick.Active() && state.document) {
    auto* fil = state.document->FindFeatureById(state.edgePick.filletId);
    if (fil == nullptr) {
      state.edgePick = {}; // the mode ends with its feature
    } else if (esc && state.pendingFeatureId != state.edgePick.filletId) {
      // Escape ends a re-pick session; while previewing it stays with the
      // preview's own cancel handling.
      state.edgePick = {};
      escUsed = true;
    }
  } else if (state.slotPick.Active()) {
    if (state.document == nullptr ||
        state.document->FindFeatureById(state.slotPick.featureId) == nullptr) {
      state.slotPick = {}; // the slot ends with its feature
    } else if (esc) {
      state.slotPick = {};
      escUsed = true;
    }
  } else if (state.measure.active && esc) {
    state.measure = MeasureState{}; // Escape exits (clears pair + readout)
    escUsed = true;
  } else if (state.simulation.IsActive() && esc) {
    // Escape disarms the BC tool, or exits the mode when nothing is armed.
    if (state.simulation.tool != SimulationState::Tool::None)
      state.simulation.tool = SimulationState::Tool::None;
    else
      ExitSimulation(state);
    escUsed = true;
  }

  if (!state.faceSelector || !state.meshRenderer)
    return;

  // Assembly tab: occurrences select through the pipeline; double-click
  // enters the referenced part in context (assembly ghosted, A4). While the
  // mate tool is arming, clicks pick connector faces (its own picker —
  // connector frames need the (occurrence, face) pair, see U1d notes).
  if (state.assembly) {
    // Escape cancels an in-progress mate.
    if (state.mateTool.active && esc) {
      CancelMateTool(state);
      escUsed = true;
    }

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

    // Double-click enters in context: the first click of the pair already
    // selected the occurrence via the pipeline; consume the second so it
    // doesn't also cycle. Not while measuring — those clicks are picks.
    if (overViewport && !state.measure.active &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (const uint64_t occId = SelectedOccurrenceId(state))
        EnterInContextEdit(state, occId);
      return;
    }

    HandleSelectionInput(state, overViewport, /*allowEscapeClear=*/!escUsed);
    return;
  }

  // The unified pick pipeline (U1b/U1c): default selection — edges/faces/
  // bodies, hover preview, Ctrl multi-select, same-spot cycling — or, while
  // a tool is armed, its filtered pick routed to that tool.
  HandleSelectionInput(state, overViewport, /*allowEscapeClear=*/!escUsed);
}

} // namespace ui
