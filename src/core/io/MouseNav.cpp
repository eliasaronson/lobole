// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/MouseNav.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace mousenav {

namespace {

constexpr std::array<const char* const, 11> kAccepted{
    "middle",   "ctrl+middle",   "shift+middle",  "alt+middle",
    "right",    "ctrl+right",    "shift+right",   "alt+right",
    "alt+left", "ctrl+alt+left", "shift+alt+left"};

std::string AcceptedList() {
  std::string out;
  for (const char* g : kAccepted)
    out += (out.empty() ? "" : ", ") + std::string(g);
  return out;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.remove_suffix(1);
  return s;
}

// The presets, built on first use (they hold vectors, and Settings' defaults
// read them -- a namespace-scope table could be read before it is built). Each
// is that program's DEFAULT navigation as its own documentation states it,
// mapped onto the gestures above; the source is beside each row, and where the
// program's mapping cannot be expressed here the comment says what was done
// instead.
std::span<const Preset> PresetTable() {
  static const Preset kPresets[] = {
      // Ours (docs/ui/README.md#viewport): the middle and right buttons and
      // Alt+left all orbit; Ctrl makes any of them pan, Shift zoom; Alt on the
      // middle button rolls.
      {"lobole",
       "Loböle",
       {{{"middle", "right", "alt+left"},
         {"ctrl+middle", "ctrl+right", "ctrl+alt+left"},
         {"shift+middle", "shift+right", "shift+alt+left"},
         {"alt+middle"}}}},
      // SolidWorks: "Rotate View: Drag with the middle mouse button", "Pan:
      // Hold down Ctrl and drag with the middle mouse button", "Zoom In/Out:
      // Hold down Shift and drag with the middle mouse button", "Roll View:
      // Hold down Alt and drag with the middle mouse button".
      // https://help.solidworks.com/2025/english/SolidWorks/sldworks/r_Middle_Mouse_Button.htm
      // https://help.solidworks.com/2025/english/SolidWorks/sldworks/t_roll_view.htm
      // https://help.solidworks.com/2025/english/SolidWorks/sldworks/t_zoom_in_out.htm
      {"solidworks",
       "SolidWorks",
       {{{"middle"}, {"ctrl+middle"}, {"shift+middle"}, {"alt+middle"}}}},
      // Fusion, its default "Fusion" mouse setup: "Zoom: Roll the middle mouse
      // button or Ctrl + Shift + middle mouse button / Pan: Middle mouse button
      // / Orbit: Shift + middle mouse button". No roll drag.
      // https://help.autodesk.com/cloudhelp/ENU/Fusion-GetStarted/files/GUID-878489CD-3A23-4303-8450-C2F4F8E410B1.htm
      // https://help.autodesk.com/view/fusion360/ENU/?guid=GUID-F0491540-0324-470A-B651-2238D0EFAC30
      // NOT EXPRESSIBLE: the zoom drag holds two keys. It is bound as
      // ctrl+middle, which is what Ctrl+Shift+middle resolves to here (Ctrl
      // first, NavMap::ActionFor), so Fusion's own gesture zooms -- and so does
      // Ctrl+middle alone, which Fusion leaves unbound.
      {"fusion",
       "Fusion",
       {{{"shift+middle"}, {"middle"}, {"ctrl+middle"}, {}}}},
      // Onshape, its defaults: 3D rotate "Right-mouse-button click, then drag"
      // (and, on Windows, "Alt + Right Mouse button" for a constrained rotate),
      // 2D pan "Ctrl + Right-mouse-button click, then drag /
      // Middle-mouse-button
      // click, then drag", zoom on the wheel only, no roll.
      // https://cad.onshape.com/help/Content/View/view_navigation_and_the_view_cube.htm
      // CLOSEST, not the same: Alt+right is bound to our ordinary orbit; we
      // have no constrained (turntable) rotate.
      {"onshape",
       "Onshape",
       {{{"right", "alt+right"}, {"ctrl+right", "middle"}, {}, {}}}},
      // Blender, the default keymap: "Rotate the view ... by clicking and
      // dragging MMB", "Shift-MMB -- Pan the view", "zoom in and out by
      // rolling the Wheel or dragging with Ctrl-MMB"; roll is on the numpad
      // only. Read from the manual's source, because docs.blender.org refused
      // the fetch:
      // https://projects.blender.org/blender/blender-manual/raw/branch/main/manual/editors/3dview/navigate/navigation.rst
      // (rendered at
      // https://docs.blender.org/manual/en/latest/editors/3dview/navigate/navigation.html).
      // Not here: Alt+MMB (set the pivot / snap to an axis view) and the
      // Shift+Ctrl+MMB dolly, which resolves to the Ctrl zoom.
      {"blender",
       "Blender",
       {{{"middle"}, {"shift+middle"}, {"ctrl+middle"}, {}}}},
  };
  return kPresets;
}

constexpr std::array<const char* const, 6> kPresetNames{
    "lobole", "solidworks", "fusion", "onshape", "blender", kCustomPreset};

} // namespace

std::span<const char* const> AcceptedGestures() { return kAccepted; }

