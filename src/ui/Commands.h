#pragma once

#include "core/io/Keymap.h"
#include "ui/AppState.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>

// The executable half of a binding (docs/ui/keybindings.md).
// keymap::Binding (core) says what a command is called, which context it
// belongs to and which chord runs it; Command (here) says whether it can run
// right now and what happens when it does.
//
// FOUR consumers, one table: the keyboard dispatcher, the toolbar (tooltips
// and clicks), the command palette, and the Ctrl+/ shortcuts window. Before
// this they were four hand-maintained lists that could — and did — disagree
// about what a key was bound to.
namespace ui {

struct Command {
  const char* id;                         // matches a keymap::Binding::id
  std::function<bool(AppState&)> enabled; // null = always available
  std::function<void(AppState&)> run;
};

std::span<const Command> Commands();
const Command* FindCommand(std::string_view id);

// The context the app is in right now. This is DrawToolbar's own branch,
// factored out and shared, so the toolbar you can see and the keys that work
// can never disagree about which mode you are in.
keymap::Context ActiveContext(const AppState& state);

bool CommandEnabled(AppState& state, std::string_view id);
// Runs a command by id. Returns false for an unknown or disabled one, so a
// caller (the RPC, the palette) can say why nothing happened.
bool RunCommand(AppState& state, std::string_view id);

// The chord this command answers to right now, as text; "" when unbound.
std::string CommandChord(const AppState& state, std::string_view id);
// "Fit view (F)" — the tooltip with its live chord appended, unchanged when
// the command is unbound. Every toolbar tooltip goes through this instead of
// spelling the chord in the literal, which is how the toolbar's hints used to
// drift from the keys that actually fired.
std::string WithChord(const AppState& state, std::string_view id,
                      const char* tooltip);

// The chord pressed this frame, for Preferences > Keyboard's click-to-capture.
// Empty until a NON-modifier key goes down, so holding Shift on its way to
// Shift+G does not bind "Shift".
keymap::Chord CaptureChord();

// A binding cannot call ImGui::OpenPopup itself: the popup id is built from
// the ID stack of the window that opens it, and the dispatcher runs outside
// every window. The command records a request; the toolbar that owns the
// popup calls this on its next frame and opens it if the name matches.
bool TakePopupRequest(AppState& state, const char* name);

// Every binding for the active context plus the globals it does not shadow,
// resolved once per frame. Replaces the four independent if-chains in
// MainMenuBar, ViewportInput, SketchInteraction and CommandPalette.
void DispatchShortcuts(AppState& state);

} // namespace ui
