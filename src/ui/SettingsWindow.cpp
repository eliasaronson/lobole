// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/SettingsWindow.h"
#include "ui/Widgets.h"

#include "ui/ExprInput.h" // expression-capable numeric fields

#include "core/io/GitVault.h"

#include "common/Logger.h"
#include "core/Units.h"
#include "core/cam/CamMachine.h"
#include "core/cam/CamPost.h"
#include "core/io/Keymap.h"
#include "core/io/Serialization.h" // the machine store
#include "core/io/Settings.h"
#include "ui/Commands.h"
#include "ui/MachineActions.h"
#include "ui/PrintProfileActions.h"

#include "IconsFontAwesome6.h"

#include "imgui.h"
#include "imgui_internal.h" // PushOverrideID: flat ids across the panes

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <functional>
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

// The window is split into scrolling panes (child windows), and a child
// window starts its own id scope -- which would move every control from
// "//###SettingsWindow/###set_ssao" to a path through the pane's mangled
// name, for every test and script that addresses one. So a pane hashes its
// items from the WINDOW's seed: every control keeps one flat, stable path
// whichever pane, page or search result draws it (docs/ui/settings.md).
void BeginPane(const char* name, ImVec2 size, ImGuiChildFlags flags = 0) {
  const ImGuiID seed = ImGui::GetCurrentWindow()->IDStack.back();
  ImGui::BeginChild(name, size, flags);
  ImGui::PushOverrideID(seed);
}

void EndPane() {
  ImGui::PopID();
  ImGui::EndChild();
}

// The muted paragraph at the top of a hand-written page.
void PageBlurb(const char* text) {
  ImGui::PushStyleColor(ImGuiCol_Text,
                        ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextUnformatted(text);
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
}

// A form row's label on its own line, then the next control's width: the
// label above rather than after the control, so a long one ("Positive jog
// moves the top toward") wraps in a narrow window instead of running off it.
void FormLabel(const char* label) {
  ImGui::TextUnformatted(label);
  ImGui::SetNextItemWidth(
      std::min(ControlWidth(), ImGui::GetContentRegionAvail().x));
}

// A library page (docs/ui/settings.md#library-pages): the user's own things
// in a list on the left, the selected one's form on the right. Machines is
// one; a new library is one of these plus a kHandPages entry. Every list
// edit is the page's own (each item is its own file, written at once), so
// this only lays out and reports.
struct LibraryPage {
  // Prefixes every control: ###<id>_new, ###<id>_dup, ###<id>_delete, and
  // ###<id>_item_<name> per row.
  std::string id;
  const char* blurb = "";
  const char* noun = "item";  // "machine": the tooltips and the empty text
  const char* icon = nullptr; // the rows' glyph
  // Re-asked AFTER the buttons: Delete has just changed the list, and an
  // answer from before it indexed a list that no longer held the item.
  std::function<std::vector<std::string>()> names;
  int* selected = nullptr; // -1 = none
  std::function<void()> create;
  std::function<void(int)> duplicate;
  std::function<void(int)> remove;
  std::function<void(int)> form;      // the selected item's editor
  const std::string* error = nullptr; // the last refusal, shown over the form
};

void DrawLibraryPage(const LibraryPage& page) {
  PageBlurb(page.blurb);
  ImGui::Spacing();

  const ImGuiStyle& style = ImGui::GetStyle();
  const float em = ImGui::GetFontSize();
  std::vector<std::string> names = page.names();
  // The list is as wide as its longest name, within reason: a name longer
  // than that is elided by the row, and the form keeps the rest.
  float listW = em * 9.0f;
  for (const std::string& n : names)
    listW = std::max(listW, ImGui::CalcTextSize(n.c_str()).x + em * 2.5f +
                                style.WindowPadding.x * 2.0f);
  listW = std::min(listW, em * 14.0f);

  BeginPane((page.id + "_list").c_str(), ImVec2(listW, 0.0f),
            ImGuiChildFlags_Borders);
  const auto have = [&] {
    return *page.selected >= 0 &&
           *page.selected < static_cast<int>(names.size());
  };
  const std::string noun = page.noun;
  if (ImGui::Button((ICON_FA_PLUS "###" + page.id + "_new").c_str()))
    page.create();
  ImGui::SetItemTooltip("New %s", noun.c_str());
  ImGui::SameLine();
  ImGui::BeginDisabled(!have());
  if (ImGui::Button((ICON_FA_CLONE "###" + page.id + "_dup").c_str()) && have())
    page.duplicate(*page.selected);
  ImGui::SetItemTooltip("Duplicate this %s", noun.c_str());
  ImGui::SameLine();
  if (ImGui::Button((ICON_FA_TRASH_CAN "###" + page.id + "_delete").c_str()) &&
      have())
    page.remove(*page.selected);
  ImGui::SetItemTooltip("Delete this %s", noun.c_str());
  ImGui::EndDisabled();
  names = page.names();

  ImGui::Separator();
  if (names.empty()) {
    // Wrapped: "No print profiles yet." is wider than the narrowest list.
    ImGui::PushStyleColor(ImGuiCol_Text,
                          ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("No %ss yet.", noun.c_str());
    ImGui::PopStyleColor();
  }
  for (size_t i = 0; i < names.size(); i++) {
    ListRowSpec row;
    row.glyph = page.icon;
    row.label = names[i] + "###" + page.id + "_item_" + names[i];
    row.selected = static_cast<int>(i) == *page.selected;
    if (ListRow(row).clicked)
      *page.selected = static_cast<int>(i);
  }
  EndPane();

  ImGui::SameLine();
  BeginPane((page.id + "_form").c_str(), ImVec2(0.0f, 0.0f));
  if (page.error != nullptr && !page.error->empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.40f, 1.0f));
    ImGui::TextWrapped("%s", page.error->c_str());
    ImGui::PopStyleColor();
  }
  if (have())
    page.form(*page.selected);
  else if (!names.empty())
    ImGui::TextDisabled("Pick a %s on the left.", noun.c_str());
  else
    ImGui::TextDisabled("Make one with %s.", ICON_FA_PLUS);
  EndPane();
}

