#include "ui/SketchToolbar.h"

#include "core/features/ExtrudeFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"
#include "core/io/Serialization.h" // DocumentIO (break-link commit)
#include "core/sketch/SketchSolver.h"
#include "ui/AppActions.h"
#include "ui/FileDialogs.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include <cstdio>

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <set>

namespace ui {

namespace {
const char* ConstraintKindName(sketch::ConstraintKind k) {
  using K = sketch::ConstraintKind;
  switch (k) {
  case K::Coincident: return "coincident";
  case K::Horizontal: return "horizontal";
  case K::Vertical: return "vertical";
  case K::Parallel: return "parallel";
  case K::Perpendicular: return "perpendicular";
  case K::Equal: return "equal";
  case K::Distance: return "distance";
  case K::Radius: return "radius";
  case K::Angle: return "angle";
  case K::Tangent: return "tangent";
  case K::PointOnLine: return "point-on-line";
  case K::Symmetric: return "symmetric";
  case K::Midpoint: return "midpoint";
  case K::Fixed: return "fixed";
  case K::Concentric: return "concentric";
  case K::Collinear: return "collinear";
  case K::DistanceX: return "distance-x";
  case K::DistanceY: return "distance-y";
  case K::Diameter: return "diameter";
  case K::DistancePL: return "distance-pl";
  }
  return "?";
}
} // namespace

void DrawSketchToolbar(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  if (edit.pickingPlane) {
    ImGui::Begin("Sketch Tools");
    ImGui::TextWrapped(
        "Pick a plane or a model face to sketch on. (Esc to cancel)");
    if (ImGui::Button("Cancel"))
      edit.pickingPlane = false;
    ImGui::End();
    return;
  }

  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  // The tools + common constraints live in the top toolbar (Toolbar.cpp) now;
  // this panel holds the active tool's options, dimensions, extra constraints,
  // status, and Extrude.
  ImGui::Begin("Sketch");
  ImGui::TextUnformatted(sketch->GetName().c_str());
  ImGui::Separator();

  // Active-tool options.
  if (edit.tool == SketchTool::Mirror) {
    ImGui::TextUnformatted("Mirror: select geometry, then click an axis line.");
    ImGui::Checkbox("Linked##gen", &edit.linkGenerative);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Add symmetry constraints so the mirror tracks edits "
                        "(off = one-shot copy)");
  } else if (edit.tool == SketchTool::Fillet) {
    ImGui::TextUnformatted("Fillet: click a corner where two lines meet.");
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("Radius##fillet", &edit.filletRadius, 0.02f, 0.01f, 1e6f,
                     "%.3f");
  } else if (edit.tool == SketchTool::Pattern) {
    ImGui::TextUnformatted("Pattern: select geometry, set params, then Apply.");
    ImGui::RadioButton("Linear", !edit.patternCircular);
    if (ImGui::IsItemClicked())
      edit.patternCircular = false;
    ImGui::SameLine();
    ImGui::RadioButton("Circular", edit.patternCircular);
    if (ImGui::IsItemClicked())
      edit.patternCircular = true;
    ImGui::SetNextItemWidth(80);
    ImGui::DragInt("Count##pat", &edit.patternCount, 0.1f, 2, 1000);
    if (!edit.patternCircular) {
      ImGui::SetNextItemWidth(140);
      ImGui::DragFloat2("Step (dx,dy)##pat", &edit.patternStep.x, 0.05f);
    } else {
      ImGui::SetNextItemWidth(140);
      ImGui::DragFloat2("Centre##pat", &edit.patternCenter.x, 0.05f);
      ImGui::SetNextItemWidth(80);
      ImGui::DragFloat("Angle##pat", &edit.patternAngleDeg, 0.5f, -360.0f,
                       360.0f, "%.1f deg");
    }
    ImGui::Checkbox("Linked##pat", &edit.linkGenerative);
    ImGui::SameLine();
    ImGui::BeginDisabled(edit.selected.empty());
    if (ImGui::Button("Apply##pat"))
      ApplyPattern(state);
    ImGui::EndDisabled();
  } else if (edit.tool == SketchTool::Offset) {
    ImGui::TextUnformatted("Offset: select a closed loop, set distance, Apply.");
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("Distance##off", &edit.offsetDistance, 0.02f, -1e6f, 1e6f,
                     "%.3f");
    ImGui::SameLine();
    ImGui::Checkbox("Round corners##off", &edit.offsetJoinArc);
    ImGui::BeginDisabled(edit.selected.empty());
    if (ImGui::Button("Apply##off"))
      ApplyOffset(state);
    ImGui::EndDisabled();
  } else if (edit.tool == SketchTool::CalibrateImage) {
    if (edit.calibStage < 2) {
      ImGui::TextWrapped(
          "Click two points on the image a known distance apart "
          "(e.g. the ends of a printed dimension).");
    } else {
      ImGui::TextUnformatted("Real distance between the two points:");
      ImGui::SetNextItemWidth(120);
      ImGui::InputFloat("##calibdist", &edit.calibDistance, 0.0f, 0.0f, "%.4g");
      ImGui::Checkbox("Align to axis", &edit.calibAlignAxis);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Also rotate the image so the two points sit on the "
                          "horizontal axis");
      if (ImGui::Button("Apply##calib")) {
        CalibrateReferenceImageAction(state, edit.calibImageId, edit.calibP1,
                                      edit.calibP2, edit.calibDistance,
                                      edit.calibAlignAxis);
        edit.calibStage = 0;
        edit.calibImageId = 0;
        edit.tool = SketchTool::Select;
        ImGui::End(); // the ModifyFeatureCommand replaced the feature: `sketch`
        return;       // is now dangling — bail out and redraw next frame
      }
      ImGui::SameLine();
      if (ImGui::Button("Re-pick##calib"))
        edit.calibStage = 0;
    }
  }
  // actionMsg / staged guidance is shown in the viewport tool-prompt bar
  // (DrawToolPrompt), not duplicated here.

  ImGui::Checkbox("Auto-constrain", &edit.autoConstrain);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Snap and add constraints while drawing (hold Ctrl to "
                      "place freely) — Shift+S to toggle");

  // Reference images (v49): import an underlay, tune it, calibrate it to a real
  // dimension, or delete it. Opacity/hide are live cosmetic edits (not undoable);
  // import/calibrate/delete each commit one undo step and REPLACE the sketch
  // feature — so after any of them `sketch` dangles: End() + return immediately.
  ImGui::SeparatorText("Reference images");
  if (ImGui::Button("Insert image...##ref")) {
    OpenReferenceImageDialog(state); // may add + commit (feature replaced)
    ImGui::End();
    return;
  }
  for (size_t i = 0; i < sketch->images.size(); i++) {
    sketch::ReferenceImage& img = sketch->images[i];
    ImGui::PushID(static_cast<int>(img.id));
    const bool selected = edit.selectedImageId == img.id;
    char label[80];
    std::snprintf(label, sizeof(label), "Image %u  (%d x %d px)", img.id,
                  img.widthPx, img.heightPx);
    if (ImGui::Selectable(label, selected))
      edit.selectedImageId = selected ? 0 : img.id;
    if (edit.selectedImageId == img.id) {
      ImGui::SetNextItemWidth(160);
      float op = static_cast<float>(img.opacity);
      if (ImGui::SliderFloat("Opacity##ref", &op, 0.05f, 1.0f, "%.2f"))
        img.opacity = op; // live cosmetic edit
      bool hidden = img.hidden;
      if (ImGui::Checkbox("Hide##ref", &hidden))
        img.hidden = hidden;
      ImGui::SameLine();
      if (ImGui::Button("Calibrate##ref")) {
        edit.calibImageId = img.id;
        edit.calibStage = 0;
        edit.calibAlignAxis = false;
        edit.tool = SketchTool::CalibrateImage;
      }
      ImGui::SameLine();
      if (ImGui::Button("Delete##ref")) {
        DeleteReferenceImageAction(state, img.id); // feature replaced
        ImGui::PopID();
        ImGui::End();
        return;
      }
    }
    ImGui::PopID();
  }

  // Constraint status + management. (Relational constraints moved to the top
  // toolbar; dimensional ones go through the Dimension tool / inline editing.)
  ImGui::SeparatorText("Status");
  if (edit.dof == 0)
    ImGui::TextColored(ImVec4(0.55f, 0.95f, 0.60f, 1.0f), "Fully constrained");
  else if (edit.dof > 0)
    ImGui::TextColored(ImVec4(0.55f, 0.80f, 1.0f, 1.0f), "%d DOF free", edit.dof);
  else
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "Not solved");
  if (!edit.conflictingConstraints.empty())
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu conflicting",
                       edit.conflictingConstraints.size());
  ImGui::Checkbox("Show constraints", &edit.showConstraintGlyphs);

  if (!sketch->constraints.empty() &&
      ImGui::TreeNode("Constraints##list")) {
    std::set<size_t> bad(edit.conflictingConstraints.begin(),
                         edit.conflictingConstraints.end());
    int toDelete = -1;
    for (size_t i = 0; i < sketch->constraints.size(); i++) {
      ImGui::PushID(static_cast<int>(i));
      if (ImGui::SmallButton("x"))
        toDelete = static_cast<int>(i);
      ImGui::SameLine();
      const char* name = ConstraintKindName(sketch->constraints[i].kind);
      if (bad.count(i))
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", name);
      else
        ImGui::TextUnformatted(name);
      ImGui::PopID();
    }
    ImGui::TreePop();
    if (toDelete >= 0)
      DeleteConstraint(state, static_cast<size_t>(toDelete));
  }

  // Linked patterns: edit the full transform live (regenerates id-stably),
  // delete the op, and highlight the one whose instance/source is selected.
  if (!sketch->derived.empty()) {
    ImGui::SeparatorText("Patterns");
    int deleteOp = -1;
    for (size_t i = 0; i < sketch->derived.size(); i++) {
      ImGui::PushID(static_cast<int>(i));
      const sketch::DerivedOp& op = sketch->derived[i];
      const bool circular = op.kind == sketch::DerivedKind::CircularPattern;

      // Is a generated instance (or a source) of this op currently selected?
      bool opSelected = false;
      for (const auto& r : edit.selected) {
        for (const auto& in : op.instances)
          opSelected |= (in.genId == r.entity);
        for (uint32_t sid : op.sourceIds)
          opSelected |= (sid == r.entity);
      }
      const char* label = circular ? "Circular" : "Linear";
      if (opSelected)
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "%s (selected)",
                           label);
      else
        ImGui::TextUnformatted(label);
      ImGui::SameLine();
      if (ImGui::SmallButton(ICON_FA_TRASH_CAN "##delpat"))
        deleteOp = static_cast<int>(i);

      int count = op.count;
      glm::vec2 vec = op.vec;
      float angleDeg = static_cast<float>(glm::degrees(op.scalar));
      bool changed = false;
      ImGui::SetNextItemWidth(90);
      changed |= ImGui::DragInt("Count", &count, 0.1f, 2, 1000);
      if (circular) {
        ImGui::SetNextItemWidth(140);
        changed |= ImGui::DragFloat2("Centre", &vec.x, 0.05f);
        ImGui::SetNextItemWidth(90);
        changed |= ImGui::DragFloat("Angle", &angleDeg, 0.5f, -360.0f, 360.0f,
                                    "%.1f deg");
      } else {
        ImGui::SetNextItemWidth(140);
        changed |= ImGui::DragFloat2("Step", &vec.x, 0.05f);
      }
      if (changed)
        EditPatternParams(state, i, vec,
                          circular ? glm::radians(angleDeg) : 0.0, count);
      ImGui::PopID();
    }
    if (deleteOp >= 0) // deferred so we don't mutate `derived` mid-iteration
      RemovePattern(state, static_cast<size_t>(deleteOp));
  }

  ImGui::SeparatorText("Profile");
  ImGui::Text("Entities: %zu  Constraints: %zu", sketch->entities.size(),
              sketch->constraints.size());

  const bool canExtrude = !sketch::BuildLoops(sketch->entities).empty();
  ImGui::Text("Closed profile: %s", canExtrude ? "yes" : "no");

  // Linked copies (violet geometry): status + break-link per op.
  {
    bool anyLinks = false;
    for (const auto& op : sketch->derived)
      if (op.kind == sketch::DerivedKind::LinkedCopy)
        anyLinks = true;
    if (anyLinks) {
      ImGui::SeparatorText("Linked copies");
      for (size_t oi = 0; oi < sketch->derived.size(); oi++) {
        const auto& op = sketch->derived[oi];
        if (op.kind != sketch::DerivedKind::LinkedCopy)
          continue;
        ImGui::PushID(static_cast<int>(oi));
        std::string srcName = "this sketch";
        if (op.sourceSketchId != 0) {
          Feature* src = state.document->FindFeatureById(op.sourceSketchId);
          srcName = src != nullptr ? src->GetName() : "(missing sketch)";
        }
        ImGui::Text("%zu from %s", op.sourceIds.size(), srcName.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Break link")) {
          // Keeps the outputs as free geometry (ids intact); one undo step.
          size_t index = 0;
          if (ResolveActiveSketch(state, &index) == sketch) {
            nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
            sketch->BreakLink(oi);
            state.document->Regenerate();
            nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
            state.commands->PushApplied(
                std::make_unique<ModifyFeatureCommand>(index, before, after));
          }
          ImGui::PopID();
          break; // op list changed; redraw next frame
        }
        ImGui::PopID();
      }
    }
  }

  ImGui::Separator();
  ImGui::BeginDisabled(!canExtrude);
  if (ImGui::Button("Extrude..."))
    BeginSketchExtrude(state); // Shift+E does the same
  ImGui::SameLine();
  if (ImGui::Button("Revolve..."))
    BeginSketchRevolve(state); // Shift+R does the same
  ImGui::EndDisabled();

  ImGui::SameLine();
  if (ImGui::Button("Exit Sketch"))
    edit = SketchEditState{};

  ImGui::End();
}

