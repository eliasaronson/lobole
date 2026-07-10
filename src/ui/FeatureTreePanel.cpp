#include "ui/FeatureTreePanel.h"

#include "core/DatumPlaneFeature.h"
#include "core/Feature.h"
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
      if (ImGui::Selectable(label.c_str(),
                            state.selectedFeature == static_cast<int>(i),
                            flags)) {
        state.selectedFeature = static_cast<int>(i);
        if (datum && ImGui::IsMouseDoubleClicked(0)) {
          const int n = static_cast<int>(doc->FeatureCount()) + 1;
          BeginSketchOnDatum(state, *datum, "Sketch " + std::to_string(n));
        }
      }
      if (ImGui::BeginPopupContextItem()) {
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
