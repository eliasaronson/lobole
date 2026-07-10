#include "ui/VariablesPanel.h"

#include "core/Command.h"
#include "core/Document.h"

#include "imgui.h"

#include <cstdio>
#include <string>
#include <vector>

namespace ui {

namespace {

// Persistent text for whichever cell is being edited. Every cell renders an
// InputText each frame, but only the active cell uses this buffer (claimed on
// activation); the rest are fed a per-frame temp seeded from the model, which
// is fine because ImGui keeps its own state for the active widget only.
struct CellEdit {
  int row = -1;
  int column = -1;
  char buf[256] = {};
};

// A name no existing parameter uses (var1, var2, ...).
std::string FreshName(const Document& doc) {
  for (int i = 1;; i++) {
    std::string name = "var" + std::to_string(i);
    if (doc.FindParameter(name) == nullptr) {
      return name;
    }
  }
}

} // namespace

void DrawVariablesPanel(AppState& state) {
  ImGui::Begin("Variables");
  if (!state.document) {
    ImGui::End();
    return;
  }
  Document& doc = *state.document;

  static CellEdit edit;
  // Rename/delete refusals; shown under the table until the next action.
  static std::string actionError;

  if (ImGui::Button("+ Add variable")) {
    state.commands->Execute(
        std::make_unique<SetVariableCommand>(FreshName(doc), "0"), doc);
    actionError.clear();
  }

  // Full-width editable cell. Returns the committed text when this cell just
  // finished an edit, nullptr otherwise.
  const auto cellInput = [&](int row, int column, const char* id,
                             const std::string& current) -> const char* {
    const bool editing = edit.row == row && edit.column == column;
    char tmp[sizeof(edit.buf)];
    snprintf(tmp, sizeof(tmp), "%s", current.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText(id, editing ? edit.buf : tmp, sizeof(edit.buf));
    if (ImGui::IsItemActivated()) {
      edit.row = row;
      edit.column = column;
      snprintf(edit.buf, sizeof(edit.buf), "%s", current.c_str());
    }
    if (editing && ImGui::IsItemDeactivatedAfterEdit()) {
      edit.row = edit.column = -1;
      return edit.buf;
    }
    return nullptr;
  };

  if (ImGui::BeginTable("##variables", 5,
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch,
                            1.4f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch,
                            1.0f);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 22.0f);
    ImGui::TableHeadersRow();

    // Copy: command execution below mutates the table mid-loop.
    const std::vector<Parameter> parameters = doc.GetParameters();
    for (int i = 0; i < static_cast<int>(parameters.size()); i++) {
      const Parameter& p = parameters[i];
      ImGui::TableNextRow();
      ImGui::PushID(i);

      // Name — commits a rename (rewrites referencing expressions).
      ImGui::TableSetColumnIndex(0);
      if (const char* text = cellInput(i, 0, "##name", p.name)) {
        const std::string newName = text;
        if (!newName.empty() && newName != p.name) {
          if (state.commands->Execute(
                  std::make_unique<RenameVariableCommand>(p.name, newName),
                  doc)) {
            actionError.clear();
          } else {
            actionError = "Cannot rename '" + p.name + "' to '" + newName +
                          "': invalid or already-used name.";
          }
        }
      }

      // Expression — commits a redefinition.
      ImGui::TableSetColumnIndex(1);
      const bool hasError = !p.error.empty();
      if (hasError) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
      }
      if (const char* text = cellInput(i, 1, "##expr", p.expression)) {
        if (p.expression != text) {
          state.commands->Execute(
              std::make_unique<SetVariableCommand>(p.name, text, p.comment),
              doc);
          actionError.clear();
        }
      }
      if (hasError) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("%s", p.error.c_str());
        }
      }

      // Value — read-only result of the last evaluation.
      ImGui::TableSetColumnIndex(2);
      if (hasError) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           p.error.c_str());
      } else {
        ImGui::Text("%.6g", p.value);
      }

      // Comment
      ImGui::TableSetColumnIndex(3);
      if (const char* text = cellInput(i, 3, "##comment", p.comment)) {
        if (p.comment != text) {
          state.commands->Execute(
              std::make_unique<SetVariableCommand>(p.name, p.expression, text),
              doc);
        }
      }

      // Delete — refused (with the referencing list) while referenced.
      ImGui::TableSetColumnIndex(4);
      if (ImGui::SmallButton("x")) {
        const std::vector<std::string> refs =
            doc.FindParameterReferences(p.name);
        if (refs.empty()) {
          state.commands->Execute(
              std::make_unique<DeleteVariableCommand>(p.name), doc);
          actionError.clear();
        } else {
          actionError = "Cannot delete '" + p.name + "' — referenced by";
          for (const std::string& r : refs) {
            actionError += " " + r + ",";
          }
          actionError.back() = '.';
        }
      }

      ImGui::PopID();
    }
    ImGui::EndTable();
  }

  if (!actionError.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                       actionError.c_str());
    ImGui::PopTextWrapPos();
  }

  ImGui::End();
}

} // namespace ui
