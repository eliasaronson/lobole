// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <compare>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Keyboard bindings: which chord runs which command, in which context
// (docs/ui/keybindings.md). The metadata lives here — in core, with no
// ImGui and no AppState — for the same reason Settings does: designer_tests
// links designer_core alone, so the defaults table, the chord parser and the
// conflict check are unit-testable without a renderer.
//
// The other half of a command, `enabled` and `run`, lives in ui/Commands.cpp
// keyed by the same id. A GUI test asserts the two tables are 1:1, so a
// command cannot exist in one and not the other.
//
// NOT here, on purpose: Escape, Enter, Delete and the arrow keys. Those
// resolve against armed tools, pending previews, plane-picking, slot-picks
// and text fields across ~15 call sites — they are modal verbs, not commands,
// and making them rebindable would mean moving that priority order into the
// dispatcher. The Preferences window says so rather than implying otherwise.
namespace keymap {

// Which workspace a binding belongs to. Matches the branch DrawToolbar
// already takes (ui::ActiveContext is that branch, factored out), so the
// toolbar you see and the keys that work cannot disagree.
//
// A context binding SHADOWS a global one with the same chord: that is the
// whole point of contexts. `N` is Coincident while sketching and align-to-
// face everywhere else, and neither has to know about the other.
enum class Context : uint8_t {
  Global,
  Part,
  Sketch,
  Assembly,
  Drawing,
  Job, // a CAM job tab (docs/cam/gui.md#the-job-toolbar)
  Simulation,
};

// Stable name (JSON, RPC) and the heading the Preferences window shows.
const char* ContextName(Context c);
const char* ContextLabel(Context c);
std::span<const Context> AllContexts();

// Our own key enum rather than ImGuiKey: core does not link ImGui, and
// nothing here should change if a vendored ImGui renumbers its enum. ui/Keys
// owns the one mapping to ImGuiKey.
enum class Key : uint16_t {
  None = 0,
  A,
  B,
  C,
  D,
  E,
  F,
  G,
  H,
  I,
  J,
  K,
  L,
  M,
  N,
  O,
  P,
  Q,
  R,
  S,
  T,
  U,
  V,
  W,
  X,
  Y,
  Z,
  Num0,
  Num1,
  Num2,
  Num3,
  Num4,
  Num5,
  Num6,
  Num7,
  Num8,
  Num9,
  F1,
  F2,
  F3,
  F4,
  F5,
  F6,
  F7,
  F8,
  F9,
  F10,
  F11,
  F12,
  Space,
  Tab,
  Comma,
  Period,
  Slash,
  Semicolon,
  Apostrophe,
  Minus,
  Equal,
  LeftBracket,
  RightBracket,
  Backslash,
  Grave,
  Home,
  End,
  PageUp,
  PageDown,
  Insert,
};

enum Mod : uint8_t {
  ModNone = 0,
  ModCtrl = 1 << 0,
  ModShift = 1 << 1,
  ModAlt = 1 << 2,
};

struct Chord {
  Key key = Key::None;
  uint8_t mods = ModNone;

  bool operator==(const Chord&) const = default;
  auto operator<=>(const Chord&) const = default;
  // An unbound command is a valid state, not an error — see Binding::chord.
  explicit operator bool() const { return key != Key::None; }
};

// "Ctrl+Shift+S", "Shift+1", "F". Case-insensitive on the modifiers, and the
// modifier order in the text does not matter. Returns an empty chord and
// names the problem in `error` on anything it cannot read; an EMPTY string
// parses to an empty chord with no error (that is how a command is unbound).
Chord ParseChord(std::string_view text, std::string* error = nullptr);
// The inverse, canonically ordered Ctrl+Shift+Alt+key. FormatChord of
// ParseChord is the identity for every chord in the defaults table (tested).
std::string FormatChord(Chord c);

struct Binding {
  const char* id; // "sketch.line" — stable; the JSON key and the RPC name
  Context ctx;
  const char* label;   // "Line" — Preferences row, palette, shortcuts window
  const char* section; // grouping inside a context ("Create", "View", ...)
  // Default chord, as text so the table reads as what a person would type.
  // EMPTY means bindable but unbound: better than inventing a mnemonic
  // nobody remembers, and the Preferences window turns it into one click.
  const char* chord;
  // A fixed second chord, not user-editable. Exists for the two duplicates
  // this app already shipped (Ctrl+Shift+Z for redo) — dropping them would
  // break muscle memory, and promoting them to real bindings would put two
  // rows in Preferences for one command.
  const char* altChord = "";
  // Kept out of the Preferences list and the shortcuts window. One tenant:
  // the frame-timing HUD, which is deliberately undocumented (PerfHud.h).
  bool hidden = false;
};

std::span<const Binding> Defaults();
const Binding* Find(std::string_view id);

// Two commands in the same context wanting the same chord. A global binding
// and a context binding that collide are NOT a conflict — the context one
// shadows it, deliberately.
struct Conflict {
  std::string a, b; // command ids, `a` < `b` so the pair is stable
  Context ctx;
  Chord chord;
};

// The defaults, plus whatever the user rebound. Copyable and cheap; the app
// keeps one in AppState and the Preferences window edits it live.
class Keymap {
public:
  // Resolved: the user's override if there is one, else the default.
  Chord Get(std::string_view id) const;
  Chord Alt(std::string_view id) const; // fixed secondary, never overridden
  std::string Text(std::string_view id) const; // "" when unbound
  bool IsDefault(std::string_view id) const;

  // Empty text unbinds. Returns false and names the problem on text that is
  // not a chord; the map is left untouched.
  bool Set(std::string_view id, std::string_view chordText,
           std::string* error = nullptr);
  void Reset(std::string_view id); // back to the default
  void ResetAll();
  void ResetContext(Context c);

  // Every command that would answer to `c` in context `ctx`, ignoring `except`
  // (pass the id being edited). Used for the live warning in Preferences.
  std::vector<std::string> Bound(Context ctx, Chord c,
                                 std::string_view except = {}) const;
  std::vector<Conflict> Conflicts() const;

  // Only what differs from the defaults — that is what reaches settings.json,
  // so a later change to a default reaches anyone who never touched it.
  const std::map<std::string, std::string>& Overrides() const {
    return m_overrides;
  }
  // Ids this build does not know are kept verbatim, same rule as
  // Settings::unknown: an older build must not delete a newer one's keymap.
  void SetOverrides(std::map<std::string, std::string> overrides);

  bool operator==(const Keymap& other) const = default;

private:
  std::map<std::string, std::string> m_overrides;
};

// settings.json's "keys" object <-> the override map. Values are chord text;
// an entry mapping to "" is an explicit unbind, which is not the same as
// having no entry (that is "use the default").
nlohmann::json OverridesToJson(const Keymap& k);
bool ApplyOverridesJson(Keymap& k, const nlohmann::json& j, std::string* error);

} // namespace keymap
