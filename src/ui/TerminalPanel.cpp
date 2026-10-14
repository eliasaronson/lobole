// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/TerminalPanel.h"
#include "ui/PanelTitles.h"

#ifdef HAVE_TERMINAL

#include "common/Logger.h"
#include "common/PtySession.h"
#include "common/RuntimePaths.h"
#include "common/TerminalScreen.h"
#include "ui/Theme.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, for the dock-on-first-use path
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ui {

namespace {

// The panel's own colours. The terminal's default fg/bg are pushed into
// libvterm so unstyled cells come back already themed — see
// Screen::SetDefaultColors.
constexpr uint32_t kTermBg = 0x14161Bu;
constexpr uint32_t kTermFg = 0xD5D8DEu;

ImU32 ToImCol(uint32_t rgb) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
}

// Every key that has to reach a program, in the order a terminal cares about.
struct KeyMap {
  ImGuiKey imgui;
  term::Screen::Key key;
};

constexpr std::array<KeyMap, 26> kKeys{{
    {ImGuiKey_Enter, term::Screen::Key::Enter},
    {ImGuiKey_KeypadEnter, term::Screen::Key::KeypadEnter},
    {ImGuiKey_Tab, term::Screen::Key::Tab},
    {ImGuiKey_Backspace, term::Screen::Key::Backspace},
    {ImGuiKey_Escape, term::Screen::Key::Escape},
    {ImGuiKey_UpArrow, term::Screen::Key::Up},
    {ImGuiKey_DownArrow, term::Screen::Key::Down},
    {ImGuiKey_LeftArrow, term::Screen::Key::Left},
    {ImGuiKey_RightArrow, term::Screen::Key::Right},
    {ImGuiKey_Home, term::Screen::Key::Home},
    {ImGuiKey_End, term::Screen::Key::End},
    {ImGuiKey_PageUp, term::Screen::Key::PageUp},
    {ImGuiKey_PageDown, term::Screen::Key::PageDown},
    {ImGuiKey_Insert, term::Screen::Key::Insert},
    {ImGuiKey_Delete, term::Screen::Key::Delete},
    {ImGuiKey_F1, term::Screen::Key::F1},
    {ImGuiKey_F2, term::Screen::Key::F2},
    {ImGuiKey_F3, term::Screen::Key::F3},
    {ImGuiKey_F4, term::Screen::Key::F4},
    {ImGuiKey_F5, term::Screen::Key::F5},
    {ImGuiKey_F6, term::Screen::Key::F6},
    {ImGuiKey_F7, term::Screen::Key::F7},
    {ImGuiKey_F8, term::Screen::Key::F8},
    {ImGuiKey_F9, term::Screen::Key::F9},
    {ImGuiKey_F10, term::Screen::Key::F10},
    {ImGuiKey_F11, term::Screen::Key::F11},
}};

int ModsNow() {
  const ImGuiIO& io = ImGui::GetIO();
  int mods = 0;
  if (io.KeyShift)
    mods |= static_cast<int>(term::Screen::Mod::Shift);
  if (io.KeyAlt)
    mods |= static_cast<int>(term::Screen::Mod::Alt);
  if (io.KeyCtrl)
    mods |= static_cast<int>(term::Screen::Mod::Ctrl);
  return mods;
}

} // namespace

// One session: a process, the screen it draws on, and how this panel is
// looking at it. Everything here is per-tab; nothing about the panel itself
// belongs in it.
struct Session {
  int id = 0; // monotonic and never reused — see kTabIdIsNotAnIndex below
  term::PtySession pty;
  std::unique_ptr<term::Screen> screen;
  int rows = 24;
  int cols = 80;
  std::string label;      // the session's program, for the header
  std::string startError; // why the last start failed, if it did
  int exitStatus = -1;    // of the last session
  int scrollBack = 0;     // lines scrolled up into history; 0 = live
  bool capturing = false; // the pointer is over us and a session is running
  std::string foreground; // process with the terminal, refreshed on output

  // Selection, in BUFFER lines (0 = oldest scrollback) so it survives
  // scrolling and new output. anchor is where the drag started; head follows
  // the pointer. Equal = nothing selected.
  size_t selAnchorLine = 0, selHeadLine = 0;
  int selAnchorCol = 0, selHeadCol = 0;
  bool selecting = false; // a drag is in progress
  bool hasSelection = false;
};

// The panel: a list of sessions and which one is on top. Lives behind a
// shared_ptr in AppState so the header stays free of libvterm.
struct TerminalState {
  std::vector<std::unique_ptr<Session>> sessions;
  int active = 0; // index into `sessions`
  int nextId = 1; // never decremented, so an id is never reused

  // A session id to select on the next frame, or 0.
  //
  // Needed because the tab bar is the AUTHORITY on which tab is selected:
  // BeginTabItem reports it, and the loop below writes it straight back into
  // `active` every frame. So assigning `active` from anywhere else — a
  // keyboard chord, a test — is overwritten before it can take effect, and
  // silently: the count is right, the selection is not. The only way to move
  // the bar is to hand ImGui SetSelected on the tab you mean.
  int pendingSelect = 0;
};

