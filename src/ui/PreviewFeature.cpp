#include "ui/PreviewFeature.h"
#include "ui/FloatingWindow.h"
#include "ui/SelectionPipeline.h"
#include "ui/StudyPanel.h" // DrawSimulationToolWindow

#include "ui/DrawingPanel.h" // DrawAddViewToolWindow
#include "ui/ExprInput.h"    // expression-capable numeric fields
#include "ui/OccurrencePanel.h"

#include "cad/SketchPlane.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h" // DrawingDimKind (dim session)
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/SketchFeature.h" // occurrence names (measure rows)
#include "core/io/Serialization.h"       // ConfigurationToJson
#include "ui/CamTool.h"                  // DrawCamToolWindow (CAM verbs)
#include "ui/FastenerTool.h"
#include "ui/FeatureEditor.h"
#include "ui/MeasureActions.h"
#include "ui/SketchToolbar.h" // DrawSketchToolWindow (U8a)
#include "ui/Theme.h"         // ElevatedWindowBorder, DrawWindowShadow
#include "ui/ToleranceActions.h"
#include "ui/ViewActions.h" // SelectFeature

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat> // std::min/max (session-window clamp)

namespace ui {

uint64_t BeginPreviewFeature(AppState& state,
                             std::unique_ptr<Feature> feature) {
  // Added directly (not via a command) so it regenerates + renders while the
  // user decides; committed to the undo stack only on accept.
  state.document->AddFeature(std::move(feature));
  const size_t idx = state.document->FeatureCount() - 1;
  Feature* added = state.document->GetFeature(idx);
  state.pendingFeatureId = added->GetId();
  state.editFeatureId = 0; // a new preview supersedes an editor session
  // Select-first (U7b): whatever was selected seeds the new tool's slots
  // before SelectFeature replaces the selection with the feature itself;
  // then the first unfilled required slot arms for the next viewport click
  // (nothing arms when the seed satisfied everything — Enter commits).
  SeedSlotsFromSelection(state, *added);
  SelectFeature(state, added->GetId());
  ArmNextUnfilledSlot(state, *added);
  return state.pendingFeatureId;
}

bool DocumentFingerprint::operator==(const DocumentFingerprint& o) const {
  if (features != o.features || volumes.size() != o.volumes.size())
    return false;
  for (size_t i = 0; i < volumes.size(); i++) {
    // A relative tolerance: OCC rebuilds the same solid to the last bit only
    // by luck, and a change too small to see is not a change worth claiming.
    const double scale =
        std::max({1.0, std::abs(volumes[i]), std::abs(o.volumes[i])});
    if (std::abs(volumes[i] - o.volumes[i]) > 1e-9 * scale)
      return false;
  }
  return true;
}

DocumentFingerprint FingerprintOf(const Document& doc) {
  DocumentFingerprint fp;
  fp.features = doc.FeatureCount();
  fp.volumes.reserve(doc.GetBodies().size());
  for (const Body& b : doc.GetBodies())
    fp.volumes.push_back(b.shape.Volume());
  return fp;
}

bool HasPendingFeature(const AppState& state) {
  return state.pendingFeatureId != 0;
}

Feature* ResolvePendingFeature(AppState& state, size_t* outIndex) {
  if (state.pendingFeatureId == 0 || !state.document)
    return nullptr;
  return state.document->FindFeatureById(state.pendingFeatureId, outIndex);
}

namespace {
// A flat pattern REPLACES the folded shape rather than adding a body, so it is
// authored suppressed and a configuration switches it on
// (docs/sheetmetal/README.md#features): the base configuration stays the
// folded part. Accepting leaves the blank on screen, as it was previewed, by
// making a `Flat` configuration that unsuppresses it and activating it — all
// one undo step with the feature itself. The Sheet Metal panel's "Show the
// blank" toggle then switches between the two.
void ShowTheBlank(AppState& state, uint64_t flatId, size_t mark) {
  Document& doc = *state.document;
  auto add = std::make_unique<AddConfigurationCommand>("Flat");
  AddConfigurationCommand* added = add.get();
  if (!state.commands->Execute(std::move(add), doc))
    return;
  const Configuration* cfg = doc.FindConfiguration(added->ConfigurationId());
  if (cfg == nullptr)
    return;
  const nlohmann::json before = DocumentIO::ConfigurationToJson(*cfg);
  nlohmann::json after = before;
  after["unsuppressed_features"] = std::vector<uint64_t>{flatId};
  state.commands->Execute(std::make_unique<ModifyConfigurationCommand>(
                              "Show the flat pattern", before, after),
                          doc);
  state.commands->Execute(
      std::make_unique<SetActiveConfigurationCommand>(cfg->id), doc);
  state.commands->CoalesceSince(mark, "Add flat pattern");
}
} // namespace

void AcceptPendingFeature(AppState& state) {
  size_t idx = 0;
  Feature* f = ResolvePendingFeature(state, &idx);
  if (!f) {
    state.pendingFeatureId = 0;
    return;
  }
  // A tool whose inputs are not there yet must not commit. The UI disables
  // Accept for the same reason (the slot gate), but keyboard paths, scripts
  // and future callers all land here.
  if (!f->WhyIncomplete().empty())
    return;
  // Remove (id preserved) then re-add through a command, so accept is a single
  // undoable action and downstream id references stay valid.
  const uint64_t id = f->GetId();
  std::unique_ptr<Feature> owned = state.document->RemoveFeature(idx);
  state.pendingFeatureId = 0;
  const bool blank = owned->TypeName() == "FlatPattern";
  if (blank) // authored off; ShowTheBlank switches it on in a configuration
    owned->SetSuppressed(true);
  const size_t mark = state.commands->UndoDepth();
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(owned)),
                          *state.document);
  if (blank)
    ShowTheBlank(state, id, mark);
  SelectFeature(state, id);
  state.slotPick = {}; // armed slots end with the session
}

