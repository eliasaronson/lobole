# The terminal panel

A dockable terminal inside the app, so an agent (Claude Code) or a shell runs
in the same window as the model it is editing, already bound to this
instance. It is off by default — a CAD app should not open with a shell in
it, and the panel costs a process the moment it is used — behind
`terminal_panel` in Edit > Preferences > General. Turning it on creates the panel and a right-hand dock node for it;
turning it off ends its sessions rather than hiding them, because a process
kept alive behind a closed panel is one nobody can see or stop.

**Docking.** A panel turned on mid-session has no dock entry in any saved
layout, so `DockLayout` splits a right node out of the dockspace for it the
first time it appears undocked (otherwise it would float over the viewport).
The lookup uses the full window name `Terminal###TerminalPanel`: ImGui hashes
from the `###`, so `FindWindowByName("Terminal")` silently finds nothing. A
layout rebuild while the panel is **off** deletes the node it lived in, and
ImGui would re-home the window into the central node, over the whole 3D
viewport; the layout code keeps it on the right, and
`terminal/survives_a_dock_layout_rebuild` rebuilds with the panel on and off
and checks it comes back submitted and on the right.

## Layers

```
src/common/PtySession.{h,cpp}      fork+exec under a PTY, non-blocking read,
                                   write, TIOCSWINSZ, exit status,
                                   foreground process name
src/common/TerminalScreen.{h,cpp}  libvterm wrapper: bytes in, a cell grid +
                                   scrollback out. No ImGui, no platform code.
src/ui/TerminalPanel.{h,cpp}       the panel: draws the grid, maps keys,
                                   owns the sessions and the tabs
```

The split is what makes it testable: `TerminalScreen` is pure bytes → grid and
links into `designer_tests`; `PtySession` is exercised headlessly with
`bash -c`; only drawing and key mapping need a GUI test.

