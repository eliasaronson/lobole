#include "ui/FeatureTreePanel.h"

#include "core/DatumPlaneFeature.h"
#include "core/Feature.h"
#include "core/FilletFeature.h"
#include "core/SketchFeature.h"
#include "ui/SketchInteraction.h"

#include "imgui.h"

namespace ui {

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
    for (size_t i = 0; i < doc->FeatureCount(); i++) {
      Feature* feature = doc->GetFeature(i);
      // Suffix keeps IDs unique and test paths stable even if names repeat
      const std::string label =
          feature->GetName() + "##" + std::to_string(i);
      // Datum planes are double-clickable to start a sketch on them, like the
      // base planes above.
      auto* datum = dynamic_cast<DatumPlaneFeature*>(feature);
      const ImGuiSelectableFlags flags =
          datum ? ImGuiSelectableFlags_AllowDoubleClick : 0;
      // Features that failed to regenerate show red, with the reason on hover.
      const bool errored = !feature->GetError().empty();
      if (errored)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.92f, 0.35f, 0.35f, 1.0f));
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
      }
      if (ImGui::BeginPopupContextItem()) {
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
    if (doc->FeatureCount() == 0) {
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
