// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef HAVE_TERMINAL

#include "common/PtySession.h"
#include "common/TerminalScreen.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

// The terminal's testable half (docs/ui/terminal.md): bytes in, grid
// out, with no ImGui and no display. Everything Claude Code needs of a
// terminal is asserted here rather than by looking at a panel.
using term::PtySession;
using term::Screen;

TEST_CASE("terminal: plain text lands in the grid", "[terminal]") {
  Screen s(5, 20);
  s.Write("hello");
  CHECK(s.Line(0) == "hello");
  CHECK(s.Cursor().row == 0);
  CHECK(s.Cursor().col == 5);
  CHECK(s.Line(1).empty());
}

TEST_CASE("terminal: the redraw pattern Claude Code actually uses",
          "[terminal]") {
  // It does NOT use the alternate screen: it rewrites in place with
  // cursor-column addressing (CHA) and erase-in-line (EL). A widget that
  // appends text would show both versions stacked.
  Screen s(4, 30);
  s.Write("prompt: draft answer");
  REQUIRE(s.Line(0) == "prompt: draft answer");
  s.Write("\x1b[9G"); // column 9, 1-based
  s.Write("\x1b[K");  // erase to end of line
  s.Write("final answer");
  CHECK(s.Line(0) == "prompt: final answer");
}

TEST_CASE("terminal: 24-bit colour survives as RGB", "[terminal]") {
  Screen s(2, 10);
  // The amber Claude Code draws its rules in, then a reset.
  s.Write("\x1b[38;2;255;193;7mX\x1b[39mY");
  const term::Cell x = s.At(0, 0);
  CHECK(x.fg == 0xFFC107u);
  const term::Cell y = s.At(0, 1);
  CHECK(y.fg != 0xFFC107u); // reset really reset
  // An indexed colour must be resolved against the palette, not left as an
  // index — an unresolved index renders as black on black.
  Screen s2(2, 10);
  s2.Write("\x1b[38;5;196mR");
  CHECK(s2.At(0, 0).fg != 0u);
}

TEST_CASE("terminal: attributes are readable per cell", "[terminal]") {
  Screen s(2, 10);
  s.Write("\x1b[1mB\x1b[22m\x1b[4mU\x1b[24m\x1b[7mR");
  CHECK((s.At(0, 0).attrs & term::kBold) != 0);
  CHECK((s.At(0, 1).attrs & term::kUnderline) != 0);
  CHECK((s.At(0, 2).attrs & term::kReverse) != 0);
  CHECK((s.At(0, 1).attrs & term::kBold) == 0);
}

TEST_CASE("terminal: wrapping and scrollback", "[terminal]") {
  Screen s(3, 10);
  s.Write("0123456789abc"); // wraps after 10 columns
  CHECK(s.Line(0) == "0123456789");
  CHECK(s.Line(1) == "abc");

  // Push past the bottom: the top line moves into scrollback rather than
  // being lost, which is the only reason an inline-rendering TUI is usable.
  s.Write("\r\nline2\r\nline3\r\nline4");
  REQUIRE(s.ScrollbackCount() >= 1);
  CHECK(s.ScrollbackText(0) == "0123456789");
  CHECK(s.Line(s.Rows() - 1) == "line4");
}

TEST_CASE("terminal: resize truncates rather than reflowing — by design",
          "[terminal]") {
  // Worth pinning down because it looks like a bug and is not: libvterm does
  // NOT rewrap text already on screen when the width changes. Narrowing
  // truncates it, and widening does not bring it back.
  Screen s(4, 20);
  s.Write("word one two three");
  REQUIRE(s.Line(0) == "word one two three");
  s.Resize(4, 10);
  CHECK(s.Cols() == 10);
  CHECK(s.Line(0) == "word one t");
  s.Resize(4, 20);
  CHECK(s.Line(0) == "word one t"); // gone for good

  // That is harmless for the case this exists to serve, because a program
  // told about the new size redraws itself — which is exactly what Claude
  // Code does on SIGWINCH, and what the pty resize test below proves it is
  // told. The one visible consequence: history scrolled off before a resize
  // keeps the old width.
}

TEST_CASE("terminal: key encoding comes back out as bytes", "[terminal]") {
  Screen s(4, 20);
  s.SendChar(U'a');
  CHECK(s.TakeOutput() == "a");
  CHECK(s.TakeOutput().empty()); // drained

  s.SendChar(U'c', static_cast<int>(Screen::Mod::Ctrl));
  CHECK(s.TakeOutput() == std::string("\x03")); // Ctrl+C is a real Ctrl+C

  s.SendKey(Screen::Key::Enter);
  CHECK(s.TakeOutput() == "\r");

  s.SendKey(Screen::Key::Up);
  const std::string up = s.TakeOutput();
  CHECK(up.size() >= 3);
  CHECK(up[0] == '\x1b');
}