libvterm (the emulator neovim's `:terminal` uses) owns the escape-sequence
state machine. Claude Code needs a real grid — it renders inline with cursor
addressing and erase-in-line, uses 24-bit colour, bracketed paste and SIGWINCH
reflow — but no alternate screen and no mouse reporting.

`-DENABLE_TERMINAL=OFF` removes the panel, its preference and its tests and
stops the build looking for libvterm. The panel is not built on Windows
(`PtySession` is POSIX only).

## Rendering

A second font, `JetBrainsMono-Regular.ttf` (box-drawing and `❯` coverage),
loaded at startup even while the panel is off. Cell size comes from its
advance; columns and rows follow the panel's content region, and a resize is
sent to the child with `TIOCSWINSZ`. Each cell's glyph is placed individually
(a rasterised advance never lands exactly on the cell width, and flowing text
drifts into ragged columns); backgrounds are batched into runs and skipped
where they equal the default. `Screen::SetDefaultColors` pushes the theme's
foreground and background into libvterm, so unstyled cells arrive themed.
Scrollback is a ring of `kScrollbackLimit` (10 000) lines.

## Keyboard: hover, not focus

**While the pointer is over the panel, the terminal owns the keyboard**
(`TerminalCapturesKeys`), including Ctrl chords: Ctrl+C, Ctrl+D and Ctrl+Z
are the first things anyone presses in a terminal. `DispatchShortcuts` returns
early while it holds, so Ctrl+Z over the terminal does not undo a feature and
Ctrl+Z over the viewport does. Hover rather than focus, because an invisible
focus state deciding whether Ctrl+Z undoes a feature or suspends a job is what
makes an embedded terminal feel dangerous.

The chords the terminal keeps for itself, all with Shift because the
unshifted forms mean something to a TUI: `Ctrl+Shift+C` copy, `Ctrl+Shift+V`
paste, `Ctrl+Shift+T` new tab, `Ctrl+Shift+W` close tab,
`Ctrl+Shift+PageUp/PageDown` switch tabs. They are hard-coded in
`HandleInput`, not commands: the panel swallows the keyboard, so a keymap
binding would never fire inside it, and a `Terminal` keymap context would
break the rule that contexts are the toolbar's branches
([keybindings.md](keybindings.md)).

## Sessions

Opening the panel starts `$SHELL` in the project directory (the working
directory when the project is unsaved). Every session exports
`DESIGNER_PORT` = this instance's remote port, so `claude`,
`python3 -m lobole.client` or anything else that reads it reaches this
window.
If the app was started without `--remote`, the panel opens a port on demand
(`AppState::ensureRemotePort`), since a terminal is useless to the agent if
the app it sits in is unreachable.

The **Claude Code** button (`###term_claude`) types
`claude --mcp-config <tmp>/lobole-mcp-<port>.json` into the shell. The
config is written per session: it runs `python3 -m lobole.mcp` against this
instance's port, with `PYTHONPATH` naming the package the build staged
beside its assets (or installed under `share/lobole/python/`;
[packaging](../packaging/README.md#the-agent-surface)). It it never edits
the user's global MCP registration or a project's `.mcp.json`. Typing the
command rather than running it keeps it legible and repeatable, and makes
Claude Code a child of the user's shell, so quitting it returns to a prompt.

**Tabs.** Each session is a `Session` in `TerminalState::sessions` with a
monotonic `id`; tab ids are `###term_tab_<id>`, never the vector index (an
index-keyed tab hands ImGui's state for one session to another when a tab
closes). `+` (`###term_newtab`) sits on the button row. The session tab bar
appears only with two or more sessions, and shrinks tabs
(`FittingPolicyResizeDown`) rather than scrolling them. `DockLayout` sets
`NoTabBar` on the terminal's dock node while it is the only window there (so
one bar reads `zsh` rather than two bars reading `Terminal` and `zsh`), clears
it when another window docks in, and does so every frame because a saved
layout never re-runs the DockBuilder branches.

- **Every session is pumped every frame**, not just the visible one: an
  undrained background tab fills its PTY buffer and the child blocks
  mid-write.
- A tab is labelled with the process in front of it (`tcgetpgrp` + `/proc`),
  so it reads `claude`, `zsh` or `ninja`; there is no inline rename.
- A background tab is resized when it is next drawn, as `tmux` does.
- `Stop` (`###term_stop`) sends SIGHUP then SIGKILL to a session that has
  stopped answering Ctrl+C, keeping its scrollback.
- An exited session keeps its final screen and status and offers
  `New shell` (`###term_shell`); closing the last tab leaves that state
  rather than closing the panel, since the preference is what opens and
  closes the panel.

## Selection and the clipboard

Drag to select; double-click selects a word, where `. / - _ ~` count as word
characters because a path is what anyone double-clicks in a terminal;
`Ctrl+Shift+C` copies; a click clears. Selection lives in **buffer**
coordinates (line 0 is the oldest scrollback line), so it survives scrolling
and new output, and one helper, `BufferLineOfRow`, maps a visual row to a
buffer line for drawing, hit-testing and copying alike. Text extraction
(`Screen::TextRange`, `Screen::WordAt`) lives in `TerminalScreen`, so copying
across the scroll boundary is tested headlessly. Trailing blanks are trimmed
per line.

**Middle-click** pastes the panel's own selection, falling back to the system
clipboard. It is read before the selection branches, because a middle press
also activates the grid's button, which would otherwise clear the selection a
moment before pasting it. Pastes go through `Screen::SendPaste`, wrapped in
bracketed-paste markers when the program asked for them, so a multi-line paste
does not read as a run of Enter presses.

**OSC 52.** A TUI copies by asking the terminal to (`ESC ] 52 ; c ; <base64>`).
libvterm's selection callbacks deliver it decoded in chunks into a
`kClipboardChunk` (64 KiB) buffer; `Screen` accumulates up to
`kClipboardWriteLimit` (1 MiB) and truncates past it; the panel drains it with
`TakeClipboardWrite()` beside `TakeOutput()`, pull-based, so `Screen` never
calls into ImGui. Any session may write, including a background tab; last
write wins. CLIPBOARD and PRIMARY both map to the one clipboard GLFW has.
**The read form is refused**: the query callback is null, and a query produces
no reply bytes at all. Answering would let any program in the terminal read
the user's clipboard back over its stdin.

## Security

A terminal runs arbitrary commands as the user, as their own terminal does;
it adds convenience, not privilege. It is not a remote-controllable surface:
no RPC runs a command, and nothing in `ui_action` types into it.

## Tests

- Headless (`tests/test_terminal.cpp`): grid contents after known byte
  streams (wrapping, cursor addressing + erase, truecolor, scrollback, and a
  resize that truncates rather than reflows, by design), `PtySession` round
  trips (output, input, exit status, size changes, the foreground name),
  selection text across the scroll boundary, word selection, OSC 52 writes (decoded, fragmented, truncated) and a query that
  produces zero reply bytes.
- GUI (`terminal/*`): off by default and docked right when on; survives a dock
  layout rebuild; runs a command and shows its output; drag selects and
  copies; hover decides who gets the keys (both directions); middle-click
  pastes the selection; tabs run and drain in the background (the test proves
  the tab really is in the background); `Ctrl+Shift+T` opens a tab; reopens
  after being turned off.

## Not built

A Windows (ConPTY) `PtySession`; mouse reporting; sixel/kitty graphics,
ligatures, bidi; answering XTVERSION/DA queries; the X11 PRIMARY selection as
a separate clipboard; a font-size control for the terminal; an
"Open terminal here" action that launches the user's own terminal; a shell
inside the Flatpak sandbox. Terminals are one panel with its own tabs, not N
dockable windows: dock state persists in `imgui.ini` by id, and sessions die
at exit.