// Preferences > Machines (docs/cam/setup.md#machine-profiles). Not a
// `Field` either: a LIST of structs, each its own file in
// ~/.config/lobole/machines, written on every edit the way settings.json
// is -- there is no Save button to forget. A job names one of these and
// embeds a copy; the CAM panel says when the two differ.
std::string g_machineError; // the last refusal from the store, in words

void CommitMachine(AppState& state, const cam::Machine& m) {
  std::string error;
  if (!SaveMachine(state, m, &error))
    g_machineError = error;
  else
    g_machineError.clear();
}

std::vector<std::string> MachineNames(const AppState& state) {
  std::vector<std::string> names;
  for (const cam::Machine& m : state.machines)
    names.push_back(m.name);
  return names;
}

void SelectMachine(AppState& state, const std::string& name) {
  for (size_t i = 0; i < state.machines.size(); i++)
    if (state.machines[i].name == name)
      state.machineEditIndex = static_cast<int>(i);
}

// The selected machine's editor. Every edit is written to the file at once;
// a rename is a delete of the old file and a save of the new, refused (and
// reverted) when the new name would share a file with another machine.
void DrawMachineForm(AppState& state, int index) {
  cam::Machine& m = state.machines[static_cast<size_t>(index)];
  static char nameBuf[96];
  static int nameFor = -1;
  if (nameFor != index || !ImGui::IsAnyItemActive()) {
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", m.name.c_str());
    nameFor = index;
  }
  FormLabel("Name");
  ImGui::InputText("###mach_name", nameBuf, sizeof(nameBuf));
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
      SelectMachine(state, renamed.name);
      return; // `m` is gone
    }
  }
  ImGui::Spacing();

  bool edited = false;
  // The work area as a SIZE per axis, each field named. It was two rows of
  // three unlabelled boxes (travel min / travel max), which read as six
  // numbers with no axis on them; a hobby machine is known by its bed --
  // "600 by 400, 80 of Z" -- and that is what is asked. Min stays where the
  // file put it (zero unless a file said otherwise), max = min + size.
  ImGui::TextUnformatted("Work area (mm)");
  ImGui::SameLine();
  ImGui::TextDisabled("zero checks nothing");
  static const char* const kAxis[3] = {"X###mach_wa_x", "Y###mach_wa_y",
                                       "Z###mach_wa_z"};
  for (int a = 0; a < 3; a++) {
    double size = m.travelMax[a] - m.travelMin[a];
    ImGui::SetNextItemWidth(
        std::min(ControlWidth() * 0.5f, ImGui::GetContentRegionAvail().x));
    if (ExprDragDouble(state, kAxis[a], &size, 1.0f, 0.0, 100000.0, "%.0f")) {
      m.travelMax[a] = m.travelMin[a] + size;
      edited = true;
    }
  }
  FormLabel("Post");
  if (ImGui::BeginCombo("###mach_post", m.post.c_str())) {
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
  double rapid = m.rapidFeed;
  FormLabel("Rapid rate (mm/min, 0 = not stated)");
  if (ExprDragDouble(state, "###mach_rapid", &rapid, 10.0f, 0.0, 100000.0,
                     "%.0f")) {
    m.rapidFeed = rapid;
    edited = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How fast the machine traverses. It times rapids in the "
                      "estimate, caps the axes on a turning cut, and is the "
                      "ceiling the feed check compares against. Rapids are "
                      "always written as G0.");
  if (m.hasRotaryAxis) {
    double amax = m.rotaryMaxFeed;
    FormLabel("A top speed (deg/min, 0 = not stated)");
    if (ExprDragDouble(state, "###mach_amax", &amax, 10.0f, 0.0, 1000000.0,
                       "%.0f")) {
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
    FormLabel("Rotary lies along");
    if (ImGui::Combo("###mach_rot_along", &along, alongNames, 3)) {
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
      FormLabel("Chuck at");
      if (ImGui::Combo("###mach_rot_end", &end, ends, 2)) {
        r.chuckEnd = end == 0 ? -1 : 1;
        edited = true;
      }
      int word = r.word == 'B' ? 1 : 0;
      const char* words[] = {"A", "B"};
      FormLabel("Driven as");
      if (ImGui::Combo("###mach_rot_word", &word, words, 2)) {
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
      FormLabel("Positive jog moves the top toward");
      if (ImGui::Combo("###mach_rot_top", &top, tops, 2)) {
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
    const auto size = [&](const char* label, const char* id, double& v,
                          const char* tip) {
      double f = v;
      FormLabel(label);
      if (ExprDragDouble(state, id, &f, 0.5f, 0.0, 500.0, "%.1f")) {
        v = f;
        edited = true;
      }
      ImGui::SetItemTooltip("%s", tip);
    };
    size("Chuck diameter (mm, 0 = 90)", "###mach_chuck_d", h.chuckDiameter,
         "The chuck body at its widest. Nothing may come nearer the axis "
         "than this over the jaws.");
    size("Stock in the jaws (mm, 0 = 15)", "###mach_grip", h.gripLength,
         "How much of the stock the jaws hold, from its end. A setup can "
         "say its own.");
    if (ImGui::Checkbox("Has a tailstock###mach_tail", &h.hasTailstock))
      edited = true;
    if (h.hasTailstock) {
      size("Centre length (mm, 0 = 25)", "###mach_centre_l", h.centreLength,
           "From the centre's point to where it meets the tailstock.");
      size("Centre diameter (mm, 0 = 14)", "###mach_centre_d", h.centreDiameter,
           "The centre at its base.");
      size("Tailstock body (mm, 0 = 60)", "###mach_tail_d", h.tailBodyDiameter,
           "The tailstock behind the centre, at its widest.");
    }
    size("Clearance (mm, 0 = 3)", "###mach_hold_clear", h.clearance,
         "Kept from the chuck and the tailstock, on top of the tool's "
         "radius.");
  }
  // The tool library (M4): what the shop owns for this machine. A new setup
  // on it copies the table; "Add tool" in the CAM panel offers these first.
  ImGui::Spacing();
  ImGui::TextUnformatted("Tools");
  ImGui::SameLine();
  ImGui::TextDisabled("a setup copies them");
  const float em = ImGui::GetFontSize();
  int removeTool = 0;
  for (cam::Tool& t : m.library) {
    ImGui::PushID(t.number);
    ImGui::Text("T%d", t.number);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.5f);
    double dia = t.diameter;
    if (ExprDragDouble(state, "mm###libdia", &dia, 0.1f, 0.1, 50.0, "%.3f")) {
      t.diameter = dia;
      edited = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.5f);
    double feed = t.feedCut;
    if (ExprDragDouble(state, "F###libfeed", &feed, 10.0f, 1.0, 10000.0,
                       "%.0f")) {
      t.feedCut = feed;
      edited = true;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.5f);
    int shape = static_cast<int>(t.type);
    if (ImGui::Combo("###libtype", &shape, "flat\0ball\0V-bit\0drill\0")) {
      t.type = static_cast<cam::ToolType>(shape);
      edited = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_XMARK "###librm"))
      removeTool = t.number;
    ImGui::SetItemTooltip("Remove T%d", t.number);
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
    CommitMachine(state, m);
}

bool DrawMachinesPage(AppState& state) {
  LibraryPage page;
  page.id = "mach";
  page.noun = "machine";
  page.icon = ICON_FA_INDUSTRY;
  page.blurb =
      "The machines in your shop: post dialect, arc fitting, rotary axis, "
      "work area and rapid. A CAM setup picks one and keeps a copy, so a job "
      "file opens on a machine that has never seen the profile.";
  page.names = [&state] { return MachineNames(state); };
  page.selected = &state.machineEditIndex;
  page.error = &g_machineError;
  page.create = [&state] {
    // A name nobody has, so the file can be written at once: the form
    // renames it, and a rename is delete-then-save.
    cam::Machine fresh;
    for (int n = 1;; n++) {
      fresh.name = "Machine " + std::to_string(n);
      if (cam::FindMachine(state.machines, fresh.name) == nullptr)
        break;
    }
    CommitMachine(state, fresh);
    SelectMachine(state, fresh.name);
  };
  page.duplicate = [&state](int index) {
    cam::Machine copy = state.machines[static_cast<size_t>(index)];
    const std::string base = copy.name;
    for (int n = 2;; n++) {
      copy.name = base + " " + std::to_string(n);
      if (cam::FindMachine(state.machines, copy.name) == nullptr)
        break;
    }
    CommitMachine(state, copy);
    SelectMachine(state, copy.name);
  };
  page.remove = [&state](int index) {
    std::string error;
    if (!DeleteMachine(state, state.machines[static_cast<size_t>(index)].name,
                       &error))
      g_machineError = error;
    else
      g_machineError.clear();
    state.machineEditIndex = -1;
  };
  page.form = [&state](int index) { DrawMachineForm(state, index); };
  DrawLibraryPage(page);
  return false; // the machine store is not settings.json
}

// Preferences > Print profiles (docs/tolerances/README.md#print-profiles).
// The machine page's shape: the user's profiles, each its own file in
// ~/.config/lobole/print_profiles, written on every edit. A printed part
// names one and embeds a copy; the Print process section says when the two
// differ.
std::string g_profileError; // the last refusal from the store, in words

std::vector<std::string> PrintProfileNames(const AppState& state) {
  std::vector<std::string> names;
  for (const printfit::Profile& p : state.printProfiles)
    names.push_back(p.name);
  return names;
}

void SelectPrintProfile(AppState& state, const std::string& name) {
  for (size_t i = 0; i < state.printProfiles.size(); i++)
    if (state.printProfiles[i].name == name)
      state.printProfileEditIndex = static_cast<int>(i);
}

void CommitPrintProfile(AppState& state, const printfit::Profile& p) {
  std::string error;
  const std::string name = p.name;
  if (!SavePrintProfile(state, p, &error)) {
    g_profileError = error;
    // The store still holds the last good one: show it, not the refused
    // edit, so the form never reads as saved when it was not.
    ReloadPrintProfiles(state);
  } else {
    g_profileError.clear();
  }
  SelectPrintProfile(state, name);
}

// "2026-10-10": the date a coupon row was typed in.
std::string Today() {
  const std::time_t now = std::time(nullptr);
  char buf[16];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d", std::localtime(&now));
  return buf;
}

// The selected profile's editor. Every edit is written at once; a rename is
// refused when the name is taken, as a machine's is.
void DrawPrintProfileForm(AppState& state, int index) {
  printfit::Profile p = state.printProfiles[static_cast<size_t>(index)];
  static char nameBuf[96];
  static int nameFor = -1;
  if (nameFor != index || !ImGui::IsAnyItemActive()) {
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", p.name.c_str());
    nameFor = index;
  }
  FormLabel("Name");
  ImGui::InputText("###prof_name", nameBuf, sizeof(nameBuf));
  if (ImGui::IsItemDeactivatedAfterEdit() && p.name != nameBuf &&
      nameBuf[0] != '\0') {
    std::string error;
    const std::string to = nameBuf;
    if (!RenamePrintProfile(state, p.name, to, &error)) {
      g_profileError = error;
    } else {
      g_profileError.clear();
      SelectPrintProfile(state, to);
      return; // `p` is stale
    }
  }

  bool edited = false;
  // The base pair: what the rows are allowances FOR. Only the pairs the
  // built-in tables carry, so the form cannot make what a save refuses.
  FormLabel("Process and material");
  const std::string pair = p.table.process + " " + p.table.material;
  if (ImGui::BeginCombo("###prof_pair", pair.c_str())) {
    for (const printfit::Table& t : printfit::Tables()) {
      const std::string label = t.process + " " + t.material + "###prof_pair_" +
                                t.process + "_" + t.material;
      if (ImGui::Selectable(label.c_str(),
                            t.process == p.table.process &&
                                t.material == p.table.material)) {
        p.table.process = t.process;
        p.table.material = t.material;
        edited = true;
      }
    }
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("The process and the material the rows are for. "
                        "Changing it keeps the rows.");

  static char condBuf[512];
  static int condFor = -1;
  if (condFor != index || !ImGui::IsAnyItemActive()) {
    std::snprintf(condBuf, sizeof(condBuf), "%s", p.table.conditions.c_str());
    condFor = index;
  }
  FormLabel("Conditions");
  ImGui::SetNextItemWidth(-FLT_MIN);
  ImGui::InputText("###prof_conditions", condBuf, sizeof(condBuf));
  if (ImGui::IsItemDeactivatedAfterEdit() && p.table.conditions != condBuf) {
    p.table.conditions = condBuf;
    edited = true;
  }
  ImGui::SetItemTooltip("What the numbers assume: the printer, the nozzle, "
                        "the slicer preset and its compensation.%s%s",
                        p.table.conditions.empty() ? "" : "\n\n",
                        p.table.conditions.c_str());

  double defl = p.stlDeflectionMm;
  FormLabel("STL chord deviation (mm, 0 = a tenth of the smallest gap)");
  if (ExprInputDouble(state, "###prof_deflection", &defl, "%.4g") &&
      defl != p.stlDeflectionMm) {
    p.stlDeflectionMm = defl;
    edited = true;
  }
  ImGui::SetItemTooltip("What a printed part's STL is exported at. Export "
                        "the coupon at the same, and it calibrates the "
                        "chords and the printer at once.");

  // The vertical counterpart of the gap rows
  // (docs/tolerances/README.md#print-profiles): whole layers, so it needs
  // the layer height it is counted in.
  double layer = p.layerHeightMm;
  FormLabel("Layer height (mm, 0 = not stated)");
  if (ExprInputDouble(state, "###prof_layer", &layer, "%.4g") &&
      layer != p.layerHeightMm) {
    p.layerHeightMm = layer;
    edited = true;
  }
  ImGui::SetItemTooltip("The slicer preset's layer height: what the air "
                        "over another part is counted in.");
  double zgap = p.zGapMm;
  FormLabel("Air over another part (mm, whole layers; 0 = not measured)");
  if (ExprInputDouble(state, "###prof_zgap", &zgap, "%.4g") &&
      zgap != p.zGapMm) {
    p.zGapMm = zgap;
    edited = true;
  }
  ImGui::SetItemTooltip("The vertical air a face needs over another part "
                        "printed under it to come away from it: the print "
                        "coupon's air row says how many layers. Every part "
                        "still starts on the bed; Check print's stacking "
                        "line judges by this, else by three layers.");

  // The printer's build volume (v179): what Check print's bed line fits a
  // plate into. Three zeros = not stated, and that line is skipped.
  {
    std::array<double, 3> bed = p.bedMm.value_or(std::array<double, 3>{});
    FormLabel("Bed x, y, z (mm, 0 = not stated)");
    const float w = (ImGui::GetContentRegionAvail().x -
                     2.0f * ImGui::GetStyle().ItemSpacing.x) /
                    3.0f;
    bool changed = false;
    const char* ids[] = {"###prof_bed_x", "###prof_bed_y", "###prof_bed_z"};
    for (int k = 0; k < 3; k++) {
      if (k > 0)
        ImGui::SameLine();
      ImGui::SetNextItemWidth(w);
      double v = bed[k];
      if (ExprInputDouble(state, ids[k], &v, "%.4g") && v != bed[k]) {
        bed[k] = v;
        changed = true;
      }
    }
    if (changed) {
      if (bed[0] == 0.0 && bed[1] == 0.0 && bed[2] == 0.0)
        p.bedMm.reset();
      else
        p.bedMm = bed;
      edited = true;
    }
    ImGui::SetItemTooltip("The printer's build volume: Check print's bed "
                          "line, and the generators that pack plates, fit "
                          "a plate into it. A Bambu Lab P1S is 256 x 256 x "
                          "256.");
  }

  // v181: the bed's keep-out zones, in its own coordinates from the
  // front-left corner. With any, Check print's bed line judges where each
  // piece is, as the print job puts it there.
  {
    FormLabel("Keep-out zones (x0, y0, x1, y1 mm on the bed)");
    int remove = -1;
    for (size_t z = 0; z < p.bedExcludeMm.size(); z++) {
      ImGui::PushID(static_cast<int>(z));
      double v[4] = {p.bedExcludeMm[z][0], p.bedExcludeMm[z][1],
                     p.bedExcludeMm[z][2], p.bedExcludeMm[z][3]};
      const float w =
          (ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() -
           4.0f * ImGui::GetStyle().ItemSpacing.x) /
          4.0f;
      const char* ids[] = {"###prof_zone_x0", "###prof_zone_y0",
                           "###prof_zone_x1", "###prof_zone_y1"};
      for (int k = 0; k < 4; k++) {
        if (k > 0)
          ImGui::SameLine();
        ImGui::SetNextItemWidth(w);
        if (ExprInputDouble(state, ids[k], &v[k], "%.4g") &&
            v[k] != p.bedExcludeMm[z][static_cast<size_t>(k)]) {
          p.bedExcludeMm[z][static_cast<size_t>(k)] = v[k];
          edited = true;
        }
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("x###prof_zone_remove"))
        remove = static_cast<int>(z);
      ImGui::SetItemTooltip("Remove this keep-out zone");
      ImGui::PopID();
    }
    if (remove >= 0) {
      p.bedExcludeMm.erase(p.bedExcludeMm.begin() + remove);
      edited = true;
    }
    if (ImGui::SmallButton("Add a keep-out zone###prof_zone_add")) {
      // A zone in the bed's front-left corner, a tenth of the bed each way:
      // a start to type the printer's own over.
      const double bx = p.bedMm ? (*p.bedMm)[0] : 0.0;
      const double by = p.bedMm ? (*p.bedMm)[1] : 0.0;
      p.bedExcludeMm.push_back({0.0, 0.0, bx / 10.0, by / 10.0});
      edited = true;
    }
    ImGui::SetItemTooltip("A rectangle of the bed no piece may sit on: a "
                          "P1S keeps 0, 0 to 18, 28 clear for its wiper. "
                          "Needs the bed above.");
  }
  double lineWidth = p.lineWidthMm;
  FormLabel("Wall line width (mm, 0 = not stated)");
  if (ExprInputDouble(state, "###prof_line_width", &lineWidth, "%.4g") &&
      lineWidth != p.lineWidthMm) {
    p.lineWidthMm = lineWidth;
    edited = true;
  }
  ImGui::SetItemTooltip("The slicer preset's wall line width: a printed "
                        "part's mass takes each wall loop as this thick.");

  // The rows: kind, size range, allowance, source.
  ImGui::Spacing();
  ImGui::TextUnformatted("Allowances (mm on the diameter)");
  ImGui::SameLine();
  ImGui::TextDisabled("model = size + allowance");
  int removeRow = -1;
  // Stretched by weight, never wider than the pane: a page does not scroll
  // sideways (docs/ui/settings.md#the-window). A source is elided in its
  // cell and whole in the tooltip.
  const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_BordersInnerH;
  if (ImGui::BeginTable("###prof_rows", 6, flags)) {
    ImGui::TableSetupColumn("Kind", 0, 2.0f);
    ImGui::TableSetupColumn("From", 0, 1.0f);
    ImGui::TableSetupColumn("To (0 = up)", 0, 1.0f);
    ImGui::TableSetupColumn("Allowance", 0, 1.1f);
    ImGui::TableSetupColumn("Source", 0, 2.2f);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < p.table.rows.size(); i++) {
      printfit::Row& r = p.table.rows[i];
      // Flat, numbered ids (###prof_row_allow_2): a script or a test
      // addresses one row's field without knowing the table's id scope.
      const auto id = [i](const char* what) {
        return std::string("###prof_row_") + what + "_" + std::to_string(i);
      };
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ImGui::BeginCombo(id("kind").c_str(),
                            printfit::PrintedName(r.kind).c_str())) {
        for (printfit::Printed k : printfit::AllPrinted())
          if (ImGui::Selectable(printfit::PrintedName(k).c_str(),
                                k == r.kind)) {
            r.kind = k;
            edited = true;
          }
        ImGui::EndCombo();
      }
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      double from = r.fromMm;
      if (ExprInputDouble(state, id("from").c_str(), &from, "%.4g") &&
          from != r.fromMm) {
        r.fromMm = from;
        edited = true;
      }
      ImGui::TableNextColumn();
      // "And up" is an open end: shown and typed as 0.
      double to = r.toMm >= printfit::kAboveMm ? 0.0 : r.toMm;
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (ExprInputDouble(state, id("to").c_str(), &to, "%.4g")) {
        const double next = to <= 0.0 ? printfit::kAboveMm : to;
        if (next != r.toMm) {
          r.toMm = next;
          edited = true;
        }
      }
      ImGui::SetItemTooltip("The size the range stops at; 0 = and up");
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(-FLT_MIN);
      double allow = r.allowanceMm;
      if (ExprInputDouble(state, id("allow").c_str(), &allow, "%+.4g") &&
          allow != r.allowanceMm) {
        r.allowanceMm = allow;
        // A number typed in is a measurement replacing a default: say whose.
        r.source = "coupon " + Today();
        edited = true;
      }
      if (r.kind == printfit::Printed::Gap)
        ImGui::SetItemTooltip("The whole diametral gap: %s mm a side",
                              units::FormatNumber(r.allowanceMm / 2.0).c_str());
      if (r.kind == printfit::Printed::Press)
        ImGui::SetItemTooltip(
            "A cap's bore less the pin it is pressed on, on the diameter: "
            "%s mm of interference a side as modelled",
            units::FormatNumber(-r.allowanceMm / 2.0).c_str());
      ImGui::TableNextColumn();
      ImGui::TextDisabled("%s", r.source.empty() ? "-" : r.source.c_str());
      if (!r.source.empty())
        ImGui::SetItemTooltip("%s", r.source.c_str());
      ImGui::TableNextColumn();
      if (ImGui::SmallButton((ICON_FA_XMARK + id("rm")).c_str()))
        removeRow = static_cast<int>(i);
      ImGui::SetItemTooltip("Remove this row");
    }
    ImGui::EndTable();
  }
  if (removeRow >= 0) {
    p.table.rows.erase(p.table.rows.begin() + removeRow);
    edited = true;
  }
  if (ImGui::SmallButton("Add row###prof_add_row")) {
    printfit::Row r;
    r.kind = printfit::Printed::Gap;
    r.toMm = printfit::kAboveMm;
    r.allowanceMm = 0.3;
    for (const printfit::Row& have : p.table.rows)
      if (have.kind == r.kind)
        r.allowanceMm = have.allowanceMm;
    r.source = "coupon " + Today();
    p.table.rows.push_back(r);
    edited = true;
  }
  if (edited)
    CommitPrintProfile(state, p);
}

bool DrawPrintProfilesPage(AppState& state) {
  LibraryPage page;
  page.id = "prof";
  page.noun = "print profile";
  page.icon = ICON_FA_PRINT;
  page.blurb =
      "Your printers' allowances: one printer, filament and slicer preset "
      "each, corrected from a test coupon (generators/print_coupon.py). New "
      "starts from the built-in table. A printed part picks one in its Print "
      "process section and keeps a copy, so it opens anywhere with the "
      "numbers it was modelled with.";
  page.names = [&state] { return PrintProfileNames(state); };
  page.selected = &state.printProfileEditIndex;
  page.error = &g_profileError;
  page.create = [&state] {
    // The defaults, corrected: a new profile starts as the built-in table
    // of the selected one's pair, else the first pair.
    std::string process = printfit::Tables().front().process;
    std::string material = printfit::Tables().front().material;
    const int sel = state.printProfileEditIndex;
    if (sel >= 0 && sel < static_cast<int>(state.printProfiles.size())) {
      process = state.printProfiles[static_cast<size_t>(sel)].table.process;
      material = state.printProfiles[static_cast<size_t>(sel)].table.material;
    }
    const auto fresh = printfit::ProfileFromBuiltin(
        FreePrintProfileName(state, "New profile"), process, material);
    if (fresh)
      CommitPrintProfile(state, *fresh);
  };
  page.duplicate = [&state](int index) {
    printfit::Profile copy = state.printProfiles[static_cast<size_t>(index)];
    copy.name = FreePrintProfileName(state, copy.name);
    CommitPrintProfile(state, copy);
  };
  page.remove = [&state](int index) {
    std::string error;
    if (!DeletePrintProfile(
            state, state.printProfiles[static_cast<size_t>(index)].name,
            &error))
      g_profileError = error;
    else
      g_profileError.clear();
    state.printProfileEditIndex = -1;
  };
  page.form = [&state](int index) { DrawPrintProfileForm(state, index); };
  DrawLibraryPage(page);
  return false; // the profile store is not settings.json
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

// The table's sections get a glyph by name; a section this list does not
// know still becomes a page, with the generic one, so a new row needs no
// window change.
const char* SectionIcon(const std::string& section) {
  static const std::pair<const char*, const char*> kIcons[] = {
      {"Appearance", ICON_FA_PALETTE},
      {"General", ICON_FA_SLIDERS},
      {"Performance", ICON_FA_GAUGE_HIGH},
      {"Sharing", ICON_FA_CODE_BRANCH},
  };
  for (const auto& [name, icon] : kIcons)
    if (section == name)
      return icon;
  return ICON_FA_GEAR;
}

// The pages the settings table does not make, after its sections, in this
// order. A page here is a name, a glyph and a draw function; a LIBRARY page
// also names its items, so search can find one, and opens one when a match
// is clicked. A new library page is one more row whose draw function fills
// a LibraryPage (Machines and Print profiles are two).
struct HandPage {
  const char* name;
  const char* icon;
  bool (*draw)(AppState&); // true when a preference in settings.json changed
  std::vector<std::string> (*items)(const AppState&) = nullptr; // a library
  void (*open)(AppState&, int index) = nullptr;
};

const HandPage kHandPages[] = {
    {"Keyboard", ICON_FA_KEYBOARD, DrawKeyboardSection},
    {"Machines", ICON_FA_INDUSTRY, DrawMachinesPage, MachineNames,
     [](AppState& s, int i) { s.machineEditIndex = i; }},
    {"Print profiles", ICON_FA_PRINT, DrawPrintProfilesPage, PrintProfileNames,
     [](AppState& s, int i) { s.printProfileEditIndex = i; }},
};

const HandPage* FindHandPage(const std::string& name) {
  for (const HandPage& p : kHandPages)
    if (name == p.name)
      return &p;
  return nullptr;
}

// Case-insensitive substring; an empty needle matches everything.
bool Matches(const std::string& haystack, const std::string& needle) {
  const auto lower = [](std::string v) {
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return v;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

// The muted heading over a page, or over one category's matches in search.
void PageTitle(const PreferenceCategory& c) {
  ImGui::TextUnformatted(c.icon);
  ImGui::SameLine();
  ImGui::TextUnformatted(c.name.c_str());
  ImGui::Separator();
  ImGui::Spacing();
}

// One table row: the control, then the blurb (or, for the GPU, what is in
// use and whether the choice is pending).
bool DrawRow(AppState& state, const settings::Field& f) {
  const bool changed = DrawField(state, f);
  // The GPU row says what is actually in use, and says so LOUDLY when the
  // choice no longer matches it — "nothing happened" is the correct
  // behaviour for a restart-tier setting, but only if the window admits it.
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
                         "Restart Lobole to render on %s.", want.c_str());
      ImGui::PopTextWrapPos();
    }
  } else {
    DrawBlurb(f);
  }
  ImGui::Spacing();
  return changed;
}

// The search results: every table row whose label or blurb matches, under
// its category's name, and every library item whose name does, as a row
// that opens it. A hand-written page whose NAME matches is offered too, so
// "keyboard" is not a dead end.
bool DrawSearchResults(AppState& state,
                       const std::vector<PreferenceCategory>& categories) {
  const std::string& q = state.settingsSearch;
  bool changed = false;
  bool any = false;
  for (const PreferenceCategory& c : categories) {
    if (!c.rows.empty()) {
      std::vector<const settings::Field*> hits;
      for (const settings::Field* f : c.rows)
        if (Matches(f->label, q) || Matches(f->blurb, q))
          hits.push_back(f);
      if (hits.empty())
        continue;
      any = true;
      PageTitle(c);
      for (const settings::Field* f : hits)
        changed |= DrawRow(state, *f);
      ImGui::Spacing();
      continue;
    }
    const HandPage* page = FindHandPage(c.name);
    if (page == nullptr)
      continue;
    std::vector<std::pair<int, std::string>> hits;
    if (page->items != nullptr) {
      const std::vector<std::string> items = page->items(state);
      for (size_t i = 0; i < items.size(); i++)
        if (Matches(items[i], q))
          hits.emplace_back(static_cast<int>(i), items[i]);
    }
    const bool nameHit = Matches(c.name, q);
    if (hits.empty() && !nameHit)
      continue;
    any = true;
    PageTitle(c);
    if (nameHit) {
      ListRowSpec row;
      row.glyph = c.icon;
      row.label = "Open " + c.name + "###find_page_" + c.name;
      if (ListRow(row).clicked) {
        state.settingsCategory = c.name;
        state.settingsSearch.clear();
      }
    }
    for (const auto& [index, name] : hits) {
      ListRowSpec row;
      row.glyph = c.icon;
      row.label = name + "###find_" + c.name + "_" + name;
      row.tail = "open";
      if (ListRow(row).clicked) {
        page->open(state, index);
        state.settingsCategory = c.name;
        state.settingsSearch.clear();
      }
    }
    ImGui::Spacing();
  }
  if (!any)
    ImGui::TextDisabled("Nothing matches \"%s\".", q.c_str());
  return changed;
}

} // namespace

std::vector<PreferenceCategory>
PreferenceCategories(std::span<const settings::Field> fields) {
  // Sections in order of first appearance, each EXACTLY ONCE, whatever the
  // order of the rows: the two Sharing rows once landed between the
  // Performance rows, and a window that opened a new header whenever the
  // section changed drew Performance twice under one id.
  std::vector<PreferenceCategory> out;
  for (const settings::Field& f : fields) {
    auto it =
        std::find_if(out.begin(), out.end(), [&](const PreferenceCategory& c) {
          return c.name == f.section;
        });
    if (it == out.end()) {
      out.push_back({f.section, SectionIcon(f.section), {}, false});
      it = out.end() - 1;
    }
    it->rows.push_back(&f);
  }
  for (const HandPage& p : kHandPages)
    out.push_back({p.name, p.icon, {}, p.items != nullptr});
  return out;
}

void OpenPreferences(AppState& state, std::string_view category) {
  state.settingsWindowOpen = true;
  state.settingsFocusRequested = true;
  state.settingsSearch.clear();
  for (const PreferenceCategory& c : PreferenceCategories())
    if (c.name == category) {
      state.settingsCategory = c.name;
      return;
    }
  LOG_WARN("OpenPreferences: no page called '{}'", category);
}

void ApplySettings(AppState& state) {
  const settings::Settings& s = state.settings;
  // Who a commit is by (F9 C1). GitVault holds it rather than reading the
  // settings file, which would be a file read per commit.
  GitVault::SetPreferredIdentity(s.gitUserName, s.gitUserEmail);
  state.uiFontScale = s.uiFontScale;
  state.showShadows = s.shadows;
  state.lightsLockedToModel = s.lockLightsToModel;
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
  ImGui::SetNextWindowSize(ImVec2(std::min(em * 46.0f, vp->WorkSize.x * 0.9f),
                                  std::min(em * 34.0f, vp->WorkSize.y * 0.85f)),
                           ImGuiCond_Appearing);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f,
                                 vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                          ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (state.settingsFocusRequested) {
    ImGui::SetNextWindowFocus();
    state.settingsFocusRequested = false;
  }
  if (!ImGui::Begin("Preferences###SettingsWindow",
                    &state.settingsWindowOpen)) {
    // Collapsed or closed: an armed capture would keep eating every key
    // press with no visible row to explain why.
    state.keyCaptureId.clear();
    ImGui::End();
    return;
  }

  bool changed = false;
  const ImGuiStyle& style = ImGui::GetStyle();
  const std::vector<PreferenceCategory> categories = PreferenceCategories();
  const PreferenceCategory* current = &categories.front();
  for (const PreferenceCategory& c : categories)
    if (c.name == state.settingsCategory)
      current = &c;
  const bool searching = !state.settingsSearch.empty();

  // The footer's height is kept free under both columns.
  const float footerH =
      ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2.0f + 1.0f;

  // Left: the search field and one row per category, as wide as the longest
  // name (measured, so it follows the font size).
  float navW = em * 9.0f;
  for (const PreferenceCategory& c : categories)
    navW = std::max(navW, ImGui::CalcTextSize(c.name.c_str()).x + em * 3.0f +
                              style.WindowPadding.x * 2.0f);
  BeginPane("prefs_nav", ImVec2(navW, -footerH), ImGuiChildFlags_Borders);
  {
    char buffer[128] = {};
    std::snprintf(buffer, sizeof(buffer), "%s", state.settingsSearch.c_str());
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("###set_search",
                                 ICON_FA_MAGNIFYING_GLASS " Search", buffer,
                                 sizeof(buffer))) {
      state.settingsSearch = buffer;
      state.keyCaptureId.clear();
    }
    ImGui::Spacing();
    for (const PreferenceCategory& c : categories) {
      ListRowSpec row;
      row.glyph = c.icon;
      row.label = c.name + "###cat_" + c.name;
      row.selected = !searching && &c == current;
      if (ListRow(row).clicked) {
        state.settingsCategory = c.name;
        state.settingsSearch.clear();
        state.keyCaptureId.clear(); // armed against a row now hidden
      }
    }
  }
  EndPane();

  // Right: the page, alone; or, while the search field holds text, what it
  // found across every page.
  ImGui::SameLine();
  BeginPane("prefs_page", ImVec2(0.0f, -footerH));
  if (searching) {
    changed |= DrawSearchResults(state, categories);
  } else {
    PageTitle(*current);
    if (!current->rows.empty()) {
      for (const settings::Field* f : current->rows)
        changed |= DrawRow(state, *f);
    } else if (const HandPage* page = FindHandPage(current->name)) {
      changed |= page->draw(state);
    }
  }
  EndPane();

  ImGui::Separator();
  // A table page resets alone; a library has no reset, because its items
  // are the user's own data, not preferences with a default.
  if (!searching && !current->rows.empty()) {
    const std::string label =
        "Reset " + current->name + " to defaults###set_reset_category";
    if (ImGui::Button(label.c_str())) {
      settings::ResetSection(state.settings, current->name);
      changed = true;
    }
    ImGui::SameLine();
  }
  // A user who has broken their config should not have to find the JSON.
  if (ImGui::Button("Reset all to defaults###set_reset_all")) {
    const auto unknown = state.settings.unknown; // not ours to discard
    state.settings = settings::Settings{};
    state.settings.unknown = unknown;
    state.keyCaptureId.clear(); // a capture armed against a row that is gone
    changed = true;
  }
  ImGui::SameLine();
  // The path, its START elided when the footer is too narrow for it: the
  // file name and the directory it is in are the end a person reads.
  const std::string path = settings::ConfigPath();
  const float room = ImGui::GetContentRegionAvail().x;
  std::string shown = path;
  if (ImGui::CalcTextSize(shown.c_str()).x > room) {
    const char* ellipsis = "\xe2\x80\xa6";
    size_t cut = 0;
    while (cut < path.size() &&
           ImGui::CalcTextSize((ellipsis + path.substr(cut)).c_str()).x >
               room) {
      cut++;
      while (cut < path.size() && (path[cut] & 0xC0) == 0x80)
        cut++; // never inside a UTF-8 sequence
    }
    shown = ellipsis + path.substr(cut);
  }
  ImGui::TextDisabled("%s", shown.c_str());
  if (shown != path)
    ImGui::SetItemTooltip("%s", path.c_str());

  if (changed)
    MarkSettingsChanged(state);
  if (!state.settingsWindowOpen)
    state.keyCaptureId.clear(); // closed by the title-bar X
  ImGui::End();
}

} // namespace ui