TEST_CASE("terminal: paste is bracketed only when asked for", "[terminal]") {
  Screen s(4, 40);
  s.SendPaste("ab");
  CHECK(s.TakeOutput() == "ab"); // no markers: the program never enabled them

  s.Write("\x1b[?2004h"); // now it does
  s.SendPaste("ab");
  const std::string bracketed = s.TakeOutput();
  CHECK(bracketed == "\x1b[200~ab\x1b[201~");
}

TEST_CASE("terminal: the program can hide the cursor and set a title",
          "[terminal]") {
  Screen s(3, 20);
  CHECK(s.CursorVisible());
  s.Write("\x1b[?25l");
  CHECK_FALSE(s.CursorVisible());
  s.Write("\x1b[?25h");
  CHECK(s.CursorVisible());
  s.Write("\x1b]0;my title\x07");
  CHECK(s.Title() == "my title");
}

TEST_CASE("terminal: selection text spans scrollback and screen",
          "[terminal]") {
  // Copy has to work across the scroll boundary, which is the whole reason
  // the screen exposes one address space instead of two.
  Screen s(3, 12);
  s.Write("alpha\r\nbravo\r\ncharlie\r\ndelta");
  REQUIRE(s.ScrollbackCount() == 1); // "alpha" scrolled off
  REQUIRE(s.TotalLines() == 4);
  CHECK(s.TextRange(0, 0, 0, 12) == "alpha"); // from scrollback
  CHECK(s.TextRange(3, 0, 3, 12) == "delta"); // from the live screen
  CHECK(s.TextRange(0, 0, 3, 12) == "alpha\nbravo\ncharlie\ndelta");

  // Partial columns: start mid-word on the first line, end mid-word on the
  // last, full width in between.
  CHECK(s.TextRange(1, 2, 2, 4) == "avo\nchar");

  // Trailing blanks are trimmed PER LINE — otherwise every pasted line after
  // the first arrives padded out to the terminal width.
  const std::string two = s.TextRange(1, 0, 2, 12);
  CHECK(two == "bravo\ncharlie");

  // Degenerate ranges are empty, not reversed or clamped into nonsense.
  CHECK(s.TextRange(2, 5, 2, 5).empty());
  CHECK(s.TextRange(3, 0, 1, 0).empty());
}

TEST_CASE("terminal: double-click picks a word, not a line", "[terminal]") {
  Screen s(2, 40);
  s.Write("run ./tools/gui-tests-xvfb.sh --fast now");
  int begin = 0, end = 0;
  s.WordAt(0, 8, begin, end); // inside the path
  CHECK(s.TextRange(0, begin, 0, end) == "./tools/gui-tests-xvfb.sh");
  s.WordAt(0, 1, begin, end);
  CHECK(s.TextRange(0, begin, 0, end) == "run");
  // Whitespace selects nothing rather than grabbing the neighbouring word.
  s.WordAt(0, 3, begin, end);
  CHECK(begin == end);
}

// --- the PTY half -------------------------------------------------------

