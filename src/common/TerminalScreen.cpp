#include "common/TerminalScreen.h"

#include <vterm.h>

#include <algorithm>
#include <cstring>

namespace term {

namespace {

uint32_t PackColor(VTermScreen* screen, VTermColor c) {
  // Indexed colours must be resolved against the palette first, or every
  // 256-colour program renders black on black.
  vterm_screen_convert_color_to_rgb(screen, &c);
  return (static_cast<uint32_t>(c.rgb.red) << 16) |
         (static_cast<uint32_t>(c.rgb.green) << 8) |
         static_cast<uint32_t>(c.rgb.blue);
}

void AppendUtf8(std::string& out, char32_t ch) {
  if (ch < 0x80) {
    out.push_back(static_cast<char>(ch));
  } else if (ch < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
    out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  } else if (ch < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
    out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (ch >> 18)));
    out.push_back(static_cast<char>(0x80 | ((ch >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
  }
}

VTermModifier ToVTermMod(int mods) {
  int m = VTERM_MOD_NONE;
  if (mods & static_cast<int>(Screen::Mod::Shift))
    m |= VTERM_MOD_SHIFT;
  if (mods & static_cast<int>(Screen::Mod::Alt))
    m |= VTERM_MOD_ALT;
  if (mods & static_cast<int>(Screen::Mod::Ctrl))
    m |= VTERM_MOD_CTRL;
  return static_cast<VTermModifier>(m);
}

VTermKey ToVTermKey(Screen::Key k) {
  switch (k) {
  case Screen::Key::Enter:
    return VTERM_KEY_ENTER;
  case Screen::Key::Tab:
    return VTERM_KEY_TAB;
  case Screen::Key::Backspace:
    return VTERM_KEY_BACKSPACE;
  case Screen::Key::Escape:
    return VTERM_KEY_ESCAPE;
  case Screen::Key::Up:
    return VTERM_KEY_UP;
  case Screen::Key::Down:
    return VTERM_KEY_DOWN;
  case Screen::Key::Left:
    return VTERM_KEY_LEFT;
  case Screen::Key::Right:
    return VTERM_KEY_RIGHT;
  case Screen::Key::Home:
    return VTERM_KEY_HOME;
  case Screen::Key::End:
    return VTERM_KEY_END;
  case Screen::Key::PageUp:
    return VTERM_KEY_PAGEUP;
  case Screen::Key::PageDown:
    return VTERM_KEY_PAGEDOWN;
  case Screen::Key::Insert:
    return VTERM_KEY_INS;
  case Screen::Key::Delete:
    return VTERM_KEY_DEL;
  case Screen::Key::KeypadEnter:
    return VTERM_KEY_KP_ENTER;
  default:
    break;
  }
  const int fn = static_cast<int>(k) - static_cast<int>(Screen::Key::F1) + 1;
  return static_cast<VTermKey>(VTERM_KEY_FUNCTION_0 + fn);
}

} // namespace

struct Screen::Impl {
  VTerm* vt = nullptr;
  VTermScreen* screen = nullptr;
};

// The libvterm callbacks, as one befriended type rather than loose functions:
// they write Screen's private state, and a `friend` has to name something.
struct ScreenCallbacks {
  // libvterm hands back a whole line of cells when one scrolls off the top.
  static int PushLine(int cols, const VTermScreenCell* cells, void* user);
  static int MoveCursor(VTermPos pos, VTermPos old, int visible, void* user);
  static int SetTermProp(VTermProp prop, VTermValue* val, void* user);
  static void Output(const char* s, size_t len, void* user);
};

Screen::Screen(int rows, int cols)
    : m_impl(new Impl), m_rows(std::max(1, rows)), m_cols(std::max(1, cols)) {
  m_impl->vt = vterm_new(m_rows, m_cols);
  vterm_set_utf8(m_impl->vt, 1);
  m_impl->screen = vterm_obtain_screen(m_impl->vt);

  static VTermScreenCallbacks cbs = {};
  cbs.sb_pushline = ScreenCallbacks::PushLine;
  cbs.movecursor = ScreenCallbacks::MoveCursor;
  cbs.settermprop = ScreenCallbacks::SetTermProp;
  vterm_screen_set_callbacks(m_impl->screen, &cbs, this);
  vterm_screen_reset(m_impl->screen, 1);
  vterm_output_set_callback(m_impl->vt, ScreenCallbacks::Output, this);
}

Screen::~Screen() {
  if (m_impl != nullptr) {
    if (m_impl->vt != nullptr)
      vterm_free(m_impl->vt);
    delete m_impl;
  }
}

void Screen::Write(const char* bytes, size_t n) {
  vterm_input_write(m_impl->vt, bytes, n);
}

void Screen::Resize(int rows, int cols) {
  rows = std::max(1, rows);
  cols = std::max(1, cols);
  if (rows == m_rows && cols == m_cols)
    return;
  m_rows = rows;
  m_cols = cols;
  vterm_set_size(m_impl->vt, rows, cols);
}

Cell Screen::At(int row, int col) const {
  Cell out;
  if (row < 0 || col < 0 || row >= m_rows || col >= m_cols)
    return out;
  VTermPos pos{row, col};
  VTermScreenCell c;
  if (vterm_screen_get_cell(m_impl->screen, pos, &c) == 0)
    return out;
  out.ch = c.chars[0];
  out.width = static_cast<uint8_t>(c.width);
  out.fg = PackColor(m_impl->screen, c.fg);
  out.bg = PackColor(m_impl->screen, c.bg);
  if (c.attrs.bold)
    out.attrs |= kBold;
  if (c.attrs.italic)
    out.attrs |= kItalic;
  if (c.attrs.underline != 0)
    out.attrs |= kUnderline;
  if (c.attrs.reverse)
    out.attrs |= kReverse;
  if (c.attrs.strike)
    out.attrs |= kStrike;
  return out;
}

std::string Screen::Line(int row) const {
  std::string out;
  for (int c = 0; c < m_cols; c++) {
    const Cell cell = At(row, c);
    if (cell.width == 0)
      continue; // trailing half of a double-width glyph
    AppendUtf8(out, cell.ch == 0 ? U' ' : cell.ch);
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

std::string Screen::ScrollbackText(size_t i) const {
  std::string out;
  if (i >= m_scrollback.size())
    return out;
  for (const Cell& cell : m_scrollback[i]) {
    if (cell.width == 0)
      continue;
    AppendUtf8(out, cell.ch == 0 ? U' ' : cell.ch);
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

void Screen::SendChar(char32_t c, int mods) {
  vterm_keyboard_unichar(m_impl->vt, static_cast<uint32_t>(c),
                         ToVTermMod(mods));
}

void Screen::SendKey(Key k, int mods) {
  vterm_keyboard_key(m_impl->vt, ToVTermKey(k), ToVTermMod(mods));
}

void Screen::SendPaste(std::string_view text) {
  // start/end_paste emit the bracketed-paste markers only when the program
  // enabled them, so this is safe to call unconditionally.
  vterm_keyboard_start_paste(m_impl->vt);
  for (size_t i = 0; i < text.size();) {
    // Decode UTF-8 so a pasted é is one keypress, not two bytes.
    const auto b0 = static_cast<unsigned char>(text[i]);
    char32_t cp = b0;
    size_t len = 1;
    if (b0 >= 0xF0) {
      len = 4;
      cp = b0 & 0x07u;
    } else if (b0 >= 0xE0) {
      len = 3;
      cp = b0 & 0x0Fu;
    } else if (b0 >= 0xC0) {
      len = 2;
      cp = b0 & 0x1Fu;
    }
    if (i + len > text.size())
      len = 1;
    for (size_t k = 1; k < len; k++)
      cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
    if (cp == U'\n' || cp == U'\r')
      vterm_keyboard_key(m_impl->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
    else
      vterm_keyboard_unichar(m_impl->vt, static_cast<uint32_t>(cp),
                             VTERM_MOD_NONE);
    i += len;
  }
  vterm_keyboard_end_paste(m_impl->vt);
}

std::string Screen::TakeOutput() {
  std::string out;
  out.swap(m_pendingOutput);
  return out;
}

int ScreenCallbacks::PushLine(int cols, const VTermScreenCell* cells,
                              void* user) {
  auto* self = static_cast<Screen*>(user);
  std::vector<Cell> line;
  line.reserve(static_cast<size_t>(cols));
  for (int c = 0; c < cols; c++) {
    Cell cell;
    cell.ch = cells[c].chars[0];
    cell.width = static_cast<uint8_t>(cells[c].width);
    // The palette lives in the screen; a scrollback line keeps the colours it
    // had, so scrolling back does not repaint history in today's theme.
    VTermColor fg = cells[c].fg, bg = cells[c].bg;
    cell.fg = (static_cast<uint32_t>(fg.rgb.red) << 16) |
              (static_cast<uint32_t>(fg.rgb.green) << 8) | fg.rgb.blue;
    cell.bg = (static_cast<uint32_t>(bg.rgb.red) << 16) |
              (static_cast<uint32_t>(bg.rgb.green) << 8) | bg.rgb.blue;
    if (cells[c].attrs.bold)
      cell.attrs |= kBold;
    if (cells[c].attrs.reverse)
      cell.attrs |= kReverse;
    line.push_back(cell);
  }
  self->m_scrollback.push_back(std::move(line));
  if (self->m_scrollback.size() > kScrollbackLimit)
    self->m_scrollback.erase(self->m_scrollback.begin());
  return 1;
}

int ScreenCallbacks::MoveCursor(VTermPos pos, VTermPos /*old*/, int /*visible*/,
                                void* user) {
  auto* self = static_cast<Screen*>(user);
  self->m_cursor = CursorPos{pos.row, pos.col};
  return 1;
}

int ScreenCallbacks::SetTermProp(VTermProp prop, VTermValue* val, void* user) {
  auto* self = static_cast<Screen*>(user);
  switch (prop) {
  case VTERM_PROP_CURSORVISIBLE:
    self->m_cursorVisible = val->boolean != 0;
    return 1;
  case VTERM_PROP_TITLE:
    // libvterm 0.3 delivers the title in fragments.
    if (val->string.initial)
      self->m_title.clear();
    self->m_title.append(val->string.str, val->string.len);
    return 1;
  default:
    return 0;
  }
}

void ScreenCallbacks::Output(const char* s, size_t len, void* user) {
  static_cast<Screen*>(user)->m_pendingOutput.append(s, len);
}

} // namespace term
