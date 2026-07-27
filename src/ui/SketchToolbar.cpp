#include "ui/SketchToolbar.h"

#include "core/features/ExtrudeFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h" // DocumentIO (break-link commit)
#include "core/sketch/SketchProfile.h"
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
  case K::Coincident:
    return "coincident";
  case K::Horizontal:
    return "horizontal";
  case K::Vertical:
    return "vertical";
  case K::Parallel:
    return "parallel";
  case K::Perpendicular:
    return "perpendicular";
  case K::Equal:
    return "equal";
  case K::Distance:
    return "distance";
  case K::Radius:
    return "radius";
  case K::Angle:
    return "angle";
  case K::Tangent:
    return "tangent";
  case K::PointOnLine:
    return "point-on-line";
  case K::Symmetric:
    return "symmetric";
  case K::Midpoint:
    return "midpoint";
  case K::Fixed:
    return "fixed";
  case K::Concentric:
    return "concentric";
  case K::Collinear:
    return "collinear";
  case K::DistanceX:
    return "distance-x";
  case K::DistanceY:
    return "distance-y";
  case K::Diameter:
    return "diameter";
  case K::DistancePL:
    return "distance-pl";
  }
  return "?";
}
} // namespace

// The active sketch tool's session in the floating Tool window (U8a). Only
// tools with options/Apply/collected targets get a window — plain drawing
// tools are fully served by the staged prompt pill.
void DrawSketchToolWindow(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  if (edit.pickingPlane) {
    BeginToolWindowFrame(state, "New Sketch");
    ImGui::TextWrapped(
        "Pick a plane or a model face to sketch on. (Esc to cancel)");
    if (ImGui::Button("Cancel"))
      edit.pickingPlane = false;
    ImGui::End();
    return;
  }

  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;

  const bool hasSession =
      edit.tool == SketchTool::Mirror || edit.tool == SketchTool::Fillet ||
      edit.tool == SketchTool::Pattern || edit.tool == SketchTool::Offset ||
      edit.tool == SketchTool::CalibrateImage ||
      edit.tool == SketchTool::Dimension;
  if (!hasSession)
    return;

  // Selection-count row, slot-style: the tool's source geometry.
  const auto geometryRow = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (edit.selected.empty())
      ImGui::TextDisabled("(select in the sketch)");
    else
      ImGui::Text("%zu selected", edit.selected.size());
  };

  switch (edit.tool) {
  case SketchTool::Mirror: {
    BeginToolWindowFrame(state, "Mirror");
    geometryRow("Geometry");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Axis");
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (edit.mirrorAxisPhase)
      ImGui::TextColored(ImGui::GetStyleColorVec4(ImGuiCol_NavCursor),
                         "(click an axis line)");
    else
      ImGui::TextDisabled("Enter confirms geometry first");
    ImGui::Checkbox("Linked##gen", &edit.linkGenerative);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Add symmetry constraints so the mirror tracks edits "
                        "(off = one-shot copy)");
    break;
  }
  case SketchTool::Fillet:
    BeginToolWindowFrame(state, "Sketch Fillet");
    ImGui::TextUnformatted("Click a corner where two lines meet.");
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("Radius##fillet", &edit.filletRadius, 0.02f, 0.01f, 1e6f,
                     "%.3f");
    break;
  case SketchTool::Pattern: {
    BeginToolWindowFrame(state, "Sketch Pattern");
    geometryRow("Geometry");
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
    ImGui::Separator();
    ImGui::BeginDisabled(edit.selected.empty());
    if (ImGui::Button("Apply##pat"))
      ApplyPattern(state);
    ImGui::EndDisabled();
    break;
  }
  case SketchTool::Offset:
    BeginToolWindowFrame(state, "Offset");
    geometryRow("Loop");
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("Distance##off", &edit.offsetDistance, 0.02f, -1e6f, 1e6f,
                     "%.3f");
    ImGui::SameLine();
    ImGui::Checkbox("Round corners##off", &edit.offsetJoinArc);
    ImGui::Separator();
    ImGui::BeginDisabled(edit.selected.empty());
    if (ImGui::Button("Apply##off"))
      ApplyOffset(state);
    ImGui::EndDisabled();
    break;
  case SketchTool::CalibrateImage:
    BeginToolWindowFrame(state, "Calibrate Image");
    if (edit.calibStage < 2) {
      ImGui::TextWrapped("Click two points on the image a known distance apart "
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
        ImGui::End(); // the commit replaced the feature — redraw next frame
        return;
      }
      ImGui::SameLine();
      if (ImGui::Button("Re-pick##calib"))
        edit.calibStage = 0;
    }
    break;
  case SketchTool::Dimension: {
    BeginToolWindowFrame(state, "Dimension");
    if (edit.dimPlacing) {
      ImGui::TextDisabled("Click to place the dimension");
    } else if (edit.selected.empty()) {
      ImGui::TextDisabled("Pick points/lines/curves to dimension");
    } else {
      // Accumulated targets, each removable (kept in lockstep with the
      // parallel dimPickBody flags).
      for (size_t i = 0; i < edit.selected.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::SmallButton("x")) {
          edit.selected.erase(edit.selected.begin() + static_cast<long>(i));
          if (i < edit.dimPickBody.size())
            edit.dimPickBody.erase(edit.dimPickBody.begin() +
                                   static_cast<long>(i));
          ImGui::PopID();
          break;
        }
        ImGui::SameLine();
        const bool body = i < edit.dimPickBody.size() && edit.dimPickBody[i];
        ImGui::Text("%s of entity %u", body ? "body" : "point",
                    edit.selected[i].entity);
        ImGui::PopID();
      }
    }
    break;
  }
  default:
    return; // unreachable (hasSession gate)
  }
  ImGui::End();
}