void CancelPendingFeature(AppState& state) {
  size_t idx = 0;
  if (ResolvePendingFeature(state, &idx)) {
    state.document->RemoveFeature(idx);
  }
  state.pendingFeatureId = 0;
  state.selection.Clear();
  state.slotPick = {};
}

void BeginToolWindowFrame(AppState& state, const std::string& title) {
  // The floating tool-session window (U7): anchored top-right of the 3D
  // viewport when it (re)opens -- NEVER bottom-center, which occluded the
  // viewport clicks GUI tests make -- and draggable for the session. One
  // stable id: only one session exists at a time, and tests address rows
  // as //<Title>###ToolWindow/###slotN. The placement (re-anchor when the
  // viewport moves, clamp inside the app, border and shadow) is the one every
  // floating window shares (ui/FloatingWindow.h); this window was where it
  // was worked out.
  FloatingPlacement placement{state.toolWindowAnchor,
                              state.toolWindowAnchorFrames,
                              state.toolWindowLastFrame};
  BeginFloatingWindow(
      state, title + "###ToolWindow", nullptr, placement, FloatAnchor::TopRight,
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize);
  state.toolWindowAnchor = placement.anchor;
  state.toolWindowAnchorFrames = placement.anchorFrames;
  state.toolWindowLastFrame = placement.lastFrame;
  // Publish the rect so the camera fit can keep the model out from under it,
  // and so a window anchored BelowToolWindow stacks under it.
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  state.toolWindowRect =
      glm::vec4(pos.x, pos.y, pos.x + size.x, pos.y + size.y);
}

namespace {

// Measure session (U7d): the picked-element list + readout, previously a
// Properties/Occurrence panel section. Rows keep the per-item "x".
void DrawMeasureToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Measure");
  ImGui::TextDisabled("Pick faces/edges - click again to deselect. Esc exits.");
  for (size_t i = 0; i < state.measure.picked.size(); i++) {
    const auto& e = state.measure.picked[i];
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      state.measure.picked.erase(state.measure.picked.begin() +
                                 static_cast<long>(i));
      UpdateMeasureReadout(state);
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    // Named the way a person would. The stable toponame is still the
    // identity — and still one hover away, for when it IS the thing you
    // need (a fillet ref, an RPC call).
    ImGui::TextWrapped("%s", MeasureElementLabel(state, e).c_str());
    ImGui::SetItemTooltip("%s", e.ref.name.c_str());
    ImGui::PopID();
  }
  if (!state.measure.readout.empty())
    ImGui::TextWrapped("%s", state.measure.readout.c_str());

