#include "ui/TerminalPanel.h"

#ifdef HAVE_TERMINAL

#include "common/Logger.h"
#include "common/PtySession.h"
#include "common/RuntimePaths.h"
#include "common/TerminalScreen.h"
#include "ui/Theme.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, for the dock-on-first-use path

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

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

// Session + screen + the panel's own view state. Lives behind a shared_ptr in
// AppState so the header stays free of libvterm.
struct TerminalState {
  term::PtySession pty;
  std::unique_ptr<term::Screen> screen;
  int rows = 24;
  int cols = 80;
  std::string label;      // what is running, for the header
  std::string startError; // why the last start failed, if it did
  int exitStatus = -1;    // of the last session
  bool everStarted = false;
  int scrollBack = 0;     // lines scrolled up into history; 0 = live
  bool capturing = false; // the pointer is over us and a session is running
};

namespace {

TerminalState& Ensure(AppState& state) {
  if (!state.terminal)
    state.terminal = std::make_shared<TerminalState>();
  return *state.terminal;
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

void StartSession(AppState& state, TerminalState& t,
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
  t.everStarted = true;
  t.label = label;
}

// tools/designer_mcp.py, if this build can find it: next to the executable
// in a dev tree, or in the installed data directory. Empty when neither has
// it — the session still starts, just without MCP wired up.
std::string FindMcpServer() {
  std::error_code ec;
  const std::array<std::filesystem::path, 3> candidates{
      RuntimePaths::DataDir() / "tools" / "designer_mcp.py",
      RuntimePaths::ExecutableDir() / ".." / "tools" / "designer_mcp.py",
      RuntimePaths::ExecutableDir() / "tools" / "designer_mcp.py",
  };
  for (const std::filesystem::path& p : candidates) {
    const std::filesystem::path norm = p.lexically_normal();
    if (std::filesystem::exists(norm, ec))
      return norm.string();
  }
  return {};
}

// Claude Code, bound to THIS instance. That binding is the entire reason for
// running it in here rather than in a terminal beside the app: cwd is the
// project, DESIGNER_PORT names this window's remote port (opening one if the
// app was started without --remote), and the generated MCP config points the
// `designer` tools at that port. Nothing to wire up by hand, and no chance of
// attaching to the other instance you have open.
void StartClaude(AppState& state, TerminalState& t) {
  term::PtySession::Options opts;
  opts.program = "claude";

  int port = state.remotePort;
  if (port <= 0 && state.ensureRemotePort)
    port = state.ensureRemotePort();
  if (port > 0)
    opts.env.emplace_back("DESIGNER_PORT", std::to_string(port));

  const std::string server = FindMcpServer();
  if (port > 0 && !server.empty()) {
    // A config file rather than `claude mcp add`: this is per-session, it
    // must not edit the user's global MCP registration, and a project
    // .mcp.json belongs to the project, not to us.
    const std::filesystem::path cfg =
        std::filesystem::temp_directory_path() /
        ("designer-mcp-" + std::to_string(port) + ".json");
    std::string json = "{\"mcpServers\":{\"designer\":{\"command\":"
                       "\"python3\",\"args\":[\"" +
                       server + "\",\"--port\",\"" + std::to_string(port) +
                       "\"]}}}";
    std::error_code ec;
    FILE* f = std::fopen(cfg.string().c_str(), "wb");
    if (f != nullptr) {
      std::fwrite(json.data(), 1, json.size(), f);
      std::fclose(f);
      opts.args.push_back("--mcp-config");
      opts.args.push_back(cfg.string());
    }
    (void)ec;
  }
  StartSession(state, t, opts, "Claude Code");
}

void StartShell(AppState& state, TerminalState& t) {
  term::PtySession::Options opts; // empty program = $SHELL
  const char* shell = std::getenv("SHELL");
  StartSession(state, t, opts,
               shell != nullptr && *shell != '\0'
                   ? std::filesystem::path(shell).filename().string()
                   : "shell");
}

// Reads what the child wrote and hands the terminal's own replies back to it.
void PumpSession(TerminalState& t) {
  if (t.screen == nullptr)
    return;
  const std::string out = t.pty.Poll();
  if (!out.empty()) {
    t.screen->Write(out.data(), out.size());
    t.scrollBack = 0; // new output snaps back to live, as a terminal does
  }
  const std::string reply = t.screen->TakeOutput();
  if (!reply.empty())
    t.pty.Write(reply.data(), reply.size());
  if (!t.pty.Running() && t.exitStatus < 0)
    t.exitStatus = t.pty.ExitStatus();
}

// Keyboard, but only while the pointer is over the panel. See the header and
// docs/ui/terminal-panel.md: hover, not focus, is what decides — an invisible
// focus state deciding whether Ctrl+Z undoes a feature or suspends a job is
// the thing that makes an embedded terminal feel dangerous.
void HandleInput(TerminalState& t) {
  ImGuiIO& io = ImGui::GetIO();
  term::Screen& screen = *t.screen;

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
    if (ImGui::IsKeyPressed(k.imgui, true))
      screen.SendKey(k.key, mods);
  }

  // Ctrl+letter: ImGui produces no character for these, and they are the
  // whole point (Ctrl+C, Ctrl+D, Ctrl+Z, Ctrl+R...).
  if (io.KeyCtrl && !io.KeySuper) {
    for (int i = 0; i < 26; i++) {
      const auto key = static_cast<ImGuiKey>(ImGuiKey_A + i);
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

  // Ctrl+Shift+V pastes, as in every terminal — Ctrl+V has to stay literal.
  if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
    const char* clip = ImGui::GetClipboardText();
    if (clip != nullptr && *clip != '\0')
      screen.SendPaste(clip);
  }

  const std::string keys = screen.TakeOutput();
  if (!keys.empty())
    t.pty.Write(keys.data(), keys.size());

  // Nothing else gets these keys this frame: not a focused text field
  // elsewhere, not the menu's global shortcuts. The app-side gate
  // (TerminalCapturesKeys) covers our own code; this covers ImGui's.
  io.ClearInputKeys();
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

void DrawEmptyState(AppState& state, TerminalState& t) {
  ImGui::Spacing();
  ImGui::TextWrapped(
      "Run Claude Code, a shell, or git beside the model. Typing goes here "
      "only while the pointer is over this panel.");
  ImGui::Spacing();
  ImGui::TextDisabled("%s", StartDirectory(state).c_str());
  ImGui::Spacing();
  if (ImGui::Button("Claude Code###term_claude"))
    StartClaude(state, t);
  ImGui::SameLine();
  if (ImGui::Button("Shell###term_shell"))
    StartShell(state, t);
  if (!t.startError.empty()) {
    ImGui::Spacing();
    ImGui::TextWrapped("Could not start: %s", t.startError.c_str());
  }
}

} // namespace

bool TerminalCapturesKeys(const AppState& state) {
  // Computed from the window rect rather than read from a flag set while
  // drawing: the menu bar draws BEFORE this panel, so a cached flag would be
  // one frame stale and the first keystroke after the pointer arrives would
  // still reach the app. Hover, not focus, is the rule the user asked for —
  // it is visible, which invisible focus is not.
  if (!state.settings.terminalPanel || !state.terminal)
    return false;
  if (!state.terminal->pty.Running())
    return false; // no session: the app keeps its shortcuts
  ImGuiWindow* w = ImGui::FindWindowByName("Terminal###TerminalPanel");
  if (w == nullptr || !w->WasActive || w->Hidden || w->Collapsed)
    return false;
  return ImGui::IsMouseHoveringRect(
      w->Pos, ImVec2(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y), false);
}

term::Screen* TerminalScreenOf(AppState& state) {
  return state.terminal ? state.terminal->screen.get() : nullptr;
}

bool TerminalRunning(const AppState& state) {
  return state.terminal && state.terminal->pty.Running();
}

void StartTerminalCommand(AppState& state, const std::string& program,
                          const std::vector<std::string>& args) {
  TerminalState& t = Ensure(state);
  term::PtySession::Options opts;
  opts.program = program;
  opts.args = args;
  StartSession(state, t, opts, program);
}

void ShutdownTerminal(AppState& state) {
  if (state.terminal)
    state.terminal->pty.Terminate();
}

void DrawTerminalPanel(AppState& state) {
  if (!state.settings.terminalPanel) {
    // Turning the preference off ends the session rather than hiding it: a
    // process kept alive behind a closed panel is a process nobody can see.
    if (state.terminal && state.terminal->pty.Running())
      ShutdownTerminal(state);
    return;
  }

  TerminalState& t = Ensure(state);
  t.capturing = false;

  if (!ImGui::Begin("Terminal###TerminalPanel", nullptr,
                    ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    if (t.screen != nullptr)
      PumpSession(t); // keep draining while collapsed, or output piles up
    return;
  }

  // --- header -----------------------------------------------------------
  if (t.pty.Running()) {
    ImGui::TextDisabled("%s", t.label.c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("Stop###term_stop"))
      t.pty.Terminate();
  } else if (t.everStarted) {
    ImGui::TextDisabled("%s exited (%d)", t.label.c_str(), t.exitStatus);
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart###term_restart")) {
      if (t.label == "Claude Code")
        StartClaude(state, t);
      else
        StartShell(state, t);
    }
  }

  if (t.screen == nullptr) {
    DrawEmptyState(state, t);
    ImGui::End();
    return;
  }

  PumpSession(t);

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

  // The cursor: solid while we have the keyboard, hollow when we do not, so
  // "will my typing go here?" is answerable at a glance.
  const term::CursorPos cur = t.screen->Cursor();
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
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

  // An invisible button over the grid so the panel takes clicks (focus, and
  // later selection) without ImGui thinking the window is empty.
  ImGui::InvisibleButton("###term_grid", avail,
                         ImGuiButtonFlags_MouseButtonLeft |
                             ImGuiButtonFlags_MouseButtonMiddle);

  if (hovered) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel != 0.0f) {
      t.scrollBack =
          std::clamp(t.scrollBack + static_cast<int>(wheel * 3.0f), 0,
                     static_cast<int>(t.screen->ScrollbackCount()));
    }
  }

  if (t.capturing)
    HandleInput(t);

  ImGui::End();
}

} // namespace ui

#else // !HAVE_TERMINAL

namespace ui {
void DrawTerminalPanel(AppState&) {}
bool TerminalCapturesKeys(const AppState&) { return false; }
void ShutdownTerminal(AppState&) {}
} // namespace ui

#endif
