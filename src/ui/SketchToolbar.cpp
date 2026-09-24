#include "ui/SketchToolbar.h"
#include "ui/Icons.h"
#include "ui/SelectionPipeline.h"

#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/RevolveFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h" // DocumentIO (break-link commit)
#include "core/sketch/SketchProfile.h"
#include "core/sketch/SketchSolver.h"
#include "ui/CreateActions.h"
#include "ui/ExprInput.h" // expression-capable numeric fields
#include "ui/FileDialogs.h"
#include "ui/ImportActions.h"
#include "ui/PreviewFeature.h"
#include "ui/SketchInteraction.h"

#include <cstdio>

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
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

namespace {

// Slot-style re-pick row for the op edit session (sources / mirror axis):
// click arms the mode, shown in the accent color while armed.
bool OpPickRow(const char* label, const std::string& value, bool armed,
               const char* id) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label);
  ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
  if (armed) {
    const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImVec4(accent.x, accent.y, accent.z, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(accent.x, accent.y, accent.z, 0.55f));
  }
  const bool clicked = ImGui::Button(
      ((armed ? "(pick in the sketch)" : value) + std::string(id)).c_str(),
      ImVec2(ImGui::GetFontSize() * 12.0f, 0.0f));
  if (armed)
    ImGui::PopStyleColor(2);
  return clicked;
}

// Derived-op edit session (U8 polish): live pattern parameter editors /
// linked-copy break-link for sketch->derived[edit.editOpIndex].
void DrawOpEditSession(AppState& state, SketchFeature& sketch) {
  SketchEditState& edit = state.sketchEdit;
  const size_t oi = static_cast<size_t>(edit.editOpIndex);
  const sketch::DerivedOp& op = sketch.derived[oi];

  // Sources row, shared by mirror and both pattern kinds: arming loads the
  // current sources into the selection for collect-phase editing.
  const auto sourcesRow = [&] {
    if (OpPickRow("Sources", std::to_string(op.sourceIds.size()) + " entities",
                  edit.opPickSources, "###opSources")) {
      edit.opPickSources = !edit.opPickSources;
      edit.opPickAxis = false;
      edit.selected.clear();
      if (edit.opPickSources)
        for (uint32_t sid : op.sourceIds)
          edit.selected.push_back({sid, sketch::kWholeEntity});
    }
  };

  if (op.kind == sketch::DerivedKind::LinkedCopy) {
    BeginToolWindowFrame(state, "Linked Copy");
    std::string srcName = "this sketch";
    if (op.sourceSketchId != 0) {
      Feature* src = state.document->FindFeatureById(op.sourceSketchId);
      srcName = src != nullptr ? src->GetName() : "(missing sketch)";
    }
    ImGui::Text("%zu entities from %s", op.sourceIds.size(), srcName.c_str());
    ImGui::Separator();
    if (ImGui::Button("Break link")) {
      // Keeps the outputs as free geometry (ids intact); one undo step.
      size_t idx = 0;
      if (ResolveActiveSketch(state, &idx) == &sketch) {
        nlohmann::json before = DocumentIO::FeatureToJson(sketch);
        sketch.BreakLink(oi);
        state.document->Regenerate();
        nlohmann::json after = DocumentIO::FeatureToJson(sketch);
        state.commands->PushApplied(
            std::make_unique<ModifyFeatureCommand>(idx, before, after));
      }
      edit.editOpIndex = -1;
      ImGui::End();
      return; // op list changed; redraw next frame
    }
    ImGui::SameLine();
    if (ImGui::Button("Close"))
      edit.editOpIndex = -1;
    ImGui::End();
    return;
  }

  if (op.kind == sketch::DerivedKind::Mirror) {
    BeginToolWindowFrame(state, "Mirror");
    sourcesRow();
    if (OpPickRow("Axis",
                  sketch::IsDatum(op.axisEntity)
                      ? std::string("datum axis")
                      : "line " + std::to_string(op.axisEntity),
                  edit.opPickAxis, "###opAxis")) {
      edit.opPickAxis = !edit.opPickAxis;
      edit.opPickSources = false;
    }
    ImGui::TextDisabled("Copies track the sources and the axis.");
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_TRASH_CAN " Delete")) {
      RemovePattern(state, oi); // removes the op + its generated copies
      edit.editOpIndex = -1;
      edit.opPickSources = edit.opPickAxis = false;
      ImGui::End();
      return;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
      edit.editOpIndex = -1;
      edit.opPickSources = edit.opPickAxis = false;
    }
    ImGui::End();
    return;
  }

  const bool circular = op.kind == sketch::DerivedKind::CircularPattern;
  BeginToolWindowFrame(state, circular ? "Circular Pattern" : "Linear Pattern");
  sourcesRow();
  int count = op.count;
  glm::vec2 vec = op.vec;
  glm::vec2 vec2 = op.vec2;
  int count2 = op.count2;
  float angleDeg = static_cast<float>(glm::degrees(op.scalar));
  bool changed = false;
  ImGui::SetNextItemWidth(90);
  changed |= ExprDragInt(state, "Count", &count, 0.1f, 2, 1000);
  if (circular) {
    ImGui::SetNextItemWidth(140);
    changed |= ExprDragFloat2(state, "Centre", &vec.x, 0.05f);
    ImGui::SetNextItemWidth(90);
    changed |= ExprDragFloat(state, "Angle", &angleDeg, 0.5f, -360.0f, 360.0f,
                             "%.1f deg");
  } else {
    ImGui::SetNextItemWidth(140);
    changed |= ExprDragFloat2(state, "Step", &vec.x, 0.05f);
    ImGui::SetNextItemWidth(90);
    changed |= ExprDragInt(state, "Rows", &count2, 0.1f, 1, 1000);
    if (count2 > 1) {
      ImGui::SetNextItemWidth(140);
      changed |= ExprDragFloat2(state, "Row step", &vec2.x, 0.05f);
    }
  }
  if (changed) {
    // Commits + REPLACES the sketch feature — `sketch`/`op` dangle after;
    // nothing below reads them.
    EditPatternParams(state, oi, vec, circular ? glm::radians(angleDeg) : 0.0,
                      count, vec2, count2);
  }
  ImGui::Separator();
  if (ImGui::Button(ICON_FA_TRASH_CAN " Delete")) {
    RemovePattern(state, oi);
    edit.editOpIndex = -1;
    edit.opPickSources = edit.opPickAxis = false;
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    edit.editOpIndex = -1;
    edit.opPickSources = edit.opPickAxis = false;
  }
  ImGui::End();
}

