# A terminal inside the app — design

> Status: **built 2026-08-20** (T1–T4; T5 polish outstanding — see the phase
> table). Signed off with three changes from the design, all from the user:
> the panel is **off by default behind a preference**, it docks **right**
> rather than at the bottom, and the keyboard gate is **hover**, not focus.
> From `todo.txt`:
> *"terminal in the program for cc?"* — the goal is running Claude Code in the
> same window as the model it is editing, rather than alt-tabbing between a
> terminal and the app.

## Why this, and why it is not just a nicety

This project's fifth stated goal is that AI assistants use *the same interface
as humans*. That already holds at the API layer — the MCP server drives the
same command layer the GUI does. What does not hold is the **workspace**: the
agent lives in a terminal somewhere else, the model lives here, and the human
is the transport between them. Every dogfooding round in
[../dogfooding.md](../dogfooding.md) has been run with a terminal on one half
of the screen and the app on the other.

Two concrete wins beyond "it's in the window":

1. **The agent is bound to the instance you are looking at.** Today you start
   the app, find its port, and register an MCP server against it (`--port
   8437`, or the `.mcp.json` in this repo). A terminal launched *by* the app
   knows its own port, its own project directory, and its own instance file —
   so `claude` starts already connected to the model on screen. That is not
   available to any external terminal without the user doing the wiring.
2. **A shell where the model is.** Git, the build, `scratch/*.py` regeneration
   scripts, `tools/designer_remote.py` — all of them are things this project's
   own workflow already does beside the app, in the project directory.

## What a terminal has to do to host Claude Code (measured, not assumed)

Claude Code was run under a PTY and its output captured
(`--- spike, 2026-08-20 ---`). What it actually uses:

| feature | used? | consequence |
|---|---|---|
| alternate screen (1049/47) | **no** | it renders *inline* and relies on the normal buffer + scrollback |
| cursor-column addressing (CHA `ESC[nG`) + erase-in-line | **heavily** | a naive "append text" widget cannot draw it; a real grid is required |
| 24-bit colour SGR | yes (173 cells on one screen) | 256-colour-only rendering would lose the UI |
| bracketed paste (2004), focus events (1004) | yes | must be tracked and honoured on paste |
| synchronised output (2026) | yes | ignoring it is safe (worst case: a torn frame) |
| kitty keyboard protocol push/pop (`ESC[>1u`) | yes | safe to ignore — it falls back to legacy key encoding |
| `ESC[>q` (XTVERSION), DA | queried | safe to leave unanswered |
| SIGWINCH reflow | yes | it re-renders its whole block; resize must be wired to `TIOCSWINSZ` |
| mouse tracking (1000/1006) | **no** | mouse reporting can be deferred entirely |
| box-drawing + `❯` glyphs | yes (U+2500, U+276F) | the font must cover them; Roboto does not |

So the requirement is "a real VT100/xterm-class emulator", but a bounded one:
no alt screen, no mouse, no sixel.

**Feasibility, verified:** the captured bytes were fed through **libvterm**
(MIT, 167 KB installed, the library neovim's `:terminal` uses) and it
reconstructs the screen exactly — the trust prompt, the reflow at 90 columns,
and the truecolor attributes. The hard half of this feature is a vendored
dependency, not code we write.

## Options considered

**A. PTY + libvterm + an ImGui-drawn cell grid. — RECOMMENDED.**
We own a character grid and draw it; libvterm owns the escape-sequence state
machine. Portable to the future Windows port (ConPTY replaces `forkpty`; the
grid and renderer are unchanged), screenshot-able, testable headlessly, and
themable to match the app. Cost: the input mapping and the renderer, roughly a
panel's worth of code, plus the platform PTY layer.

**B. Reparent a real terminal (`xterm -into $XID`, XEmbed).**
A perfect terminal for free — and rejected. It is an opaque native child
window: it ignores ImGui docking and z-order (it would sit *over* floating
tool windows), it does not appear in `screenshot`/`render_sheet` output or in
the GUI test engine, it is X11-only (this session is X11 today, but Wayland
and the Windows port are both stated directions), and inside the Flatpak
sandbox there is no host `xterm` to embed. It buys a terminal at the cost of
being outside every mechanism this app uses to verify itself.

**C. A native "Claude" chat panel over `claude -p --output-format stream-json`
(or the Agent SDK).**
Not a terminal at all: we render messages, tool calls and approvals as ImGui
widgets. This is genuinely attractive later — a tool call could highlight the
feature it touched, and an approval could show a geometry preview — but as the
answer to *this* request it is wrong. It re-implements permission prompts,
plan mode, slash commands and their drift across Claude Code releases; it
cannot run `git`, `ninja` or a scratch script; and it is a bigger surface than
the emulator it avoids. Worth revisiting as a *second*, complementary panel
once the terminal exists — not instead of it.

**D. Launch the user's own terminal in the project directory.**
A menu item, twenty minutes, and it is the honest fallback if A stalls. It
does not put the agent in the window, which is the whole request — but it
should ship anyway as `Tools ▸ Open terminal here`, because it also covers the
Flatpak case where the sandbox has no shell worth embedding.

## The design (option A)

### Layers, and where they live

```
src/common/PtySession.{h,cpp}   fork+exec under a PTY, non-blocking read
                                thread, write, TIOCSWINSZ, exit status
src/common/TerminalScreen.{h,cpp}  libvterm wrapper: bytes in, a cell grid +
                                scrollback out. NO ImGui, NO platform code.
src/ui/TerminalPanel.{h,cpp}    the dockable panel: draws the grid, maps
                                ImGui keys to terminal input, owns the
                                session and the "start Claude Code" action.
```

The split is what makes it testable: `TerminalScreen` is pure
bytes → grid and links into `designer_tests`; `PtySession` is exercised
headlessly by running `printf`/`bash -c` and asserting on the grid; only the
drawing and key mapping need a GUI test.

### Rendering

A monospace font is added to the atlas as a **second font** (the app already
merges Font Awesome into Roboto at runtime from `assets/fonts`) — a font with
box-drawing coverage, so `─` and `❯` are not tofu. Cell size comes from the
font's advance width; columns/rows follow the panel's content region, so
resizing the dock node resizes the PTY.

Per cell: a background rect (skipped when it equals the panel background) and
the glyph, drawn straight into the window's `ImDrawList` — one path, no
per-cell ImGui widgets. Colours come from libvterm as RGB (it resolves the
palette), with the default fg/bg mapped onto the app theme so the terminal
does not look pasted in from another program.

Scrollback: libvterm's `sb_pushline` callback into a ring buffer (default
10 000 lines), with the wheel scrolling it.