// Add once a body exists; the first extrude starts the solid.
static ExtrudeMode DefaultBodyMode(const AppState& state) {
  return (state.document && !state.document->GetBodies().empty())
             ? ExtrudeMode::Add
             : ExtrudeMode::NewBody;
}

void BeginSketchExtrude(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const uint64_t sketchId = edit.activeSketchId;
  edit = SketchEditState{}; // exit sketch mode so the solid + dialog show
  BeginPreviewFeature(state, std::make_unique<ExtrudeFeature>(
                                 "Extrude " + std::to_string(n), sketchId,
                                 1.0, DefaultBodyMode(state)));
}

void BeginSketchRevolve(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const uint64_t sketchId = edit.activeSketchId;
  // Default the axis to the sketch's sole construction line if it has
  // exactly one; else the global Y axis (changeable in the dialog).
  AxisRef axis;
  axis.kind = AxisRef::Kind::Y;
  uint32_t soleConstructionLine = 0;
  int constructionLines = 0;
  for (const auto& e : sketch->entities) {
    const auto* line = std::get_if<sketch::Line>(&e);
    if (line != nullptr && line->construction) {
      soleConstructionLine = line->id;
      constructionLines++;
    }
  }
  if (constructionLines == 1) {
    axis.kind = AxisRef::Kind::SketchLine;
    axis.sketchId = sketchId;
    axis.entityId = soleConstructionLine;
  }
  edit = SketchEditState{};
  auto rev = std::make_unique<RevolveFeature>(
      "Revolve " + std::to_string(n), sketchId, 360.0, DefaultBodyMode(state));
  rev->axis = axis;
  BeginPreviewFeature(state, std::move(rev));
}

} // namespace ui