// Reference-image session (U8 polish): opacity/hide live edits, calibrate
// hand-off, delete — for edit.selectedImageId. Opened by the toolbar's
// Insert image button (on the fresh image) or an Operations row.
void DrawImageSession(AppState& state, SketchFeature& sketch) {
  SketchEditState& edit = state.sketchEdit;
  sketch::ReferenceImage* img = nullptr;
  for (auto& im : sketch.images)
    if (im.id == edit.selectedImageId)
      img = &im;
  if (img == nullptr) { // deleted/undone underneath the session
    edit.selectedImageId = 0;
    edit.tool = SketchTool::Select;
    return;
  }
  BeginToolWindowFrame(state, "Reference Image");
  ImGui::Text("Image %u  (%d x %d px)", img->id, img->widthPx, img->heightPx);
  ImGui::TextDisabled("Drag the image in the viewport to move it.");

  // Opacity and Hide are PERSISTED fields (format v49), so an edit to either
  // belongs on the undo stack. Opacity is a drag, so it follows the
  // HandleEdit recipe: JSON baseline when the slider is grabbed, exactly one
  // ModifyFeatureCommand when it is released — never one per frame.
  size_t sketchIdx = 0;
  const bool resolved = ResolveActiveSketch(state, &sketchIdx) == &sketch;
  const auto commitImageEdit = [&]() {
    if (!resolved || state.sketchEdit.imageEditBaseline.is_null())
      return;
    nlohmann::json after = DocumentIO::FeatureToJson(sketch);
    if (after != state.sketchEdit.imageEditBaseline) {
      state.commands->PushApplied(std::make_unique<ModifyFeatureCommand>(
          sketchIdx, state.sketchEdit.imageEditBaseline, after));
    }
    state.sketchEdit.imageEditBaseline = nlohmann::json();
  };

  ImGui::SetNextItemWidth(160);
  float op = static_cast<float>(img->opacity);
  if (ImGui::SliderFloat("Opacity", &op, 0.05f, 1.0f, "%.2f"))
    img->opacity = op; // live; the command lands on release
  if (ImGui::IsItemActivated() && resolved)
    state.sketchEdit.imageEditBaseline = DocumentIO::FeatureToJson(sketch);
  if (ImGui::IsItemDeactivatedAfterEdit())
    commitImageEdit();

  bool hidden = img->hidden;
  if (ImGui::Checkbox("Hide", &hidden)) {
    // One click, so grab and release are the same moment.
    if (resolved)
      state.sketchEdit.imageEditBaseline = DocumentIO::FeatureToJson(sketch);
    img->hidden = hidden;
    commitImageEdit();
  }
  ImGui::Separator();
  if (ImGui::Button("Calibrate...")) {
    edit.calibImageId = img->id;
    edit.calibStage = 0;
    edit.calibAlignAxis = false;
    edit.tool = SketchTool::CalibrateImage;
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete")) {
    DeleteReferenceImageAction(state, img->id); // feature replaced
    edit.selectedImageId = 0;
    edit.tool = SketchTool::Select;
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close###image_close")) {
    edit.selectedImageId = 0;
    edit.tool = SketchTool::Select;
  }
  ImGui::End();
}

} // namespace

