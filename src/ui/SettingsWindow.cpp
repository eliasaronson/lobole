#include "ui/SettingsWindow.h"
#include "ui/Widgets.h"

#include "ui/ExprInput.h" // expression-capable numeric fields

#include "core/io/GitVault.h"

#include "common/Logger.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamPost.h"
#include "core/io/Keymap.h"
#include "core/io/Serialization.h" // the machine store
#include "core/io/Settings.h"
#include "ui/Commands.h"
#include "ui/MachineActions.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {

namespace {

// A slider drag is one write, not sixty.
constexpr double kSaveDebounceSeconds = 1.0;

const char* TierNote(settings::Tier tier) {
  switch (tier) {
  case settings::Tier::Live:
    return nullptr; // no note: it already happened
  case settings::Tier::NextTime:
    return "takes effect next time";
  case settings::Tier::Restart:
    return "needs a restart";
  }
  return nullptr;
}

// The muted explanation under a control, plus the tier when it is not Live.
// A settings window that does not explain its settings moves the confusion
// rather than removing it.
void DrawBlurb(const settings::Field& f, const char* extra = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  std::string text = f.blurb;
  if (const char* note = TierNote(f.tier))
    text += std::string(" (") + note + ")";
  if (extra)
    text += std::string(" ") + extra;
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// Width reserved for a control: MEASURED, never a pixel literal. The style
// scales with the display's content scale and with the font-size row this
// very window owns, so a hard-coded number is wrong on someone's screen.
float ControlWidth() {
  const ImGuiStyle& style = ImGui::GetStyle();
  return ImGui::CalcTextSize("NVIDIA GeForce RTX 4060 Laptop GPU").x +
         style.FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}

bool DrawField(AppState& state, const settings::Field& f) {
  settings::Settings& s = state.settings;
  const std::string id = std::string("###set_") + f.key;
  bool changed = false;
  ImGui::TextUnformatted(f.label);

  const float width =
      std::min(ControlWidth(), ImGui::GetContentRegionAvail().x);
  ImGui::SetNextItemWidth(width);

  if (auto* m = std::get_if<bool settings::Settings::*>(&f.member)) {
    bool value = s.*(*m);
    if (ImGui::Checkbox(id.c_str(), &value)) {
      s.*(*m) = value;
      changed = true;
    }
  } else if (auto* m =
                 std::get_if<std::string settings::Settings::*>(&f.member)) {
    if (f.freeText) {
      // A name or an email: no list to choose from. Committed on the way out
      // of the field rather than per keystroke, so half-typed values never
      // reach the settings file.
      char buffer[256] = {};
      const std::string current = s.*(*m);
      std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());
      if (ImGui::InputText(id.c_str(), buffer, sizeof(buffer),
                           ImGuiInputTextFlags_EnterReturnsTrue) ||
          ImGui::IsItemDeactivatedAfterEdit()) {
        s.*(*m) = buffer;
        changed = true;
      }
    } else if (!f.values.empty()) {
      const std::string current = s.*(*m);
      if (ImGui::BeginCombo(id.c_str(), current.c_str())) {
        for (const char* option : f.values) {
          if (ImGui::Selectable(option, current == option)) {
            s.*(*m) = option;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
    } else {
      // Free-text with a machine-supplied list: the GPU row. "Automatic" is
      // the empty string, spelled out so it does not read as a broken entry.
      const std::string current = s.*(*m);
      const char* label = current.empty() ? "Automatic" : current.c_str();
      if (ImGui::BeginCombo(id.c_str(), label)) {
        if (ImGui::Selectable("Automatic", current.empty())) {
          (s.*(*m)).clear();
          changed = true;
        }
        for (const std::string& device : state.gpuDevices) {
          if (ImGui::Selectable(device.c_str(), current == device)) {
            s.*(*m) = device;
            changed = true;
          }
        }
        ImGui::EndCombo();
      }
    }
  } else if (!f.choices.empty()) {
    // A fixed set of numbers is a combo, not a slider: "1024 or 2048" is a
    // choice, and a slider would imply 1536 means something.
    const double current = [&] {
      if (auto* fm = std::get_if<float settings::Settings::*>(&f.member))
        return static_cast<double>(s.*(*fm));
      if (auto* im = std::get_if<int settings::Settings::*>(&f.member))
        return static_cast<double>(s.*(*im));
      if (auto* dm = std::get_if<double settings::Settings::*>(&f.member))
        return s.*(*dm);
      return 0.0;
    }();
    // "%g", not to_string + trimming: 0.85f * 100 is 85.000002 in float, and
    // a combo offering "85.000002%" reads as a bug.
    const auto text = [&f](double v) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "%g%s", v * f.displayScale, f.unit);
      return std::string(buf);
    };
    if (ImGui::BeginCombo(id.c_str(), text(current).c_str())) {
      for (float option : f.choices) {
        const bool selected = std::abs(current - option) < 1e-6;
        if (ImGui::Selectable(text(option).c_str(), selected)) {
          if (auto* fm = std::get_if<float settings::Settings::*>(&f.member))
            s.*(*fm) = option;
          else if (auto* im = std::get_if<int settings::Settings::*>(&f.member))
            s.*(*im) = static_cast<int>(option);
          else if (auto* dm =
                       std::get_if<double settings::Settings::*>(&f.member))
            s.*(*dm) = option;
          changed = true;
        }
      }
      ImGui::EndCombo();
    }
  } else if (auto* dm = std::get_if<double settings::Settings::*>(&f.member)) {
    int value = static_cast<int>(s.*(*dm));
    if (ExprDragInt(state, id.c_str(), &value, 1.0f, static_cast<int>(f.min),
                    static_cast<int>(f.max), "%d s")) {
      s.*(*dm) = value;
      changed = true;
    }
  }
  return changed;
}

// Preferences > Machines (docs/cam/setup.md#machine-profiles). Not a
// `Field` either: a LIST of structs, each its own file in
// ~/.config/designer/machines, written on every edit the way settings.json
// is -- there is no Save button to forget. A job names one of these and
// embeds a copy; the CAM panel says when the two differ.
std::string g_machineError; // the last refusal from the store, in words

void DrawMachinesSection(AppState& state) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(
      "The machines in your shop: post dialect, arc fitting, rotary axis, "
      "work area and rapid. A CAM setup picks one and keeps a copy, so a job "
      "file opens on a machine that has never seen the profile.");
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();

  const auto commit = [&](const cam::Machine& m) {
    std::string error;
    if (!SaveMachine(state, m, &error))
      g_machineError = error;
    else
      g_machineError.clear();
  };

  if (ImGui::Button("New###mach_new")) {
    // A name nobody has, so the file can be written at once: the editor
    // below renames it, and a rename is delete-then-save.
    cam::Machine fresh;
    for (int n = 1;; n++) {
      fresh.name = "Machine " + std::to_string(n);
      if (cam::FindMachine(state.machines, fresh.name) == nullptr)
        break;
    }
    commit(fresh);
    for (size_t i = 0; i < state.machines.size(); i++)
      if (state.machines[i].name == fresh.name)
        state.machineEditIndex = static_cast<int>(i);
  }
  ImGui::SameLine();
  const bool have =
      state.machineEditIndex >= 0 &&
      state.machineEditIndex < static_cast<int>(state.machines.size());
  ImGui::BeginDisabled(!have);
  if (ImGui::Button("Duplicate###mach_dup") && have) {
    cam::Machine copy =
        state.machines[static_cast<size_t>(state.machineEditIndex)];
    const std::string base = copy.name;
    for (int n = 2;; n++) {
      copy.name = base + " " + std::to_string(n);
      if (cam::FindMachine(state.machines, copy.name) == nullptr)
        break;
    }
    commit(copy);
    for (size_t i = 0; i < state.machines.size(); i++)
      if (state.machines[i].name == copy.name)
        state.machineEditIndex = static_cast<int>(i);
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete###mach_delete") && have) {
    std::string error;
    if (!DeleteMachine(
            state,
            state.machines[static_cast<size_t>(state.machineEditIndex)].name,
            &error))
      g_machineError = error;
    else
      g_machineError.clear();
    state.machineEditIndex = -1;
  }
  ImGui::EndDisabled();
  // Re-asked AFTER the buttons: Delete has just emptied the selection, and
  // the stale answer indexed a list that no longer held it.
  const bool selected =
      state.machineEditIndex >= 0 &&
      state.machineEditIndex < static_cast<int>(state.machines.size());

  if (state.machines.empty())
    ImGui::TextDisabled("No machines yet.");
  for (size_t i = 0; i < state.machines.size(); i++) {
    ImGui::PushID(static_cast<int>(i));
    if (ImGui::Selectable(state.machines[i].name.c_str(),
                          static_cast<int>(i) == state.machineEditIndex))
      state.machineEditIndex = static_cast<int>(i);
    ImGui::PopID();
  }
  if (!g_machineError.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", g_machineError.c_str());
    ImGui::PopStyleColor();
  }
  if (!selected)
    return;

  // The editor. Every edit is written to the file at once; a rename is a
  // delete of the old file and a save of the new, refused (and reverted)
  // when the new name would share a file with another machine.
  cam::Machine& m = state.machines[static_cast<size_t>(state.machineEditIndex)];
  ImGui::Separator();
  static char nameBuf[96];
  static int nameFor = -1;
  if (nameFor != state.machineEditIndex || !ImGui::IsAnyItemActive()) {
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", m.name.c_str());
    nameFor = state.machineEditIndex;
  }
  ImGui::SetNextItemWidth(ControlWidth());
  ImGui::InputText("Name###mach_name", nameBuf, sizeof(nameBuf));
  if (ImGui::IsItemDeactivatedAfterEdit() && m.name != nameBuf &&
      nameBuf[0] != '\0') {
    cam::Machine renamed = m;
    renamed.name = nameBuf;
    std::string error;
    if (cam::FindMachine(state.machines, renamed.name) != nullptr) {
      g_machineError =
          "there is already a machine called '" + renamed.name + "'";
    } else if (!DocumentIO::SaveMachine(MachineDir(), renamed, &error)) {
      g_machineError = error;
    } else {
      (void)DocumentIO::DeleteMachine(MachineDir(), m.name, nullptr);
      ReloadMachines(state);
      g_machineError.clear();
      for (size_t i = 0; i < state.machines.size(); i++)
        if (state.machines[i].name == renamed.name)
          state.machineEditIndex = static_cast<int>(i);
      return; // `m` is gone
    }
  }

  bool edited = false;
  // The work area as a SIZE per axis, each field named. It was two rows of
  // three unlabelled boxes (travel min / travel max), which read as six
  // numbers with no axis on them; a hobby machine is known by its bed --
  // "600 by 400, 80 of Z" -- and that is what is asked. Min stays where the
  // file put it (zero unless a file said otherwise), max = min + size.
  ImGui::TextDisabled("Work area (mm) — leave at zero to check nothing");
  static const char* const kAxis[3] = {"X###mach_wa_x", "Y###mach_wa_y",
                                       "Z###mach_wa_z"};
  for (int a = 0; a < 3; a++) {
    float size = static_cast<float>(m.travelMax[a] - m.travelMin[a]);
    ImGui::SetNextItemWidth(ControlWidth() * 0.5f);
    if (ExprDragFloat(state, kAxis[a], &size, 1.0f, 0.0f, 100000.0f, "%.0f")) {
      m.travelMax[a] = m.travelMin[a] + size;
      edited = true;
    }
  }
  ImGui::SetNextItemWidth(ControlWidth());
  if (ImGui::BeginCombo("Post###mach_post", m.post.c_str())) {
    for (const std::string& id : cam::PostIds())
      if (ImGui::Selectable(id.c_str(), id == m.post)) {
        m.post = id;
        edited = true;
      }
    ImGui::EndCombo();
  }
  if (ImGui::Checkbox("Arcs (G2/G3)###mach_arcs", &m.fitArcs))
    edited = true;
  if (ImGui::Checkbox("Has a 4th axis (A)###mach_rotary", &m.hasRotaryAxis))
    edited = true;
  float rapid = static_cast<float>(m.rapidFeed);
  ImGui::SetNextItemWidth(ControlWidth());
  if (ExprDragFloat(state, "Rapid rate (mm/min, 0 = not stated)###mach_rapid",
                    &rapid, 10.0f, 0.0f, 100000.0f, "%.0f")) {
    m.rapidFeed = rapid;
    edited = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How fast the machine traverses. It times rapids in the "
                      "estimate, caps the axes on a turning cut, and is the "
                      "ceiling the feed check compares against. Rapids are "
                      "always written as G0.");
  if (m.hasRotaryAxis) {
    float amax = static_cast<float>(m.rotaryMaxFeed);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ExprDragFloat(state,
                      "A top speed (deg/min, 0 = not stated)###mach_amax",
                      &amax, 10.0f, 0.0f, 1000000.0f, "%.0f")) {
      m.rotaryMaxFeed = amax;
      edited = true;
    }
    ImGui::SetItemTooltip(
        "How fast the 4th axis turns at most -- the A motor's velocity in the "
        "controller's motor tuning. A cut that turns the part is held to it, "
        "and the cycle time counts the hold.");

    // How the rotary is INSTALLED
    // (docs/cam/rotary.md#how-the-rotary-is-installed). Not stated, a setup
    // held in the rotary does not post: the answer to which way the part lies
    // is the machine's, not a guess.
    cam::RotaryMount& r = m.rotary;
    int along = r.along == 'x' ? 1 : r.along == 'y' ? 2 : 0;
    const char* alongNames[] = {"not stated", "machine X", "machine Y"};
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::Combo("Rotary lies along###mach_rot_along", &along, alongNames,
                     3)) {
      if (along == 0) {
        r = cam::RotaryMount{};
      } else {
        r.along = along == 1 ? 'x' : 'y';
        // A new statement starts from the common one, shown, for the user to
        // change: the chuck at the minus end, driven as A, right-handed.
        r.topAxis = r.along == 'x' ? 'y' : 'x';
        if (r.topSign == 0)
          r.topSign = r.along == 'x' ? -1 : 1;
      }
      edited = true;
    }
    ImGui::SetItemTooltip("Which machine axis the rotary's axis is parallel "
                          "to. Until this is stated, a setup that holds its "
                          "part in the rotary is not posted.");
    if (r.Stated()) {
      int end = r.chuckEnd < 0 ? 0 : 1;
      const char* ends[] = {"the minus end", "the plus end"};
      ImGui::SetNextItemWidth(ControlWidth());
      if (ImGui::Combo("Chuck at###mach_rot_end", &end, ends, 2)) {
        r.chuckEnd = end == 0 ? -1 : 1;
        edited = true;
      }
      int word = r.word == 'B' ? 1 : 0;
      const char* words[] = {"A", "B"};
      ImGui::SetNextItemWidth(ControlWidth());
      if (ImGui::Combo("Driven as###mach_rot_word", &word, words, 2)) {
        r.word = word == 0 ? 'A' : 'B';
        edited = true;
      }
      ImGui::SetItemTooltip("The letter the controller drives the rotary "
                            "with (Mach3: the axis its motor is set up on).");
      int top = r.topSign > 0 ? 0 : 1;
      const std::string plus = std::string("+") + char(std::toupper(r.topAxis));
      const std::string minus =
          std::string("-") + char(std::toupper(r.topAxis));
      const char* tops[] = {plus.c_str(), minus.c_str()};
      ImGui::SetNextItemWidth(ControlWidth());
      if (ImGui::Combo("Positive jog moves the top toward###mach_rot_top", &top,
                       tops, 2)) {
        r.topSign = top == 0 ? 1 : -1;
        edited = true;
      }
      ImGui::SetItemTooltip(
          "Jog the rotary positive with a pen mark on top of the chuck, and "
          "watch which way the mark goes. That is the sign of every angle in "
          "the file.");
    }

