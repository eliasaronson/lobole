#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// A terminal screen: bytes from a PTY in, a character grid out
// (docs/ui/terminal-panel.md).
//
// Wraps libvterm — the escape-sequence state machine neovim's `:terminal`
// uses — because hosting Claude Code needs a real VT: it renders INLINE (no
// alternate screen) by addressing columns and erasing lines, in 24-bit
// colour, and reflows on resize. A widget that appends text cannot draw that.
//
// Deliberately free of ImGui and of the platform: this is the half that can
// be tested headlessly, by writing known bytes and reading back the grid.
namespace term {

// Attribute bits, kept as one byte rather than five bools so a cell stays
// small — a 200x50 screen plus scrollback is a lot of cells.
enum CellAttr : uint8_t {
  kBold = 1 << 0,
  kItalic = 1 << 1,
  kUnderline = 1 << 2,
  kReverse = 1 << 3,
  kStrike = 1 << 4,
};

struct Cell {
  char32_t ch = 0;   // 0 = blank
  uint32_t fg = 0;   // 0xRRGGBB, already resolved from the palette
  uint32_t bg = 0;   // 0xRRGGBB
  uint8_t attrs = 0; // CellAttr bits
  uint8_t width = 1; // 2 for a double-width glyph; 0 for its trailing half
};

struct CursorPos {
  int row = 0;
  int col = 0;
};

class Screen {
public:
  Screen(int rows, int cols);
  ~Screen();
  Screen(const Screen&) = delete;
  Screen& operator=(const Screen&) = delete;

  // The colours a cell has when the program has not set any. Setting them to
  // the panel's own theme is what keeps the terminal from being a black
  // rectangle pasted into the app — and it lets the renderer skip drawing a
  // background for every default cell.
  void SetDefaultColors(uint32_t fg, uint32_t bg);
  [[nodiscard]] uint32_t DefaultBg() const { return m_defaultBg; }
  [[nodiscard]] uint32_t DefaultFg() const { return m_defaultFg; }

  // Feed output from the process.
  void Write(const char* bytes, size_t n);
  void Write(std::string_view s) { Write(s.data(), s.size()); }

  // Rows/cols follow the panel; the caller must also resize the PTY, or the
  // program keeps drawing at the old width.
  void Resize(int rows, int cols);
  [[nodiscard]] int Rows() const { return m_rows; }
  [[nodiscard]] int Cols() const { return m_cols; }

  // Visible screen, row 0 at the top.
  [[nodiscard]] Cell At(int row, int col) const;
  // The row as UTF-8 with trailing blanks trimmed. For tests and for copy.
  [[nodiscard]] std::string Line(int row) const;

  // Scrollback, oldest first. Lines are pushed off the top by the program.
  [[nodiscard]] size_t ScrollbackCount() const { return m_scrollback.size(); }
  [[nodiscard]] const std::vector<Cell>& ScrollbackLine(size_t i) const {
    return m_scrollback[i];
  }
  [[nodiscard]] std::string ScrollbackText(size_t i) const;

  // --- one address space over scrollback + screen -------------------------
  // Selection has to survive scrolling and new output, so it is expressed in
  // BUFFER lines: 0 is the oldest scrollback line, and the visible screen is
  // the last Rows() of them. Keeping this in the screen rather than in the
  // panel is what makes copy testable without a display.
  [[nodiscard]] size_t TotalLines() const {
    return m_scrollback.size() + static_cast<size_t>(m_rows);
  }
  [[nodiscard]] Cell CellAtLine(size_t line, int col) const;
  // Inclusive of `startCol`, exclusive of `endCol` on the last line; trailing
  // blanks are trimmed per line, and lines are joined with '\n' — what a
  // terminal puts on the clipboard.
  [[nodiscard]] std::string TextRange(size_t startLine, int startCol,
                                      size_t endLine, int endCol) const;
  // The word around (line, col), as [begin, end) columns. Empty (begin ==
  // end) over whitespace. For double-click selection.
  void WordAt(size_t line, int col, int& begin, int& end) const;

  [[nodiscard]] CursorPos Cursor() const { return m_cursor; }
  [[nodiscard]] bool CursorVisible() const { return m_cursorVisible; }
  [[nodiscard]] const std::string& Title() const { return m_title; }

  // --- the clipboard, one direction only ---------------------------------
  // A program inside the terminal cannot reach the system clipboard itself;
  // it asks us to, by writing OSC 52. Claude Code's own copy is exactly this,
  // and dropping the sequence made every copy inside it a silent no-op.
  //
  // Pulled rather than pushed, like TakeOutput(): the caller drains this once
  // a frame and hands it to the platform. That keeps this class ignorant of
  // ImGui — which is the whole reason a clipboard write can be asserted in a
  // headless test.
  //
  // Nothing is offered in the other direction. OSC 52 also has a *read* form,
  // and answering it would let anything running in the panel exfiltrate
  // whatever the user last copied. See Screen::Screen for the refusal.
  [[nodiscard]] std::optional<std::string> TakeClipboardWrite();

  // --- input ------------------------------------------------------------
  // Key encoding is libvterm's job too, so the panel never hand-writes an
  // escape sequence: it says "Up with Ctrl" and the right bytes come out of
  // TakeOutput(), in whatever mode the program has put the terminal in.
  enum class Mod : int { None = 0, Shift = 1, Alt = 2, Ctrl = 4 };
  enum class Key {
    Enter,
    Tab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Insert,
    Delete,
    KeypadEnter,
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
  };
  void SendChar(char32_t c, int mods = 0);
  void SendKey(Key k, int mods = 0);
  // Wrapped in the bracketed-paste markers when the program asked for them,
  // so an editor pastes instead of interpreting every character as a command.
  void SendPaste(std::string_view text);

  // Bytes the terminal wants to send to the process: key presses, and the
  // occasional automatic reply. The caller drains this into the PTY.
  [[nodiscard]] std::string TakeOutput();

private:
  struct Impl;
  Impl* m_impl = nullptr; // libvterm state, kept out of this header
  int m_rows = 0;
  int m_cols = 0;
  CursorPos m_cursor;
  bool m_cursorVisible = true;
  std::string m_title;
  std::string m_pendingOutput;
  uint32_t m_defaultFg = 0xD5D8DEu;
  uint32_t m_defaultBg = 0x0F1115u;
  std::vector<std::vector<Cell>> m_scrollback;

  // OSC 52. libvterm base64-decodes into m_clipboardBuffer and hands it back
  // in fragments; m_clipboardPending assembles them and m_clipboardReady
  // holds a completed write until someone takes it.
  std::vector<char> m_clipboardBuffer;
  std::string m_clipboardPending;
  std::optional<std::string> m_clipboardReady;

  friend struct ScreenCallbacks;
};

// How many scrollback lines a Screen keeps before dropping the oldest.
inline constexpr size_t kScrollbackLimit = 10000;

// How much decoded text libvterm hands back per OSC 52 fragment. Only the
// chunk size — a longer payload arrives in several fragments.
inline constexpr size_t kClipboardChunk = 64 * 1024;

// The most a program may put on the clipboard in one write. Past this the
// write is truncated rather than honoured whole: a clipboard is a thing a
// person is about to paste somewhere, and a megabyte of it is already far
// past anything anyone means to.
inline constexpr size_t kClipboardWriteLimit = 1024 * 1024;

} // namespace term