### Input, and the shortcut conflict this creates

The interesting problem is not the escape sequences, it is that **the app's
menu shortcuts are registered `ImGuiInputFlags_RouteGlobal`** — Ctrl+Z, Ctrl+S,
Ctrl+N, Ctrl+I (`MainMenuBar.cpp:457`). A terminal that does not take those
keys is a broken terminal (Ctrl+C, Ctrl+D, Ctrl+Z are the first things anyone
presses), and an app that loses Ctrl+Z to a background panel is a broken app.

The rule: **while the terminal panel is focused, it owns the keyboard.** The
menu's shortcut block gates on a focused terminal and returns early; the panel
consumes every key event and converts it, including the modifier chords.
Exactly one chord stays with the app — the one that focuses/unfocuses the
terminal itself — and the panel says so on its header. This is a one-line gate
plus a GUI test asserting both directions (Ctrl+Z inside the terminal does
*not* undo a feature; Ctrl+Z outside it does).

Key mapping is the fiddly-but-bounded part: printable text via ImGui's input
queue characters, control characters from Ctrl+letter, the arrow/Home/End/
Delete/Function set as their xterm sequences, and paste wrapped in
`ESC[200~`/`ESC[201~` when bracketed paste is on.

### The integration that makes it worth more than an external terminal

A **`Claude Code` button** in the panel header (and `Tools ▸ Claude Code`):

- working directory = the project directory (or the repo root when unsaved),
- `DESIGNER_PORT` = this instance's remote port — **opening one if the app was
  started without `--remote`**, since a terminal is useless to the agent if
  the app it is inside is not reachable,
- `--mcp-config` pointing at a generated config for *this* instance, so the
  `designer` MCP tools address the model on screen with no port to guess.

The instance file (`$XDG_STATE_HOME/designer/instances/<port>.json`) already
exists and already carries port/pid/project, so this is wiring, not new
machinery.

### Opening the panel opens a shell

The first version put a chooser here — `Claude Code` or `Shell` — and it was
wrong in a way that only showed up in use: it is a **one-shot menu**. Once you
picked, the panel had a session, and the only route back to the choice was
restarting the app. The decision it was asking about is really just "what do I
type first", which is a question terminals already answer.

So the panel opens `$SHELL` in the project directory, and `Claude Code` is a
button in the header for the whole session. It **types the command into the
shell** rather than running it behind your back:

- what the button does is legible — you can see the command it ran, and
  re-run or edit it;
- Claude Code becomes a child of *your* shell, so quitting it returns you to a
  prompt instead of a dead panel;
- and it stays available, so the panel is never in a mode.

Every session — not just Claude Code — is started with `DESIGNER_PORT`
exported, so typing `claude` yourself, or `python3 tools/designer_remote.py`,
also reaches this window without being told which instance to attach to. The
button adds only the `--mcp-config` that gives Claude Code the `designer`
tools bound to that same port.

The header names the process that actually holds the terminal, read from
`tcgetpgrp` + `/proc`, so it says `claude` while Claude Code is in front and
`bash` at the prompt — a header that keeps saying "bash" while a TUI has the
keyboard is telling the user the wrong thing. When the session exits the panel
keeps the final screen, shows the status, and offers `New shell`: a terminal
that silently goes blank on exit reads as a crash of the *app*.

## Phases