    // What holds the part: the chuck and the tailstock, which every rotary
    // toolpath is kept out of. 0 = not measured, and a default BIGGER than
    // most hardware stands in, which the verdict names.
    cam::RotaryHolding& h = m.holding;
    const auto size = [&](const char* label, double& v, const char* tip) {
      float f = static_cast<float>(v);
      ImGui::SetNextItemWidth(ControlWidth());
      if (ExprDragFloat(state, label, &f, 0.5f, 0.0f, 500.0f, "%.1f")) {
        v = f;
        edited = true;
      }
      ImGui::SetItemTooltip("%s", tip);
    };
    size("Chuck diameter (mm, 0 = 90)###mach_chuck_d", h.chuckDiameter,
         "The chuck body at its widest. Nothing may come nearer the axis "
         "than this over the jaws.");
    size("Stock in the jaws (mm, 0 = 15)###mach_grip", h.gripLength,
         "How much of the stock the jaws hold, from its end. A setup can "
         "say its own.");
    if (ImGui::Checkbox("Has a tailstock###mach_tail", &h.hasTailstock))
      edited = true;
    if (h.hasTailstock) {
      size("Centre length (mm, 0 = 25)###mach_centre_l", h.centreLength,
           "From the centre's point to where it meets the tailstock.");
      size("Centre diameter (mm, 0 = 14)###mach_centre_d", h.centreDiameter,
           "The centre at its base.");
      size("Tailstock body (mm, 0 = 60)###mach_tail_d", h.tailBodyDiameter,
           "The tailstock behind the centre, at its widest.");
    }
    size("Clearance (mm, 0 = 3)###mach_hold_clear", h.clearance,
         "Kept from the chuck and the tailstock, on top of the tool's "
         "radius.");
  }
  // The tool library (M4): what the shop owns for this machine. A new setup
  // on it copies the table; "Add tool" in the CAM panel offers these first.
  ImGui::TextDisabled("Tools (a setup copies them)");
  int removeTool = 0;
  for (cam::Tool& t : m.library) {
    ImGui::PushID(t.number);
    ImGui::Text("T%d", t.number);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    float dia = static_cast<float>(t.diameter);
    if (ExprDragFloat(state, "mm###libdia", &dia, 0.1f, 0.1f, 50.0f, "%.3f")) {
      t.diameter = dia;
      edited = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    float feed = static_cast<float>(t.feedCut);
    if (ExprDragFloat(state, "F###libfeed", &feed, 10.0f, 1.0f, 10000.0f,
                      "%.0f")) {
      t.feedCut = feed;
      edited = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    int shape = static_cast<int>(t.type);
    if (ImGui::Combo("###libtype", &shape, "flat\0ball\0V-bit\0drill\0")) {
      t.type = static_cast<cam::ToolType>(shape);
      edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x###librm"))
      removeTool = t.number;
    ImGui::PopID();
  }
  if (removeTool != 0) {
    std::erase_if(m.library,
                  [&](const cam::Tool& t) { return t.number == removeTool; });
    edited = true;
  }
  if (ImGui::SmallButton("Add tool###mach_add_tool")) {
    cam::Tool t;
    for (const cam::Tool& have : m.library)
      t.number = std::max(t.number, have.number + 1);
    t.name = "tool " + std::to_string(t.number);
    m.library.push_back(t);
    edited = true;
  }
  if (edited)
    commit(m);
}

// Preferences > Keyboard. Not a `Field`: the table above is one scalar per
// row rendered as a slider or a combo, and a chord is neither
// (docs/ui/keybindings.md). One collapsing node per context, because a
// context binding legitimately reuses a key another context owns — `N` is
// Coincident in a sketch and align-to-face everywhere else — so grouping is
// what makes the list readable instead of looking full of duplicates.
bool DrawKeyboardSection(AppState& state) {
  keymap::Keymap& keys = state.settings.keys;
  bool changed = false;

  static char filter[64] = "";
  ImGui::SetNextItemWidth(
      std::min(ControlWidth(), ImGui::GetContentRegionAvail().x));
  ImGui::InputTextWithHint("###key_filter", "Filter by name or key", filter,
                           sizeof(filter));
  ImGui::SameLine();
  if (ImGui::Button("Reset all keys###key_reset_all")) {
    keys.ResetAll();
    changed = true;
  }

  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(
      "Click a key to rebind it; Backspace clears it, Escape cancels. A "
      "mode's binding wins over the same key listed under Everywhere. "
      "Escape, Enter, Delete and the arrow keys are fixed — they answer to "
      "whatever tool, preview or field is open, so they are not commands.");
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Case-insensitive substring over the label AND the chord, so "shift+" and
  // "fillet" both find the fillet row.
  const auto matches = [](const std::string& haystack, const char* needle) {
    if (*needle == '\0')
      return true;
    const auto lower = [](std::string v) {
      std::transform(v.begin(), v.end(), v.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      return v;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
  };

  for (keymap::Context ctx : keymap::AllContexts()) {
    // Which rows survive the filter — counted first, so a context with no
    // match disappears instead of showing an empty node.
    std::vector<const keymap::Binding*> rows;
    for (const keymap::Binding& b : keymap::Defaults()) {
      if (b.ctx != ctx || b.hidden)
        continue;
      if (matches(std::string(b.section) + " " + b.label + " " +
                      keys.Text(b.id),
                  filter))
        rows.push_back(&b);
    }
    if (rows.empty())
      continue;

    const std::string nodeId = std::string(keymap::ContextLabel(ctx)) +
                               "###keyctx_" + keymap::ContextName(ctx);
    if (!ImGui::TreeNodeEx(nodeId.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
      continue;

    if (ImGui::SmallButton(
            (std::string("Reset these###keyreset_") + keymap::ContextName(ctx))
                .c_str())) {
      keys.ResetContext(ctx);
      changed = true;
    }

    if (ImGui::BeginTable(
            (std::string("keytbl_") + keymap::ContextName(ctx)).c_str(), 3,
            ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
      ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 8.0f);
      ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                              ImGui::GetFontSize() * 3.5f);
      for (const keymap::Binding* b : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(b->label);

        ImGui::TableNextColumn();
        const bool capturing = state.keyCaptureId == b->id;
        const std::string chord = keys.Text(b->id);
        const std::string buttonLabel =
            (capturing
                 ? std::string("Press a key\xe2\x80\xa6")
                 : (chord.empty() ? std::string("\xe2\x80\x94") : chord)) +
            "###keybtn_" + b->id;
        if (capturing)
          ImGui::PushStyleColor(
              ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(buttonLabel.c_str(), ImVec2(-FLT_MIN, 0.0f)))
          state.keyCaptureId = capturing ? "" : b->id;
        if (capturing)
          ImGui::PopStyleColor();

        if (capturing) {
          // Escape gets out without binding — it is the one key a person
          // reaches for when they change their mind, so it can never BE the
          // binding. Backspace clears instead, which is the other thing they
          // came here to do.
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            state.keyCaptureId.clear();
          } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            keys.Set(b->id, "");
            state.keyCaptureId.clear();
            changed = true;
          } else if (const keymap::Chord pressed = CaptureChord()) {
            keys.Set(b->id, keymap::FormatChord(pressed));
            state.keyCaptureId.clear();
            changed = true;
          }
        }

        ImGui::TableNextColumn();
        ImGui::BeginDisabled(keys.IsDefault(b->id));
        if (ImGui::SmallButton(
                (std::string("Reset###keyrst_") + b->id).c_str())) {
          keys.Reset(b->id);
          changed = true;
        }
        ImGui::EndDisabled();

        // The collision, named, on the row that causes it. A conflict is
        // ALLOWED — refusing one would strand anyone mid-swap, who has to
        // put a key on B before taking it off A — but it is never silent.
        const std::vector<std::string> clash =
            keys.Bound(ctx, keys.Get(b->id), b->id);
        if (!clash.empty()) {
          std::string others;
          for (const std::string& id : clash) {
            const keymap::Binding* other = keymap::Find(id);
            others += (others.empty() ? "" : ", ");
            others += other ? other->label : id;
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                             "   also bound to %s", others.c_str());
        }
      }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  }
  return changed;
}

} // namespace

void ApplySettings(AppState& state) {
  const settings::Settings& s = state.settings;
  // Who a commit is by (F9 C1). GitVault holds it rather than reading the
  // settings file, which would be a file read per commit.
  GitVault::SetPreferredIdentity(s.gitUserName, s.gitUserEmail);
  state.uiFontScale = s.uiFontScale;
  state.showShadows = s.shadows;
  state.showAmbientOcclusion = s.ssao != "off";
  state.ssaoFullRes = s.ssao == "full";
  state.shadowMapSize = s.shadowMap;
}

void MarkSettingsChanged(AppState& state) {
  // The font atlas is rebuilt BETWEEN frames, not during one, so a scale
  // change has to ask for it — ApplySettings only moves the number. Without
  // this, changing Font size in the window did nothing at all: only the old
  // View > Font Size menu ever set the flag.
  const bool fontChanged = state.uiFontScale != state.settings.uiFontScale;
  ApplySettings(state);
  if (fontChanged)
    state.fontRebuildRequested = true;
  state.settingsDirtyAt = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
}

void FlushSettings(AppState& state, double now) {
  if (state.settingsDirtyAt < 0.0 ||
      now - state.settingsDirtyAt < kSaveDebounceSeconds)
    return;
  state.settingsDirtyAt = -1.0;
  std::string error;
  // Against settingsSaved, not against defaults: only the keys THIS instance
  // changed are written, so a second instance's preferences survive.
  if (!settings::Save(state.settings, state.settingsSaved, &error)) {
    LOG_WARN("Could not save settings: {}", error);
    return;
  }
  state.settingsSaved = state.settings;
}

void DrawSettingsWindow(AppState& state) {
  if (!state.settingsWindowOpen)
    return;
  // Sized in FONT UNITS, not pixels — this window owns the font-size setting,
  // so a literal is wrong the moment someone uses it — and re-applied on
  // every APPEARING rather than FirstUseEver: the first version shipped too
  // small, and FirstUseEver would leave everyone who had already opened it
  // stuck with that size forever in imgui.ini. Centred for the same reason a
  // remembered position is wrong after a resize: it can be off-screen.
  //
  // NOT AlwaysAutoResize: the blurbs wrap to the content region, so letting
  // content set the width makes each blurb one long line, which then sets the
  // width — the window grew past the screen edge.
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowSize(ImVec2(std::min(em * 30.0f, vp->WorkSize.x * 0.9f),
                                  std::min(em * 38.0f, vp->WorkSize.y * 0.85f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::Begin("Preferences###SettingsWindow",
                    &state.settingsWindowOpen)) {
    // Collapsed or closed: an armed capture would keep eating every key
    // press with no visible row to explain why.
    state.keyCaptureId.clear();
    ImGui::End();
    return;
  }

  bool changed = false;

  // Sections in order of first appearance, each drawn EXACTLY ONCE. Not "open
  // a new header whenever the section changes from the previous row": the two
  // Sharing rows once landed between the Performance rows, and that loop drew
  // Performance twice — two collapsing headers sharing one
  // "###sec_Performance" id, which is an ImGui id collision and leaves the
  // pair fighting over the open/closed state. The table is grouped again, and
  // this no longer depends on it being so.
  std::vector<std::string> sections;
  for (const settings::Field& f : settings::Fields())
    if (std::find(sections.begin(), sections.end(), f.section) ==
        sections.end())
      sections.push_back(f.section);

  for (const std::string& section : sections) {
    const std::string headerId = section + "###sec_" + section;
    if (!SectionHeader(headerId.c_str(), /*count=*/-1,
                       /*defaultOpen=*/true))
      continue;
    for (const settings::Field& f : settings::Fields()) {
      if (section != f.section)
        continue;
      changed |= DrawField(state, f);
      // The GPU row says what is actually in use, and says so LOUDLY when the
      // choice no longer matches it — "nothing happened" is the correct
      // behaviour for a restart-tier setting, but only if the window admits
      // it.
      if (std::string(f.key) == "gpu_device" && !state.gpuName.empty()) {
        const std::string& want = state.settings.gpuDevice;
        const bool pending =
            !want.empty() && state.gpuName.find(want) == std::string::npos;
        const std::string inUse = "Rendering on: " + state.gpuName + ".";
        DrawBlurb(f, inUse.c_str());
        if (!state.gpuPreferenceNote.empty() &&
            state.gpuPreferenceNote.find(want) != std::string::npos) {
          // Startup already tried and could not: promising a restart here
          // would be a lie, and the user would take it.
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                             state.gpuPreferenceNote.c_str());
          ImGui::PopTextWrapPos();
        } else if (pending) {
          ImGui::PushTextWrapPos(0.0f);
          ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                             "Restart Designer to render on %s.", want.c_str());
          ImGui::PopTextWrapPos();
        }
      } else {
        DrawBlurb(f);
      }
      ImGui::Spacing();
    }
  }

  if (SectionHeader("Keyboard###sec_Keyboard"))
    changed |= DrawKeyboardSection(state);
  if (SectionHeader("Machines###sec_Machines"))
    DrawMachinesSection(state);

  ImGui::Separator();
  // A user who has broken their config should not have to find the JSON.
  if (ImGui::Button("Reset all to defaults###set_reset_all")) {
    const auto unknown = state.settings.unknown; // not ours to discard
    state.settings = settings::Settings{};
    state.settings.unknown = unknown;
    state.keyCaptureId.clear(); // a capture armed against a row that is gone
    changed = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", settings::ConfigPath().c_str());

  if (changed)
    MarkSettingsChanged(state);
  if (!state.settingsWindowOpen)
    state.keyCaptureId.clear(); // closed by the title-bar X
  ImGui::End();
}

} // namespace ui
