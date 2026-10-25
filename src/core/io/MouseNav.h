// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Mouse navigation: which drag orbits, pans, zooms and rolls the view
// (docs/ui/keybindings.md#mouse-navigation). The gesture table lives here, in
// core with no ImGui, for the reason the keymap does: the parser, the presets
// and the clash check are unit-testable without a renderer, and the viewport,
// the drawing sheet and the cheat-sheet all ask the one table instead of
// testing buttons each in their own way.
namespace mousenav {

// What a drag does to the view. ui::ViewportDrag is this type.
enum class Action : uint8_t { None, Orbit, Pan, Zoom, Roll };

// ImGuiMouseButton's numbering, so the input layer passes its button through.
enum class Button : uint8_t { Left = 0, Right = 1, Middle = 2 };

// The one held key a gesture adds to its button. On the left button Alt is
// part of the BUTTON (a bare left is picking, box select and every handle,
// so it navigates only with Alt held), and the extra key is Ctrl or Shift.
enum class Mod : uint8_t { None, Ctrl, Shift, Alt };

struct Gesture {
  Button button = Button::Middle;
  Mod mod = Mod::None;
  bool operator==(const Gesture&) const = default;
};

// Every gesture this build accepts, in its canonical text ("middle",
// "ctrl+right", "shift+alt+left", ...). The settings rows offer exactly these.
std::span<const char* const> AcceptedGestures();

// "middle", "Ctrl+Middle", "alt+ctrl+left" (any case, modifiers in any
// order). nullopt, with the accepted gestures named in `error`, for anything
// else -- a bare "left" included.
std::optional<Gesture> ParseGesture(std::string_view text,
                                    std::string* error = nullptr);
// The canonical text: ParseGesture(FormatGesture(g)) == g.
std::string FormatGesture(Gesture g);
// For people: "Ctrl+Middle drag", "Alt+Left drag".
std::string DescribeGesture(Gesture g);

// The four settings rows, in Action order (Orbit..Roll).
constexpr const char* kActionKeys[4] = {"nav_orbit", "nav_pan", "nav_zoom",
                                        "nav_roll"};
const char* ActionLabel(Action a); // "Rotate", "Pan", "Zoom", "Roll"

// The gesture texts of the four rows, Orbit..Roll.
using Rows = std::array<std::vector<std::string>, 4>;

// A preset: one other program's navigation, written from its own
// documentation (the source is cited beside each row in MouseNav.cpp).
struct Preset {
  const char* name;  // the settings value: "solidworks"
  const char* label; // "SolidWorks"
  Rows rows;
};
std::span<const Preset> Presets(); // "lobole" (the defaults) first
const Rows& DefaultRows();         // the "lobole" preset's
const Preset* FindPreset(std::string_view name);
// Every value the nav_preset row accepts: the presets, then "custom".
std::span<const char* const> PresetNames();
constexpr const char* kCustomPreset = "custom";
// The preset whose rows are exactly `rows` (order within a row ignored), or
// "custom" when none is.
const char* MatchPreset(const Rows& rows);

// The rows, parsed and checked: every text a gesture, and no gesture claimed
// by two actions (there is no order between them to settle it).
class NavMap {
public:
  static std::optional<NavMap> Build(const Rows& rows,
                                     std::string* error = nullptr);

  // What a drag on `button` with these keys held does now. Asked every frame,
  // so a key pressed mid-drag switches the action. Several keys held resolve
  // as Ctrl, then Shift, then Alt -- the first one held is the gesture's.
  // None for a combination nothing is bound to (and for a left button without
  // Alt, always).
  Action ActionFor(Button button, bool ctrl, bool shift, bool alt) const;
  // Whether a press of `button` starts a navigation drag at all: anything is
  // bound to it (for the left button, only with Alt held).
  bool Navigates(Button button, bool alt) const;
  std::vector<Gesture> GesturesFor(Action a) const;

private:
  std::vector<std::pair<Gesture, Action>> m_bindings;
};

} // namespace mousenav