namespace {

// What a keyboard chord asked the panel — not the session — to do. HandleInput
// cannot act on these itself: making a session needs the project directory and
// the instance port, which live in AppState, and closing one has to happen
// outside the loop that is drawing them.
enum class TabRequest { None, New, Close, Prev, Next };

TerminalState& Ensure(AppState& state) {
  if (!state.terminal)
    state.terminal = std::make_shared<TerminalState>();
  return *state.terminal;
}

// The session on top, or null when every tab has been closed. Callers that
// only want to know "is there a terminal here" go through this rather than
// indexing, because `active` is only meaningful against a non-empty list.
Session* Active(TerminalState& t) {
  if (t.sessions.empty())
    return nullptr;
  const int i =
      std::clamp(t.active, 0, static_cast<int>(t.sessions.size()) - 1);
  return t.sessions[static_cast<size_t>(i)].get();
}

const Session* Active(const TerminalState& t) {
  return Active(const_cast<TerminalState&>(t));
}

// Where a session should start: the open project's directory, else the
// current one. A terminal that opens in `/` is useless for the thing this
// panel exists for.
std::string StartDirectory(const AppState& state) {
  if (!state.projectPath.empty()) {
    std::error_code ec;
    const std::filesystem::path p(state.projectPath);
    if (std::filesystem::is_directory(p, ec))
      return p.string();
    if (p.has_parent_path())
      return p.parent_path().string();
  }
  std::error_code ec;
  const std::filesystem::path cwd = std::filesystem::current_path(ec);
  return ec ? std::string(".") : cwd.string();
}

void StartSession(AppState& state, Session& t,
                  const term::PtySession::Options& base,
                  const std::string& label) {
  t.pty.Terminate();
  t.screen = std::make_unique<term::Screen>(t.rows, t.cols);
  t.screen->SetDefaultColors(kTermFg, kTermBg);
  t.startError.clear();
  t.exitStatus = -1;
  t.scrollBack = 0;

  term::PtySession::Options opts = base;
  opts.rows = t.rows;
  opts.cols = t.cols;
  if (opts.cwd.empty())
    opts.cwd = StartDirectory(state);

  std::string error;
  if (!t.pty.Start(opts, &error)) {
    t.startError = error;
    LOG_WARN("Terminal: {}", error);
    return;
  }
  t.label = label;
}

// The directory holding the lobole Python package (python/lobole/), if this
// build can find one: the checkout's own beside a dev build, the copy staged
// in its assets, or the installed data directory. Empty when none has it —
// the session still starts, just without MCP wired up.
std::string FindPythonPackageRoot() {
  std::error_code ec;
  const std::array<std::filesystem::path, 3> candidates{
      RuntimePaths::DataDir() / "python",
      RuntimePaths::ExecutableDir() / ".." / "python",
      RuntimePaths::ExecutableDir() / "python",
  };
  for (const std::filesystem::path& p : candidates) {
    const std::filesystem::path norm = p.lexically_normal();
    if (std::filesystem::exists(norm / "lobole" / "mcp.py", ec))
      return norm.string();
  }
  return {};
}

// This window's remote port, opening one on demand. A terminal inside the app
// is only better than a terminal beside it if what runs here can reach the
// model on screen, and that reach is a port.
int InstancePort(AppState& state) {
  if (state.remotePort > 0)
    return state.remotePort;
  if (state.ensureRemotePort)
    return state.ensureRemotePort();
  return 0;
}

// Writes a per-session MCP config naming this instance's port and returns its
// path (empty when there is nothing to point at). A file rather than `claude
// mcp add`: this is per session, it must not edit the user's global MCP
// registration, and a project .mcp.json belongs to the project, not to us.
std::string WriteMcpConfig(int port) {
  const std::string root = FindPythonPackageRoot();
  if (port <= 0 || root.empty())
    return {};
  const std::filesystem::path cfg =
      std::filesystem::temp_directory_path() /
      ("lobole-mcp-" + std::to_string(port) + ".json");
  const nlohmann::json config = {
      {"mcpServers",
       {{"lobole",
         {{"command", "python3"},
          {"args", {"-m", "lobole.mcp", "--port", std::to_string(port)}},
          {"env", {{"PYTHONPATH", root}}}}}}}};
  const std::string json = config.dump();
  FILE* f = std::fopen(cfg.string().c_str(), "wb");
  if (f == nullptr)
    return {};
  std::fwrite(json.data(), 1, json.size(), f);
  std::fclose(f);
  return cfg.string();
}

// The command that starts Claude Code bound to THIS window. It is typed into
// the shell rather than run behind the user's back: what the button does is
// then legible, repeatable, and the session it starts is a child of their
// shell — so exiting it returns them to a prompt, not to a dead panel.
std::string ClaudeCommand(AppState& state) {
  const std::string cfg = WriteMcpConfig(InstancePort(state));
  std::string cmd = "claude";
  if (!cfg.empty())
    cmd += " --mcp-config " + cfg;
  return cmd;
}

// $SHELL, with this window's port already exported: typing `claude` — or
// `python3 -m lobole.client`, or anything else that reads
// DESIGNER_PORT — then talks to the model on screen without being told which
// instance to attach to.
void StartShell(AppState& state, Session& t) {
  term::PtySession::Options opts; // empty program = $SHELL
  const int port = InstancePort(state);
  if (port > 0)
    opts.env.emplace_back("DESIGNER_PORT", std::to_string(port));
  const char* shell = std::getenv("SHELL");
  StartSession(state, t, opts,
               shell != nullptr && *shell != '\0'
                   ? std::filesystem::path(shell).filename().string()
                   : "shell");
}

// Reads what the child wrote and hands the terminal's own replies back to it.
void PumpSession(Session& t) {
  if (t.screen == nullptr)
    return;
  const std::string out = t.pty.Poll();
  if (!out.empty()) {
    t.screen->Write(out.data(), out.size());
    t.scrollBack = 0; // new output snaps back to live, as a terminal does
    // Output is exactly when what-is-in-front can have changed, so the /proc
    // read happens then rather than every frame.
    t.foreground = t.pty.ForegroundName();
  }
  // OSC 52: the program asked us to put something on the SYSTEM clipboard.
  // This is Claude Code's own copy, and every other TUI's — they cannot reach
  // the clipboard themselves, so a terminal that drops the sequence makes
  // copying inside them do nothing at all, silently.
  if (std::optional<std::string> clip = t.screen->TakeClipboardWrite())
    ImGui::SetClipboardText(clip->c_str());

  const std::string reply = t.screen->TakeOutput();
  if (!reply.empty())
    t.pty.Write(reply.data(), reply.size());
  if (!t.pty.Running() && t.exitStatus < 0)
    t.exitStatus = t.pty.ExitStatus();
}

// Selection in reading order, whichever way the drag went.
void OrderedSelection(const Session& t, size_t& l0, int& c0, size_t& l1,
                      int& c1) {
  const bool forward =
      t.selAnchorLine < t.selHeadLine ||
      (t.selAnchorLine == t.selHeadLine && t.selAnchorCol <= t.selHeadCol);
  l0 = forward ? t.selAnchorLine : t.selHeadLine;
  c0 = forward ? t.selAnchorCol : t.selHeadCol;
  l1 = forward ? t.selHeadLine : t.selAnchorLine;
  c1 = forward ? t.selHeadCol : t.selAnchorCol;
}

std::string SelectionText(const Session& t) {
  if (!t.hasSelection || t.screen == nullptr)
    return {};
  size_t l0 = 0, l1 = 0;
  int c0 = 0, c1 = 0;
  OrderedSelection(t, l0, c0, l1, c1);
  return t.screen->TextRange(l0, c0, l1, c1);
}

// The buffer line shown at visual row `r`, given the current scrollback
// offset. One place computing this, used by drawing, hit-testing and copy —
// three subtly different versions of it is how off-by-one selections happen.
size_t BufferLineOfRow(const Session& t, int r, int sb) {
  const size_t sbCount = t.screen->ScrollbackCount();
  const int fromHistory = sb - r;
  if (fromHistory > 0)
    return sbCount - static_cast<size_t>(fromHistory);
  return sbCount + static_cast<size_t>(r - sb);
}

// Keyboard, but only while the pointer is over the panel. See the header and
// docs/ui/terminal.md: hover, not focus, is what decides — an invisible
// focus state deciding whether Ctrl+Z undoes a feature or suspends a job is
// the thing that makes an embedded terminal feel dangerous.
TabRequest HandleInput(Session& t) {
  ImGuiIO& io = ImGui::GetIO();
  term::Screen& screen = *t.screen;

  // The tab chords, claimed before anything is forwarded. They cannot be U12
  // commands: the panel swallows the keyboard on purpose, so a binding would
  // never fire in here, and keymap::Context is deliberately the branch
  // DrawToolbar takes (Part/Sketch/Assembly/…) — a Terminal context would
  // break the invariant that the toolbar you see and the keys that work
  // cannot disagree. So they join Ctrl+Shift+C/V as chords the terminal keeps
  // for itself. Shift throughout, because every unshifted form means
  // something to a program: Ctrl+W deletes a word, Ctrl+PageDown scrolls.
  TabRequest request = TabRequest::None;
  if (io.KeyCtrl && io.KeyShift) {
    if (ImGui::IsKeyPressed(ImGuiKey_T, false))
      request = TabRequest::New;
    else if (ImGui::IsKeyPressed(ImGuiKey_W, false))
      request = TabRequest::Close;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
      request = TabRequest::Prev;
    else if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
      request = TabRequest::Next;
  }
  const bool tabChord = request != TabRequest::None;

  for (ImWchar c : io.InputQueueCharacters) {
    if (c == 0)
      continue;
    // Ctrl chords arrive as key presses below, not as characters.
    if (io.KeyCtrl && c < 0x20)
      continue;
    screen.SendChar(static_cast<char32_t>(c));
  }

  const int mods = ModsNow();
  for (const KeyMap& k : kKeys) {
    if (tabChord &&
        (k.imgui == ImGuiKey_PageUp || k.imgui == ImGuiKey_PageDown))
      continue; // this press moved between tabs; the program never sees it
    if (ImGui::IsKeyPressed(k.imgui, true))
      screen.SendKey(k.key, mods);
  }

  // Ctrl+letter: ImGui produces no character for these, and they are the
  // whole point (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+R...).
  if (io.KeyCtrl && !io.KeySuper) {
    for (int i = 0; i < 26; i++) {
      const auto key = static_cast<ImGuiKey>(ImGuiKey_A + i);
      // Ctrl+Shift+C/V are the copy/paste pair below, and Ctrl+Shift+T/W the
      // tab pair above — none of them are control codes.
      if (io.KeyShift && (key == ImGuiKey_C || key == ImGuiKey_V ||
                          key == ImGuiKey_T || key == ImGuiKey_W))
        continue;
      if (ImGui::IsKeyPressed(key, true))
        screen.SendChar(static_cast<char32_t>('a' + i),
                        static_cast<int>(term::Screen::Mod::Ctrl));
    }
  }
  // Alt+letter (readline's word motions) — Alt with a character is Esc-prefix.
  if (io.KeyAlt && !io.KeyCtrl) {
    for (int i = 0; i < 26; i++) {
      const auto key = static_cast<ImGuiKey>(ImGuiKey_A + i);
      if (ImGui::IsKeyPressed(key, true))
        screen.SendChar(static_cast<char32_t>('a' + i),
                        static_cast<int>(term::Screen::Mod::Alt));
    }
  }

  // Ctrl+Shift+V pastes and Ctrl+Shift+C copies, as in every terminal —
  // Ctrl+C has to stay an interrupt and Ctrl+V has to stay literal.
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
    const char* clip = ImGui::GetClipboardText();
    if (clip != nullptr && *clip != '\0')
      screen.SendPaste(clip);
  }
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) {
    const std::string text = SelectionText(t);
    if (!text.empty())
      ImGui::SetClipboardText(text.c_str());
  }

  const std::string keys = screen.TakeOutput();
  if (!keys.empty())
    t.pty.Write(keys.data(), keys.size());

  // Nothing else gets these keys this frame: not a focused text field
  // elsewhere, not the menu's global shortcuts. The app-side gate
  // (TerminalCapturesKeys) covers our own code; this covers ImGui's.
  io.ClearInputKeys();
  return request;
}

