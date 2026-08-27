#include "ui/ExprComplete.h"

#include "core/Document.h"

#include "imgui_internal.h" // BringWindowToDisplayFront / GetInputTextState

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ui {

namespace {

// What the expression engine knows besides the document's variables. Kept
// here rather than asked of the engine because the engine's interface is
// evaluate-only; if a second engine ever lands, this list moves behind it.
constexpr const char* kFunctions[] = {"sin", "cos", "tan",   "sqrt", "abs",
                                      "min", "max", "floor", "ceil"};
constexpr const char* kUnits[] = {"mm", "cm", "m", "in", "ft", "deg", "rad"};

struct Candidate {
  std::string name;
  std::string hint; // "= 42.5", "project 30", "function", "unit"
  bool isFunction = false;
};

// One field completes at a time — there is one active item.
ImGuiID g_owner = 0;
std::vector<Candidate> g_cands;
int g_sel = 0;
bool g_selMoved = false; // the highlight was moved deliberately (Up/Down)
ImVec2 g_pos;            // where the popup goes: under the field
float g_width = 0.0f;
int g_start = 0, g_end = 0; // the identifier prefix being completed
int g_frame = 0;            // last frame Refresh ran

bool IsIdentChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool StartsWithNoCase(const std::string& s, const std::string& prefix) {
  if (s.size() < prefix.size())
    return false;
  for (size_t i = 0; i < prefix.size(); i++)
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  return true;
}

} // namespace

void ExprCompleteClose() {
  g_owner = 0;
  g_cands.clear();
  g_sel = 0;
  g_selMoved = false;
}

void ExprCompleteRefresh(AppState& state, ImGuiID fieldId, const char* buf) {
  if (ImGui::GetActiveID() != fieldId || state.document == nullptr) {
    if (g_owner == fieldId)
      ExprCompleteClose();
    return;
  }
  ImGuiInputTextState* st = ImGui::GetInputTextState(fieldId);
  if (st == nullptr) {
    ExprCompleteClose();
    return;
  }
  const int len = static_cast<int>(std::strlen(buf));
  const int cursor = std::clamp(st->GetCursorPos(), 0, len);
  int start = cursor;
  while (start > 0 && IsIdentChar(buf[start - 1]))
    start--;
  // Nothing under the caret, or a number (10e5 is a literal, not a name).
  if (start == cursor ||
      std::isdigit(static_cast<unsigned char>(buf[start])) != 0) {
    ExprCompleteClose();
    return;
  }
  const std::string prefix(buf + start, buf + cursor);

  std::vector<Candidate> cands;
  const Document& doc = *state.document;
  const SymbolTable* project = doc.ProjectSymbols();
  for (const auto& [name, value] : doc.Symbols()) {
    if (!StartsWithNoCase(name, prefix))
      continue;
    char hint[64];
    const bool isProject = project != nullptr && project->count(name) != 0;
    std::snprintf(hint, sizeof(hint), isProject ? "project  = %.6g" : "= %.6g",
                  value);
    cands.push_back({name, hint, false});
  }
  std::sort(
      cands.begin(), cands.end(),
      [](const Candidate& a, const Candidate& b) { return a.name < b.name; });
  for (const char* f : kFunctions)
    if (StartsWithNoCase(f, prefix))
      cands.push_back({f, "function", true});
  for (const char* u : kUnits)
    if (StartsWithNoCase(u, prefix))
      cands.push_back({u, "unit", false});

  // Nothing to offer when the name is already complete and unambiguous —
  // which is also what stops the popup reappearing over the name it just
  // inserted.
  if (cands.empty() || (cands.size() == 1 && cands[0].name == prefix)) {
    ExprCompleteClose();
    return;
  }
  constexpr size_t kMaxRows = 8;
  if (cands.size() > kMaxRows)
    cands.resize(kMaxRows);

  if (g_owner != fieldId)
    ExprCompleteClose();
  g_owner = fieldId;
  g_cands = std::move(cands);
  g_sel = std::clamp(g_sel, 0, static_cast<int>(g_cands.size()) - 1);
  g_start = start;
  g_end = cursor;
  g_pos = ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
  g_width = ImGui::GetItemRectSize().x;
  g_frame = ImGui::GetFrameCount();
}

ExprPick ExprCompletePopup(AppState& state, ImGuiID fieldId, const char* buf,
                           size_t bufSize) {
  (void)state;
  ExprPick out;
  if (g_owner != fieldId || g_cands.empty())
    return out;
  // The field stopped being drawn (its panel closed, its feature went away).
  if (ImGui::GetFrameCount() > g_frame + 1) {
    ExprCompleteClose();
    return out;
  }

  const int n = static_cast<int>(g_cands.size());
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
    g_sel = (g_sel + 1) % n;
    g_selMoved = true;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
    g_sel = (g_sel + n - 1) % n;
    g_selMoved = true;
  }
  // Tab always accepts. Enter accepts only once a row has been chosen on
  // purpose — otherwise Enter is what commits the field, and a popup that
  // happens to be open must not swallow it.
  bool accept = ImGui::IsKeyPressed(ImGuiKey_Tab) ||
                (g_selMoved && (ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                                ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)));

  const ImGuiStyle& style = ImGui::GetStyle();
  ImGui::SetNextWindowPos(g_pos);
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(std::max(g_width, ImGui::GetFontSize() * 12.0f), 0.0f),
      ImVec2(FLT_MAX, FLT_MAX));
  ImGui::Begin(
      "##expr_complete", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNavInputs |
          ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking);
  // Above the panel that owns the field without taking focus from it: a
  // focused window renders over an unfocused one, and the field's window is
  // by definition focused.
  ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
  const float hintCol = ImGui::GetFontSize() * 9.0f;
  for (int i = 0; i < n; i++) {
    // "name##i": the row shows the name and is addressable by it (a test or
    // a script picks "plate_width##0"), while the index keeps the id unique
    // if a parameter ever shares a name with a function.
    const std::string rowId = g_cands[i].name + "##" + std::to_string(i);
    if (ImGui::Selectable(rowId.c_str(), i == g_sel)) {
      // (Selectable reports on RELEASE; the press is what deactivates the
      // field, so the press below is the one that has to be caught.)
      g_sel = i;
      accept = true;
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      g_sel = i;
      accept = true;
    }
    ImGui::SameLine(hintCol, style.ItemInnerSpacing.x);
    ImGui::TextDisabled("%s", g_cands[i].hint.c_str());
  }
  ImGui::End();

  if (!accept)
    return out;

  // Splice the name over the prefix. A function brings its open paren, so
  // the caret lands where the argument goes.
  const Candidate& c = g_cands[g_sel];
  std::string insert = c.name;
  if (c.isFunction)
    insert += "(";
  const int len = static_cast<int>(std::strlen(buf));
  const int start = std::clamp(g_start, 0, len);
  const int end = std::clamp(g_end, start, len);
  std::string next(buf, buf + start);
  next += insert;
  const int caret = static_cast<int>(next.size());
  next.append(buf + end, buf + len);
  if (next.size() + 1 > bufSize)
    return out; // would not fit: leave the text alone
  out.picked = true;
  out.caret = caret;
  out.text = std::move(next);
  ExprCompleteClose();
  return out;
}