// Persistent sketch sections at the bottom of the Model panel while editing
// (U8a): reference images, constraint list, linked patterns, linked copies.
// The old Status section is gone — the DOF pill already says it.
void DrawSketchModelSections(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  if (!sketch)
    return;

  // Reference images (v49): import an underlay, tune it, calibrate it to a
  // real dimension, or delete it. Opacity/hide are live cosmetic edits (not
  // undoable); import/calibrate/delete each commit one undo step and REPLACE
  // the sketch feature — so after any of them `sketch` dangles: return
  // immediately and redraw next frame.
  ImGui::SeparatorText("Reference images");
  if (ImGui::Button("Insert image...##ref")) {
    OpenReferenceImageDialog(state); // may add + commit (feature replaced)
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
        return;
      }
    }
    ImGui::PopID();
  }

  // Constraint management (relational buttons live on the toolbar; the DOF
  // status lives in the pill). Conflicts still flagged here beside the list.
  if (!sketch->constraints.empty()) {
    ImGui::SeparatorText("Constraints");
    if (!edit.conflictingConstraints.empty())
      ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%zu conflicting",
                         edit.conflictingConstraints.size());
    if (ImGui::TreeNode("Constraints##list")) {
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
      if (toDelete >= 0) {
        // DeleteConstraint commits a ModifyFeatureCommand, replacing the
        // active feature — `sketch` dangles; redraw next frame.
        DeleteConstraint(state, static_cast<size_t>(toDelete));
        return;
      }
    }
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
          size_t idx = 0;
          if (ResolveActiveSketch(state, &idx) == sketch) {
            nlohmann::json before = DocumentIO::FeatureToJson(*sketch);
            sketch->BreakLink(oi);
            state.document->Regenerate();
            nlohmann::json after = DocumentIO::FeatureToJson(*sketch);
            state.commands->PushApplied(
                std::make_unique<ModifyFeatureCommand>(idx, before, after));
          }
          ImGui::PopID();
          break; // op list changed; redraw next frame
        }
        ImGui::PopID();
      }
    }
  }
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
                                 "Extrude " + std::to_string(n), sketchId, 1.0,
                                 DefaultBodyMode(state)));
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
