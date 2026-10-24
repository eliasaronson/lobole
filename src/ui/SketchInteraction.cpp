// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SketchInteraction.h"
#include "ui/Overlays.h"
#include "ui/SelectionPipeline.h"
#include "ui/SketchInteractionInternal.h"

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
#include "ui/ExprComplete.h"  // variable completion in the dim editors
#include "ui/PrimitiveTool.h" // the Box/Cylinder/Sphere tool borrows the pick
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

namespace ui {

// A template, so it lives with the router that instantiates it: the collect
// phase takes the caller's "what to do when the selection is complete" as a
// lambda, and a lambda in HandleSketchViewport cannot name a definition in
// another translation unit.
// Collect-phase interaction shared by the select-first generative tools
// (Mirror step 1, Pattern, Offset): a click toggles the whole entity in the
// selection, an empty-space DRAG box-selects (additive, window/crossing),
// and an empty-space TAP or Enter calls `finish` — only once something is
// selected. Escape stays the abort path (the ladder in HandleSketchViewport).
template <class F>
void HandleCollectPhase(AppState& state, SketchFeature& sketch,
                        const glm::vec2& mousePos, bool hoverHit,
                        uint32_t hoverEntity, F&& finish) {
  SketchEditState& edit = state.sketchEdit;
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (hoverHit) {
      bool removed = false;
      for (size_t i = edit.selected.size(); i-- > 0;)
        if (edit.selected[i].entity == hoverEntity) {
          edit.selected.erase(edit.selected.begin() + i);
          removed = true;
        }
      if (!removed)
        edit.selected.push_back({hoverEntity, sketch::kWholeEntity});
    } else {
      edit.boxActive = true;
      edit.boxStart = edit.boxEnd = mousePos;
    }
  }
  if (edit.boxActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
    edit.boxEnd = mousePos;
  if (edit.boxActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    if (glm::length(edit.boxEnd - edit.boxStart) < 4.0f) {
      if (!edit.selected.empty())
        finish(); // tap in empty space: confirm/continue
    } else {
      const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
      auto project = [&](glm::dvec2 p) {
        return SketchUVToScreen(state, sketch.plane, p);
      };
      for (uint32_t id : sketch::BoxSelect(sketch.entities, edit.boxStart,
                                           edit.boxEnd, crossing, project)) {
        bool seen = false;
        for (const auto& r : edit.selected)
          seen |= (r.entity == id);
        if (!seen)
          edit.selected.push_back({id, sketch::kWholeEntity});
      }
    }
    edit.boxActive = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Enter) && !edit.selected.empty())
    finish();
}