// One row of cells. Each glyph is drawn at ITS OWN cell origin rather than
// letting a run of text flow at the font's advance: even a monospace font's
// advance does not land exactly on our cell width once it is rasterised at a
// scaled size, and the drift is visible as ragged columns within a few
// characters. Backgrounds are still batched into runs, which is where the
// draw calls would otherwise pile up.
void DrawRow(ImDrawList* dl, ImVec2 origin, float cellW, float cellH, int row,
             const term::Cell* cells, int cols, uint32_t defaultBg) {
  const float y = origin.y + static_cast<float>(row) * cellH;

  // Background runs first, so a glyph is never painted over.
  int runStart = -1;
  uint32_t runBg = 0;
  const auto flushBg = [&](int endCol) {
    if (runStart < 0)
      return;
    dl->AddRectFilled(
        ImVec2(origin.x + static_cast<float>(runStart) * cellW, y),
        ImVec2(origin.x + static_cast<float>(endCol) * cellW, y + cellH),
        ToImCol(runBg));
    runStart = -1;
  };
  for (int c = 0; c < cols; c++) {
    uint32_t bg = cells[c].bg;
    if ((cells[c].attrs & term::kReverse) != 0)
      bg = cells[c].fg;
    if (bg == defaultBg) {
      flushBg(c);
      continue;
    }
    if (runStart < 0) {
      runStart = c;
      runBg = bg;
    } else if (bg != runBg) {
      flushBg(c);
      runStart = c;
      runBg = bg;
    }
  }
  flushBg(cols);

  char utf8[8];
  for (int c = 0; c < cols; c++) {
    const term::Cell& cell = cells[c];
    if (cell.width == 0 || cell.ch == 0 || cell.ch == U' ')
      continue;
    uint32_t fg = cell.fg;
    if ((cell.attrs & term::kReverse) != 0)
      fg = cell.bg;
    const char32_t ch = cell.ch;
    int n = 0;
    if (ch < 0x80) {
      utf8[n++] = static_cast<char>(ch);
    } else if (ch < 0x800) {
      utf8[n++] = static_cast<char>(0xC0 | (ch >> 6));
      utf8[n++] = static_cast<char>(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
      utf8[n++] = static_cast<char>(0xE0 | (ch >> 12));
      utf8[n++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (ch & 0x3F));
    } else {
      utf8[n++] = static_cast<char>(0xF0 | (ch >> 18));
      utf8[n++] = static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
      utf8[n++] = static_cast<char>(0x80 | (ch & 0x3F));
    }
    utf8[n] = '\0';
    dl->AddText(ImVec2(origin.x + static_cast<float>(c) * cellW, y),
                ToImCol(fg), utf8, utf8 + n);
    if ((cell.attrs & term::kUnderline) != 0) {
      dl->AddLine(
          ImVec2(origin.x + static_cast<float>(c) * cellW, y + cellH - 1.0f),
          ImVec2(origin.x + static_cast<float>(c + 1) * cellW,
                 y + cellH - 1.0f),
          ToImCol(fg));
    }
  }
}

// Appends a session and makes it the visible one. The id is the panel's
// counter, never the vector index: ImGui keys a tab's state — which one is
// selected, where it sits after a reorder — off the id in its label, so
// reusing an index hands a new session the previous occupant's state.
Session& NewSession(AppState& state, TerminalState& panel) {
  panel.sessions.push_back(std::make_unique<Session>());
  Session& s = *panel.sessions.back();
  s.id = panel.nextId++;
  panel.active = static_cast<int>(panel.sessions.size()) - 1;
  panel.pendingSelect = s.id;
  StartShell(state, s);
  return s;
}

// Moves the selection, telling the tab bar as well as ourselves. Assigning
// `active` alone does nothing that lasts a frame — see TerminalState.
void SelectByIndex(TerminalState& panel, int i) {
  if (i < 0 || i >= static_cast<int>(panel.sessions.size()))
    return;
  panel.active = i;
  panel.pendingSelect = panel.sessions[static_cast<size_t>(i)]->id;
}

// Kills the session at `i` and drops it. Selection lands on the tab that took
// its place, which is what closing the tab you were looking at should feel
// like.
void CloseSession(TerminalState& panel, int i) {
  if (i < 0 || i >= static_cast<int>(panel.sessions.size()))
    return;
  panel.sessions[static_cast<size_t>(i)]->pty.Terminate();
  panel.sessions.erase(panel.sessions.begin() + i);
  if (panel.active >= static_cast<int>(panel.sessions.size()))
    panel.active = static_cast<int>(panel.sessions.size()) - 1;
  if (panel.active < 0)
    panel.active = 0;
  SelectByIndex(panel, panel.active);
}

// What a tab is called: the process actually in front, so it reads `claude`
// while Claude Code has the terminal and `zsh` at the prompt. Deliberately
// not a name the user types — HandleInput calls io.ClearInputKeys() whenever
// the pointer is over the panel, so an InputText in this header could not be
// typed into without a carve-out, and a label that tracks the live process is
// more honest than one typed once and left to go stale.
std::string TabLabel(const Session& s) {
  std::string name = s.foreground.empty() ? s.label : s.foreground;
  if (name.empty())
    name = "shell";
  if (!s.pty.Running())
    name += " (exited)";
  return name;
}

} // namespace