std::optional<Gesture> ParseGesture(std::string_view text, std::string* error) {
  const auto fail = [&]() -> std::optional<Gesture> {
    if (error)
      *error = "'" + std::string(text) + "' is not a gesture; one of " +
               AcceptedList();
    return std::nullopt;
  };
  const std::string lower = Lower(text);
  std::optional<Button> button;
  bool ctrl = false, shift = false, alt = false;
  std::string_view rest = lower;
  while (true) {
    const size_t plus = rest.find('+');
    const std::string_view token = Trim(rest.substr(0, plus));
    bool* flag = token == "ctrl"    ? &ctrl
                 : token == "shift" ? &shift
                 : token == "alt"   ? &alt
                                    : nullptr;
    if (flag != nullptr) {
      if (*flag)
        return fail(); // "ctrl+ctrl+middle"
      *flag = true;
    } else if (token == "left" || token == "middle" || token == "right") {
      if (button)
        return fail(); // two buttons
      button = token == "left"     ? Button::Left
               : token == "middle" ? Button::Middle
                                   : Button::Right;
    } else {
      return fail();
    }
    if (plus == std::string_view::npos)
      break;
    rest = rest.substr(plus + 1);
  }
  if (!button)
    return fail();
  Gesture g{*button, Mod::None};
  if (*button == Button::Left) {
    // Alt is the left button's own; a bare left is picking.
    if (!alt || (ctrl && shift))
      return fail();
    g.mod = ctrl ? Mod::Ctrl : shift ? Mod::Shift : Mod::None;
    return g;
  }
  if (int(ctrl) + int(shift) + int(alt) > 1)
    return fail();
  g.mod = ctrl ? Mod::Ctrl : shift ? Mod::Shift : alt ? Mod::Alt : Mod::None;
  return g;
}

std::string FormatGesture(Gesture g) {
  std::string out;
  if (g.mod == Mod::Ctrl)
    out = "ctrl+";
  else if (g.mod == Mod::Shift)
    out = "shift+";
  else if (g.mod == Mod::Alt && g.button != Button::Left)
    out = "alt+";
  switch (g.button) {
  case Button::Left:
    return out + "alt+left";
  case Button::Right:
    return out + "right";
  case Button::Middle:
    return out + "middle";
  }
  return out;
}

std::string DescribeGesture(Gesture g) {
  std::string out;
  if (g.mod == Mod::Ctrl)
    out = "Ctrl+";
  else if (g.mod == Mod::Shift)
    out = "Shift+";
  else if (g.mod == Mod::Alt && g.button != Button::Left)
    out = "Alt+";
  switch (g.button) {
  case Button::Left:
    return out + "Alt+Left drag";
  case Button::Right:
    return out + "Right drag";
  case Button::Middle:
    return out + "Middle drag";
  }
  return out;
}

const char* ActionLabel(Action a) {
  switch (a) {
  case Action::Orbit:
    return "Rotate";
  case Action::Pan:
    return "Pan";
  case Action::Zoom:
    return "Zoom";
  case Action::Roll:
    return "Roll";
  case Action::None:
    break;
  }
  return "";
}

std::span<const Preset> Presets() { return PresetTable(); }

const Rows& DefaultRows() { return PresetTable()[0].rows; }

const Preset* FindPreset(std::string_view name) {
  for (const Preset& p : PresetTable())
    if (name == p.name)
      return &p;
  return nullptr;
}

std::span<const char* const> PresetNames() { return kPresetNames; }

const char* MatchPreset(const Rows& rows) {
  // Compared as gestures, not text: "Ctrl+Middle" in a hand-edited file is
  // the same gesture as the preset's "ctrl+middle".
  const auto canon = [](const std::vector<std::string>& texts) {
    std::vector<std::string> out;
    for (const std::string& t : texts) {
      const std::optional<Gesture> g = ParseGesture(t);
      out.push_back(g ? FormatGesture(*g) : t);
    }
    std::sort(out.begin(), out.end());
    return out;
  };
  for (const Preset& p : PresetTable()) {
    bool same = true;
    for (size_t i = 0; i < 4 && same; i++)
      same = canon(rows[i]) == canon(p.rows[i]);
    if (same)
      return p.name;
  }
  return kCustomPreset;
}

std::optional<NavMap> NavMap::Build(const Rows& rows, std::string* error) {
  NavMap map;
  for (size_t i = 0; i < 4; i++) {
    const Action action = static_cast<Action>(i + 1);
    for (const std::string& text : rows[i]) {
      std::string why;
      const std::optional<Gesture> g = ParseGesture(text, &why);
      if (!g) {
        if (error)
          *error = std::string(kActionKeys[i]) + ": " + why;
        return std::nullopt;
      }
      for (const auto& [other, otherAction] : map.m_bindings) {
        if (other != *g)
          continue;
        if (otherAction == action)
          break; // listed twice in one row: harmless
        if (error)
          *error = std::string(kActionKeys[i]) + ": " + FormatGesture(*g) +
                   " is already " +
                   kActionKeys[static_cast<size_t>(otherAction) - 1] +
                   "; a gesture does one thing";
        return std::nullopt;
      }
      map.m_bindings.emplace_back(*g, action);
    }
  }
  return map;
}

Action NavMap::ActionFor(Button button, bool ctrl, bool shift, bool alt) const {
  Gesture g{button, Mod::None};
  if (button == Button::Left) {
    if (!alt)
      return Action::None;
    g.mod = ctrl ? Mod::Ctrl : shift ? Mod::Shift : Mod::None;
  } else {
    g.mod = ctrl ? Mod::Ctrl : shift ? Mod::Shift : alt ? Mod::Alt : Mod::None;
  }
  for (const auto& [bound, action] : m_bindings)
    if (bound == g)
      return action;
  return Action::None;
}

bool NavMap::Navigates(Button button, bool alt) const {
  if (button == Button::Left && !alt)
    return false;
  for (const auto& [bound, action] : m_bindings)
    if (bound.button == button)
      return true;
  return false;
}

std::vector<Gesture> NavMap::GesturesFor(Action a) const {
  std::vector<Gesture> out;
  for (const auto& [bound, action] : m_bindings)
    if (action == a)
      out.push_back(bound);
  return out;
}

} // namespace mousenav
