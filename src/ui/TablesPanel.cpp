#include "ui/TablesPanel.h"

#include "core/Expression.h"
#include "core/LookupTable.h"
#include "ui/AppState.h"
#include "ui/PanelLayout.h"
#include "ui/VariableActions.h"
#include "ui/Widgets.h"

#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <string>

// The project tables' section of the Variables panel
// (docs/variables/README.md#tables): each table as a grid of key + one
// column per value, every cell editable in place, every edit one undoable
// project-scope change through ui::SetProjectTable — the same path as
// set_table, so the panel and the RPC refuse the same things in the same
// words.

namespace ui {

namespace {

// A name no table uses (table1, table2, ...).
std::string FreshTableName(const AppState& state) {
  for (int i = 1;; i++) {
    std::string name = "table" + std::to_string(i);
    if (lookuptable::Find(state.projectTables, name) == nullptr)
      return name;
  }
}

// The next key for a new row: one past the largest numeric key, else
// "row<N>". Never a key the table has.
std::string FreshKey(const lookuptable::Table& t) {
  double top = 0.0;
  bool any = false;
  for (const lookuptable::Row& r : t.rows) {
    double v = 0.0;
    const auto res =
        std::from_chars(r.key.data(), r.key.data() + r.key.size(), v);
    if (res.ec == std::errc() && res.ptr == r.key.data() + r.key.size()) {
      top = any ? std::max(top, v) : v;
      any = true;
    }
  }
  const auto taken = [&](const std::string& k) {
    for (const lookuptable::Row& r : t.rows)
      if (r.key == k)
        return true;
    return false;
  };
  if (any) {
    std::string k = lookuptable::NumericKey(std::floor(top) + 1.0);
    if (!taken(k))
      return k;
  }
  for (int i = static_cast<int>(t.rows.size()) + 1;; i++) {
    std::string k = "row" + std::to_string(i);
    if (!taken(k))
      return k;
  }
}

// Text input committing on deactivation-after-edit; returns the committed
// text or nullptr. One persistent buffer for whichever cell is active.
const char* CellInput(const std::string& id, const std::string& current) {
  static std::string activeId;
  static char buf[256];
  const bool editing = activeId == id;
  char tmp[sizeof(buf)];
  std::snprintf(tmp, sizeof(tmp), "%s", current.c_str());
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputText(id.c_str(), editing ? buf : tmp, sizeof(buf));
  if (ImGui::IsItemActivated()) {
    activeId = id;
    std::snprintf(buf, sizeof(buf), "%s", current.c_str());
  }
  if (editing && ImGui::IsItemDeactivatedAfterEdit()) {
    activeId.clear();
    return buf;
  }
  return nullptr;
}

std::string Num(double v) {
  char b[64];
  std::snprintf(b, sizeof(b), "%.10g", v);
  return b;
}

} // namespace

void DrawTablesSection(AppState& state) {
  if (!SectionHeader("Tables###tables_header",
                     static_cast<int>(state.projectTables.size())))
    return;
  // The last refusal, shown until the next edit succeeds.
  static std::string error;
  const auto apply = [&](lookuptable::Table t) {
    error = SetProjectTable(state, std::move(t));
  };

  if (ImGui::Button("+ Add table###tables_add")) {
    lookuptable::Table t;
    t.name = FreshTableName(state);
    t.columns = {"value"};
    apply(std::move(t));
  }
  if (state.projectTables.empty())
    ImGui::TextDisabled("lookup(\"table\", key, \"column\") reads a cell");

  // A copy: every edit below replaces the table it came from.
  const std::vector<lookuptable::Table> tables = state.projectTables;
  for (const lookuptable::Table& t : tables) {
    const std::string tid = "_" + t.name;
    ImGui::Spacing();
    // The table's name, editable: a rename is a new table under the new
    // name (expressions that read the old one then say so).
    LeftLabel("Table");
    if (const char* text = CellInput("###table_name" + tid, t.name)) {
      if (t.name != text) {
        lookuptable::Table renamed = t;
        renamed.name = text;
        if (lookuptable::Find(state.projectTables, text) != nullptr) {
          error = "a table '" + std::string(text) + "' exists already";
        } else if (std::string why = lookuptable::Validate(renamed);
                   !why.empty()) {
          error = why;
        } else if (std::vector<std::string> refs =
                       ProjectTableReferences(state, t.name);
                   !refs.empty()) {
          error = "table '" + t.name + "' is read by " + refs.front() +
                  (refs.size() > 1
                       ? " and " + std::to_string(refs.size() - 1) + " more"
                       : std::string()) +
                  " -- change those first";
        } else {
          error = SetProjectTable(state, renamed);
          if (error.empty())
            error = RemoveProjectTable(state, t.name);
        }
      }
    }

    const int ncols = static_cast<int>(t.columns.size()) + 2;
    if (ImGui::BeginTable(("###table" + tid).c_str(), ncols,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_SizingStretchSame)) {
      // Header row: the key, then each column's name, editable.
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("key");
      for (size_t c = 0; c < t.columns.size(); c++) {
        ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
        if (const char* text = CellInput(
                "###tcol" + tid + "_" + std::to_string(c), t.columns[c])) {
          if (t.columns[c] != text) {
            lookuptable::Table next = t;
            next.columns[c] = text;
            apply(std::move(next));
          }
        }
      }
      ImGui::TableSetColumnIndex(ncols - 1);
      if (ImGui::SmallButton(("+###tcol_add" + tid).c_str())) {
        lookuptable::Table next = t;
        std::string name;
        for (int i = static_cast<int>(t.columns.size()) + 1;; i++) {
          name = "c" + std::to_string(i);
          bool taken = false;
          for (const std::string& c : t.columns)
            taken |= c == name;
          if (!taken)
            break;
        }
        next.columns.push_back(name);
        for (lookuptable::Row& r : next.rows)
          r.values.push_back(0.0);
        apply(std::move(next));
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Add a column");

      for (size_t r = 0; r < t.rows.size(); r++) {
        const lookuptable::Row& row = t.rows[r];
        const std::string rid = tid + "_" + std::to_string(r);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (const char* text = CellInput("###tkey" + rid, row.key)) {
          if (row.key != text) {
            lookuptable::Table next = t;
            // A key typed as a number is that number's one spelling.
            std::string key = text;
            double v = 0.0;
            const auto res =
                std::from_chars(key.data(), key.data() + key.size(), v);
            if (res.ec == std::errc() && res.ptr == key.data() + key.size() &&
                std::isfinite(v))
              key = lookuptable::NumericKey(v);
            next.rows[r].key = key;
            apply(std::move(next));
          }
        }
        for (size_t c = 0; c < t.columns.size() && c < row.values.size(); c++) {
          ImGui::TableSetColumnIndex(static_cast<int>(c) + 1);
          if (const char* text =
                  CellInput("###tcell" + rid + "_" + std::to_string(c),
                            Num(row.values[c]))) {
            const std::string s = text;
            double v = 0.0;
            const auto res = std::from_chars(s.data(), s.data() + s.size(), v);
            if (res.ec != std::errc() || res.ptr != s.data() + s.size() ||
                !std::isfinite(v)) {
              error = "table '" + t.name + "': \"" + s +
                      "\" is not a number (a cell holds a number in mm or "
                      "degrees)";
            } else if (v != row.values[c]) {
              lookuptable::Table next = t;
              next.rows[r].values[c] = v;
              apply(std::move(next));
            }
          }
        }
        ImGui::TableSetColumnIndex(ncols - 1);
        if (ImGui::SmallButton(("x###trow_del" + rid).c_str())) {
          lookuptable::Table next = t;
          next.rows.erase(next.rows.begin() + static_cast<long>(r));
          apply(std::move(next));
        }
      }
      ImGui::EndTable();
    }
    // A new row copies the last one's values: the next size in a series is
    // nearer its neighbour than it is to zero.
    if (ImGui::SmallButton(("+ Row###trow_add" + tid).c_str())) {
      lookuptable::Table next = t;
      lookuptable::Row row;
      row.key = FreshKey(t);
      row.values = t.rows.empty() ? std::vector<double>(t.columns.size(), 0.0)
                                  : t.rows.back().values;
      next.rows.push_back(std::move(row));
      apply(std::move(next));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(("Delete table###table_del" + tid).c_str()))
      error = RemoveProjectTable(state, t.name);
  }

  if (!error.empty()) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "%s", error.c_str());
    ImGui::PopTextWrapPos();
  }
  ImGui::Spacing();
}

} // namespace ui