std::string WriteTerminalMcpConfig(int port) { return WriteMcpConfig(port); }

bool TerminalCapturesKeys(const AppState& state) {
  // Computed from the window rect rather than read from a flag set while
  // drawing: the menu bar draws BEFORE this panel, so a cached flag would be
  // one frame stale and the first keystroke after the pointer arrives would
  // still reach the app. Hover, not focus, is the rule the user asked for —
  // it is visible, which invisible focus is not.
  if (!state.settings.terminalPanel || !state.terminal)
    return false;
  const Session* s = Active(*state.terminal);
  if (s == nullptr || !s->pty.Running())
    return false; // no session: the app keeps its shortcuts
  ImGuiWindow* w = ImGui::FindWindowByName(kPanelTerminal);
  if (w == nullptr || !w->WasActive || w->Hidden || w->Collapsed)
    return false;
  return ImGui::IsMouseHoveringRect(
      w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false);
}

// The test hooks act on the VISIBLE session, which is what a test that says
// "the terminal shows X" means by "the terminal".
term::Screen* TerminalScreenOf(AppState& state) {
  if (!state.terminal)
    return nullptr;
  Session* s = Active(*state.terminal);
  return s != nullptr ? s->screen.get() : nullptr;
}

bool TerminalRunning(const AppState& state) {
  if (!state.terminal)
    return false;
  const Session* s = Active(*state.terminal);
  return s != nullptr && s->pty.Running();
}