  // WHICH distance (M4). Minimum is the default and was the only one; the
  // other two answer questions minimum cannot — clearance at the worst
  // point, and hole spacing. Only offered for a pair, which is the only
  // thing a distance is between.
  if (state.measure.picked.size() == 2) {
    ImGui::SetNextItemWidth(150.0f);
    const char* label = Measure::ToString(state.measure.distanceKind);
    if (ImGui::BeginCombo("###measure_kind", label)) {
      for (const char* name : Measure::DistanceKindNames()) {
        const bool sel = std::string(name) == label;
        if (ImGui::Selectable(name, sel)) {
          Measure::DistanceKind k{};
          if (Measure::DistanceKindFromString(name, k)) {
            state.measure.distanceKind = k;
            UpdateMeasureReadout(state);
          }
        }
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("Which distance: the closest approach, the "
                          "furthest (clearance), or centre to centre.");
    ImGui::SameLine();
    if (ImGui::Button("Pin###measure_pin"))
      PinMeasurement(state);
    ImGui::SetItemTooltip("Keep this measurement on screen and start another");
    ImGui::SameLine();
    if (ImGui::Button("Copy###measure_copy")) {
      // The NUMBER and its unit, not the sentence: what gets pasted into a
      // dimension field, which parses it as an expression. The unit travels
      // with it because the value is in the DISPLAY unit and a bare number
      // is read as storage.
      const std::string v = MeasuredValueForClipboard(state);
      if (!v.empty())
        ImGui::SetClipboardText(v.c_str());
    }
    ImGui::SetItemTooltip("Copy the value with its unit (\"2mm\")");

    // ...and the same number as a VARIABLE. This is where measuring stops
    // being read-only: the point of measuring a gap is usually to make
    // something else depend on it.
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputTextWithHint("###measure_var_name", "variable name",
                             state.measureVarName,
                             sizeof(state.measureVarName));
    ImGui::SameLine();
    if (ImGui::Button("Make variable###measure_make_var")) {
      std::string why;
      if (CreateVariableFromMeasurement(state, state.measureVarName, &why))
        state.measureVarName[0] = '\0';
      else
        state.sketchEdit.actionMsg = why;
    }
    ImGui::SetItemTooltip(
        "Create a project variable holding this measurement, in mm");
  }

  // ...and what it is ALLOWED to be. Measuring a face and saying how much it
  // may vary are the same gesture on the same pick, which is why the editor
  // lives here rather than in a panel of its own: one pick is that face's
  // size, two are the distance between them, and those are exactly a
  // Tolerance's ref and ref2 (docs/tolerances/README.md#gui).
  DrawTolerancePick(state);

  // The pinned list. Hovering a row brightens its dimension in the viewport
  // — with more than one on screen the rows have to point at the geometry.
  state.measure.hoveredPinned = 0;
  if (!state.measure.pinned.empty()) {
    ImGui::Separator();
    uint64_t drop = 0;
    for (const MeasureState::Pinned& p : state.measure.pinned) {
      ImGui::PushID(static_cast<int>(p.id));
      if (ImGui::SmallButton("x"))
        drop = p.id;
      ImGui::SameLine();
      // First line only: the rows are an index, not a second readout.
      const size_t nl = p.readout.find('\n');
      ImGui::TextWrapped("%s", nl == std::string::npos
                                   ? p.readout.c_str()
                                   : p.readout.substr(0, nl).c_str());
      if (ImGui::IsItemHovered())
        state.measure.hoveredPinned = p.id;
      ImGui::PopID();
    }
    if (drop)
      UnpinMeasurement(state, drop);
    if (ImGui::SmallButton("Clear pinned###measure_unpin_all"))
      UnpinMeasurement(state, 0);
  }

  ImGui::Separator();
  // Close, as every window that applies as you go says
  // (docs/ui/panels.md); Accept/Cancel are for one that makes
  // something.
  if (ImGui::Button("Close###measure_close"))
    state.measure = MeasureState{};
  ImGui::End();
}

// Section tool: the list of cuts, one row each. Replaces the old
// View > Section View menu, which held ONE plane in function statics — so an
// agent's set_section and the menu disagreed, and the offset followed you
// between tabs of wildly different size. Works the same on part and assembly
// tabs; the sections themselves are view state and outlive the window.
void DrawSectionToolWindow(AppState& state) {
  BeginToolWindowFrame(state, "Section");
  const char* planes[] = {SketchPlane::BasePlaneLabel(0),
                          SketchPlane::BasePlaneLabel(1),
                          SketchPlane::BasePlaneLabel(2)};

  glm::vec3 bmin, bmax;
  const bool haveScene = VisibleSceneBounds(state, bmin, bmax);
  const float extent = haveScene ? glm::length(bmax - bmin) : 1.0f;

  uint64_t removeId = 0;
  for (AppState::SectionView& sec : state.sections) {
    ImGui::PushID(static_cast<int>(sec.id));
    // Radio-ish: the active row owns the viewport drag arrow.
    bool active = state.sectionActiveId == sec.id;
    if (ImGui::RadioButton("###active", active))
      state.sectionActiveId = sec.id;
    ImGui::SetItemTooltip("The arrow in the viewport drags this one");
    ImGui::SameLine();
    ImGui::Checkbox("###visible", &sec.visible);
    ImGui::SetItemTooltip("Show this cut");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    // Source: the three base planes, then this part's datum planes, then the
    // picked face if there is one. Datums and faces need a part document, so
    // an assembly tab shows the three base planes alone — which is what it
    // has, not a bug.
    std::vector<const char*> sources = {planes[0], planes[1], planes[2]};
    std::vector<uint64_t> sourceIds = {0, 0, 0};
    if (state.document != nullptr) {
      for (size_t i = 0; i < state.document->FeatureCount(); i++) {
        if (auto* dp = dynamic_cast<DatumPlaneFeature*>(
                state.document->GetFeature(i))) {
          sources.push_back(dp->GetName().c_str());
          sourceIds.push_back(dp->GetId());
        }
      }
    }
    const bool onFace = sec.face.IsSet();
    if (onFace)
      sources.push_back("Picked face");
    int sel = sec.basePlane;
    if (onFace)
      sel = static_cast<int>(sources.size()) - 1;
    else if (sec.planeId != 0)
      for (size_t i = 3; i < sourceIds.size(); i++)
        if (sourceIds[i] == sec.planeId)
          sel = static_cast<int>(i);
    if (ImGui::Combo("###plane", &sel, sources.data(),
                     static_cast<int>(sources.size()))) {
      if (onFace && sel == static_cast<int>(sources.size()) - 1) {
        // keeping the face
      } else {
        sec.face = ElementRef{};
        sec.planeId = sourceIds[static_cast<size_t>(sel)];
        sec.basePlane = sec.planeId == 0 ? sel : sec.basePlane;
        // A new source makes the old offset meaningless — it was measured
        // from a different plane.
        sec.offset = sec.planeId == 0
                         ? CenteredSectionOffset(state, sec.basePlane)
                         : 0.0f;
      }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90.0f);
    // Step from the model's own size: 0.1 fixed units was invisible on a
    // 3-metre assembly and enormous on a 2 mm part.
    ExprDragFloat(state, "###offset", &sec.offset,
                  std::max(1e-4f, extent * 0.01f), -FLT_MAX, FLT_MAX, "%.3f");
    ImGui::SameLine();
    if (ImGui::Button("Flip###reverse"))
      sec.reverse = !sec.reverse;
    ImGui::SetItemTooltip("Keep the other side");
    ImGui::SameLine();
    if (ImGui::Button("x###remove"))
      removeId = sec.id;

    // Second row: tilt and the face pick, indented under the cut they belong
    // to. Angles are on their own line because a six-widget row is already
    // the widest thing in this window.
    ImGui::Indent(24.0f);
    ImGui::SetNextItemWidth(80.0f);
    ExprDragFloat(state, "###tiltU", &sec.tiltU, 0.5f, -89.0f, 89.0f, "%.1f°");
    ImGui::SetItemTooltip("Tilt about the plane's first in-plane axis");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ExprDragFloat(state, "###tiltV", &sec.tiltV, 0.5f, -89.0f, 89.0f, "%.1f°");
    ImGui::SetItemTooltip("Tilt about the second — the cut pivots about its "
                          "offset point, so it does not slide off the model");
    if (sec.tiltU != 0.0f || sec.tiltV != 0.0f) {
      ImGui::SameLine();
      if (ImGui::SmallButton("0°###untilt")) {
        sec.tiltU = 0.0f;
        sec.tiltV = 0.0f;
      }
    }
    if (state.document != nullptr) {
      ImGui::SameLine();
      const bool arming = state.slotPick.featureId == kSectionToolSlot &&
                          state.sectionActiveId == sec.id;
      if (arming)
        ImGui::BeginDisabled();
      if (ImGui::SmallButton(sec.face.IsSet() ? "Reface###pickface"
                                              : "Face###pickface"))
        ArmSectionFacePick(state, sec.id);
      if (arming)
        ImGui::EndDisabled();
      ImGui::SetItemTooltip("Cut along a planar face of the model");
      if (arming) {
        ImGui::SameLine();
        ImGui::TextDisabled("pick a face…");
      }
      if (sec.face.IsSet()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", sec.face.name.c_str());
      }
    }
    if (!SectionSourceResolved(state, sec)) {
      // Loud, because the alternative — cutting along some other plane — is
      // the failure that wastes an afternoon.
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                         "source missing — this cut is off");
      ImGui::SetItemTooltip(
          state.document == nullptr
              ? "A datum plane or face belongs to a part, so it has no "
                "meaning on an assembly tab. Pick a base plane here."
              : "The datum plane or face it cut along is gone. Pick a source "
                "again.");
    }
    ImGui::Unindent(24.0f);
    ImGui::PopID();
  }
  if (removeId)
    RemoveSection(state, removeId);

  if (state.sections.empty())
    ImGui::TextDisabled("No cuts yet.");
  ImGui::Separator();
  const bool full =
      state.sections.size() >= static_cast<size_t>(kMaxClipPlanes);
  ImGui::BeginDisabled(full);
  if (ImGui::Button("+ Add section###add"))
    AddSection(state, state.sections.empty() ? 0 : 1);
  ImGui::EndDisabled();
  if (full) {
    ImGui::SameLine();
    ImGui::TextDisabled("(%d is the limit)", kMaxClipPlanes);
  }
  if (haveScene && state.sectionActiveId != 0) {
    // Where the model is along the active cut's normal, so an offset that
    // cuts nothing is visibly out of range rather than a mystery.
    if (const AppState::SectionView* sec =
            FindSection(state, state.sectionActiveId)) {
      const glm::vec3 n = glm::vec3(SectionPlaneOf(state, *sec));
      float lo = FLT_MAX, hi = -FLT_MAX;
      for (int c = 0; c < 8; c++) {
        const glm::vec3 corner((c & 1) ? bmax.x : bmin.x,
                               (c & 2) ? bmax.y : bmin.y,
                               (c & 4) ? bmax.z : bmin.z);
        const float d = glm::dot(corner, n);
        lo = std::min(lo, d);
        hi = std::max(hi, d);
      }
      ImGui::TextDisabled("model spans %.3g to %.3g along this normal", lo, hi);
    }
  }
  ImGui::Separator();
  if (ImGui::Button("Clear all###clear"))
    ClearSections(state);
  ImGui::SameLine();
  if (ImGui::Button("Close###done"))
    state.sectionToolOpen = false; // the cuts stay; only the window closes
  ImGui::End();
}

// Drawing-dimension session (U7d): armed kind, the refs picked so far (each
// removable — was accumulate-only), cancel. The 2D pick loop itself stays in
// HandleDimTool (sheet space; the 3D pipeline doesn't run there).
void DrawDimToolWindow(AppState& state) {
  auto& tool = state.dimTool;
  const int need = (tool.kind == DrawingDimKind::Radius ||
                    tool.kind == DrawingDimKind::Diameter)
                       ? 1
                       : 2;
  BeginToolWindowFrame(state, DrawingDimKindToString(tool.kind));
  ImGui::TextDisabled("Pick edge %d of %d in one view  (Esc cancels)",
                      static_cast<int>(tool.refs.size()) + 1, need);
  for (size_t i = 0; i < tool.refs.size(); i++) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::SmallButton("x")) {
      tool.refs.erase(tool.refs.begin() + static_cast<long>(i));
      if (tool.refs.empty())
        tool.viewId = 0; // no refs left: the next pick may fix any view
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    const ProjRef& r = tool.refs[i];
    ImGui::TextUnformatted(r.edge.IsSet() ? r.edge.name.c_str()
                                          : r.silFace.name.c_str());
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::Button("Cancel"))
    tool = AppState::DimTool{};
  ImGui::End();
}

} // namespace