bool HandleSketchViewport(AppState& state, const glm::vec2& mousePos,
                          bool overViewport) {
  SketchEditState& edit = state.sketchEdit;

  // --- Plane picking -------------------------------------------------------
  if (edit.pickingPlane) {
    edit.hoveredPlane = -1;
    edit.hoveredDatumId = 0;
    if (overViewport) {
      Ray ray = ScreenRay(state, mousePos);
      // A model face wins whenever the ray hits one (faces are solid; you're
      // pointing at the body). The default/datum planes are only candidates
      // where no face is under the cursor — and only within their finite quad,
      // since the planes are infinite and would otherwise be "hit" everywhere.
      RayHit hit = state.faceSelector->PickFace(ray);
      // A section cap is not a face you can sketch on: it has no name, and
      // the face behind it is a wall the user did not point at. Treated as
      // no hit, so the datum planes below stay reachable through a cut.
      if (hit.cap)
        hit = RayHit{};
      int plane = -1;
      uint64_t datumId = 0;
      if (!hit.hit) {
        const float halfExtent = ScenePlaneHalfExtent(state);
        float bestDist = 1e30f;
        for (int i = 0; i < 3; i++) {
          SketchPlane p = DefaultPlane(i);
          if (auto uv = RayToPlaneUV(ray, p)) {
            if (std::abs(uv->x) > halfExtent || std::abs(uv->y) > halfExtent)
              continue; // ray misses the visible quad
            const float dist = static_cast<float>(
                glm::length(p.PlaneToWorld(*uv) - glm::dvec3(ray.origin)));
            if (dist < bestDist) {
              bestDist = dist;
              plane = i;
            }
          }
        }
        // Datum planes compete on the same nearest-hit basis as base planes.
        for (size_t i = 0; i < state.document->FeatureCount(); i++) {
          auto* dp =
              dynamic_cast<DatumPlaneFeature*>(state.document->GetFeature(i));
          if (!dp)
            continue;
          if (auto uv = RayToPlaneUV(ray, dp->OutputPlane())) {
            if (std::abs(uv->x) > halfExtent || std::abs(uv->y) > halfExtent)
              continue;
            const float dist = static_cast<float>(glm::length(
                dp->OutputPlane().PlaneToWorld(*uv) - glm::dvec3(ray.origin)));
            if (dist < bestDist) {
              bestDist = dist;
              plane = -1; // a datum beat the base planes
              datumId = dp->GetId();
            }
          }
        }
      }
      edit.hoveredPlane = plane;
      edit.hoveredDatumId = datumId;

      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const int n = static_cast<int>(state.document->FeatureCount()) + 1;
        const std::string name = "Sketch " + std::to_string(n);
        // Reattach mode: a named face re-points an existing sketch's faceRef.
        if (edit.reattachSketchId != 0) {
          if (hit.hit) {
            if (auto ref = FaceElementRef(state, hit.meshId, hit.faceIndex))
              ReattachSketchFace(state, edit.reattachSketchId, *ref);
          }
          edit.pickingPlane = false;
          edit.reattachSketchId = 0;
        } else if (edit.purpose == SketchPurpose::Primitive ||
                   edit.purpose == SketchPurpose::Template) {
          // The Box / Cylinder / Sphere tool borrows this pick: what was
          // clicked is what the primitive stands on. A refusal (a curved
          // face) stays armed and says why; success ends the pick.
          std::string why;
          if (hit.hit) {
            why = BeginPlacedOnFace(state, hit.meshId, hit.faceIndex);
          } else if (datumId != 0 || plane >= 0) {
            primitive::On on;
            if (datumId != 0) {
              on.kind = primitive::OnKind::PlaneFeature;
              on.featureId = datumId;
            } else {
              on.kind = primitive::OnKind::WorldPlane;
              on.world = static_cast<PlaneType>(plane);
            }
            why = BeginPlacedOn(state, on);
          }
          if (!why.empty())
            edit.actionMsg = why;
          return true; // `edit` was reset on success; touch nothing more
        } else if (hit.hit && edit.purpose == SketchPurpose::Hole) {
          // Hole sessions demand a genuinely planar face; a refusal stays
          // armed with an explanation rather than making a bad sketch.
          if (!BeginHoleSketchOnFace(state, hit.meshId, hit.faceIndex))
            edit.actionMsg = "Hole: that face is curved - pick a flat one.";
        } else if (hit.hit) {
          // The B-rep face's own frame when it is planar (a plane baked from
          // the render mesh is a float approximation of it); the mesh's
          // otherwise.
          glm::dvec3 c, nrm, u;
          if (!ExactFacePlane(state, hit.meshId, hit.faceIndex, c, nrm, u)) {
            c = state.faceSelector->GetFaceCenter(hit.meshId, hit.faceIndex);
            nrm = state.faceSelector->GetFaceNormal(hit.meshId, hit.faceIndex);
            u = std::abs(nrm.y) < 0.9 ? glm::dvec3(0, 1, 0)
                                      : glm::dvec3(1, 0, 0);
            u = glm::normalize(u - glm::dot(u, nrm) * nrm);
          }
          const SketchPlane facePlane(c, nrm, u);
          // Attach to the face by its stable name when it has one; else bake.
          if (auto ref = FaceElementRef(state, hit.meshId, hit.faceIndex)) {
            BeginSketchOnFace(state, *ref, facePlane, name);
          } else {
            BeginSketchOnPlane(state, facePlane, name);
          }
        } else if (datumId != 0) {
          if (auto* dp = dynamic_cast<DatumPlaneFeature*>(
                  state.document->FindFeatureById(datumId)))
            BeginSketchOnDatum(state, *dp, name, edit.purpose);
        } else if (plane >= 0) {
          BeginSketchOnPlane(state, DefaultPlane(plane), name, edit.purpose);
        }
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      // Reset the whole session state, not just the flag: `reattachSketchId`
      // (and any future arming intent) would otherwise survive the cancel and
      // hijack the next sketch.
      edit = SketchEditState{};
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
  if (edit.tool != SketchTool::Trim && edit.trimPathActive) {
    edit.trimPathActive = false; // the tool changed under a drag
    edit.trimPath.clear();
    edit.trimPathPreview.clear();
  }

  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    // Back out one level per press: close the inline dimension editor →
    // cancel an in-progress action → return to the Select tool → clear the
    // selection → leave sketch edit.
    if (edit.editingDim >= 0 || edit.dimOnCreateOpen) {
      // The inline editor / dimension-on-create field (drawn later this
      // frame) consumes it.
    } else if (edit.tool == SketchTool::Dimension && edit.dimPlacing) {
      edit.dimPlacing = false; // back to target accumulation (targets kept)
    } else if (edit.trimPathActive) {
      edit.trimPathActive = false; // drop the drag-trim path, trim nothing
      edit.trimPath.clear();
      edit.trimPathPreview.clear();
    } else if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
      edit.splinePoles.clear(); // cancel the in-progress spline
    } else if (edit.hasPendingPoint || edit.hasPendingPoint2 ||
               edit.hasPendingPoint3 || edit.hasPendingPoint4 ||
               edit.boxActive) {
      edit.hasPendingPoint = false;
      edit.hasPendingPoint2 = false;
      edit.hasPendingPoint3 = false;
      edit.hasPendingPoint4 = false;
      edit.boxActive = false;
    } else if (edit.tool == SketchTool::Mirror && edit.mirrorAxisPhase) {
      edit.mirrorAxisPhase = false; // axis-pick → back to collecting geometry
    } else if (edit.opPickSources || edit.opPickAxis) {
      edit.opPickSources = edit.opPickAxis = false; // back to the session
      edit.selected.clear();
    } else if (edit.editOpIndex >= 0) {
      edit.editOpIndex = -1; // close the derived-op edit session
    } else if (edit.tool == SketchTool::Dimension && !edit.selected.empty()) {
      // Clear the accumulated targets first; a second Esc disarms the tool.
      edit.selected.clear();
      edit.dimPickBody.clear();
    } else if (edit.tool != SketchTool::Select) {
      edit.tool = SketchTool::Select;
      edit.mirrorAxisPhase = false;
      edit.actionMsg.clear();
    } else if (edit.selectedConstraint >= 0) {
      edit.selectedConstraint = -1; // deselect the constraint first
    } else if (!edit.selected.empty()) {
      edit.selected.clear();
    } else if (edit.purpose == SketchPurpose::Hole) {
      CancelHoleSession(state); // discards the sketch if nothing was drawn
    } else {
      edit = SketchEditState{}; // leave sketch edit
    }
    return true;
  }

  // Enter ends centre placement and opens the hole options — the ✓ button's
  // keyboard twin. Guarded like the Escape ladder: a live value field owns
  // the key. (Sketch mode has no other global Enter binding.)
  if (edit.purpose == SketchPurpose::Hole && !ImGui::GetIO().WantTextInput &&
      edit.editingDim < 0 && !edit.dimOnCreateOpen &&
      ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false)) {
    BeginSketchHole(state);
    return true;
  }

  // The sketch keymap moved to the one command table (ui/Commands.cpp), which
  // DrawUI dispatches before this runs. The re-resolve stays: a binding can
  // apply a constraint, which REPLACES the sketch feature (ModifyFeatureCommand
  // swaps the object), and the pointer above is then a dangling free. (Latent
  // use-after-free; it started crashing once R3's regeneration allocations
  // reused the freed block.)
  sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return true; // the shortcut ended sketch edit; viewport was consumed

  if (!overViewport)
    return true;

  Ray ray = ScreenRay(state, mousePos);

  // Project tool picks MODEL geometry (a body edge/face), not a plane point.
  if (edit.tool == SketchTool::Project)
    return HandleProjectTool(state, *sketch, index, ray);

  auto uvOpt = RayToPlaneUV(ray, sketch->plane);
  if (!uvOpt)
    return true;
  glm::dvec2 uv = *uvOpt;

  // Auto-constraint inference for the drawing tools: snap the cursor to nearby
  // geometry so the preview and the click both use the snapped point, and stash
  // the result for the ghost-glyph overlay. Ctrl or the toggle suppresses it.
  // Segment-level snaps (H/V/parallel/perp against the previous point) apply to
  // the Line tool and to Spline pole placement (relative to the last pole); the
  // other tools snap point-by-point.
  const bool drawTool =
      edit.tool == SketchTool::Point || edit.tool == SketchTool::Line ||
      edit.tool == SketchTool::Circle || edit.tool == SketchTool::Rectangle ||
      edit.tool == SketchTool::RectangleCenter ||
      edit.tool == SketchTool::Arc || edit.tool == SketchTool::ArcThreePoint ||
      edit.tool == SketchTool::Ellipse || edit.tool == SketchTool::EllipseArc ||
      edit.tool == SketchTool::Spline;
  const bool infer = drawTool && edit.autoConstrain && !ImGui::GetIO().KeyCtrl;
  if (infer) {
    const float ppu = PixelsPerUV(state, sketch->plane, uv);
    // The fixed "other end" for segment-level H/V/parallel/perp snapping: the
    // pending Line start, or the last placed Spline pole.
    std::optional<glm::dvec2> other;
    if (edit.tool == SketchTool::Line && edit.hasPendingPoint)
      other = edit.pendingPoint;
    else if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty())
      other = edit.splinePoles.back();
    // While placing a spline, the poles dropped so far this session aren't in
    // the sketch yet, so InferPoint can't see them. Expose them as Point
    // snap-targets so a new pole can go coincident with / H/V-aligned to an
    // earlier pole (and snap onto the first pole to close the loop). Synthetic
    // ids in a reserved range below kDatumBase; never committed as refs (the
    // spline commits with inference off).
    sketch::Snap snap;
    if (edit.tool == SketchTool::Spline && !edit.splinePoles.empty()) {
      std::vector<sketch::Entity> withPoles = sketch->entities;
      uint32_t synthId = 0xFFF00000u;
      for (const glm::dvec2& pole : edit.splinePoles) {
        sketch::Point pt;
        pt.p = pole;
        pt.id = synthId++;
        withPoles.push_back(pt);
      }
      snap = sketch::InferPoint(withPoles, uv, other, 0, ppu);
    } else {
      snap = sketch::InferPoint(sketch->entities, uv, other, 0, ppu);
    }
    uv = snap.uv;
    edit.currentSnap = snap;
    edit.hasCurrentSnap = !snap.glyphs.empty();
  }
  edit.hoverPoint = uv;

  // A click-selected constraint (dimension pill / glyph) takes Delete
  // priority over entities — selection is mutually exclusive, so this is
  // unambiguous. Handled BEFORE the pill-hover gate below (the cursor is
  // usually still on the pill that was just clicked). Guarded against text
  // fields (Backspace edits text, not the sketch).
  if ((ImGui::IsKeyPressed(ImGuiKey_Delete) ||
       ImGui::IsKeyPressed(ImGuiKey_Backspace)) &&
      !ImGui::GetIO().WantTextInput && edit.selectedConstraint >= 0) {
    const size_t ci = static_cast<size_t>(edit.selectedConstraint);
    edit.selectedConstraint = -1;
    edit.editingDim = -1; // indices shift; don't leave the editor dangling
    DeleteConstraint(state, ci); // replaces the sketch feature
    return true;
  }

  // A dimension pill under the cursor (or mid-drag) owns the mouse —
  // DrawDimensions runs the drag/inline-edit; don't also draw/select under it.
  if (edit.dimHot >= 0 || edit.dimDrag >= 0)
    return true;

  // Derived-op re-pick modes (armed by the op edit session's rows):
  // sources via the shared collect phase, the mirror axis via one click.
  // Independent of the active tool.
  if (edit.editOpIndex >= 0 && (edit.opPickSources || edit.opPickAxis)) {
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit)
      edit.hoveredRef = hover.ref;
    if (edit.opPickAxis) {
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover.hit) {
        if (EditOpAxis(state, static_cast<size_t>(edit.editOpIndex),
                       hover.ref.entity))
          edit.opPickAxis = false;
      }
      return true;
    }
    HandleCollectPhase(
        state, *sketch, mousePos, hover.hit, hover.ref.entity, [&] {
          std::vector<uint32_t> ids;
          for (const auto& r : edit.selected)
            ids.push_back(r.entity);
          if (EditOpSources(state, static_cast<size_t>(edit.editOpIndex),
                            ids)) {
            edit.opPickSources = false;
            edit.selected.clear();
          }
        });
    return true;
  }

  if (edit.tool == SketchTool::Select) {
    // Track what the cursor is over for hover highlighting (endpoint-first,
    // then entity body).
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    edit.hoveredBody = hover.body;
    edit.hoveredRef = hover.ref;

    // A constraint glyph under the cursor owns clicks within its rect (it
    // may overlap an entity — the glyph wins there).
    if (edit.glyphHot >= 0) {
      edit.hasHover = false;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        edit.selectedConstraint = edit.glyphHot;
        edit.selected.clear(); // constraint vs entity Delete stays unambiguous
      }
      return true;
    }

    // Delete removes the selected entities (and cleans up their constraints).
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) && !ImGui::GetIO().WantTextInput &&
        !edit.selected.empty()) {
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
      sketch->RegenerateDerived();
      state.document->Regenerate();
      nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
      if (after != before)
        state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *state.document);
      if (dropped > 0)
        LOG_INFO("Deleted {} dependent constraint(s)", dropped);
      return true;
    }
    // Click selects an element (for constraints) and begins a drag: an endpoint
    // moves that point, a line body translates the whole line, a circle rim
    // resizes it. Empty click clears the selection.
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const glm::vec2 pos(mousePos.x, mousePos.y);
      const std::vector<SketchPick> candidates =
          PickSketchCandidates(state, *sketch, pos, kSketchBodyTolPx);
      const bool samePos = glm::length(pos - edit.lastClickPos) < 6.0f;
      edit.lastClickPos = pos;
      if (!candidates.empty()) {
        // Same-spot repeat cycles the stacked candidates (endpoint → line
        // body → …, the U1 model-space convention); a fresh spot takes the
        // best one — points outrank bodies.
        edit.pickCycle =
            samePos ? (edit.pickCycle + 1) % static_cast<int>(candidates.size())
                    : 0;
        const SketchPick pick = candidates[static_cast<size_t>(edit.pickCycle)];
        if (!ImGui::GetIO().KeyCtrl)
          edit.selected.clear();
        edit.selectedConstraint = -1; // entity selection replaces it
        edit.selected.push_back(pick.ref);
        BeginDragSession(state, *sketch, index, pick, uv);
      } else {
        edit.pickCycle = 0;
        // Missed geometry: start a rubber-band box (don't clear yet — a plain
        // click clears on release).
        edit.boxActive = true;
        edit.boxStart = edit.boxEnd = mousePos;
      }
    }
    if (edit.boxActive && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      edit.boxEnd = mousePos;
    if (edit.boxActive && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      const bool add = ImGui::GetIO().KeyCtrl;
      if (glm::length(edit.boxEnd - edit.boxStart) < 4.0f) {
        if (!add) {
          edit.selected.clear(); // tiny box == a plain empty click
          edit.selectedConstraint = -1;
        }
      } else {
        const bool crossing = edit.boxEnd.x < edit.boxStart.x; // right→left
        auto project = [&](glm::dvec2 p) {
          return SketchUVToScreen(state, sketch->plane, p);
        };
        auto ids = sketch::BoxSelect(sketch->entities, edit.boxStart,
                                     edit.boxEnd, crossing, project);
        if (!add)
          edit.selected.clear();
        for (uint32_t id : ids)
          edit.selected.push_back({id, sketch::kWholeEntity});
      }
      edit.boxActive = false;
    }
    if (state.sketchDrag.active && ImGui::IsMouseDown(ImGuiMouseButton_Left))
      UpdateDragSession(state, *sketch, uv);
    if (state.sketchDrag.active &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Left))
      EndDragSession(state, *sketch);
    return true;
  }

  if (edit.tool == SketchTool::Trim) {
    // Click a line segment to remove the piece under the cursor (bounded by
    // its crossings). Preview highlights just that sub-segment (not the whole
    // line) so it's clear what the click deletes.
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = false; // whole-entity highlight suppressed in Trim
    edit.hasTrimPreview = false;
    // Pressed on empty space and dragged: a path, and every span it crosses
    // goes on release, one solve and one undo step
    // (docs/sketching/README.md#trim). A press released where it began is
    // the click that leaves the tool.
    if (edit.trimPathActive) {
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float ppu = PixelsPerUV(state, sketch->plane, uv);
        if (glm::length(uv - edit.trimPath.back()) * ppu >= 2.0f) {
          edit.trimPath.push_back(uv);
          SketchFeature trial = *sketch;
          edit.trimPathPreview.clear();
          trial.TrimAlongPath(edit.trimPath, &edit.trimPathPreview);
        }
        return true;
      }
      const std::vector<glm::dvec2> path = std::move(edit.trimPath);
      edit.trimPathActive = false;
      edit.trimPath.clear();
      edit.trimPathPreview.clear();
      const float moved = ImGui::GetIO().MouseDragMaxDistanceSqr[0];
      if (path.size() < 2 || moved < 16.0f) {
        edit.tool = SketchTool::Select; // a plain click: done trimming
        return true;
      }
      nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
      if (sketch->TrimAlongPath(path) > 0) {
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
      }
      return true;
    }
    if (hover.hit) {
      if (auto span = sketch->TrimSpanAt(hover.ref.entity, uv)) {
        edit.hasTrimPreview = true;
        edit.trimPreviewA = span->first;
        edit.trimPreviewB = span->second;
      }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover.hit) {
      // Generated instances are re-stamped from their source on every
      // regeneration — a trim would apply and then silently revert. Guard
      // with guidance instead of the confusing no-op.
      bool derivedInstance = false;
      for (const auto& op : sketch->derived)
        for (const auto& in : op.instances)
          derivedInstance |= (in.genId == hover.ref.entity);
      if (derivedInstance) {
        edit.actionMsg =
            "This entity is generated by a pattern/mirror — trim its source, "
            "or delete the operation to free the copies.";
        return true;
      }
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
        sketch->RegenerateDerived();
        state.document->Regenerate();
        nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
        state.commands->Execute(
            std::make_unique<ModifyFeatureCommand>(index, before, after),
            *state.document);
      }
    }
    // A press in empty space starts a path (a tap there, released without
    // a drag, still finishes the tool); Enter finishes it (Escape = abort).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hover.hit) {
      edit.trimPathActive = true;
      edit.trimPath = {uv};
      edit.trimPathPreview.clear();
    } else if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      edit.tool = SketchTool::Select;
    }
    return true;
  }

  // Load → Solve → WriteBack → Regenerate → one command, given the pre-edit
  // snapshot. Shared by the generative tools (mirror/fillet).
  auto commitMutation = [&](const nlohmann::json& before) {
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
  };

  if (edit.tool == SketchTool::Mirror) {
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit)
      edit.hoveredRef = hover.ref;

    if (!edit.mirrorAxisPhase) {
      // Step 1 — collect the geometry to mirror (shared collect phase:
      // click toggles, drag box-selects, tap/Enter continues to the axis).
      HandleCollectPhase(state, *sketch, mousePos, hover.hit, hover.ref.entity,
                         [&] { edit.mirrorAxisPhase = true; });
      return true;
    }

    // Step 2 — click a line (drawn, construction, or an X/Y datum axis).
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hover.hit) {
      const size_t ei = sketch->IndexOfEntity(hover.ref.entity);
      const bool isLine =
          sketch::IsDatum(hover.ref.entity) ||
          (ei != SIZE_MAX &&
           std::holds_alternative<sketch::Line>(sketch->entities[ei]));
      if (!isLine) {
        edit.actionMsg = "Mirror: the axis must be a line or an X/Y axis.";
      } else {
        CommitMirrorAcross(state, *sketch, index, hover.ref.entity);
      }
    }
    return true;
  }

  if (edit.tool == SketchTool::Pattern || edit.tool == SketchTool::Offset) {
    // Circular centre pick (v51): the session's Pick button armed it — this
    // click sets the centre. Hitting an entity POINT stores its reference so
    // the pattern tracks it; empty space bakes the raw coordinate.
    if (edit.patternPickCenter &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      const SketchPick pt = PickSketchPoint(
          state, *sketch, glm::vec2(mousePos.x, mousePos.y), 16.0f);
      if (pt.hit && !pt.body) {
        edit.patternCenterRef = pt.ref;
        const size_t pi = sketch->IndexOfEntity(pt.ref.entity);
        if (pi != SIZE_MAX)
          edit.patternCenter =
              sketch::EntityPointUV(sketch->entities[pi], pt.ref.which);
      } else {
        edit.patternCenterRef = {0, 0};
        edit.patternCenter = uv;
      }
      edit.patternPickCenter = false;
      return true;
    }
    // In-tool selection (same collect phase as Mirror): click toggles, drag
    // box-selects; a TAP in empty space or Enter = Apply (the session
    // window's button); Escape aborts via the ladder.
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit)
      edit.hoveredRef = hover.ref;
    const bool pattern = edit.tool == SketchTool::Pattern;
    HandleCollectPhase(state, *sketch, mousePos, hover.hit, hover.ref.entity,
                       [&] {
                         if (pattern)
                           ApplyPattern(state);
                         else
                           ApplyOffset(state);
                       });
    return true;
  }

  if (edit.tool == SketchTool::Image) {
    // The reference-image session lives in the Tool window; on the canvas,
    // grabbing anywhere on the selected image DRAGS it to a new position.
    // Other clicks are consumed doing nothing (Esc / Done closes).
    sketch::ReferenceImage* img = nullptr;
    for (auto& im : sketch->images)
      if (im.id == edit.selectedImageId)
        img = &im;
    if (img == nullptr)
      return true;
    if (edit.imageDragActive) {
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // One undo entry for the whole drag (the opacity-slider recipe).
        edit.imageDragActive = false;
        if (!edit.imageEditBaseline.is_null()) {
          nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
          if (after != edit.imageEditBaseline)
            state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
                index, edit.imageEditBaseline, after));
          edit.imageEditBaseline = nlohmann::json();
        }
      } else {
        img->origin = edit.imageDragStartOrigin + (uv - edit.imageDragGrabUV);
      }
      return true;
    }
    // Point-in-image: invert  origin + R(rotation) * (px,py)*unitsPerPixel.
    const auto overImage = [&](glm::dvec2 p) {
      const glm::dvec2 d = p - img->origin;
      const double cs = std::cos(-img->rotation);
      const double sn = std::sin(-img->rotation);
      const glm::dvec2 px =
          glm::dvec2(cs * d.x - sn * d.y, sn * d.x + cs * d.y) /
          img->unitsPerPixel;
      return px.x >= 0.0 && px.x <= img->widthPx && px.y >= 0.0 &&
             px.y <= img->heightPx;
    };
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overImage(uv)) {
      edit.imageDragActive = true;
      edit.imageDragGrabUV = uv;
      edit.imageDragStartOrigin = img->origin;
      edit.imageEditBaseline = DocumentIO::FeatureToJson(*sketch);
    }
    return true;
  }

  if (edit.tool == SketchTool::CalibrateImage) {
    // Capture two plane-UV points on the reference image (e.g. the two ends of
    // a printed dimension line). The Sketch panel then prompts for the real
    // distance between them and applies the scale (stage 2). Uses the raw plane
    // point — no entity snapping, since the target is the image, not geometry.
    if (edit.calibImageId == 0) {
      edit.actionMsg =
          "Select a reference image and press Calibrate in the Sketch panel.";
      return true;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      if (edit.calibStage == 0) {
        edit.calibP1 = uv;
        edit.calibStage = 1;
      } else if (edit.calibStage == 1) {
        edit.calibP2 = uv;
        edit.calibStage = 2; // the panel now shows the distance field
      }
    }
    return true;
  }

  if (edit.tool == SketchTool::Fillet) {
    // Click a corner: fillet the two lines sharing the nearest endpoint.
    SketchPick vtx = PickSketchPoint(state, *sketch,
                                     glm::vec2(mousePos.x, mousePos.y), 16.0f);
    edit.hasHover = vtx.hit;
    if (vtx.hit)
      edit.hoveredRef = vtx.ref;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && vtx.hit) {
      const size_t vi = sketch->IndexOfEntity(vtx.ref.entity);
      if (vi != SIZE_MAX) {
        const glm::dvec2 cornerUV =
            sketch::EntityPointUV(sketch->entities[vi], vtx.ref.which);
        std::vector<uint32_t> lines; // lines with an endpoint at the corner
        for (const auto& e : sketch->entities)
          if (auto* l = std::get_if<sketch::Line>(&e))
            if (glm::length(l->a - cornerUV) < 1e-3f ||
                glm::length(l->b - cornerUV) < 1e-3f)
              lines.push_back(sketch::EntityId(e));
        if (lines.size() >= 2) {
          nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
          if (sketch->FilletCorner(lines[0], lines[1], edit.filletRadius)) {
            commitMutation(before);
            edit.actionMsg.clear();
          } else {
            edit.actionMsg = "Fillet: radius " +
                             std::to_string(edit.filletRadius) +
                             " doesn't fit this corner — try a smaller radius.";
          }
        } else {
          edit.actionMsg = "Fillet: click a corner where two lines meet.";
        }
      }
    }
    // A tap in empty space or Enter finishes the tool (Escape = abort).
    if ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !vtx.hit) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      edit.tool = SketchTool::Select;
      edit.actionMsg.clear();
    }
    return true;
  }

  if (edit.tool == SketchTool::Dimension) {
    if (edit.dimPlacing) {
      // Placement stage: the ghost (DrawDimensions) follows the cursor; a
      // click commits the kind + label position. Single-line length
      // placement first offers the click to refinement (B): landing on
      // another line/point switches the dimension kind instead of placing.
      // Highlight the pickable candidate under the cursor so the user sees
      // what a refine click would take (same hover as first-pick).
      if (edit.dimPlaceFromLine != 0) {
        SketchPick hover =
            PickSketchEntity(state, *sketch, glm::vec2(mousePos.x, mousePos.y),
                             kSketchBodyTolPx);
        // The measured line itself can't refine (it just places); don't
        // highlight it as a target.
        if (hover.hit && hover.ref.entity != edit.dimPlaceFromLine) {
          edit.hasHover = true;
          edit.hoveredBody = hover.body;
          edit.hoveredRef = hover.ref;
        }
      }
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (edit.dimPlaceFromLine != 0) {
          const SketchPick pick = PickSketchEntity(
              state, *sketch, glm::vec2(mousePos.x, mousePos.y),
              kSketchBodyTolPx);
          if (pick.hit && RefineLinePlacement(state, *sketch, pick))
            return true;
        }
        // May replace the sketch feature — the local pointer dies here.
        CommitPlacedDimension(state, *sketch, uv);
      }
      return true;
    }
    SketchPick hover = PickSketchEntity(
        state, *sketch, glm::vec2(mousePos.x, mousePos.y), kSketchBodyTolPx);
    edit.hasHover = hover.hit;
    if (hover.hit) {
      edit.hoveredBody = hover.body;
      edit.hoveredRef = hover.ref;
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      // May replace the sketch feature through the command system — the
      // local pointer is dead after this call.
      HandleDimensionClick(state, *sketch, hover);
    }
    return true;
  }

  // Spline: commit the accumulated poles as a BSpline (>= 2 needed). May
  // replace the sketch feature, so the local pointer dies — return after.
  auto finishSpline = [&]() {
    if (edit.splinePoles.size() >= 2) {
      sketch::BSpline bs;
      bs.poles = edit.splinePoles;
      CommitEntity(state, *sketch, index, bs, false);
    }
    edit.splinePoles.clear();
  };
  if (edit.tool == SketchTool::Spline && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
    finishSpline();
    return true;
  }

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    if (edit.tool == SketchTool::Spline) {
      // A double-click finishes; a single click adds another pole. On the
      // double-click frame the first click already placed its pole, so just
      // finish without appending a duplicate.
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        finishSpline();
      else
        edit.splinePoles.push_back(uv);
    } else if (edit.tool == SketchTool::Point) {
      CommitEntity(state, *sketch, index, sketch::Point{uv}, infer);
    } else if (edit.tool == SketchTool::Text) {
      PlaceSketchText(state, uv); // may replace the sketch feature
      return true;
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
          arc = sketch::ArcCenterStartEnd(edit.pendingPoint, edit.pendingPoint2,
                                          uv);
        else
          arc = sketch::ArcFromThreePoints(edit.pendingPoint, uv,
                                           edit.pendingPoint2);
        if (arc)
          CommitEntity(state, *sketch, index, *arc, infer);
        edit.hasPendingPoint = false;
        edit.hasPendingPoint2 = false;
      }
    } else if (edit.tool == SketchTool::Ellipse) {
      // 3-click: centre → major-axis end → minor radius.
      if (!edit.hasPendingPoint) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else if (!edit.hasPendingPoint2) {
        edit.pendingPoint2 = uv;
        edit.hasPendingPoint2 = true;
      } else {
        sketch::Ellipse el = sketch::EllipseFromClicks(edit.pendingPoint,
                                                       edit.pendingPoint2, uv);
        if (el.rMajor > 1e-6 && el.rMinor > 1e-6)
          CommitEntity(state, *sketch, index, el, infer);
        edit.hasPendingPoint = false;
        edit.hasPendingPoint2 = false;
      }
    } else if (edit.tool == SketchTool::EllipseArc) {
      // 5-click: centre → major-axis end → minor radius → start → end. The
      // first three define the ellipse; the last two pick the swept params.
      if (!edit.hasPendingPoint) {
        edit.pendingPoint = uv;
        edit.hasPendingPoint = true;
      } else if (!edit.hasPendingPoint2) {
        edit.pendingPoint2 = uv;
        edit.hasPendingPoint2 = true;
      } else if (!edit.hasPendingPoint3) {
        edit.pendingPoint3 = uv;
        edit.hasPendingPoint3 = true;
      } else if (!edit.hasPendingPoint4) {
        edit.pendingPoint4 = uv;
        edit.hasPendingPoint4 = true;
      } else {
        const sketch::Ellipse el = sketch::EllipseFromClicks(
            edit.pendingPoint, edit.pendingPoint2, edit.pendingPoint3);
        if (el.rMajor > 1e-6 && el.rMinor > 1e-6) {
          double sp = sketch::EllipseParamOfPoint(
              el.center, el.rMajor, el.rMinor, el.rotation, edit.pendingPoint4);
          double ep = sketch::EllipseParamOfPoint(el.center, el.rMajor,
                                                  el.rMinor, el.rotation, uv);
          // CCW-always: keep the swept range (ep - sp) in (0, 2π].
          while (ep <= sp)
            ep += glm::two_pi<double>();
          CommitEntity(state, *sketch, index,
                       sketch::EllipseArc{el.center, el.rMajor, el.rMinor,
                                          el.rotation, sp, ep},
                       infer);
        }
        edit.hasPendingPoint = edit.hasPendingPoint2 = false;
        edit.hasPendingPoint3 = edit.hasPendingPoint4 = false;
      }
    } else if (!edit.hasPendingPoint) {
      edit.pendingPoint = uv;
      edit.hasPendingPoint = true;
    } else {
      const glm::dvec2 a = edit.pendingPoint;
      switch (edit.tool) {
      case SketchTool::Line:
        CommitLine(state, *sketch, index, a, uv, infer);
        break;
      case SketchTool::Circle:
        CommitEntity(state, *sketch, index,
                     sketch::Circle{a, glm::length(uv - a)}, infer);
        break;
      case SketchTool::Rectangle:
        CommitRectangle(state, *sketch, index, a, uv, infer);
        break;
      case SketchTool::RectangleCenter:
        CommitCenterRectangle(state, *sketch, index, a, uv, infer);
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

} // namespace ui
