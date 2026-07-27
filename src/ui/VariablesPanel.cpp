#include "ui/VariablesPanel.h"

#include "core/Command.h"
#include "core/Document.h"
#include "ui/AppActions.h"

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

// One editable row: a parameter plus which scope it lives in (Phase V). Project
// variables are shared by every part; part variables are the active document's
// local scope and shadow a same-named project var.
struct VarRow {
  Parameter param;
  bool isProject = true;
};

// A name no project OR active-part variable uses (var1, var2, ...).
std::string FreshName(const AppState& state) {
  for (int i = 1;; i++) {
    std::string name = "var" + std::to_string(i);
    const bool taken =
        FindProjectParameter(state, name) != nullptr ||
        (state.document && state.document->FindParameter(name) != nullptr);
    if (!taken)
      return name;
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

  // Add defaults to PROJECT scope (VD2: define a driving dimension once).
  if (ImGui::Button("+ Add variable")) {
    SetProjectVariable(state, FreshName(state), "0", "");
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

  // Redefine a variable in its scope (project or part-local).
  const auto setVar = [&](const VarRow& r, const std::string& expr,
                          const std::string& comment) {
    if (r.isProject) {
      SetProjectVariable(state, r.param.name, expr, comment);
    } else {
      state.commands->Execute(
          std::make_unique<SetVariableCommand>(r.param.name, expr, comment),
          doc);
    }
  };

  if (ImGui::BeginTable("##variables", 6,
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch,
                            1.4f);
    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch,
                            1.0f);
    ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 22.0f);
    ImGui::TableHeadersRow();

    // Copy: command execution below mutates the tables mid-loop. Project
    // variables first (the default scope), then the active part's local ones.
    std::vector<VarRow> rows;
    for (const Parameter& p : state.projectParameters)
      rows.push_back({p, true});
    for (const Parameter& p : doc.GetParameters())
      rows.push_back({p, false});

    for (int i = 0; i < static_cast<int>(rows.size()); i++) {
      const VarRow& r = rows[i];
      const Parameter& p = r.param;
      ImGui::TableNextRow();
      ImGui::PushID(i);

      // Scope — Project (shared) vs Part (this document, shadows project).
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(r.isProject ? "Project" : "Part");

      // Name — commits a rename (rewrites referencing expressions).
      ImGui::TableSetColumnIndex(1);
      if (const char* text = cellInput(i, 1, "##name", p.name)) {
        const std::string newName = text;
        if (!newName.empty() && newName != p.name) {
          const bool ok = r.isProject
                              ? RenameProjectVariable(state, p.name, newName)
                              : state.commands->Execute(
                                    std::make_unique<RenameVariableCommand>(
                                        p.name, newName),
                                    doc);
          if (ok) {
            actionError.clear();
          } else {
            actionError = "Cannot rename '" + p.name + "' to '" + newName +
                          "': invalid or already-used name.";
          }
        }
      }

      // Expression — commits a redefinition.
      ImGui::TableSetColumnIndex(2);
      const bool hasError = !p.error.empty();
      if (hasError) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
      }
      if (const char* text = cellInput(i, 2, "##expr", p.expression)) {
        if (p.expression != text) {
          setVar(r, text, p.comment);
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
      ImGui::TableSetColumnIndex(3);
      if (hasError) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           p.error.c_str());
      } else {
        ImGui::Text("%.6g", p.value);
      }

      // Comment
      ImGui::TableSetColumnIndex(4);
      if (const char* text = cellInput(i, 4, "##comment", p.comment)) {
        if (p.comment != text)
          setVar(r, p.expression, text);
      }

      // Delete — refused (with the referencing list) while referenced.
      ImGui::TableSetColumnIndex(5);
      if (ImGui::SmallButton("x")) {
        std::vector<std::string> refs;
        const bool ok =
            r.isProject
                ? RemoveProjectVariable(state, p.name, &refs)
                : (refs = doc.FindParameterReferences(p.name),
                   refs.empty() &&
                       state.commands->Execute(
                           std::make_unique<DeleteVariableCommand>(p.name),
                           doc));
        if (ok) {
          actionError.clear();
        } else {
          actionError = "Cannot delete '" + p.name + "' — referenced by";
          for (const std::string& r2 : refs)
            actionError += " " + r2 + ",";
          if (!refs.empty())
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