int TerminalSessionCount(const AppState& state) {
  return state.terminal ? static_cast<int>(state.terminal->sessions.size()) : 0;
}

void NewTerminalTab(AppState& state) { NewSession(state, Ensure(state)); }

void CloseTerminalTab(AppState& state, int index) {
  if (state.terminal)
    CloseSession(*state.terminal, index);
}

void SelectTerminalTab(AppState& state, int index) {
  if (state.terminal)
    SelectByIndex(*state.terminal, index);
}

static const Session* TabAt(const AppState& state, int index) {
  if (!state.terminal || index < 0 ||
      index >= static_cast<int>(state.terminal->sessions.size()))
    return nullptr;
  return state.terminal->sessions[static_cast<size_t>(index)].get();
}

bool TerminalTabRunning(const AppState& state, int index) {
  const Session* s = TabAt(state, index);
  return s != nullptr && s->pty.Running();
}

int TerminalActiveTab(const AppState& state) {
  return state.terminal ? state.terminal->active : -1;
}

const term::Screen* TerminalTabScreen(const AppState& state, int index) {
  const Session* s = TabAt(state, index);
  return s != nullptr ? s->screen.get() : nullptr;
}

void StartTerminalCommand(AppState& state, const std::string& program,
                          const std::vector<std::string>& args) {
  TerminalState& panel = Ensure(state);
  if (panel.sessions.empty()) {
    panel.sessions.push_back(std::make_unique<Session>());
    panel.sessions.back()->id = panel.nextId++;
    panel.active = 0;
  }
  term::PtySession::Options opts;
  opts.program = program;
  opts.args = args;
  StartSession(state, *Active(panel), opts, program);
}

