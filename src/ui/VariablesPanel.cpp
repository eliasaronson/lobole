#include "ui/VariablesPanel.h"

#include "ui/DockLayout.h" // PanelHeading
#include "ui/ExprComplete.h"
#include "ui/PanelLayout.h"
#include "ui/PanelTitles.h"

#include "common/Logger.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Units.h"
#include "ui/VariableActions.h"

#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
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

// Document units (U13e). Here rather than in Preferences because they are a
// property of the DOCUMENT, not of the reader — Preferences would make a
// part authored in inches show as millimetres to whoever opened it next.
// This panel is already where the document's own parameters live.
//
// Display only: storage is millimetres, grams and degrees whatever these
// say, so changing one cannot move a vertex — which is also why it does not
// touch the undo stack or bump the document version. It does mark the
// project dirty, since it is written to the file.
void DrawUnitsSection(AppState& state, Document& doc) {
  if (!ImGui::CollapsingHeader("Units###units_header"))
    return;

  units::System sys = doc.Units();
  bool changed = false;

  const auto combo = [&](const char* label, const char* id,
                         const std::vector<const char*>& names,
                         const char* current, auto&& apply) {
    LeftLabel(label);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, current)) {
      for (const char* n : names)
        if (ImGui::Selectable(n, std::strcmp(n, current) == 0)) {
          apply(n);
          changed = true;
        }
      ImGui::EndCombo();
    }
  };

  combo("Length", "###units_length", units::LengthNames(),
        units::ToString(sys.length), [&](const char* n) {
          // The parser refuses rather than substitutes, and these strings
          // come straight from its own name list — so a false here is a bug
          // in that pairing, not bad input.
          if (!units::LengthFromString(n, sys.length))
            LOG_ERROR("units: unknown length '{}'", n);
        });
  combo("Mass", "###units_mass", units::MassNames(), units::ToString(sys.mass),
        [&](const char* n) {
          if (!units::MassFromString(n, sys.mass))
            LOG_ERROR("units: unknown mass '{}'", n);
        });
  combo("Angle", "###units_angle", units::AngleNames(),
        units::ToString(sys.angle), [&](const char* n) {
          if (!units::AngleFromString(n, sys.angle))
            LOG_ERROR("units: unknown angle '{}'", n);
        });

  if (changed) {
    doc.SetUnits(sys);
    // Not a document version bump: units are display-only, and bumping would
    // mark every solved study stale over a change that moves nothing.
    state.projectMetaDirty = true;
  }
  ImGui::Spacing();
}

} // namespace