namespace {

// Drains the session into the screen until `pred` holds or the budget runs
// out. Real processes are asynchronous; a fixed sleep would be either flaky
// or slow.
template <class Pred>
bool PumpUntil(PtySession& pty, Screen& screen, Pred pred, int maxMs = 5000) {
  for (int elapsed = 0; elapsed < maxMs; elapsed += 10) {
    const std::string out = pty.Poll();
    if (!out.empty())
      screen.Write(out.data(), out.size());
    if (pred())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

} // namespace

TEST_CASE("pty: a child's output reaches the screen", "[terminal][pty]") {
  Screen screen(10, 40);
  PtySession pty;
  PtySession::Options opts;
  opts.program = "/bin/sh";
  opts.args = {"-c", "printf 'hello pty'"};
  opts.rows = 10;
  opts.cols = 40;
  std::string error;
  REQUIRE(pty.Start(opts, &error));

  CHECK(PumpUntil(pty, screen, [&] { return screen.Line(0) == "hello pty"; }));
  CHECK(PumpUntil(pty, screen, [&] { return !pty.Running(); }));
  CHECK(pty.ExitStatus() == 0);
}

TEST_CASE("pty: a failed exec explains itself on the terminal",
          "[terminal][pty]") {
  // The parent has no other channel to the user, so the child prints the
  // reason before dying — a panel that just goes blank reads as a crash.
  Screen screen(10, 60);
  PtySession pty;
  PtySession::Options opts;
  opts.program = "definitely-not-a-real-program-xyz";
  opts.rows = 10;
  opts.cols = 60;
  std::string error;
  REQUIRE(pty.Start(opts, &error)); // fork succeeds; exec is what fails
  CHECK(PumpUntil(pty, screen, [&] {
    return screen.Line(0).find("could not run") != std::string::npos;
  }));
  CHECK(PumpUntil(pty, screen, [&] { return !pty.Running(); }));
  CHECK(pty.ExitStatus() == 127);
}

TEST_CASE("pty: input reaches the child, and exit status is reported",
          "[terminal][pty]") {
  Screen screen(10, 40);
  PtySession pty;
  PtySession::Options opts;
  opts.program = "/bin/sh";
  opts.args = {"-c", "read line; printf 'got:%s' \"$line\"; exit 3"};
  opts.rows = 10;
  opts.cols = 40;
  std::string error;
  REQUIRE(pty.Start(opts, &error));

  screen.SendChar(U'h');
  screen.SendChar(U'i');
  screen.SendKey(Screen::Key::Enter);
  const std::string keys = screen.TakeOutput();
  pty.Write(keys.data(), keys.size());

  CHECK(PumpUntil(pty, screen, [&] {
    for (int r = 0; r < screen.Rows(); r++)
      if (screen.Line(r).find("got:hi") != std::string::npos)
        return true;
    return false;
  }));
  CHECK(PumpUntil(pty, screen, [&] { return !pty.Running(); }));
  CHECK(pty.ExitStatus() == 3);
}

TEST_CASE("pty: the child sees the size, and sees it change",
          "[terminal][pty]") {
  // Without TIOCSWINSZ a resized panel leaves the program drawing at the old
  // width — the single most visible way an embedded terminal looks broken.
  Screen screen(24, 80);
  PtySession pty;
  PtySession::Options opts;
  opts.program = "/bin/sh";
  opts.args = {"-c",
               "trap 'printf \"W=%s \" $(stty size | cut -d\" \" -f2)' WINCH; "
               "printf 'W=%s ' $(stty size | cut -d' ' -f2); "
               "for i in 1 2 3 4 5 6 7 8 9 10; do sleep 0.2; done"};
  opts.rows = 24;
  opts.cols = 80;
  std::string error;
  REQUIRE(pty.Start(opts, &error));

  REQUIRE(PumpUntil(pty, screen, [&] {
    return screen.Line(0).find("W=80") != std::string::npos;
  }));
  pty.Resize(30, 100);
  screen.Resize(30, 100);
  CHECK(PumpUntil(pty, screen, [&] {
    for (int r = 0; r < screen.Rows(); r++)
      if (screen.Line(r).find("W=100") != std::string::npos)
        return true;
    return false;
  }));
  pty.Terminate();
  CHECK_FALSE(pty.Running());
}

TEST_CASE("pty: the header can name what is actually in front",
          "[terminal][pty]") {
  // The session is a shell, but what has the keyboard may be something the
  // shell started — Claude Code, usually. A header that keeps saying "bash"
  // while a TUI is running is telling the user the wrong thing.
  Screen screen(10, 40);
  PtySession pty;
  PtySession::Options opts;
  opts.program = "/bin/sh";
  opts.args = {"-c", "printf 'ready'; sleep 5"};
  std::string error;
  REQUIRE(pty.Start(opts, &error));
  REQUIRE(PumpUntil(pty, screen, [&] {
    return screen.Line(0).find("ready") != std::string::npos;
  }));
  const std::string front = pty.ForegroundName();
  CHECK((front == "sh" || front == "sleep")); // whichever holds the terminal
  pty.Terminate();
  CHECK(pty.ForegroundName().empty()); // nothing running, nothing claimed
}

TEST_CASE("pty: terminate is safe twice and kills the child",
          "[terminal][pty]") {
  PtySession pty;
  PtySession::Options opts;
  opts.program = "/bin/sh";
  opts.args = {"-c", "while true; do sleep 1; done"};
  std::string error;
  REQUIRE(pty.Start(opts, &error));
  REQUIRE(pty.Running());
  pty.Terminate();
  CHECK_FALSE(pty.Running());
  pty.Terminate(); // no crash, no hang, no second wait
  CHECK_FALSE(pty.Running());
}

// --- OSC 52: the clipboard, one direction only ---------------------------
// Claude Code's own copy is an OSC 52 write. Until this landed the sequence
// was parsed and dropped, so every copy inside it did nothing at all — while
// the panel's own drag-and-Ctrl+Shift+C worked, which is exactly what hid it.

TEST_CASE("terminal: OSC 52 puts text on the clipboard", "[terminal]") {
  Screen s(4, 40);
  CHECK_FALSE(
      s.TakeClipboardWrite().has_value());   // nothing asked, nothing offered
  s.Write("\x1b]52;c;aGVsbG8gd29ybGQ=\x07"); // "hello world"
  auto write = s.TakeClipboardWrite();
  REQUIRE(write.has_value());
  CHECK(*write == "hello world");
  // Taken means taken: the next frame must not paste it again.
  CHECK_FALSE(s.TakeClipboardWrite().has_value());
  // And none of it landed on the screen.
  CHECK(s.Line(0).empty());
}

TEST_CASE("terminal: an OSC 52 write split across reads still assembles",
          "[terminal]") {
  // A PTY read boundary can fall anywhere, and it usually falls in the
  // middle of a long base64 payload — which is the only kind Claude Code
  // sends.
  Screen s(4, 40);
  const std::string seq = "\x1b]52;c;VGhlIHF1aWNrIGJyb3duIGZveA==\x07";
  for (size_t i = 0; i < seq.size(); i++)
    s.Write(seq.data() + i, 1); // one byte at a time: the worst case
  auto write = s.TakeClipboardWrite();
  REQUIRE(write.has_value());
  CHECK(*write == "The quick brown fox");
}

TEST_CASE("terminal: OSC 52 terminated by ST, and to PRIMARY", "[terminal]") {
  // ESC \ is as common a terminator as BEL, and `p` (PRIMARY) as common a
  // target as `c` — both land on the one clipboard the platform gives us.
  Screen s(4, 40);
  s.Write("\x1b]52;p;cHJpbWFyeQ==\x1b\\");
  auto write = s.TakeClipboardWrite();
  REQUIRE(write.has_value());
  CHECK(*write == "primary");
}

TEST_CASE("terminal: an over-long clipboard write is truncated, not honoured",
          "[terminal]") {
  Screen s(4, 40);
  // 2 MiB of 'A' — past kClipboardWriteLimit, and past anything a person
  // means to paste.
  const std::string big(2u * 1024 * 1024, 'A');
  std::string b64;
  {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (size_t i = 0; i < big.size(); i += 3) {
      const uint32_t n = (static_cast<uint32_t>(big[i]) << 16) |
                         (static_cast<uint32_t>(big[i + 1]) << 8) |
                         static_cast<uint32_t>(big[i + 2]);
      b64.push_back(kAlphabet[(n >> 18) & 0x3F]);
      b64.push_back(kAlphabet[(n >> 12) & 0x3F]);
      b64.push_back(kAlphabet[(n >> 6) & 0x3F]);
      b64.push_back(kAlphabet[n & 0x3F]);
    }
  }
  s.Write("\x1b]52;c;" + b64 + "\x07");
  auto write = s.TakeClipboardWrite();
  REQUIRE(write.has_value());
  CHECK(write->size() == term::kClipboardWriteLimit);
  CHECK(write->front() == 'A');
}

TEST_CASE("terminal: a clipboard READ is refused, not answered", "[terminal]") {
  // OSC 52's query form hands whatever the user last copied to whatever is
  // running in the panel, over its own stdin. The refusal has to be silence:
  // an empty answer is still an answer, and a program can act on it.
  Screen s(4, 40);
  s.Write("\x1b]52;c;?\x07");
  CHECK(s.TakeOutput().empty()); // nothing sent back to the process
  CHECK_FALSE(s.TakeClipboardWrite().has_value());
}

TEST_CASE("terminal: the last clipboard write wins", "[terminal]") {
  // Two writes between drains — a background tab and a foreground one, say.
  // The clipboard has one slot; it holds what a paste would have produced.
  Screen s(4, 40);
  s.Write("\x1b]52;c;Zmlyc3Q=\x07"); // "first"
  s.Write("\x1b]52;c;c2Vjb25k\x07"); // "second"
  auto write = s.TakeClipboardWrite();
  REQUIRE(write.has_value());
  CHECK(*write == "second");
}

#endif // HAVE_TERMINAL
