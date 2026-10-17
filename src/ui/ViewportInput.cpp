// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/ViewportInput.h"
#include "ui/AssemblyActions.h"
#include "ui/DrawingPanel.h"
#include "ui/ListSelection.h"
#include "ui/SimulationActions.h"
#include "ui/StudyPanel.h"

#include "cad/FaceSelector.h"
#include "common/Logger.h"
#include "render/Camera.h"
#include "render/MeshRenderer.h"
#include "ui/DragMath.h"      // GizmoScale — the click's grab radius
#include "ui/FeatureEditor.h" // kMateSessionSlot (U7c)
#include "ui/PreviewFeature.h"
#include "ui/PreviewHandles.h"
#include "ui/PrimitiveTool.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteraction.h"
#include "ui/TerminalPanel.h"

#include "imgui.h"

namespace ui {

namespace {

// Which handle is under the cursor, recomputed every frame that no drag is
// running (docs/ui/viewport-handles.md). Before this, the handle hit test ran
// ONLY on click, so nothing could know what the pointer was over until the
// button went down — which is exactly the feedback that was missing.
// Routes a click/drag to the direct-manipulation handles (the pending
// feature's, and the active section's). Returns true when it consumed the
// event.
//
// Called from BOTH the assembly branch and the part path: the section arrow
// is the one handle that exists on an assembly tab, and this block used to
// live only in the part path, so dragging it there fell through to the picker
// — it highlighted on hover and then did nothing.
bool RouteHandleInput(AppState& state, bool overViewport) {
  if (!HasViewportHandle(state))
    return false;
  const ImGuiIO& io = ImGui::GetIO();
  const bool clicked =
      overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  const bool down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
  const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
  if (!state.handleDrag.active && !clicked)
    return false;
  if (!state.faceSelector || !state.camera)
    return false;
  Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  return HandlePreviewHandleInput(state, ray, clicked, down, released);
}

void UpdateHandleHover(AppState& state, bool overViewport) {
  const bool dragging = state.handleDrag.active || state.gizmoDrag.active;
  if (dragging || !overViewport || state.viewportHandles.empty() ||
      !state.camera || !state.faceSelector) {
    if (!dragging)
      state.hoveredHandle = 0;
    return;
  }
  const ImGuiIO& io = ImGui::GetIO();
  const Ray ray = state.faceSelector->ScreenToWorldRay(
      io.MousePos.x - state.viewportPos.x, io.MousePos.y - state.viewportPos.y,
      static_cast<int>(state.viewportSize.x),
      static_cast<int>(state.viewportSize.y), *state.camera);
  // The same grab radius the click uses, so the highlight cannot promise a
  // grab the click would miss.
  const int hit =
      HitTestHandles(state.viewportHandles, ray, 0.14f * GizmoScale(state));
  state.hoveredHandle =
      hit >= 0 ? state.viewportHandles[static_cast<size_t>(hit)].id : 0;
}

} // namespace

void HandleViewportInput(AppState& state) {
  // A drawing tab drives a 2D sheet, not the orbit camera.
  if (state.drawing) {
    HandleSheetInput(state);
    return;
  }
  if (!state.camera)
    return;

  // The pick pipeline re-sets this when it runs; consumed frames (sketch,
  // assembly) must not leave a stale edge-hover preview behind.
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
      state.camera->Pan(io.MouseWheelH * wheelPanPx,
                        -io.MouseWheel * wheelPanPx);
    } else {
      if (io.MouseWheelH != 0.0f)
        state.camera->Pan(io.MouseWheelH * wheelPanPx, 0.0);
      if (io.MouseWheel != 0.0f)
        state.camera->Zoom(-io.MouseWheel * 2.0); // gentler per-notch step
    }
  }

  // Standard CAD navigation drags: plain = rotate, Ctrl = pan,
  // Shift = zoom, Alt = roll. The drag starts over the viewport and keeps
  // tracking until release even if the cursor crosses a panel; modifiers
  // are re-read every frame so pressing Ctrl mid-drag switches to pan.
  // Both the middle button and the (otherwise-unused-over-the-viewport)
  // right button drive it — the right button is the no-middle-button /
  // trackpad path (rotate with no modifier, +Ctrl pan, +Shift zoom). Left
  // stays selection. Alt+left-drag still navigates too, for completeness;
  // Alt is only needed to start it and the click never reaches picking.
  if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    state.viewportDrag = ViewportDrag::Orbit; // actual mode resolved below
    state.viewportDragLeft = false;
    state.viewportDragButton = ImGuiMouseButton_Middle;
    state.camera->StopAnimation();
  } else if (overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
             state.viewportDrag == ViewportDrag::None) {
    state.viewportDrag = ViewportDrag::Orbit;
    state.viewportDragLeft = false;
    state.viewportDragButton = ImGuiMouseButton_Right;
    state.camera->StopAnimation();
  } else if (overViewport && io.KeyAlt &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             state.viewportDrag == ViewportDrag::None) {
    state.viewportDrag = ViewportDrag::Orbit;
    state.viewportDragLeft = true;
    state.viewportDragButton = ImGuiMouseButton_Left;
    state.camera->StopAnimation();
  }
  if (state.viewportDrag != ViewportDrag::None &&
      ImGui::IsMouseReleased(state.viewportDragButton)) {
    state.viewportDrag = ViewportDrag::None;
  }
  if (state.viewportDrag != ViewportDrag::None) {
    // Roll needs a modifier that neither the right nor the left path can
    // reach without ambiguity, so it stays middle-button-only (as before).
    const bool canRoll = state.viewportDragButton == ImGuiMouseButton_Middle;
    if (io.KeyCtrl) {
      state.viewportDrag = ViewportDrag::Pan;
    } else if (io.KeyShift) {
      state.viewportDrag = ViewportDrag::Zoom;
    } else if (io.KeyAlt && canRoll) {
      state.viewportDrag = ViewportDrag::Roll;
    } else {
      state.viewportDrag = ViewportDrag::Orbit;
    }
  }

  // The right button is also a click (an occurrence's menu, in the pick
  // pipeline), so its drag moves nothing until it passes the drag threshold:
  // a click must not nudge the view.
  const bool rightClickSoFar =
      state.viewportDragButton == ImGuiMouseButton_Right &&
      io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Right] <
          io.MouseDragThreshold * io.MouseDragThreshold;
  if ((io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) &&
      !rightClickSoFar) {
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

  // CAD keyboard navigation (when the UI isn't capturing keys):
  // arrows rotate (Shift = 90 degree steps), Ctrl+arrows pan, Alt+left/right
  // roll, Z / Shift+Z zoom out/in
  if (!io.WantCaptureKeyboard && !TerminalCapturesKeys(state)) {
    // 15 degrees per press (90 with Shift); orbit is 0.005 rad per "pixel"
    const double step = (io.KeyShift ? 1.5707963 : 0.2617994) / 0.005;
    const double panStep = 40.0;

    auto arrow = [&](ImGuiKey key, double dx, double dy) {
      if (!ImGui::IsKeyPressed(key))
        return;
      if (io.KeyCtrl) {
        state.camera->Pan(dx * panStep, dy * panStep);
      } else if (io.KeyAlt) {
        if (dx != 0.0)
          state.camera->Roll(dx * 26.0);
      } else {
        state.camera->Orbit(dx * step, dy * step);
      }
    };
    arrow(ImGuiKey_LeftArrow, -1.0, 0.0);
    arrow(ImGuiKey_RightArrow, 1.0, 0.0);
    arrow(ImGuiKey_UpArrow, 0.0, -1.0);
    arrow(ImGuiKey_DownArrow, 0.0, 1.0);
    // (No Z zoom key: it fired alongside Ctrl+Z / Ctrl+Shift+Z undo/redo.
    // Zoom is the wheel/trackpad; F fits.)
  }

  // The view/model keymap (F, Shift+F, N, S, Shift+P, Shift+1..7) moved to
  // the one command table — ui::DispatchShortcuts, called from DrawUI. The
  // arrow-key navigation above stays here: it is a held-modifier gesture
  // against the camera, not a command, and it is not rebindable
  // (docs/ui/keybindings.md).

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
  const bool esc = !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape);
  if (state.handleDrag.active && esc) {
    // Escape aborts a handle drag only — the preview itself survives.
    AbortPreviewHandleDrag(state);
    escUsed = true;
  } else if (state.primitive.dragging && esc) {
    AbortPrimitiveDrag(state); // the same for the primitive's ghost
    escUsed = true;
  } else if (state.slotPick.Active()) {
    // The session's own answer, from the ONE table that also routes its picks
    // (ToolSession in FeatureEditor.h). This used to be a second chain of
    // sentinels here, and a session missing from it was disarmed the same
    // frame it was armed — silently, so the tool merely appeared to ignore
    // clicks.
    const ToolSession* session = ToolSessionFor(state.slotPick.featureId);
    const bool valid = session != nullptr
                           ? session->alive(state)
                           : (state.document != nullptr &&
                              state.document->FindFeatureById(
                                  state.slotPick.featureId) != nullptr);
    if (!valid) {
      state.slotPick = {}; // the slot ends with its feature/session
    } else if (esc) {
      state.slotPick = {};
      escUsed = true;
    }
  } else if (state.editFeatureId != 0 && esc) {
    state.editFeatureId = 0; // Escape closes the editor session
    escUsed = true;
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

  // Delete removes the selection — features on a part tab, occurrences on
  // an assembly tab, operations on a job tab (ListSelection.h: one verb for
  // every list). HERE rather than in the keymap, deliberately: Escape, Enter,
  // Delete and the arrows resolve against armed tools, previews and text
  // fields across ~15 call sites, so they are modal verbs rather than
  // rebindable commands (Keymap.h says so). Which means this must yield to
  // everything that could also want it — a mate session being picked into,
  // an armed slot, a typing field, a sketch being edited (its entities own
  // Delete, SketchInteraction), an open feature editor — and it does, by
  // asking last.
  if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::GetIO().WantTextInput &&
      !state.slotPick.Active() && !state.mateTool.active &&
      !state.fastenerTool.open && !state.bearingTool.open &&
      !state.sketchEdit.IsEditing() && state.editFeatureId == 0) {
    DeleteSelection(state);
  }

  // Assembly tab: occurrences select through the pipeline; double-click
  // enters the referenced part in context (assembly ghosted, A4). Mate
  // connector picks flow through the pipeline too (U7c: Kind::Connector).
  if (state.assembly) {
    // Escape cancels an in-progress mate session (after the armed-slot
    // branch above disarmed the slot: the usual two-step cascade).
    if (state.mateTool.active && esc && !escUsed) {
      CancelMateTool(state);
      escUsed = true;
    }

    UpdateHandleHover(state, overViewport);

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

    // The section arrow, which is the only direct-manipulation handle an
    // assembly tab has. After the gizmo (the assembly's own handle wins a
    // tie) and before selection, or the click reaches the picker and selects
    // whatever part sits behind the arrow.
    if (RouteHandleInput(state, overViewport))
      return;

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

  // A JOB tab's one handle is the placement gizmo on the selected part
  // (docs/cam/gui.md#gizmos): first shot at clicks, like the assembly's
  // above, and a miss falls through to picking.
  if (state.job) {
    UpdateHandleHover(state, overViewport);
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

  UpdateHandleHover(state, overViewport);

  // Direct-manipulation handles: first shot at clicks, exactly like the
  // assembly gizmo above — a live drag continues even off-viewport, a hit
  // consumes the click, and a miss falls through to the pipeline so armed
  // slots / picking are untouched.
  //
  // Asks the handle layer whether a handle EXISTS rather than testing for a
  // pending feature: the section arrow belongs to no feature, and this gate
  // used to route every attempt to drag it into the picker, which selected
  // whatever part was behind the arrow.
  // Simulation force arrows first: in simulation mode they are the handles
  // on screen, and a missed grab must still fall through to face picking
  // (that is how a load gets placed at all).
  if (state.simulation.IsActive() && state.camera && state.faceSelector) {
    const bool clicked =
        overViewport && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    if (state.simulation.loadDrag.active || clicked) {
      const ImGuiIO& io2 = ImGui::GetIO();
      Ray ray = state.faceSelector->ScreenToWorldRay(
          io2.MousePos.x - state.viewportPos.x,
          io2.MousePos.y - state.viewportPos.y,
          static_cast<int>(state.viewportSize.x),
          static_cast<int>(state.viewportSize.y), *state.camera);
      if (HandleLoadDragInput(state, ray, clicked,
                              ImGui::IsMouseDown(ImGuiMouseButton_Left),
                              ImGui::IsMouseReleased(ImGuiMouseButton_Left)))
        return;
    }
  }

  if (RouteHandleInput(state, overViewport))
    return;

  // A primitive's ghost drags in its reference's frame
  // (ui/PrimitiveTool.h). After the handles, so its height arrow wins.
  if (HandlePrimitiveDragInput(state, overViewport))
    return;

  // The unified pick pipeline (U1b/U1c): default selection — edges/faces/
  // bodies, hover preview, Ctrl multi-select, same-spot cycling — or, while
  // a tool is armed, its filtered pick routed to that tool.
  HandleSelectionInput(state, overViewport, /*allowEscapeClear=*/!escUsed);
}

} // namespace ui
