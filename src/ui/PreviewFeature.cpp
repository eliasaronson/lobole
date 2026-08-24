#include "ui/PreviewFeature.h"
#include "ui/OccurrencePanel.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h" // DrawingDimKind (dim session)
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h" // occurrence names (measure rows)
#include "ui/AppActions.h"               // SelectFeature
#include "ui/CamTool.h"                  // DrawCamToolWindow (CAM verbs)
#include "ui/FeatureEditor.h"
#include "ui/SketchToolbar.h" // DrawSketchToolWindow (U8a)

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
  state.commands->Execute(std::make_unique<AddFeatureCommand>(std::move(owned)),
                          *state.document);
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
  // viewport when it (re)opens — NEVER bottom-center, which occluded the
  // viewport clicks GUI tests make — and draggable for the session. One
  // stable id: only one session exists at a time, and tests address rows
  // as //<Title>###ToolWindow/###slotN.
  const glm::vec2 tr = state.viewportPos +
                       glm::vec2(state.viewportSize.x, 0.0f) +
                       glm::vec2(-12.0f, 12.0f);
  // Re-anchor when the VIEWPORT moves or resizes. Placing it once, on the
  // appearing frame, had two costs: the pivot was applied against a size
  // AlwaysAutoResize had not computed yet (hence the clamp below), and any
  // later layout change — docking a panel, closing the terminal — left the
  // window stranded over the middle of the model it is asking you to click
  // (docs/cam/dogfood-round.md, round 2). Two frames: one to learn the size,
  // one to place it against the real size. Dragging it still sticks, until
  // the viewport changes again.
  // A window that has just (re)appeared is anchored afresh, whatever it was
  // doing before: it may have been dragged, or closed in one layout and
  // reopened in another, and "the viewport has not moved since" is then the
  // wrong question.
  if (state.toolWindowLastFrame + 1 != ImGui::GetFrameCount())
    state.toolWindowAnchor = glm::vec2(-1.0f, -1.0f);
  state.toolWindowLastFrame = ImGui::GetFrameCount();
  if (glm::distance(state.toolWindowAnchor, tr) > 1.0f) {
    state.toolWindowAnchor = tr;
    state.toolWindowAnchorFrames = 2;
  }
  const ImGuiCond anchorCond =
      state.toolWindowAnchorFrames > 0 ? ImGuiCond_Always : ImGuiCond_Appearing;
  if (state.toolWindowAnchorFrames > 0)
    state.toolWindowAnchorFrames--;
  ImGui::SetNextWindowPos(ImVec2(tr.x, tr.y), anchorCond, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.96f);
  ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings;
  ImGui::Begin((title + "###ToolWindow").c_str(), nullptr, flags);
  // Keep the window inside the app window. The pivot anchoring above uses
  // the window's size, which is UNKNOWN on the appearing frame with
  // AlwaysAutoResize — the window landed hanging off the right edge and
  // stayed there. Clamp every frame (dragging just stops at the edges).
  const ImGuiViewport* mainVp = ImGui::GetMainViewport();
  const ImVec2 wpos = ImGui::GetWindowPos();
  const ImVec2 wsize = ImGui::GetWindowSize();
  ImVec2 clamped = wpos;
  clamped.x = std::min(clamped.x,
                       mainVp->WorkPos.x + mainVp->WorkSize.x - wsize.x - 4.0f);
  clamped.y = std::min(clamped.y,
                       mainVp->WorkPos.y + mainVp->WorkSize.y - wsize.y - 4.0f);
  clamped.x = std::max(clamped.x, mainVp->WorkPos.x + 4.0f);
  clamped.y = std::max(clamped.y, mainVp->WorkPos.y + 4.0f);
  if (clamped.x != wpos.x || clamped.y != wpos.y)
    ImGui::SetWindowPos(clamped);
  // Publish the rect so the camera fit can keep the model out from under it.
  const ImVec2 finalPos = ImGui::GetWindowPos();
  state.toolWindowRect = glm::vec4(finalPos.x, finalPos.y, finalPos.x + wsize.x,
                                   finalPos.y + wsize.y);
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
    std::string where;
    if (e.occurrenceId != 0 && state.assembly)
      if (const Occurrence* occ = state.assembly->FindById(e.occurrenceId))
        where = occ->name + " / ";
    ImGui::TextWrapped("%s%s %s", where.c_str(), e.isEdge ? "Edge" : "Face",
                       e.ref.name.c_str());
    ImGui::PopID();
  }
  if (!state.measure.readout.empty())
    ImGui::TextWrapped("%s", state.measure.readout.c_str());
  ImGui::Separator();
  if (ImGui::Button("Done"))
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
  const char* planes[] = {"Front (XY)", "Top (XZ)", "Right (YZ)"};

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
    ImGui::DragFloat("###offset", &sec.offset, std::max(1e-4f, extent * 0.01f));
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
    ImGui::DragFloat("###tiltU", &sec.tiltU, 0.5f, -89.0f, 89.0f, "%.1f°");
    ImGui::SetItemTooltip("Tilt about the plane's first in-plane axis");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::DragFloat("###tiltV", &sec.tiltV, 0.5f, -89.0f, 89.0f, "%.1f°");
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
  if (ImGui::Button("Done###done"))
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
      DrawCamToolWindow(state); // the CAM verbs (docs/cam/gui.md G2)
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

  ImGui::BeginDisabled(blocking);
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.25f, 0.65f, 0.30f, 1.0f));
  if (ImGui::Button(ICON_FA_CHECK " Accept###tw_accept"))
    accept = true;
  ImGui::PopStyleColor(2);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.22f, 0.22f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                        ImVec4(0.72f, 0.27f, 0.27f, 1.0f));
  if (ImGui::Button(ICON_FA_XMARK " Cancel###tw_cancel"))
    cancel = true;
  ImGui::PopStyleColor(2);

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