// The active sketch tool's session in the floating Tool window (U8a). Only
// tools with options/Apply/collected targets get a window — plain drawing
// tools are fully served by the staged prompt pill.
void DrawSketchToolWindow(AppState& state) {
  SketchEditState& edit = state.sketchEdit;

  if (edit.pickingPlane) {
    const bool hole = edit.purpose == SketchPurpose::Hole;
    BeginToolWindowFrame(state, hole ? "Hole" : "New Sketch");
    ImGui::TextWrapped(
        hole ? "Step 1 of 3 - pick the FLAT face to drill into. (Esc to "
               "cancel)"
             : "Pick a plane or a model face to sketch on. (Esc to cancel)");
    if (ImGui::Button("Cancel"))
      edit = SketchEditState{};
    ImGui::End();
    return;
  }

  SketchFeature* sketch = ResolveActiveSketch(state);
  if (!sketch)
    return;

  // Derived-op edit session (double-click an Operations row in the Model
  // panel): pattern parameter editors / linked-copy break-link — the same
  // window the op's creating tool used.
  if (edit.editOpIndex >= 0) {
    if (static_cast<size_t>(edit.editOpIndex) >= sketch->derived.size()) {
      edit.editOpIndex = -1; // the op vanished (undo/delete)
      edit.opPickSources = edit.opPickAxis = false;
    } else {
      DrawOpEditSession(state, *sketch);
      return;
    }
  }

  const bool hasSession =
      edit.tool == SketchTool::Mirror || edit.tool == SketchTool::Fillet ||
      edit.tool == SketchTool::Pattern || edit.tool == SketchTool::Offset ||
      edit.tool == SketchTool::CalibrateImage ||
      edit.tool == SketchTool::Dimension || edit.tool == SketchTool::Image;
  if (!hasSession) {
    // A Hole session gets a window of its own while placing centres — but
    // only when no per-tool session owns the window, so switching to e.g.
    // Pattern (to array the centres) still shows Pattern's options.
    if (edit.purpose == SketchPurpose::Hole) {
      size_t centres = 0;
      for (const sketch::Entity& e : sketch->entities)
        if (std::get_if<sketch::Point>(&e) != nullptr)
          centres++;
      BeginToolWindowFrame(state, "Hole");
      ImGui::TextWrapped("Step 2 of 3 - click the hole centres on the face.");
      ImGui::Text("Centres: %zu", centres);
      bool toOptions = false, cancel = false;
      ImGui::BeginDisabled(centres == 0);
      if (ImGui::Button("Hole options (Enter)"))
        toOptions = true;
      ImGui::EndDisabled();
      ImGui::SameLine();
      if (ImGui::Button("Cancel"))
        cancel = true;
      ImGui::End();
      // Both tear the session down, so act after End() (the image-session
      // rule).
      if (toOptions)
        BeginSketchHole(state);
      else if (cancel)
        CancelHoleSession(state);
    }
    return;
  }

  // The image session needs its image; if it vanished (undo), close.
  if (edit.tool == SketchTool::Image) {
    DrawImageSession(state, *sketch);
    return;
  }

  // Selection-count row, slot-style: the tool's source geometry.
  const auto geometryRow = [&](const char* label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (edit.selected.empty())
      ImGui::TextDisabled("(click or box-select)");
    else
      ImGui::Text("%zu selected", edit.selected.size());
  };

  switch (edit.tool) {
  case SketchTool::Mirror: {
    BeginToolWindowFrame(state, "Mirror");
    // Clickable step rows: pick which step you're editing.
    // The active step shows in the accent color; a tap in empty viewport
    // space also advances geometry → axis.
    const auto stepRow = [&](const char* label, const std::string& value,
                             const char* id, bool active, bool enabled) {
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(label);
      ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
      if (active) {
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImVec4(accent.x, accent.y, accent.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(accent.x, accent.y, accent.z, 0.55f));
      }
      const bool clicked =
          ImGui::Button((value + id).c_str(),
                        ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f)) &&
          enabled;
      if (active)
        ImGui::PopStyleColor(2);
      return clicked;
    };
    if (stepRow("Geometry",
                edit.selected.empty()
                    ? "(click or box-select)"
                    : std::to_string(edit.selected.size()) + " selected",
                "###mirrorGeom", !edit.mirrorAxisPhase, true)) {
      edit.mirrorAxisPhase = false; // back to collecting
    }
    if (stepRow("Axis",
                edit.mirrorAxisPhase ? "(click an axis line)"
                                     : "(click here or tap empty space)",
                "###mirrorAxis", edit.mirrorAxisPhase,
                !edit.selected.empty())) {
      edit.mirrorAxisPhase = true;
    }
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
    ExprDragFloat(state, "Radius##fillet", &edit.filletRadius, 0.02f, 0.01f,
                  1e6f, "%.3f");
    // The number above is the NEXT fillet's radius — an existing fillet does
    // not follow it, which reads as "the box does nothing" the first time.
    // The arc stays tangent-constrained, so a radius dimension resizes it.
    ImGui::TextDisabled("Applies to the next corner. To resize an existing\n"
                        "fillet, put a Dimension on its arc.");
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
    ExprDragInt(state, "Count##pat", &edit.patternCount, 0.1f, 2, 1000);
    if (!edit.patternCircular) {
      ImGui::SetNextItemWidth(140);
      ExprDragFloat2(state, "Step (dx,dy)##pat", &edit.patternStep.x, 0.05f);
      // Second grid direction (v50): rows = 1 keeps the pattern a row.
      ImGui::SetNextItemWidth(80);
      ExprDragInt(state, "Rows##pat", &edit.patternCount2, 0.1f, 1, 1000);
      if (edit.patternCount2 > 1) {
        ImGui::SetNextItemWidth(140);
        ExprDragFloat2(state, "Row step (dx,dy)##pat", &edit.patternStep2.x,
                       0.05f);
      }
    } else {
      // Centre: type coordinates, or Pick a sketch point — picking an
      // entity point stores its REFERENCE so the pattern tracks it (v51).
      ImGui::SetNextItemWidth(140);
      if (ExprDragFloat2(state, "Centre##pat", &edit.patternCenter.x, 0.05f))
        edit.patternCenterRef = {0, 0}; // typing detaches the reference
      ImGui::SameLine();
      if (edit.patternPickCenter)
        ImGui::PushStyleColor(ImGuiCol_Button,
                              ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
      if (ImGui::Button("Pick###patCenterPick"))
        edit.patternPickCenter = !edit.patternPickCenter;
      if (edit.patternPickCenter)
        ImGui::PopStyleColor();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click a sketch point to centre on (a point keeps "
                          "the pattern tracking it)");
      if (edit.patternCenterRef.entity != 0)
        ImGui::TextDisabled("Centred on point of entity %u",
                            edit.patternCenterRef.entity);
      ImGui::SetNextItemWidth(80);
      ExprDragFloat(state, "Angle##pat", &edit.patternAngleDeg, 0.5f, -360.0f,
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
    ExprDragFloat(state, "Distance##off", &edit.offsetDistance, 0.02f, -1e6f,
                  1e6f, "%.3f");
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
      ExprInputFloat(state, "##calibdist", &edit.calibDistance, "%.4g");
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

  // Operations: the sketch's transform-like ops — reference images,
  // patterns, linked copies, linked mirrors — as tree-style rows;
  // double-click opens the op's edit session in the Tool window (same
  // grammar as the model feature tree). Inserting images lives on the
  // sketch toolbar. Always shown while editing so the section is
  // discoverable.
  {
    ImGui::SeparatorText("Operations");
    if (sketch->images.empty() && sketch->derived.empty())
      ImGui::TextDisabled("None yet (patterns, linked copies, images)");
    for (const sketch::ReferenceImage& img : sketch->images) {
      char label[96];
      std::snprintf(label, sizeof(label),
                    ICON_FA_IMAGE "  Image %u (%dx%d)###img%u", img.id,
                    img.widthPx, img.heightPx, img.id);
      const bool open =
          edit.tool == SketchTool::Image && edit.selectedImageId == img.id;
      if (ImGui::Selectable(label, open,
                            ImGuiSelectableFlags_AllowDoubleClick) &&
          ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        edit.selectedImageId = img.id;
        edit.tool = SketchTool::Image;
      }
    }
    for (size_t i = 0; i < sketch->derived.size(); i++) {
      const sketch::DerivedOp& op = sketch->derived[i];
      char label[96];
      if (op.kind == sketch::DerivedKind::LinkedCopy) {
        std::snprintf(label, sizeof(label), ICON_FA_LINK "  Linked copy (%zu)",
                      op.sourceIds.size());
      } else if (op.kind == sketch::DerivedKind::Mirror) {
        // The glyph of the tool that made the row (ui/Icons.h).
        std::snprintf(label, sizeof(label), "%s  Mirror (%zu)",
                      CommandIcon("sketch.mirror"), op.sourceIds.size());
      } else {
        std::snprintf(label, sizeof(label), "%s  %s pattern (x%d)",
                      CommandIcon("sketch.pattern"),
                      op.kind == sketch::DerivedKind::CircularPattern
                          ? "Circular"
                          : "Linear",
                      op.count);
      }
      // Highlight the op whose instance/source is currently selected.
      bool opSelected = false;
      for (const auto& r : edit.selected) {
        for (const auto& in : op.instances)
          opSelected |= (in.genId == r.entity);
        for (uint32_t sid : op.sourceIds)
          opSelected |= (sid == r.entity);
      }
      const bool open = edit.editOpIndex == static_cast<int>(i);
      if (opSelected && !open)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
      const std::string id = std::string(label) + "###op" + std::to_string(i);
      if (ImGui::Selectable(id.c_str(), open,
                            ImGuiSelectableFlags_AllowDoubleClick) &&
          ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        edit.editOpIndex = static_cast<int>(i);
        edit.opPickSources = edit.opPickAxis = false;
      }
      if (opSelected && !open)
        ImGui::PopStyleColor();
    }
    if (!sketch->images.empty() || !sketch->derived.empty())
      ImGui::TextDisabled("Double-click to edit");
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
}

// Add once a body exists; the first extrude starts the solid.
static ExtrudeMode DefaultBodyMode(const AppState& state) {
  return (state.document && !state.document->GetBodies().empty())
             ? ExtrudeMode::Add
             : ExtrudeMode::NewBody;
}

// U8b select-first: the loops containing any selected entity become the
// feature's regions — select a profile, hit Extrude, only that region
// fills. Nothing selected (or everything) = whole sketch (empty regions,
// today's behavior).
static std::vector<std::vector<uint32_t>>
RegionsFromSelection(const SketchFeature& sketch,
                     const std::vector<sketch::PointRef>& selected) {
  if (selected.empty())
    return {};
  std::set<uint32_t> selIds;
  for (const sketch::PointRef& r : selected)
    selIds.insert(r.entity);
  const std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch.entities);
  std::vector<std::vector<uint32_t>> regions;
  for (const sketch::Loop& loop : loops) {
    std::vector<uint32_t> ids = sketch::LoopEntityIds(loop);
    for (uint32_t id : ids) {
      if (selIds.count(id)) {
        // The loop's whole pattern/mirror family rides along: a selected
        // bolt-circle hole means the bolt circle, not one fifth of it.
        for (std::vector<uint32_t>& r : sketch.SiblingRegions(ids))
          if (std::find(regions.begin(), regions.end(), r) == regions.end())
            regions.push_back(std::move(r));
        break;
      }
    }
  }
  if (regions.empty() || regions.size() == loops.size())
    return {}; // selection hit no loop / every loop: whole sketch
  return regions;
}

void BeginExtrudeOfSketch(AppState& state, SketchFeature& sketch,
                          std::vector<std::vector<uint32_t>> regions) {
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  auto ext = std::make_unique<ExtrudeFeature>("Extrude " + std::to_string(n),
                                              sketch.GetId(), 1.0,
                                              DefaultBodyMode(state));
  ext->regions = std::move(regions);
  BeginPreviewFeature(state, std::move(ext));
}

SketchFeature* ClosedProfileSketch(AppState& state) {
  if (!state.document)
    return nullptr;
  const auto closed = [](Feature* f) -> SketchFeature* {
    auto* sk = dynamic_cast<SketchFeature*>(f);
    if (sk == nullptr || sketch::BuildLoops(sk->entities).empty())
      return nullptr;
    return sk;
  };
  // Select-first, then the most recent usable sketch (the CreateRib rule).
  if (SketchFeature* sel = closed(SelectedFeature(state)))
    return sel;
  for (size_t i = state.document->FeatureCount(); i > 0; i--)
    if (SketchFeature* sk = closed(state.document->GetFeature(i - 1)))
      return sk;
  return nullptr;
}

void BeginSketchExtrude(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  auto regions = RegionsFromSelection(*sketch, edit.selected);
  SketchFeature& sk = *sketch;
  edit = SketchEditState{}; // exit sketch mode so the solid + dialog show
  BeginExtrudeOfSketch(state, sk, std::move(regions));
}

void BeginRevolveOfSketch(AppState& state, SketchFeature& sketch,
                          std::vector<std::vector<uint32_t>> regions) {
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  // Default the axis to the sketch's sole construction line if it has
  // exactly one; else a world axis that LIES IN the sketch's plane.
  //
  // It used to default to Y unconditionally, which is not in the plane of a
  // top-plane (XZ) sketch — so a revolve started there was born broken
  // ("revolve axis is not in the sketch plane") with nothing picked wrong.
  // Found by the U10 defaults gate.
  AxisRef axis;
  {
    const glm::vec3 n = sketch.plane.GetNormal();
    // In-plane means perpendicular to the normal. Prefer the plane's own U
    // direction, then V, then whichever world axis is most perpendicular.
    const glm::vec3 candidates[3] = {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                     glm::vec3(0, 0, 1)};
    const AxisRef::Kind kinds[3] = {AxisRef::Kind::X, AxisRef::Kind::Y,
                                    AxisRef::Kind::Z};
    int best = 1; // Y, the historical default
    float bestScore = -1.0f;
    for (int i = 0; i < 3; i++) {
      if (std::abs(glm::dot(candidates[i], n)) > 1e-3f)
        continue; // not in the plane
      // Among in-plane axes, the one closest to the sketch's V direction
      // makes the familiar "spin the profile about its vertical" default.
      const float score =
          std::abs(glm::dot(candidates[i], sketch.plane.GetVAxis()));
      if (score > bestScore) {
        bestScore = score;
        best = i;
      }
    }
    axis.kind = kinds[best];
  }
  uint32_t soleConstructionLine = 0;
  int constructionLines = 0;
  for (const auto& e : sketch.entities) {
    const auto* line = std::get_if<sketch::Line>(&e);
    if (line != nullptr && line->construction) {
      soleConstructionLine = line->id;
      constructionLines++;
    }
  }
  if (constructionLines == 1) {
    axis.kind = AxisRef::Kind::SketchLine;
    axis.sketchId = sketch.GetId();
    axis.entityId = soleConstructionLine;
  }
  auto rev = std::make_unique<RevolveFeature>("Revolve " + std::to_string(n),
                                              sketch.GetId(), 360.0,
                                              DefaultBodyMode(state));
  rev->axis = axis;
  rev->regions = std::move(regions);
  BeginPreviewFeature(state, std::move(rev));
}

namespace {
// The face being drilled, measured: a face-attached sketch carries the
// face's outline as construction geometry, so its extent IS the face's.
// Zero when the sketch has no such outline (a plane sketch, say).
glm::dvec2 FaceExtent(const SketchFeature& sketch) {
  glm::dvec2 lo(1e30), hi(-1e30);
  bool any = false;
  const auto note = [&](const glm::vec2& p) {
    lo = glm::min(lo, glm::dvec2(p)), hi = glm::max(hi, glm::dvec2(p));
    any = true;
  };
  for (const sketch::Entity& e : sketch.entities) {
    if (const auto* l = std::get_if<sketch::Line>(&e); l && l->construction)
      note(l->a), note(l->b);
    else if (const auto* a = std::get_if<sketch::Arc>(&e); a && a->construction)
      note(a->center - glm::vec2(a->radius)),
          note(a->center + glm::vec2(a->radius));
    else if (const auto* c = std::get_if<sketch::Circle>(&e);
             c && c->construction)
      note(c->center - glm::vec2(c->radius)),
          note(c->center + glm::vec2(c->radius));
  }
  return any ? hi - lo : glm::dvec2(0.0);
}

// A first diameter for a hole in this face. The rule
// (docs/ui/tool-sessions.md#defaults-and-refusals): a default carrying
// millimetres is derived from what it acts on, never a literal. It used to be a
// literal 0.5 mm, which on an 80 x 60 plate drills something you cannot see and
// on a 5 m dock does not exist — "correct in the abstract and wrong against any
// real part" is exactly the failure that rule names.
//
// A tenth of the narrower side of the face lands on ~6 mm for a 60 mm-wide
// plate, which is a bolt hole. The fallback only applies when the face
// could not be measured at all.
double DefaultHoleDiameter(const SketchFeature& sketch) {
  const glm::dvec2 ext = FaceExtent(sketch);
  const double narrow = std::min(ext.x, ext.y);
  return narrow > 1e-6 ? narrow * 0.1 : 1.0;
}

// Hole centres placed so far in `sketch`.
size_t HolePointCount(const SketchFeature& sketch) {
  size_t n = 0;
  for (const sketch::Entity& e : sketch.entities)
    if (std::get_if<sketch::Point>(&e) != nullptr)
      n++;
  return n;
}
} // namespace

bool CanBeginSketchHole(const AppState& state) {
  const auto* sk = dynamic_cast<const SketchFeature*>(
      state.document
          ? state.document->FindFeatureById(state.sketchEdit.activeSketchId)
          : nullptr);
  return sk != nullptr && HolePointCount(*sk) > 0;
}

void BeginSketchHole(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr)
    return;
  if (HolePointCount(*sketch) == 0) {
    edit.actionMsg = "Hole: click at least one centre point first (Esc "
                     "cancels).";
    return;
  }
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const uint64_t sketchId = edit.activeSketchId;
  edit = SketchEditState{}; // exit sketch mode so the preview + dialog show
  const double dia = DefaultHoleDiameter(*sketch);
  // Blind depth follows the diameter (2x is an ordinary tapped depth) so it
  // is derived too — it only shows once "Through all" is unticked.
  auto hole = std::make_unique<HoleFeature>(
      "Hole " + std::to_string(n), sketchId, HoleKind::Simple, dia, dia * 2.0);
  hole->through = true; // the bolt-hole default; the dialog offers a depth
  BeginPreviewFeature(state, std::move(hole));
}

void CancelHoleSession(AppState& state) {
  size_t index = 0;
  SketchFeature* sketch = ResolveActiveSketch(state, &index);
  // Discard the auto-created sketch only when the user drew nothing in it —
  // a face-attached sketch's projected outline does not count as their work.
  const bool discard = sketch != nullptr && !HasUserGeometry(*sketch);
  state.sketchEdit = SketchEditState{};
  if (discard)
    state.commands->Execute(std::make_unique<RemoveFeatureCommand>(index),
                            *state.document);
  state.selection.Clear();
}

void BeginSketchRevolve(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  auto regions = RegionsFromSelection(*sketch, edit.selected);
  SketchFeature& sk = *sketch;
  edit = SketchEditState{};
  BeginRevolveOfSketch(state, sk, std::move(regions));
}

} // namespace ui