void DrawToolWindow(AppState& state) {
  // Sketch-mode sessions (U8a): plane pick + the active sketch tool's
  // options/Apply. Sketch mode owns the window while editing.
  if (state.sketchEdit.pickingPlane || state.sketchEdit.IsEditing()) {
    DrawSketchToolWindow(state);
    return;
  }
  // Mate sessions (assembly tabs) share the window; drawn by the panel
  // module that owns the mate helpers.
  if (state.assembly && state.mateTool.active) {
    DrawMateToolWindow(state);
    return;
  }
  if (state.drawing && state.dimTool.active) {
    DrawDimToolWindow(state);
    return;
  }
  if (state.drawing && state.drawingAddViewOpen) {
    DrawAddViewToolWindow(state);
    return;
  }
  size_t idx = 0;
  Feature* feature = ResolvePendingFeature(state, &idx);
  if (!feature) {
    state.pendingFeatureId = 0;
    // Committed-feature editor session (U7e): same window, live commits.
    if (state.editFeatureId != 0 && state.document &&
        !state.sketchEdit.IsEditing()) {
      size_t eidx = 0;
      Feature* ef = state.document->FindFeatureById(state.editFeatureId, &eidx);
      if (ef == nullptr) {
        state.editFeatureId = 0; // the session ends with its feature
      } else {
        BeginToolWindowFrame(state, ef->TypeName());
        DrawFeatureParamWidgets(state, *ef, eidx, /*commit=*/true);
        ImGui::Separator();
        if (ImGui::Button("Close")) {
          state.editFeatureId = 0;
          state.slotPick = {}; // armed slots end with the session
        }
        ImGui::End();
        return;
      }
    }
    if (state.measure.active)
      DrawMeasureToolWindow(state); // measure never coexists with a preview
    else if (state.sectionToolOpen)
      DrawSectionToolWindow(state);
    else if (state.camToolOpen)
      DrawCamToolWindow(state); // the CAM verbs (docs/cam/gui.md#verb-windows)
    else if (state.fastenerTool.open)
      DrawFastenerToolWindow(state); // docs/parts/README.md
    else if (state.assembly &&
             (state.asmEditOccurrence != 0 || state.asmEditRelation != 0))
      DrawAssemblyEditWindow(state); // docs/ui/panels.md
    else if (state.assembly && state.explodeToolOpen)
      DrawExplodeToolWindow(state); // docs/ui/panels.md
    else if (state.assembly && state.motionToolOpen)
      DrawMotionToolWindow(state);
    else if (state.assembly && state.simulationToolOpen)
      DrawSimulationToolWindow(state); // S1: the assembly's studies
    else if (state.document && state.simulation.active)
      DrawSimulationToolWindow(state); // a part's study, listed in its dock
    return;
  }

  BeginToolWindowFrame(state, feature->TypeName());

  DrawFeatureParamWidgets(state, *feature, idx, /*commit=*/false);

  ImGui::Separator();
  bool accept = false, cancel = false;

  // Why the preview shows nothing / why Accept is refused. The feature knows
  // ("shell needs a thinner wall", "hole sketch has no points", a dangling
  // ref), but until now the only sign was a red row in the feature tree —
  // easy to miss while looking at the dialog you are filling in. A failure
  // that only happened because an EARLIER feature failed is not this
  // dialog's fault, so it is shown without blocking Accept.
  const std::string& err = feature->GetError();
  bool blocking = !err.empty() && !feature->ErrorAfterEarlierFailure();

  // Would accepting this change anything? The regen cache already holds the
  // bodies before and after each feature, so this costs a comparison rather
  // than a rebuild: CachedBodiesAfter(idx - 1) is this feature's input, and
  // CachedBodiesAfter(idx) is what it made of it.
  //
  // A tool that runs cleanly and leaves the part identical is USUALLY a
  // mistake (an edge-less blend, a draft angle of zero, a split plane that
  // misses) — but not always: Refine on an already-clean body has nothing to
  // merge, and that is a fine answer. So this warns and never blocks (U10):
  // the silence was the bug, not the permissiveness.
  const bool touchesBodies =
      dynamic_cast<SketchFeature*>(feature) == nullptr &&
      dynamic_cast<DatumPlaneFeature*>(feature) == nullptr &&
      dynamic_cast<DatumAxisFeature*>(feature) == nullptr &&
      dynamic_cast<FrameFeature*>(feature) == nullptr &&
      dynamic_cast<CurveFeature*>(feature) == nullptr;
  bool noChange = false;
  if (!blocking && touchesBodies && state.document) {
    static const std::vector<Body> kNoBodies;
    const std::vector<Body>* after = state.document->CachedBodiesAfter(idx);
    const std::vector<Body>* before =
        idx == 0 ? &kNoBodies : state.document->CachedBodiesAfter(idx - 1);
    if (after != nullptr && before != nullptr) {
      noChange = after->size() == before->size();
      for (size_t i = 0; noChange && i < after->size(); i++) {
        const double a = (*after)[i].shape.Volume();
        const double b = (*before)[i].shape.Volume();
        noChange =
            std::abs(a - b) <= 1e-9 * std::max({1.0, std::abs(a), std::abs(b)});
      }
    }
  }

  const char* missing = nullptr;
  for (const SlotSpec& spec : SlotSpecsFor(*feature)) {
    if (spec.required && !SlotFilled(*feature, spec.slot)) {
      missing = spec.label;
      break;
    }
  }
  // A feature that says it cannot be committed (WhyIncomplete) is refused by
  // AcceptPendingFeature already; the button has to say so too, or a click
  // does nothing in silence. Slot-less inputs (a Derived's source) only
  // have this channel.
  if (missing == nullptr) {
    if (const std::string why = feature->WhyIncomplete(); !why.empty()) {
      blocking = true;
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.40f, 1.0f),
                         ICON_FA_TRIANGLE_EXCLAMATION " %s", why.c_str());
      ImGui::PopTextWrapPos();
    }
  }
  if (!state.slotPick.miss.empty() &&
      state.slotPick.featureId == feature->GetId()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.40f, 1.0f), "%s",
                       state.slotPick.miss.c_str());
    ImGui::PopTextWrapPos();
  }
  if (missing != nullptr) {
    blocking = true;
    ImGui::TextColored(ImVec4(0.85f, 0.75f, 0.40f, 1.0f),
                       ICON_FA_TRIANGLE_EXCLAMATION " Pick %s to continue",
                       missing);
  }

  if (noChange) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.35f, 1.0f),
                       ICON_FA_TRIANGLE_EXCLAMATION
                       " Changes nothing - the model would be identical "
                       "without this feature");
    ImGui::PopTextWrapPos();
  }
  if (!err.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.90f, 0.40f, 0.40f, 1.0f),
                       ICON_FA_TRIANGLE_EXCLAMATION " %s", err.c_str());
    ImGui::PopTextWrapPos();
  }

  // Accept carries the accent; Cancel is a plain button. They shipped as a
  // green/red pair, and red is this app's ERROR colour — it is what a failed
  // feature wears in the tree two panels away. Cancel is not destructive, so
  // borrowing it made the safest thing in the window look like the loudest.
  ImGui::BeginDisabled(blocking);
  ImGui::PushStyleColor(ImGuiCol_Button, AccentColor());
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentColorHovered());
  if (ImGui::Button(ICON_FA_CHECK " Accept###tw_accept"))
    accept = true;
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(ICON_FA_XMARK " Cancel###tw_cancel"))
    cancel = true;

  // Enter accepts, Escape cancels (when not typing in a field). While a
  // handle drag is live, Escape belongs to the drag (aborts it, preview
  // survives — ViewportInput's ladder). The SAME physical keypress that
  // confirms/cancels a value field must not also fire here: the field
  // deactivates that frame, so require a full idle frame in between, and
  // ignore key REPEAT (a held Enter from the field would re-fire otherwise).
  static int idleFrames = 0;
  idleFrames = ImGui::IsAnyItemActive() ? 0 : idleFrames + 1;
  if (idleFrames > 1 && !state.handleDrag.active) {
    if (!blocking && (ImGui::IsKeyPressed(ImGuiKey_Enter, /*repeat=*/false) ||
                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter,
                                          /*repeat=*/false)))
      accept = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, /*repeat=*/false))
      cancel = true;
  }

  ImGui::End();

  if (accept)
    AcceptPendingFeature(state);
  else if (cancel)
    CancelPendingFeature(state);
}

} // namespace ui
