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

namespace {

// Derived-op edit session (U8 polish): live pattern parameter editors /
// linked-copy break-link for sketch->derived[edit.editOpIndex].
void DrawOpEditSession(AppState& state, SketchFeature& sketch) {
  SketchEditState& edit = state.sketchEdit;
  const size_t oi = static_cast<size_t>(edit.editOpIndex);
  const sketch::DerivedOp& op = sketch.derived[oi];

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

  const bool circular = op.kind == sketch::DerivedKind::CircularPattern;
  BeginToolWindowFrame(state, circular ? "Circular Pattern" : "Linear Pattern");
  int count = op.count;
  glm::vec2 vec = op.vec;
  glm::vec2 vec2 = op.vec2;
  int count2 = op.count2;
  float angleDeg = static_cast<float>(glm::degrees(op.scalar));
  bool changed = false;
  ImGui::SetNextItemWidth(90);
  changed |= ImGui::DragInt("Count", &count, 0.1f, 2, 1000);
  if (circular) {
    ImGui::SetNextItemWidth(140);
    changed |= ImGui::DragFloat2("Centre", &vec.x, 0.05f);
    ImGui::SetNextItemWidth(90);
    changed |=
        ImGui::DragFloat("Angle", &angleDeg, 0.5f, -360.0f, 360.0f, "%.1f deg");
  } else {
    ImGui::SetNextItemWidth(140);
    changed |= ImGui::DragFloat2("Step", &vec.x, 0.05f);
    ImGui::SetNextItemWidth(90);
    changed |= ImGui::DragInt("Rows", &count2, 0.1f, 1, 1000);
    if (count2 > 1) {
      ImGui::SetNextItemWidth(140);
      changed |= ImGui::DragFloat2("Row step", &vec2.x, 0.05f);
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
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close"))
    edit.editOpIndex = -1;
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
  ImGui::SetNextItemWidth(160);
  float op = static_cast<float>(img->opacity);
  if (ImGui::SliderFloat("Opacity", &op, 0.05f, 1.0f, "%.2f"))
    img->opacity = op; // live cosmetic edit (not undoable)
  bool hidden = img->hidden;
  if (ImGui::Checkbox("Hide", &hidden))
    img->hidden = hidden;
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
  if (ImGui::Button("Done")) {
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

  // Derived-op edit session (double-click an Operations row in the Model
  // panel): pattern parameter editors / linked-copy break-link — the same
  // window the op's creating tool used.
  if (edit.editOpIndex >= 0) {
    if (static_cast<size_t>(edit.editOpIndex) >= sketch->derived.size()) {
      edit.editOpIndex = -1; // the op vanished (undo/delete)
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
  if (!hasSession)
    return;

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
    // Clickable step rows (Onshape-style): pick which step you're editing.
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
      // Second grid direction (v50): rows = 1 keeps the pattern a row.
      ImGui::SetNextItemWidth(80);
      ImGui::DragInt("Rows##pat", &edit.patternCount2, 0.1f, 1, 1000);
      if (edit.patternCount2 > 1) {
        ImGui::SetNextItemWidth(140);
        ImGui::DragFloat2("Row step (dx,dy)##pat", &edit.patternStep2.x, 0.05f);
      }
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

  // Operations (U8 polish): the sketch's transform-like ops — reference
  // images, patterns, linked copies — as tree-style rows; double-click
  // opens the op's edit session in the Tool window (same grammar as the
  // model feature tree). Inserting images lives on the sketch toolbar.
  // Always shown while editing so the section is discoverable. NOTE:
  // mirrors are symmetry CONSTRAINTS, not stored ops — they list under
  // Constraints, not here.
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
      } else {
        std::snprintf(label, sizeof(label), ICON_FA_GRIP "  %s pattern (x%d)",
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
        regions.push_back(std::move(ids));
        break;
      }
    }
  }
  if (regions.empty() || regions.size() == loops.size())
    return {}; // selection hit no loop / every loop: whole sketch
  return regions;
}

void BeginSketchExtrude(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const uint64_t sketchId = edit.activeSketchId;
  auto regions = RegionsFromSelection(*sketch, edit.selected);
  edit = SketchEditState{}; // exit sketch mode so the solid + dialog show
  auto ext = std::make_unique<ExtrudeFeature>(
      "Extrude " + std::to_string(n), sketchId, 1.0, DefaultBodyMode(state));
  ext->regions = std::move(regions);
  BeginPreviewFeature(state, std::move(ext));
}

void BeginSketchRevolve(AppState& state) {
  SketchEditState& edit = state.sketchEdit;
  SketchFeature* sketch = ResolveActiveSketch(state);
  if (sketch == nullptr || sketch::BuildLoops(sketch->entities).empty())
    return;
  const int n = static_cast<int>(state.document->FeatureCount()) + 1;
  const uint64_t sketchId = edit.activeSketchId;
  auto regions = RegionsFromSelection(*sketch, edit.selected);
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
  rev->regions = std::move(regions);
  BeginPreviewFeature(state, std::move(rev));
}

} // namespace ui