void DrawVariablesPanel(AppState& state) {
  // A secondary tab must never steal the shared node from Model on its
  // first appearance (fresh layout builds select the last-appearing tab).
  ImGui::Begin(kPanelVariables, nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  PanelHeading(kPanelVariables);
  // NOT `if (!state.document) return`, which is what this did until
  // 2026-09-01 — the same defect that made File > Save a silent no-op
  // everywhere but a part tab. `state.document` is NULL on an assembly or a
  // drawing tab, and PROJECT variables live on the AppState, are undoable
  // ACROSS tabs and drive every part. Bailing here made the one scope that
  // spans the project uneditable from two of the three kinds of tab, with a
  // blank panel and no reason given.
  //
  // Only the part-local half needs a document, so only the part-local half
  // asks for one. The units section does too: units are a property of a
  // document, not of the project.
  Document* doc = state.document.get();

  if (doc)
    DrawUnitsSection(state, *doc);

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
  // `complete` opts the cell into variable completion: right for a
  // definition (height = width * 2), wrong for the name being defined and
  // noise in a comment.
  const auto cellInput = [&](int row, int column, const char* id,
                             const std::string& current, float width = -FLT_MIN,
                             bool complete = false) -> const char* {
    const bool editing = edit.row == row && edit.column == column;
    char tmp[sizeof(edit.buf)];
    snprintf(tmp, sizeof(tmp), "%s", current.c_str());
    ImGui::SetNextItemWidth(width);
    char* text = editing ? edit.buf : tmp;
    bool activated = false;
    bool left = false;
    if (complete) {
      const ExprFieldEvents ev =
          ExprCompletingInput(state, id, text, sizeof(edit.buf));
      activated = ev.activated;
      left = ev.left;
    } else {
      ImGui::InputText(id, text, sizeof(edit.buf));
      activated = ImGui::IsItemActivated();
      left = ImGui::IsItemDeactivatedAfterEdit();
    }
    if (activated) {
      edit.row = row;
      edit.column = column;
      snprintf(edit.buf, sizeof(edit.buf), "%s", current.c_str());
    }
    if (editing && left) {
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
          *doc);
    }
  };

  // Named, not "##": a hidden-label table is an unaddressable ID scope, so
  // every cell inside it could only be reached by a "**" wildcard crawl —
  // which the test engine can take minutes over. `//Variables/###vars_table/
  // ###var_expr_project_len` resolves directly.
  // The table ADAPTS instead of truncating. Six columns in a 210 px dock
  // gave every header about 35 px, which rendered as "Sco… N… E… V C…" — a
  // header row that names nothing is worse than no header row. So: Scope
  // becomes a one-glyph badge (its two values are "P" and "L"), Comment
  // appears only when there is room for it, and "Expression" shortens to
  // "Expr" when there is not. Widen the dock and the full table comes back.
  const float availW = ImGui::GetContentRegionAvail().x;
  const bool wide = availW >= ImGui::GetFontSize() * 30.0f;
  const int columns = wide ? 6 : 2;
  if (ImGui::BeginTable("###vars_table", columns,
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchProp)) {
    // Blank header: the column is a badge, and "Scope" never fitted anyway.
    ImGui::TableSetupColumn("###scope", ImGuiTableColumnFlags_WidthFixed,
                            ImGui::GetFontSize() * 1.2f);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.8f);
    if (wide) {
      ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch,
                              1.4f);
      ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch,
                              0.7f);
      ImGui::TableSetupColumn("Comment", ImGuiTableColumnFlags_WidthStretch,
                              1.0f);
      ImGui::TableSetupColumn("###del", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 1.4f);
      ImGui::TableHeadersRow(); // a header row of "N… E… V…" helps nobody
    }

    // Copy: command execution below mutates the tables mid-loop. Project
    // variables first (the default scope), then the active part's local ones.
    std::vector<VarRow> rows;
    for (const Parameter& p : state.projectParameters)
      rows.push_back({p, true});
    if (doc)
      for (const Parameter& p : doc->GetParameters())
        rows.push_back({p, false});

    for (int i = 0; i < static_cast<int>(rows.size()); i++) {
      const VarRow& r = rows[i];
      const Parameter& p = r.param;
      ImGui::TableNextRow();
      // Every cell carries a STABLE NAME instead of sitting in a PushID(i)
      // scope behind a "##" label. Both of those leave a widget reachable
      // only by numeric id (docs/ui/gui-coverage.md), and between them this
      // panel could not be clicked by a test or an agent at all — which is
      // why a whole workstream had zero GUI coverage. The scope is in the id
      // because a part variable is allowed to shadow a project one, so the
      // name alone is not unique.
      const std::string rowId =
          std::string(r.isProject ? "_project_" : "_part_") + p.name;
      const std::string nameId = "###var_name" + rowId;
      const std::string exprId = "###var_expr" + rowId;
      const std::string commentId = "###var_comment" + rowId;
      const std::string delId = "x###var_del" + rowId;

      // Where each field goes. WIDE is a real table row, one column per
      // field. NARROW is two rows of two columns — badge + name, then the
      // expression and its value underneath — because five columns in a
      // 210 px dock leave 30 px each, and a 30 px name field is not a field.
      // Same reasoning as LeftLabel: when it will not fit beside, stack it.
      const auto cell = [&](int field) {
        if (wide) {
          ImGui::TableSetColumnIndex(field);
          return;
        }
        switch (field) {
        case 0: // badge, first cell of row A
          ImGui::TableSetColumnIndex(0);
          break;
        case 1: // name, rest of row A
          ImGui::TableSetColumnIndex(1);
          break;
        case 3: // "= 60" beside the name
          ImGui::SameLine();
          break;
        case 2: // the expression opens row B
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(1);
          break;
        default: // delete, at the end of row B
          ImGui::SameLine();
          break;
        }
      };

      // Scope — Project (shared) vs Local to this part (shadows project).
      // One glyph, because the word never fitted; hover says which.
      cell(0);
      ImGui::TextDisabled("%s", r.isProject ? "P" : "L");
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", r.isProject
                                    ? "Project variable — shared by every part"
                                    : "Local to this part — shadows a project "
                                      "variable of the same name");

      // Name — commits a rename (rewrites referencing expressions).
      cell(1);
      // The evaluated value sits beside the name when narrow, so the name
      // field gives it room; the expression below gets the whole row.
      const float nameWidth = wide ? -FLT_MIN : -(ImGui::GetFontSize() * 3.5f);
      if (const char* text =
              cellInput(i, 1, nameId.c_str(), p.name, nameWidth)) {
        const std::string newName = text;
        if (!newName.empty() && newName != p.name) {
          const bool ok = r.isProject
                              ? RenameProjectVariable(state, p.name, newName)
                              : state.commands->Execute(
                                    std::make_unique<RenameVariableCommand>(
                                        p.name, newName),
                                    *doc);
          if (ok) {
            actionError.clear();
          } else {
            actionError = "Cannot rename '" + p.name + "' to '" + newName +
                          "': invalid or already-used name.";
          }
        }
      }

      // Value — read-only result of the last evaluation. In a narrow panel
      // this sits beside the name ("length = 60"), which is how a person
      // reads a variable anyway; the expression gets the row below.
      const bool hasError = !p.error.empty();
      cell(3);
      if (hasError) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s",
                           p.error.c_str());
        if (ImGui::IsItemHovered()) // the reason is longer than the column
          ImGui::SetTooltip("%s", p.error.c_str());
      } else {
        ImGui::Text("%.6g", p.value);
      }

      // Expression — commits a redefinition.
      cell(2);
      if (hasError) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.0f));
      }
      const float exprWidth = wide ? -FLT_MIN : -SmallButtonReserve("x");
      if (const char* text =
              cellInput(i, 2, exprId.c_str(), p.expression, exprWidth,
                        /*complete=*/true)) {
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

      // Comment — only shown when the panel is wide enough to hold it.
      if (wide) {
        cell(4);
        if (const char* text = cellInput(i, 4, commentId.c_str(), p.comment)) {
          if (p.comment != text)
            setVar(r, p.expression, text);
        }
      }

      // Delete — refused (with the referencing list) while referenced.
      cell(wide ? 5 : 9);
      if (ImGui::SmallButton(delId.c_str())) {
        std::vector<std::string> refs;
        const bool ok =
            r.isProject
                ? RemoveProjectVariable(state, p.name, &refs)
                : (refs = doc->FindParameterReferences(p.name),
                   refs.empty() &&
                       state.commands->Execute(
                           std::make_unique<DeleteVariableCommand>(p.name),
                           *doc));
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