| | scope | done when |
|---|---|---|
| **T1** | `PtySession` + `TerminalScreen` + headless tests | **done** — 14 headless tests |
| **T2** | The panel: grid rendering, monospace font, scrollback, resize→`TIOCSWINSZ` | **done** — a live `bash` and a live Claude Code, screenshotted |
| **T3** | Input: keys, control chars, paste, and the hover gate | **done** — GUI test asserts both directions of the gate |
| **T4** | The Claude Code launch (port, cwd, MCP config) + exited state | **done** — the button types `claude` bound to this instance into the shell |
| **T5** | Polish: selection + copy (**done**), a font-size control, `Tools ▸ Open terminal here` (option D) | partly done |

T1–T2 were the risk; T3–T4 the value. What shipped, concretely:

- **Off by default**, behind `terminal_panel` in Preferences ▸ General. Turning
  it on creates the panel *and* the right-hand dock node; turning it off ends
  the session rather than hiding it.
- Per-cell glyph placement rather than flowing runs of text: even a monospace
  font's rasterised advance does not land exactly on the cell width, and the
  drift is visible as ragged columns within a few characters. Backgrounds are
  still batched into runs. A live Claude Code costs **0.43 ms** of the `ui`
  phase (measured with `frame_stats` — see [../rendering/frame-timing.md](../rendering/frame-timing.md)).
- The session's colours come from the app: `Screen::SetDefaultColors` pushes
  the theme's fg/bg into libvterm, so unstyled cells arrive already themed and
  the renderer can skip a background rect for every default cell.
- One trap worth recording: **ImGui hashes window names from the last `###`**,
  so `FindWindowByName("Terminal")` finds nothing when the window is
  `Terminal###TerminalPanel` — silently. That is why the panel first appeared
  floating over the model tree instead of docked.

## Turning it off

`-DENABLE_TERMINAL=OFF` removes the panel, its preference and its tests, and
stops the build looking for libvterm at all. The dependency is 167 KB with no
transitive deps; the switch exists because "small today" is not a promise.

## Deferred, with reasons

- **Windows (ConPTY).** The grid, renderer and input are platform-free by
  construction; only `PtySession` needs a second implementation. Deferred
  with the rest of the Windows port ([../../packaging/windows/](../../packaging/windows/)).
- **Multiple terminal tabs.** One session covers the request. The panel's
  session ownership is a `std::unique_ptr`, so a vector is a later change,
  not a redesign.
- ~~**Selection and copy-out.**~~ **Done 2026-08-21.** Drag to select,
  double-click for the word (path- and flag-friendly: `.`, `/`, `-`, `_` and
  `~` count as word characters, because a filename is what anyone is
  double-clicking a terminal for), Ctrl+Shift+C to copy, click to clear.
  Selection lives in **buffer** coordinates — line 0 is the oldest scrollback
  line — so it survives scrolling and new output, and one helper
  (`BufferLineOfRow`) maps a visual row to a buffer line for drawing,
  hit-testing and copy alike; three subtly different versions of that mapping
  is how off-by-one selections happen. The text extraction lives in
  `TerminalScreen` (`TextRange`, `WordAt`) rather than the panel, so copying
  across the scroll boundary is tested **headlessly** rather than by dragging
  in a screenshot. Trailing blanks are trimmed per line: a terminal's blank
  cells are padding, and pasting them back indents every line after the
  first. The highlight is translucent over the glyphs, not an inverted
  re-render — the text is the thing you selected in order to read.
- **Mouse reporting.** Claude Code does not use it, and neither do git or
  ninja. If someone runs `htop` in there, they can use an external terminal.
- **Sixel/kitty graphics, ligatures, bidi.** No.
- **Answering XTVERSION/DA queries.** Unanswered queries are already handled
  gracefully by every program tested; answering them invites emulating an
  xterm version we are not.
- **A shell inside the Flatpak build.** The sandbox's PATH is not the user's
  system; the panel should say so rather than pretend. Option D's "open your
  own terminal" is the right behaviour there.

## Risks

- **Font size.** A monospace TTF with box-drawing coverage is ~200–700 KB
  added to `assets/fonts`. Acceptable (the icon font is already 400 KB), but
  it should be chosen for coverage, not looks.
- **The keyboard gate is the one thing that can make the app feel broken.**
  It is also the one thing a GUI test can pin down exactly, which is why T3
  is its own phase rather than a sub-task of T2.
- **A terminal runs arbitrary commands.** So does the user's terminal, on the
  same machine, as the same user — this adds convenience, not privilege. It
  is worth stating that the panel is *not* a remote-controllable surface: the
  RPC layer gets no "run a command" method, and nothing in `ui_action` may
  type into it. An agent that wants a shell already has one; the app must not
  become a second, unauthenticated path to it.

## Verification

- Headless (`designer_tests`): grid contents after known byte streams
  (wrapping, CHA + EL redraw, SGR truecolor, scrollback push, reflow on
  resize); `PtySession` round trip against `bash -c`.
- GUI: a terminal panel running `printf 'hello'` shows `hello` in the grid;
  the focus gate in both directions; the panel's empty state offers the two
  buttons; an exited process leaves its last screen plus a status line.
- Manual, once: `claude` in the panel, driving this instance over MCP —
  which is also the acceptance test for T4.
