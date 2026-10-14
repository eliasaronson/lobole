// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ui/AppState.h"

#ifdef HAVE_TERMINAL
#include "common/TerminalScreen.h"
#endif

#include <string>
#include <vector>

namespace ui {

// The terminal panel (docs/ui/terminal.md): a dockable window on the
// right holding one PTY session per tab, so Claude Code — or a shell, or git
// — runs beside the model it is editing.
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
// How many tabs the panel has, and the two things the tab bar does.
int TerminalSessionCount(const AppState& state);
void NewTerminalTab(AppState& state);
void CloseTerminalTab(AppState& state, int index);
void SelectTerminalTab(AppState& state, int index);
// Whether the tab at `index` still has a live process — the one way to prove
// a BACKGROUND tab is being drained, since a child whose pty buffer filled up
// blocks mid-write and never gets to exit.
bool TerminalTabRunning(const AppState& state, int index);
const term::Screen* TerminalTabScreen(const AppState& state, int index);
// Which tab is on top. The tab bar is the authority on this, so a test that
// asks for a tab must check it got it.
int TerminalActiveTab(const AppState& state);
// The MCP config the Claude Code button writes for `port`, and its path
// (empty when this build finds no lobole package to point it at).
std::string WriteTerminalMcpConfig(int port);
#endif

} // namespace ui