void ShutdownTerminal(AppState& state) {
  if (!state.terminal)
    return;
  for (std::unique_ptr<Session>& s : state.terminal->sessions)
    s->pty.Terminate();
  // ...and FORGET the sessions, not just kill them. `screen` outlives the
  // process on purpose so the scrollback stays readable, which is right while
  // the panel is open and wrong the moment it closes: turning the preference
  // off and on again used to produce a panel that could never start a shell,
  // dead until the app restarted — the same one-shot trap as the chooser this
  // panel replaced (2eda31d).
  //
  // The other half of that fix was a per-session `everStarted` latch, removed
  // 2026-08-25: nothing had read it since, so it was state that two comments
  // still described as load-bearing. Forgetting the sessions is what carries
  // the behaviour now, and tests/test_actions.cpp pins it by session count.
  //
  // The preference going off already "ends the session rather than hiding
  // it" (below); ending it means the next open is a fresh one.
  state.terminal.reset();
}

void DrawTerminalPanel(AppState& state) {
  if (!state.settings.terminalPanel) {
    // Turning the preference off ends the sessions rather than hiding them: a
    // process kept alive behind a closed panel is a process nobody can see.
    if (state.terminal && !state.terminal->sessions.empty())
      ShutdownTerminal(state);
    return;
  }

  TerminalState& panel = Ensure(state);

  // EVERY session is drained, not just the visible one. A background tab
  // nobody reads fills its pty buffer (~64 KB) and then the child BLOCKS
  // mid-write — a build left running in tab 2 that appears to hang, with
  // nothing on screen to explain it. Poll() is a non-blocking read, so an
  // idle tab costs a syscall.
  for (std::unique_ptr<Session>& s : panel.sessions) {
    if (s->screen != nullptr)
      PumpSession(*s);
  }

  if (!ImGui::Begin(kPanelTerminal, nullptr,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return; // already pumped above, so collapsing cannot starve anything
  }

  // Opening the panel opens a shell. There WAS a chooser here — "Claude Code"
  // or "Shell" — and it was a one-shot menu: once you picked, the only way
  // back to it was restarting the app. That is a bad trade for a decision
  // that is really just "what do I type first", so the panel now answers it
  // the way a terminal does and leaves the rest to the prompt.
  if (panel.sessions.empty())
    NewSession(state, panel);

  // --- tabs -------------------------------------------------------------
  // The bar appears only once there is more than one session. With a single
  // shell — which is the usual state — a one-tab bar under the panel's own
  // dock tab is two rows of tab chrome saying the same thing, above a
  // terminal that wanted the height.
  //
  // Requests are collected and applied AFTER the bar, never inside it:
  // erasing from `sessions` while ImGui is walking the tabs would leave it
  // holding a pointer into a moved vector.
  TabRequest request = TabRequest::None;
  int closeIndex = -1;
  if (panel.sessions.size() > 1) {
    // ResizeDown, not Scroll: this panel is narrow by nature, and scrolling
    // tabs behind arrows hides the one you want while shrinking them still
    // shows it.
    if (ImGui::BeginTabBar("###term_tabs",
                           ImGuiTabBarFlags_Reorderable |
                               ImGuiTabBarFlags_AutoSelectNewTabs |
                               ImGuiTabBarFlags_FittingPolicyResizeDown)) {
      for (size_t i = 0; i < panel.sessions.size(); i++) {
        Session& s = *panel.sessions[i];
        // The id, not the index — see NewSession.
        const std::string label =
            TabLabel(s) + "###term_tab_" + std::to_string(s.id);
        ImGuiTabItemFlags flags = ImGuiTabItemFlags_None;
        if (panel.pendingSelect == s.id)
          flags |= ImGuiTabItemFlags_SetSelected;
        bool open = true;
        if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
          panel.active = static_cast<int>(i);
          ImGui::EndTabItem();
        }
        if (!open)
          closeIndex = static_cast<int>(i);
      }
      ImGui::EndTabBar();
    }
  }
  panel.pendingSelect = 0; // consumed; the bar owns the selection again

  Session* activePtr = Active(panel);
  if (activePtr == nullptr) { // every tab closed in the same frame
    ImGui::End();
    return;
  }
  Session& t = *activePtr;
  t.capturing = false;

  // --- header -----------------------------------------------------------
  // The tab already names the process in front, so the header does not: two
  // labels saying `bash` a row apart is noise, and the one on the tab is the
  // one that stays true when you switch away.
  if (ImGui::SmallButton("+###term_newtab"))
    request = TabRequest::New;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Another shell in this project (Ctrl+Shift+T). "
                      "Ctrl+Shift+W closes one.");
  ImGui::SameLine();

  if (t.pty.Running()) {
    if (ImGui::SmallButton("Claude Code###term_claude")) {
      const std::string cmd = ClaudeCommand(state) + "\n";
      t.pty.Write(cmd.data(), cmd.size());
    }
    if (ImGui::IsItemHovered()) {
      const int port = state.remotePort;
      if (port > 0)
        ImGui::SetTooltip("Types `claude` into this shell, wired to THIS "
                          "window (port %d) — so it edits the model you are "
                          "looking at.",
                          port);
      else
        ImGui::SetTooltip("Types `claude` into this shell, wired to THIS "
                          "window (opens a remote port for it).");
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop###term_stop"))
      t.pty.Terminate();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Kills whatever this tab is running — SIGHUP, then "
                        "SIGKILL. For a program that has stopped answering "
                        "Ctrl+C; the scrollback stays readable afterwards.");
  } else {
    // The status code, which the tab has no room for.
    ImGui::TextDisabled("exited (%d)", t.exitStatus);
    ImGui::SameLine();
    if (ImGui::SmallButton("New shell###term_shell"))
      StartShell(state, t);
    if (!t.startError.empty())
      ImGui::TextWrapped("Could not start: %s", t.startError.c_str());
  }

  if (t.screen == nullptr) {
    ImGui::End();
    return;
  }

  ImFont* mono = MonoFont();
  if (mono == nullptr) {
    ImGui::TextWrapped("The monospace font is missing, so a terminal cannot "
                       "be drawn legibly here.");
    ImGui::End();
    return;
  }

  ImGui::PushFont(mono);
  const float cellW = ImGui::CalcTextSize("M").x;
  const float cellH = ImGui::GetTextLineHeight();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const int cols =
      std::max(20, static_cast<int>(avail.x / std::max(1.0f, cellW)));
  const int rows =
      std::max(4, static_cast<int>(avail.y / std::max(1.0f, cellH)));
  if (cols != t.cols || rows != t.rows) {
    t.cols = cols;
    t.rows = rows;
    t.screen->Resize(rows, cols);
    t.pty.Resize(rows, cols); // the program only reflows if it is TOLD
  }

  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // The hit area goes FIRST so it owns the mouse for the whole grid; the
  // glyphs are drawn over it afterwards (the button paints nothing).
  ImGui::InvisibleButton("###term_grid", avail,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);
  const bool gridHovered = ImGui::IsItemHovered();
  ImGui::SetCursorScreenPos(origin);

  dl->AddRectFilled(origin, ImVec2(origin.x + avail.x, origin.y + avail.y),
                    ToImCol(t.screen->DefaultBg()));

  // Scrolled-back history first, then the live screen under it.
  std::vector<term::Cell> line(static_cast<size_t>(cols));
  const int sb = std::min<int>(t.scrollBack,
                               static_cast<int>(t.screen->ScrollbackCount()));
  for (int r = 0; r < rows; r++) {
    const int fromHistory = sb - r;
    if (fromHistory > 0) {
      const size_t idx =
          t.screen->ScrollbackCount() - static_cast<size_t>(fromHistory);
      const std::vector<term::Cell>& hist = t.screen->ScrollbackLine(idx);
      std::fill(line.begin(), line.end(), term::Cell{});
      for (size_t c = 0; c < hist.size() && c < line.size(); c++)
        line[c] = hist[c];
      DrawRow(dl, origin, cellW, cellH, r, line.data(), cols,
              t.screen->DefaultBg());
      continue;
    }
    const int screenRow = r - sb;
    for (int c = 0; c < cols; c++)
      line[static_cast<size_t>(c)] = t.screen->At(screenRow, c);
    DrawRow(dl, origin, cellW, cellH, r, line.data(), cols,
            t.screen->DefaultBg());
  }

  // --- selection ---------------------------------------------------------
  // A cell address from the pointer, clamped into the grid so a drag that
  // leaves the panel still selects to the edge rather than stopping dead.
  const auto cellUnderMouse = [&](size_t& line, int& col) {
    const ImVec2 m = ImGui::GetIO().MousePos;
    int r = static_cast<int>((m.y - origin.y) / std::max(1.0f, cellH));
    int c = static_cast<int>((m.x - origin.x) / std::max(1.0f, cellW) + 0.5f);
    r = std::clamp(r, 0, rows - 1);
    c = std::clamp(c, 0, cols);
    line = BufferLineOfRow(t, r, sb);
    col = c;
  };

  // Middle-click pastes what is selected here, falling back to the system
  // clipboard when nothing is. NOT the X11 PRIMARY selection: GLFW exposes no
  // API for it, and for the case this is actually used for — select a path in
  // the terminal, middle-click it back onto the command line — the two are
  // indistinguishable.
  //
  // It has to run BEFORE the selection branches below. A middle press
  // activates the invisible button too, and IsItemActivated() is the branch
  // that CLEARS the selection — so reading the selection afterwards would
  // find it already gone and paste the system clipboard over the thing the
  // user middle-clicked to paste.
  if (gridHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
    std::string text = SelectionText(t);
    if (text.empty()) {
      const char* clip = ImGui::GetClipboardText();
      if (clip != nullptr)
        text = clip;
    }
    if (!text.empty()) {
      t.screen->SendPaste(text); // bracketed, if the program asked for it
      const std::string bytes = t.screen->TakeOutput();
      if (!bytes.empty())
        t.pty.Write(bytes.data(), bytes.size());
    }
  }

  // Gated on the LEFT button specifically, for the reason above: activation
  // alone would let a middle-click start a selection and destroy the one it
  // was about to paste.
  if (ImGui::IsItemActivated() &&
      ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
      !ImGui::GetIO().KeyShift) {
    cellUnderMouse(t.selAnchorLine, t.selAnchorCol);
    t.selHeadLine = t.selAnchorLine;
    t.selHeadCol = t.selAnchorCol;
    t.selecting = true;
    t.hasSelection = false; // a click with no drag clears, as a terminal does
  }
  if (t.selecting && ImGui::IsItemActive()) {
    cellUnderMouse(t.selHeadLine, t.selHeadCol);
    t.hasSelection =
        t.selHeadLine != t.selAnchorLine || t.selHeadCol != t.selAnchorCol;
  }
  if (ImGui::IsItemDeactivated())
    t.selecting = false;

  // Double-click takes the word under the pointer — usually a path or a flag,
  // which is what anyone is double-clicking a terminal for.
  if (gridHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
    size_t line = 0;
    int col = 0;
    cellUnderMouse(line, col);
    int begin = 0, end = 0;
    t.screen->WordAt(line, std::min(col, cols - 1), begin, end);
    t.selecting = false;
    t.hasSelection = end > begin;
    t.selAnchorLine = t.selHeadLine = line;
    t.selAnchorCol = begin;
    t.selHeadCol = end;
  }

  if (t.hasSelection) {
    size_t l0 = 0, l1 = 0;
    int c0 = 0, c1 = 0;
    OrderedSelection(t, l0, c0, l1, c1);
    // Translucent, drawn over the glyphs: a solid highlight would mean
    // re-rendering the text in an inverted colour, and the text is the thing
    // you are selecting to read.
    const ImU32 tint = IM_COL32(110, 145, 210, 80);
    for (int r = 0; r < rows; r++) {
      const size_t bufLine = BufferLineOfRow(t, r, sb);
      if (bufLine < l0 || bufLine > l1)
        continue;
      const int from = (bufLine == l0) ? c0 : 0;
      const int to = (bufLine == l1) ? c1 : cols;
      if (to <= from)
        continue;
      dl->AddRectFilled(ImVec2(origin.x + static_cast<float>(from) * cellW,
                               origin.y + static_cast<float>(r) * cellH),
                        ImVec2(origin.x + static_cast<float>(to) * cellW,
                               origin.y + static_cast<float>(r + 1) * cellH),
                        tint);
    }
  }

  // The cursor: solid while we have the keyboard, hollow when we do not, so
  // "will my typing go here?" is answerable at a glance.
  const term::CursorPos cur = t.screen->Cursor();
  t.capturing = TerminalCapturesKeys(state);
  if (t.screen->CursorVisible() && sb == 0 && cur.row >= 0 && cur.row < rows) {
    const ImVec2 a(origin.x + static_cast<float>(cur.col) * cellW,
                   origin.y + static_cast<float>(cur.row) * cellH);
    const ImVec2 b(a.x + cellW, a.y + cellH);
    if (t.capturing)
      dl->AddRectFilled(a, b, ToImCol(t.screen->DefaultFg()));
    else
      dl->AddRect(a, b, ToImCol(t.screen->DefaultFg()));
  }
  ImGui::PopFont();

  if (gridHovered) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      t.scrollBack =
          std::clamp(t.scrollBack + static_cast<int>(wheel * 3.0f), 0,
                     static_cast<int>(t.screen->ScrollbackCount()));
    }
  }

  if (t.capturing) {
    const TabRequest fromKeys = HandleInput(t);
    if (fromKeys != TabRequest::None)
      request = fromKeys;
  }

  ImGui::End();

  // --- apply the tab requests, with the bar and the grid both behind us ---
  const int count = static_cast<int>(panel.sessions.size());
  switch (request) {
  case TabRequest::New:
    NewSession(state, panel);
    break;
  case TabRequest::Close:
    if (count > 1)
      closeIndex = panel.active;
    break;
  case TabRequest::Prev:
    SelectByIndex(panel, (panel.active + count - 1) % count);
    break;
  case TabRequest::Next:
    SelectByIndex(panel, (panel.active + 1) % count);
    break;
  case TabRequest::None:
    break;
  }
  if (closeIndex >= 0)
    CloseSession(panel, closeIndex);
}

} // namespace ui

#else // !HAVE_TERMINAL

namespace ui {
void DrawTerminalPanel(AppState&) {}
bool TerminalCapturesKeys(const AppState&) { return false; }
void ShutdownTerminal(AppState&) {}
} // namespace ui

#endif
