# U12 — One keymap: commands, contexts, and rebinding (2026-08-24)

> Status: **SHIPPED 2026-08-24.** Supersedes the "not in v1" entry in
> `docs/ui/settings.md` ("Keybindings — needs its own model: conflicts,
> chords, per-mode maps"). 117 commands, 6 contexts, rebindable from
> Edit > Preferences > Keyboard or over `set_keybinding`.

A key used to be handled in four unrelated places and *described* in five more.
Nothing could be rebound, and nothing could be listed, because no object knew a
binding existed.

## What it looks like now

`src/core/io/Keymap.{h,cpp}` — the metadata, in core so `designer_tests` (which
links `designer_core` alone) can gate it without a renderer: `Context`, a `Key`
enum of our own, `Chord`, `ParseChord`/`FormatChord`, the defaults table, and
`Keymap` (overrides + resolve + conflicts). Overrides live in `settings.json`
under `"keys"`, and only the rows that differ from a default are written, so a
later change to a default still reaches anyone who never touched it.

`src/ui/Commands.{h,cpp}` — the behaviour, keyed by the same id: `enabled` and
`run` per command, `ActiveContext` (DrawToolbar's own branch, factored out and
shared), and `DispatchShortcuts`, called once per frame from `DrawUI` **after
every panel has drawn** — that is what makes `io.WantTextInput` accurate, so a
bare `1` typed into a position field cannot also arm a mate tool.

Four consumers, one table: the dispatcher, the toolbar (`CommandButton` fills
in the chord and the gate), the palette, and the Ctrl+/ window.

## What was there before

**Dispatch — four independent `if` chains, no shared type:**

| Site | Owns |
|---|---|
| `MainMenuBar.cpp:463` `HandleShortcuts` | Ctrl+`,`/Z/Y/N/O/S/Shift+S/I via `ImGui::Shortcut(RouteGlobal)`; Ctrl+`/` separately at `:455` |
| `ViewportInput.cpp:180-270` | arrows, `F`, Shift+F, `S`, `N`, Shift+P, Shift+1..7 — inline `IsKeyPressed` |
| `SketchInteraction.cpp:672` `HandleSketchKeys` | the ~25-key Onshape sketch keymap |
| `CommandPalette.cpp:195`, `PerfHud.cpp:31` | Ctrl+K, Ctrl+Shift+F |

Plus ~15 scattered Escape/Enter/Delete checks (`PreviewFeature.cpp:574`,
`SelectionPipeline.cpp:376`, `DrawingPanel.cpp:154`, `OccurrencePanel.cpp:719`,
12 in `SketchInteraction.cpp`). `TerminalPanel.cpp`'s `ImGuiKey_` table is
key→PTY escape translation, not a shortcut — out of scope.

**Description — five copies that can drift:** menu hint strings
(`MainMenuBar.cpp:95-193`), the hard-coded `Row` tables in
`DrawShortcutsWindow` (`:277`), the per-command `shortcut` field in
`BuildCommands` (`CommandPalette.cpp:31`), 18 toolbar tooltips that spell the
chord inline (`"New Sketch (S)"`, `"Fit view (F)"`, `"Toggle planes
(Shift+P)"`, …), and this doc set.

**Coverage — 73 toolbar buttons, 18 with a chord:** Model 31/4, Sketch 28/12,
Assembly 6/1, Simulation 3/1, Drawing 5/1. All 73 funnel through one helper,
`ToolButton(icon, tooltip, enabled)` (`Toolbar.cpp:139`), plus the sketch
toolbar's `toolBtn` / `toggleBtn` / `toolGroup` lambdas.

**What already helps.** `RemoteServer.cpp:366` `ParseChord` turns
`"Ctrl+Shift+S"` into an `ImGuiKeyChord` with an error string — the parser a
settings file needs, in the wrong layer. And `BuildCommands` is most of a
command registry already: label, enablement, `std::function<void(AppState&)>`.

## The model

**Two tables, split the way `Settings` is split** — metadata in `core` so it is
unit-testable without a renderer, the executable half in `ui`, joined by a
stable command id and held together by `keys/registry_is_one_to_one`.

```cpp
// core/io/Keymap.h — no ImGui here: core does not link it, and nothing in the
// binding vocabulary should change if a vendored ImGui renumbers its enum.
enum class Context { Global, Part, Sketch, Assembly, Drawing, Simulation };
enum class Key : uint16_t { None, A, /* … */ Comma, Period, Slash, /* … */ };
struct Chord { Key key; uint8_t mods; };          // Ctrl | Shift | Alt

struct Binding {
  const char* id;       // "part.fillet" — stable; the JSON key and the RPC name
  Context ctx;
  const char* label;    // "Fillet edges"
  const char* section;  // "Modify" — grouping inside a context
  const char* chord;    // "B", or "" for bindable-but-unbound
  const char* altChord; // a FIXED second chord (only redo's Ctrl+Shift+Z)
  bool hidden;          // only the perf HUD
};
std::span<const Binding> Defaults();

Chord ParseChord(std::string_view, std::string* error = nullptr);
std::string FormatChord(Chord);

class Keymap {                    // defaults + the user's overrides
  Chord Get(std::string_view id) const;
  bool Set(std::string_view id, std::string_view chordText, std::string*);
  void Reset(std::string_view id);  void ResetContext(Context);
  std::vector<Conflict> Conflicts() const;  // same chord, SAME context
};
```

`ui/Commands.h` holds the other half — `{id, enabled, run}` — plus
`ActiveContext`, `CommandChord`, `WithChord` (the tooltip builder) and
`DispatchShortcuts`. `ui/Commands.cpp` owns the ONE `keymap::Key` → `ImGuiKey`
mapping, and its inverse for click-to-capture.

**Dispatch order is the whole point.** `DispatchShortcuts` walks the active
context's bindings first, records the chords they claim, then walks the globals
and skips any chord already claimed. The shadow is by *chord*, not by id, so a
rebind moves it with nothing else to update.

Two rules ride along, both inherited from behaviour the app already had: a
`Ctrl` chord goes through `ImGui::Shortcut(RouteGlobal)` so a focused text
field keeps its own Ctrl+Z; a bare or Shift-only chord is a plain key test
gated on `WantTextInput`, and must match the modifier state EXACTLY — without
that, `S` would also fire on Shift+S and arm two tools at once.

**Contexts are not modal sub-states.** Escape/Enter/Delete/arrows resolve
against armed tools, pending previews, plane-picking, slot-picks and text
fields. Those stay hard-coded and non-rebindable, and both the Preferences
window and the Ctrl+/ list say so rather than implying otherwise. Rebindable =
things that appear in a menu, a toolbar or the palette. CAM is a tool window,
not a toolbar mode, so it lives under Part; the two "leave this mode" buttons
(the sketch check mark, Exit Simulation) are Escape, so they have no command.

## What this fixed on the way

- **`Shift+F` fired twice in a sketch** — `ViewportInput` framed the selection
  *and* the sketch keymap armed Fillet, on one press. A context binding now
  shadows the global one; `keys/context_shadows_the_global` holds it.
- **`Ctrl+N` did strictly less than `File > New`** — the chord swapped the
  document, the menu item also cleared the part tabs, the tab folders and the
  section cuts. Two bodies, one name; now one command.
- **Arming Mirror from the keyboard ignored a preselected construction line.**
  The toolbar's `activateTool` called `MirrorToolActivated` and the keymap's
  `setTool` did not — two copies of one function that had drifted. Now
  `SetSketchTool`, once.
- **55 of 73 toolbar buttons advertised no key at all**, and the 18 that did
  spelled it in the tooltip literal. Every button now carries its live chord.

## Phases

**U12.1 — Extract, change nothing.** DONE. Both tables landed, all four
dispatch sites rewired, menu hints / palette hints / the Ctrl+/ window all
generated. The proof came out as planned: the GUI suite's ~83 literal
`KeyPress` calls stayed green with no test edited.

**U12.2 — Contexts and toolbar coverage.** DONE. `CommandButton(state, icon,
id, tooltip, active)` replaced the per-toolbar button lambdas; the 18
hand-typed `"(S)"` suffixes are gone from the literals. Every button in all
five toolbars has an id, including each flyout *member* (Ellipse, Centre Arc,
Elliptic Arc are commands, not variants of one).

**U12.3 — The missing shortcuts.** Now one table row each. The rule:

1. the mnemonic initial if free in that context;
2. else `Shift+`initial (the pattern the sketch keymap already uses —
   Shift+F fillet, Shift+G centre-rect, Shift+E extrude);
3. else **unbound but bindable** — a row showing "—". Better than inventing 40
   mnemonics nobody remembers, and the settings window makes it a 5-second fix
   for anyone who wants one. Only eight rows ship unbound (three menu-only
   commands and the five rare constraints in the sketch flyout), and
   `every command ships bound, or is on the list` names them — so a new
   toolbar button without a chord fails the suite.

Extend the Onshape keymap where it has an answer; invent only where it does
not. First pass for Part context (which has only S/N/F/Shift+F/Shift+P/
Shift+1..7 today): `E` Extrude, `R` Revolve, `W` Sweep, `H` Hole, `T` Thread,
`P` Pattern, `M` Mirror, `D` Draft.

`F` was contended three ways (Fit, zoom-to-Fit-selection, Fillet — Fusion's
`F` = Fillet collides with the SolidWorks/Onshape `F` = Fit this app already
ships and documents). Settled: `F`/`Shift+F` stay with the view, and **Fillet
is `B`** — Blend is the term OCC (`BRepFilletAPI`), NX and Creo all use, and
`B` was free in Part context. Chamfer took the bare `C` that freed up (it is
only Circle in a *sketch*), and Boolean took `U` for Union. Shipped defaults:

| Part | | | Sketch adds | | Assembly / Simulation |
|---|---|---|---|---|---|
| `S` sketch | `B` fillet | `K` shell | `.` point | `P` pattern | `I` insert |
| `E` extrude | `C` chamfer | `G` rib | `Shift+C` ellipse | `Y` glyphs | `1`-`6` mate types |
| `R` revolve | `H` hole | `U` boolean | `Shift+A` centre arc | `Shift+I` image | `1`-`3` loads |
| `W` sweep | `T` thread | `M` mirror | `Shift+M` mirror | `Shift+B` perpendicular | `R` run study |
| `L` loft | `D` draft | `P` pattern | `Shift+N` concentric | | |
| `I` pipe | `X` split | `Y` refine | | | Drawing: `I`/`U`/`E`, `D`/`X`/`Y`/`R`/`A`/`H`/`P` |
| `V` curve | `Q` simulation | `J` CAM | Primitives: `Shift+X`/`S`/`C` | Datums: `Shift+D`/`A` | |

Global tools took `Ctrl+Shift+M`/`X`/`R` (measure, section, render) so a
context's bare and Shift bindings can never shadow them.

**U12.4 — Settings.** DONE. Edit > Preferences > Keyboard: one collapsing node
per context, a filter over both the name and the key, click-to-capture
(Backspace clears, Escape cancels), an inline warning naming what a chord
collides with, and Reset per row / per context / for the lot. A conflict is
*allowed* and reported rather than refused — refusing one would strand anyone
mid-swap, who has to put a key on B before taking it off A. As predicted,
`Field`/`Member` could not absorb it, so it is its own renderer over `Keymap`.

Two pieces of session state make it safe: `AppState::keyCaptureId` gates the
dispatcher (while a capture is armed every chord means "bind me", never "run
something"), and it is cleared when the window closes or collapses — an armed
capture with no visible row would eat every key press with no explanation.

**U12.5 — RPC and docs.** DONE. `get_keymap` (every command with its context,
label, live chord, default and any conflicts; `{context: "sketch"}` narrows
it) and `set_keybinding` (`{command, chord}`, `""` unbinds, `{reset: true}`
restores), both with their `describe` row and both in the not-part-gated list.
They persist immediately rather than arming the window's debounce, for the
same reason `set_settings` does: a `--headless` instance runs no GUI main
loop.

One thing did NOT get merged: `RemoteServer`'s `ParseChord` for `key_press`
still has its own token table. It answers a different question — "press this
key", with no notion of a command — and merging it would drag ImGui-shaped
concerns into the core parser for no caller's benefit.

## Tests (all shipped)

- **Headless** (`tests/test_keymap.cpp`, 10 cases): chord round trip
  (`ParseChord`∘`FormatChord` = identity for every default); load / save /
  override / unbind / reset; an unknown `"keys"` id survives a round trip; a
  bad chord in a hand-edited file loses that one binding and not the keymap;
  **the default table has no conflicts**, a standing gate; and every command
  ships bound unless it is on the named list.
- **`keys/registry_is_one_to_one`**, in the spirit of "one row per RPC": every
  `keymap::Defaults()` id has a `ui::Command` and vice versa, and the two are
  the same size. A row in one and not the other is a command with no key, or a
  key that runs nothing — both silent.
- **`keys/context_follows_the_toolbar`**: `ActiveContext` tracks the mode.
- **`ResetAppForTest` resets the keymap to defaults** — and it came for free,
  because the keymap lives *inside* `settings`, which that function already
  reset for exactly this reason. Only `keyCaptureId` had to be added, being
  session state rather than a preference.
- **`keys/rebinding_moves_the_shortcut`**: rebind Line to Shift+J, assert the
  new chord fires, the old one does not, and the toolbar tooltip now reads
  "Line (Shift+J)". Without this the keymap could be settings-shaped
  decoration over a still-hard-coded dispatcher.
- **`keys/context_shadows_the_global`**: Shift+F in a sketch arms Fillet and
  does NOT also reframe the camera.
- The existing ~83 literal `KeyPress` calls in `GuiTests.cpp` and the
  `key_press` chords in `remote_smoke.py` / `headless_smoke.py` stay as they
  are — they exercise the defaults, which is what they were always testing.
- The Ctrl+/ window test (`GuiTests.cpp:2790`) only asserts the window exists,
  so its rows are free to become generated.

## Not in v1, and why

- **Chord sequences** (`Ctrl+K` then `S`). Needs a pending-chord state machine
  and a timeout; the flat map covers every binding the app has.
- **Per-project keymaps.** Same answer as per-project preferences: a project
  that redefines whoever opens it is wrong.
- **Rebinding Escape / Enter / Delete / arrows,** or mouse buttons. See the
  modal-state note above.
- **Presets** ("SolidWorks keys", "Fusion keys"). Cheap once the table is
  data, but it is a taste question, and it needs someone who uses those apps
  daily to be worth shipping.