namespace {

// The re-focus a pick needs, for the text fields that do not have the drag
// stand-in's own arming. One slot: one field is active at a time.
ImGuiID g_refocusId = 0;
int g_refocusCaret = -1;
int g_refocusArm = 0;

} // namespace

ExprFieldEvents ExprCompletingInput(AppState& state, const char* label,
                                    char* buf, size_t bufSize) {
  ExprFieldEvents ev;
  const ImGuiID id = ImGui::GetID(label);
  const ExprPick pick = ExprCompletePopup(state, id, buf, bufSize);
  if (pick.picked) {
    g_refocusId = id;
    g_refocusCaret = pick.caret;
    g_refocusArm = 8;
  }
  const bool refocusing = g_refocusId == id && g_refocusArm > 0;
  if (refocusing) {
    // Every frame until it lands: the click that picked a row holds the
    // active id while the button is down, and swallows the first request.
    ImGui::SetKeyboardFocusHere();
    g_refocusArm--;
    if (ImGuiInputTextState* st = ImGui::GetInputTextState(id)) {
      st->WantReloadUserBuf = true;
      st->ReloadSelectionStart = st->ReloadSelectionEnd = g_refocusCaret;
    }
  }

  ImGui::InputText(label, buf, bufSize);
  if (pick.picked) {
    // After the InputText, never before: a deactivating one writes its own
    // text into the buffer on its way out. No refresh on this frame — the
    // field's ImGui state still holds the pre-pick text, so the caret it
    // reports would not line up with the buffer.
    std::snprintf(buf, bufSize, "%s", pick.text.c_str());
  } else {
    ExprCompleteRefresh(state, id, buf);
  }

  const bool active = ImGui::IsItemActive();
  // NOT on the pick frame: the reload above ran before the picked text
  // reached the buffer, so it reloaded the pre-pick text. Keeping the
  // request alive one more frame is what actually gets the completion into
  // the field — otherwise ImGui's copy wins and commits the prefix.
  if (active && g_refocusId == id && !pick.picked) {
    g_refocusId = 0;
    g_refocusArm = 0;
    g_refocusCaret = -1;
  }
  ev.activated = ImGui::IsItemActivated() && !refocusing;
  ev.left = ImGui::IsItemDeactivated() && !pick.picked && !refocusing &&
            g_refocusId != id;
  return ev;
}

} // namespace ui
