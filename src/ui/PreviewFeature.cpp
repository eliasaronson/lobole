#include "ui/PreviewFeature.h"

#include "core/Command.h"
#include "core/Document.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h" // DrawingDimKind (dim session)
#include "core/features/CurveFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/SketchFeature.h" // occurrence names (measure rows)
#include "ui/AppActions.h"               // SelectFeature
#include "ui/FeatureEditor.h"
#include "ui/SketchToolbar.h" // DrawSketchToolWindow (U8a)

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <algorithm> // std::min/max (session-window clamp)

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
  ImGui::SetNextWindowPos(ImVec2(tr.x, tr.y), ImGuiCond_Appearing,
                          ImVec2(1.0f, 0.0f));
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
