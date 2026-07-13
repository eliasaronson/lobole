#include "ui/AppActions.h"

#include "imgui.h"

#include <glm/gtc/quaternion.hpp> // eulerAngles, quat(vec3)
#include <glm/trigonometric.hpp>  // degrees, radians

#include <cstring>
#include <string>

namespace ui {

namespace {
std::string PartNameById(const AppState& state, uint64_t partId) {
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.id == partId) return s.name;
  return "(missing part)";
}
} // namespace

// The occurrence list + properties editor, drawn in the "Model" panel (where a
// part tab shows its feature tree) when an assembly tab is active. Edits
// snapshot for undo; a drag is one undo step (snapshot on IsItemActivated).
void DrawOccurrencePanel(AppState& state) {
  ImGui::Begin("Model");
  if (!state.assembly) {
    ImGui::End();
    return;
  }
  AssemblyDocument& asmDoc = *state.assembly;

  ImGui::SeparatorText("Occurrences");
  if (asmDoc.OccurrenceCount() == 0)
    ImGui::TextDisabled("Empty. Place parts with add_occurrence.");

  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    ImGui::PushID(static_cast<int>(o.id));
    bool vis = o.visible;
    if (ImGui::Checkbox("##vis", &vis)) {
      SnapshotAssembly(state);
      asmDoc.FindById(o.id)->visible = vis;
      asmDoc.BumpVersion();
    }
    ImGui::SameLine();
    const std::string label =
        (o.name.empty() ? PartNameById(state, o.partId) : o.name) +
        (o.fixed ? "  (fixed)" : "");
    if (ImGui::Selectable(label.c_str(), state.selectedOccurrence == o.id))
      SelectOccurrence(state, o.id);
    ImGui::PopID();
  }

  Occurrence* sel = state.selectedOccurrence
                        ? asmDoc.FindById(state.selectedOccurrence)
                        : nullptr;
  if (sel) {
    ImGui::SeparatorText("Selected occurrence");
    ImGui::Text("Part: %s", PartNameById(state, sel->partId).c_str());

    char name[128];
    std::snprintf(name, sizeof(name), "%s", sel->name.c_str());
    if (ImGui::InputText("Name", name, sizeof(name),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      SnapshotAssembly(state);
      sel->name = name;
      asmDoc.BumpVersion();
    }

    glm::vec3 pos = sel->position;
    const bool posChanged = ImGui::DragFloat3("Position", &pos.x, 0.1f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state); // one snapshot per drag session
    if (posChanged) {
      sel->position = pos;
      asmDoc.BumpVersion();
    }

    glm::vec3 euler = glm::degrees(glm::eulerAngles(sel->orientation));
    const bool rotChanged = ImGui::DragFloat3("Rotation (deg)", &euler.x, 1.0f);
    if (ImGui::IsItemActivated())
      SnapshotAssembly(state);
    if (rotChanged) {
      sel->orientation = glm::quat(glm::radians(euler));
      asmDoc.BumpVersion();
    }

    bool fixed = sel->fixed;
    if (ImGui::Checkbox("Fixed (grounded)", &fixed)) {
      SnapshotAssembly(state);
      sel->fixed = fixed;
      asmDoc.BumpVersion();
    }

    if (ImGui::Button("Delete occurrence")) {
      const uint64_t id = sel->id;
      SnapshotAssembly(state);
      asmDoc.RemoveOccurrence(id);
      SelectOccurrence(state, 0);
    }
  }

  ImGui::End();
}

} // namespace ui
