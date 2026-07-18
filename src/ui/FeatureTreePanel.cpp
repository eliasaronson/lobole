#include "ui/FeatureTreePanel.h"

#include "core/Command.h"
#include "core/DatumPlaneFeature.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/Serialization.h"
#include "core/SketchFeature.h"
#include "ui/SketchInteraction.h"

#include "imgui.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

// The rollback bar: a grabbable divider drawn where regeneration stops (or at
// the end of the list). Sets `dragging` while the user holds it.
void RollbackBar(bool& dragging) {
  const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
  ImGui::PushStyleColor(ImGuiCol_Button,
                        ImVec4(accent.x, accent.y, accent.z, 0.45f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, accent);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, accent);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f);
  ImGui::Button("##RollbackBar", ImVec2(-FLT_MIN, 5.0f));
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  if (ImGui::IsItemHovered()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImGui::SetTooltip("Rollback bar - drag to replay history up to a point");
  }
  if (ImGui::IsItemActive())
    dragging = true;
}

} // namespace

void DrawFeatureTreePanel(AppState& state) {
  ImGui::Begin("Model");

  Document* doc = state.document.get();
  if (doc) {
    // Default geometry: the three base planes a sketch can be started on.
    ImGui::SeparatorText("Default geometry");
    static const char* kPlaneNames[3] = {"Front Plane", "Top Plane",
                                          "Right Plane"};
    for (int i = 0; i < 3; i++) {
      if (ImGui::Selectable(kPlaneNames[i], false,
                            ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(0)) {
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnPlane(state, DefaultPlane(i),
                             "Sketch " + std::to_string(n));
        }
      }
    }

    ImGui::SeparatorText("Features");
    const int rollback = doc->Rollback();
    const size_t count = doc->FeatureCount();
    // Inline rename (context menu > Rename): the row becomes a text field.
    static int renaming = -1;
    static char renameBuf[128];
    static bool renameFocus = false;
    // Rollback-bar drag: gap y positions collected this frame resolve the
    // drop target after the list is drawn.
    static bool draggingBar = false;
    std::vector<float> gapY; // screen y of the gap above row i; [count] = end

    for (size_t i = 0; i < count; i++) {
      gapY.push_back(ImGui::GetCursorScreenPos().y);
      if (rollback >= 0 && i == static_cast<size_t>(rollback))
        RollbackBar(draggingBar);
      Feature* feature = doc->GetFeature(i);

      if (renaming == static_cast<int>(i)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (renameFocus) {
          ImGui::SetKeyboardFocusHere();
          renameFocus = false;
        }
        const bool entered =
            ImGui::InputText("##rename", renameBuf, sizeof(renameBuf),
                             ImGuiInputTextFlags_EnterReturnsTrue);
        if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
          if (renameBuf[0] != '\0' && feature->GetName() != renameBuf) {
            nlohmann::json before = DocumentIO::FeatureToJson(*feature);
            feature->SetName(renameBuf);
            doc->Regenerate();
            nlohmann::json after = DocumentIO::FeatureToJson(*feature);
            state.commands->Execute(
                std::make_unique<ModifyFeatureCommand>(i, before, after),
                *doc);
          }
          renaming = -1;
        } else if (ImGui::IsItemDeactivated()) {
          renaming = -1; // escape / focus lost without an edit: cancel
        }
        continue;
      }

      // The rollback bar: features at/after the marker are skipped by
      // regeneration and render dimmed below the divider.
      const bool rolledPast =
          rollback >= 0 && i >= static_cast<size_t>(rollback);
      // Suffix keeps IDs unique and test paths stable even if names repeat
      const std::string label =
          feature->GetName() + "##" + std::to_string(i);
      // Datum planes are double-clickable to start a sketch on them, like the
      // base planes above.
      auto* datum = dynamic_cast<DatumPlaneFeature*>(feature);
      const ImGuiSelectableFlags flags =
          datum ? ImGuiSelectableFlags_AllowDoubleClick : 0;
      // Features that failed to regenerate show red, with the reason on
      // hover; suppressed and rolled-past features show dimmed.
      const bool suppressed = feature->IsSuppressed();
      const bool dimmed = suppressed || rolledPast;
      const bool errored = !dimmed && !feature->GetError().empty();
      if (errored)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.35f, 0.35f, 1.0f));
      else if (dimmed)
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
      if (ImGui::Selectable(label.c_str(),
                            state.selectedFeature == static_cast<int>(i),
                            flags)) {
        state.selectedFeature = static_cast<int>(i);
        if (datum && ImGui::IsMouseDoubleClicked(0)) {
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnDatum(state, *datum, "Sketch " + std::to_string(n));
        }
      }
      if (errored) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", feature->GetError().c_str());
      } else if (dimmed) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip(suppressed ? "Suppressed" : "After the rollback bar");
      }
      if (ImGui::BeginPopupContextItem()) {
        if (ImGui::MenuItem("Rename")) {
          renaming = static_cast<int>(i);
          snprintf(renameBuf, sizeof(renameBuf), "%s",
                   feature->GetName().c_str());
          renameFocus = true;
        }
        // Suppress/unsuppress: skipped by regeneration, params kept. One
        // undoable edit (suppressed round-trips through the feature JSON).
        if (ImGui::MenuItem(suppressed ? "Unsuppress" : "Suppress")) {
          nlohmann::json before = DocumentIO::FeatureToJson(*feature);
          feature->SetSuppressed(!suppressed);
          doc->Regenerate();
          nlohmann::json after = DocumentIO::FeatureToJson(*feature);
          state.commands->Execute(
              std::make_unique<ModifyFeatureCommand>(i, before, after), *doc);
        }
        // Rollback bar (not undoable — a view of the history, not an edit).
        if (ImGui::MenuItem("Rollback to here")) {
          doc->SetRollback(static_cast<int>(i));
        }
        if (rollback >= 0 && ImGui::MenuItem("Roll to end")) {
          doc->SetRollback(-1);
        }
        // A face-attached sketch can be re-pointed at another face (e.g. after
        // its face was consumed): arms face-picking for this sketch's faceRef.
        auto* sk = dynamic_cast<SketchFeature*>(feature);
        if (sk != nullptr && sk->faceRef.IsSet()) {
          if (ImGui::MenuItem("Reattach face…")) {
            state.sketchEdit = SketchEditState{};
            state.sketchEdit.pickingPlane = true;
            state.sketchEdit.reattachSketchId = sk->GetId();
          }
        }
        // A fillet/chamfer's edges can be re-picked (e.g. after a ref went
        // dangling): arms edge picking for this feature.
        auto* fil = dynamic_cast<FilletFeature*>(feature);
        if (fil != nullptr) {
          if (ImGui::MenuItem("Pick edges…")) {
            state.edgePick = {};
            state.edgePick.filletId = fil->GetId();
          }
        }
        if (ImGui::MenuItem("Delete")) {
          state.commands->Execute(std::make_unique<RemoveFeatureCommand>(i),
                                  *doc);
          if (state.selectedFeature == static_cast<int>(i)) {
            state.selectedFeature = -1;
          }
          ImGui::EndPopup();
          break; // feature list changed; redraw next frame
        }
        ImGui::EndPopup();
      }
    }
    gapY.push_back(ImGui::GetCursorScreenPos().y);
    if (count > 0 && rollback < 0)
      RollbackBar(draggingBar); // parked at the end: full history replays

    // Resolve a rollback-bar drag: preview the nearest gap while held, move
    // the marker on release. SetRollback regenerates, so only on release.
    if (draggingBar) {
      const float my = ImGui::GetIO().MousePos.y;
      size_t best = 0;
      float bestD = FLT_MAX;
      for (size_t k = 0; k < gapY.size(); k++) {
        const float d = std::abs(my - gapY[k]);
        if (d < bestD) {
          bestD = d;
          best = k;
        }
      }
      if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        const ImVec2 wp = ImGui::GetWindowPos();
        const float x0 = wp.x + ImGui::GetStyle().WindowPadding.x;
        const float x1 = wp.x + ImGui::GetWindowWidth() -
                         ImGui::GetStyle().WindowPadding.x;
        const ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_NavCursor);
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(x0, gapY[best]), ImVec2(x1, gapY[best]),
            ImGui::GetColorU32(accent), 2.0f);
      } else {
        draggingBar = false;
        doc->SetRollback(best == count ? -1 : static_cast<int>(best));
      }
    }
    if (count == 0) {
      ImGui::TextDisabled("No features yet - use the Create menu");
    }

    ImGui::SeparatorText("Bodies");
    for (const auto& body : doc->GetBodies()) {
      ImGui::BulletText("%s (volume %.2f)", body.name.c_str(),
                        body.shape.Volume());
    }
  }

  ImGui::End();
}

} // namespace ui
