#pragma once

#include "ui/AppState.h"

#ifdef HAVE_TERMINAL
#include "common/TerminalScreen.h"
#endif

#include <string>
#include <vector>

namespace ui {

// The terminal panel (docs/ui/terminal-panel.md): a dockable window on the
// right holding one PTY session, so Claude Code — or a shell, or git — runs
// beside the model it is editing.
//
// Off unless `settings.terminalPanel`. Drawing it is the whole integration:
// there is no RPC that can start a process, and no ui_action that can type
// into it, deliberately — an agent that wants a shell already has one, and
// the app must not become a second, unauthenticated path to it.
void DrawTerminalPanel(AppState& state);

// True while the pointer is over the terminal and a session is running, in
// which case the terminal owns the keyboard and the app's own shortcuts must
// stand down. Read by MainMenuBar and the command palette.
bool TerminalCapturesKeys(const AppState& state);

// Ends the session, if any. Called on shutdown so a child process is never
// orphaned by the app exiting.
void ShutdownTerminal(AppState& state);

#ifdef HAVE_TERMINAL
// For tests: the live screen (null when no session), whether one is running,
// and a way to run a specific command in the panel.
term::Screen* TerminalScreenOf(AppState& state);
bool TerminalRunning(const AppState& state);
void StartTerminalCommand(AppState& state, const std::string& program,
                          const std::vector<std::string>& args);
#endif

} // namespace ui
