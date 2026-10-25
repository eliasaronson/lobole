# Keybindings: one command table

Every command a person can run from a menu, a toolbar or the palette is one
row in one table, and every consumer reads that table: the key dispatcher,
the toolbar's tooltips and enable gates, the Ctrl+K palette, the menu hints,
and the Help > Keyboard Shortcuts window (Ctrl+/). A binding cannot be
described in one place and dispatched from another.

## Two halves, joined by a command id

- **`src/core/io/Keymap.{h,cpp}` — the metadata.** `keymap::Context`, a `Key`
  enum of our own (no ImGui in core: `designer_tests` links `designer_core`
  alone, and the binding vocabulary must not change if ImGui renumbers its
  enum), `Chord {key, mods}`, `ParseChord` / `FormatChord`, the defaults
  table `Defaults()` and `class Keymap` (the user's overrides, `Get`, `Set`,
  `Reset`, `ResetContext`, `ResetAll`, `Conflicts`).
- **`src/ui/Commands.{h,cpp}` — the behaviour.** `{id, enabled, run}` per
  command, plus `ActiveContext`, `CommandChord`, `WithChord` (the tooltip
  builder), `RunCommand` and `DispatchShortcuts`. `Commands.cpp` owns the one
  `keymap::Key` → `ImGuiKey` mapping and its inverse for click-to-capture.

A `Binding` row is `{id, ctx, label, section, chord, altChord, hidden}`. The
id (`part.fillet`, `sketch.line`, `assembly.bom`) is stable: it is the JSON
key in `settings.json` and the name `set_keybinding` takes. `altChord` is a
fixed second chord that cannot be rebound (only redo's `Ctrl+Shift+Z`);
`hidden` keeps a row out of the palette (only the perf HUD).

## Contexts and dispatch

`Context` is `Global, Part, Sketch, Assembly, Drawing, Job, Simulation`.
`ActiveContext` is the branch `DrawToolbar` takes, so the toolbar you see and
the keys that work cannot disagree. A CAM job tab is `Job`; the two "leave this
mode" buttons (the sketch check mark, Exit Simulation) are Escape and have no
command.

`DispatchShortcuts` runs once per frame from `DrawUI`, **after every panel has
drawn**, so `io.WantTextInput` is accurate and a bare `1` typed into a
position field cannot also arm a mate. It walks the active context's bindings
first, records the chords they claim, then the globals, skipping any chord
already claimed. The shadow is by chord, not by id, so a rebind moves it with
nothing else to update (Shift+F in a sketch is Fillet and does not also zoom
to the selection).

Two input rules:

- A chord with **Ctrl** goes through `ImGui::Shortcut(RouteGlobal)`, so a
  focused text field keeps its own Ctrl+Z.
- A **bare or Shift-only** chord is a plain key test gated on
  `WantTextInput`, and must match the modifier state exactly — otherwise `S`
  would also fire on Shift+S and arm two tools at once.

While the pointer is over the terminal panel, the terminal takes the keyboard
and `DispatchShortcuts` returns early ([terminal.md](terminal.md)).

**Escape, Enter, Delete and the arrows are not commands.** They answer to
whatever is open — an armed slot, a pending preview, a plane pick, a text
field, a drag — and stay hard-coded in `ViewportInput.cpp` and the sessions.
The Preferences window and the Ctrl+/ list both say so. The mouse's
navigation drags are bindable, as settings rows rather than keymap entries
([mouse navigation](#mouse-navigation)); its clicks are not.

## Default bindings

A new toolbar button ships with a chord. The rule for choosing one: the
mnemonic initial if it is free in that context; else `Shift+` the initial;
else unbound but bindable. Global tools use `Ctrl+Shift+` so no context's bare
or Shift binding can shadow them. The unbound rows are a named list in
`tests/test_keymap.cpp`, so a new button without a chord fails the suite.

| Context | Bindings |
|---|---|
| Global | `Ctrl+N/O/S`, `Ctrl+Shift+S` save as, `Ctrl+I` import, `Ctrl+Z`, `Ctrl+Y` (and `Ctrl+Shift+Z`), `Ctrl+,` preferences, `Ctrl+K` palette, `Ctrl+/` shortcuts, `F` fit, `Shift+F` zoom to selection, `Shift+P` planes, `Shift+1`..`7` named views, `Ctrl+Shift+M` measure, `Ctrl+Shift+X` section, `Ctrl+Shift+R` render, `Ctrl+Shift+F` perf HUD |
| Part | `S` sketch, `E` extrude, `R` revolve, `W` sweep, `L` loft, `I` pipe, `V` 3D curve, `B` fillet, `C` chamfer, `H` hole, `T` thread, `D` draft, `K` shell, `G` rib, `U` boolean, `M` mirror, `P` pattern, `X` split, `Y` refine, `Q` simulation, `Shift+X/S/C` box/sphere/cylinder, `Shift+D/A/E` datum plane/axis/frame; derive, the sheet-metal tools and the four direct-edit tools (Delete/Offset/Move/Replace Face) unbound |
| Sketch | `L` line, `C` circle, `Shift+C` ellipse, `G`/`Shift+G` rectangle/centre rectangle, `A`/`Shift+A`/`Ctrl+Shift+A` arcs, `S` spline, `.` point, `Shift+T` text, `X` project, `Shift+I` image, `M` trim, `O` offset, `Shift+F` fillet, `Shift+M` mirror, `P` pattern, `D` dimension, `Q` construction, `Y` glyphs, `Shift+S` auto-constrain, `F7` slice, `Shift+E`/`Shift+R`/`Shift+H` extrude/revolve/hole; constraints `H V N B Shift+B T E Shift+N`, six rare ones unbound |
| Assembly | `I` insert, `1`..`6` mate types, `F` fastener, `X` exploded views, `M` motion, `B` bill of materials, `Q` simulation, `H` hide, `Shift+H` show all |
| Drawing | `I` add view, `U` update, `E` export, `D`/`X`/`Y` distances, `R`/`Shift+R` radius/diameter, `A` angle, `H` hole callout, `P` pattern callout, `L` part note |
| Job (CAM) | `Z` top face, `L` lay face up, `A` index face, `T` look down the spindle, `P` pocket, `C` profile, `D` drill, `B` edge, `E` engrave, `Shift+C` whole, `Shift+T` face, `R` surface, `W` walls, `Shift+R`/`Shift+O` rotary finish/rough, `Shift+S` surface rough, `Shift+E` rest, `G` generate, `S` simulate |
| Simulation | `1` fixed, `2` force, `3` pressure, `R` run |

`Keymap.cpp` is the source of truth; `get_keymap` reports the live table.

## Rebinding

Overrides live in `settings.json` under `"keys"`, and only rows that differ
from a default are written, so a later change to a default still reaches
anyone who never touched it. An unknown id in the file survives a round trip;
a bad chord in a hand-edited file loses that one binding, not the keymap.

**Edit > Preferences > Keyboard** is its own page over `Keymap` (a keymap
is not a scalar settings row): one collapsing node per context, a filter over
name and key, click-to-capture (Backspace clears, Escape cancels), an inline
warning naming what a chord collides with, and Reset per row, per context and
for all. A conflict — the same chord twice in the same context — is allowed
and reported rather than refused, because refusing would strand anyone
mid-swap who has to put a key on B before taking it off A.

`AppState::keyCaptureId` gates the dispatcher while a capture is armed (every
chord means "bind me") and is cleared when the window closes or collapses, so
an armed capture with no visible row cannot eat key presses.

Over RPC: `get_keymap` (every command with context, label, live chord,
default and conflicts; `{context: "sketch"}` narrows it) and `set_keybinding`
(`{command, chord}`; `""` unbinds; `{reset: true}` restores). Both persist
immediately rather than through the window's debounce, because a
`--headless` instance runs no GUI loop. `key_press` has its own chord parser
in the remote layer: it answers "press this key", which has no notion of a
command.

## Tests

- `tests/test_keymap.cpp`: `ParseChord`∘`FormatChord` is the identity for
  every default; load / save / override / unbind / reset; unknown ids
  survive; a bad chord loses only itself; **the default table has no
  conflicts**; every command ships bound or is on the named list. Runs with
  `XDG_CONFIG_HOME` pointed at a scratch dir.
- GUI: `keys/registry_is_one_to_one` (every `Defaults()` id has a
  `ui::Command` and vice versa — a row in one and not the other is a command
  with no key or a key that runs nothing), `keys/context_follows_the_toolbar`,
  `keys/rebinding_moves_the_shortcut` (the new chord fires, the old one does
  not, the tooltip reads the new chord), `keys/context_shadows_the_global`.
- `ResetAppForTest` resets the keymap with the rest of `settings`, and
  clears `keyCaptureId`.

## Mouse navigation

Which drag orbits, pans, zooms and rolls the view is four rows on
**Preferences > Navigation** — `nav_orbit`, `nav_pan`, `nav_zoom`,
`nav_roll` — each a LIST of gestures, so one action can sit on several. The
table is `core/io/MouseNav.{h,cpp}` (no ImGui, like the keymap): a gesture
is a button — `middle`, `right`, or `alt+left` — and at most one more held
key (`ctrl`, `shift`, or `alt` on the other two), eleven in all. A bare left
button never navigates: it is picking, box select and every handle, so the
left button navigates only with Alt held. In the JSON a row is an array of
those strings; any case and modifier order parse, and the canonical text is
what is stored.

Refused, naming what is accepted, in `set_settings` and in the file alike:
a string outside the eleven (a bare `left` included), a row that is not a
list, and a gesture on two rows — there is no order between two actions to
settle it. In the file a bad row is ignored as any bad value is (the default
stays, with a warning in the log); a clash is visible only across rows, so
there all four go back to the defaults together. The Preferences window
cannot make one: a gesture another row holds is shown disabled, naming that
row.

`nav_preset` is one more row: `lobole` (the defaults below), `solidworks`,
`fusion`, `onshape`, `blender`, or `custom`. Choosing one writes the four
rows; editing a row by hand makes it whichever preset the rows now are, or
`custom` (`settings::SyncNavPreset`; a request naming a preset AND a row gets
the preset with that row changed). Each preset is that program's default
navigation from its own documentation, cited beside its row in
`MouseNav.cpp`:

| preset | rotate | pan | zoom | roll |
|---|---|---|---|---|
| `lobole` | middle, right, Alt+right, Alt+left | Ctrl+middle/right/Alt+left | Shift+middle/right/Alt+left | Alt+middle |
| `solidworks` | middle | Ctrl+middle | Shift+middle | Alt+middle |
| `fusion` | Shift+middle | middle | Ctrl+middle | — |
| `onshape` | right, Alt+right | Ctrl+right, middle | — | — |
| `blender` | middle | Shift+middle | Ctrl+middle | — |

Two are approximations, said so beside their rows: Fusion's zoom drag is
Ctrl+**Shift**+middle, two keys, so it is bound as Ctrl+middle — which is
what Ctrl+Shift+middle resolves to here, so Fusion's own gesture zooms; and
Onshape's Alt+right is a constrained (turntable) rotate, bound to the
ordinary orbit.

`NavMap::ActionFor(button, ctrl, shift, alt)` is asked every frame of a drag
(`HandleViewportInput`), so a key pressed mid-drag switches the action; with
several keys held, Ctrl wins, then Shift, then Alt. A press starts a drag on
any button something is bound to, and a held combination that is bound to
nothing moves nothing. What is not navigation stays fixed and wins where it
did: a right RELEASE short of the drag threshold is a click (the occurrence
menu, the sketch dimension menu), Shift+click aligns to a face, Ctrl+click
multi-selects. The drawing sheet asks the same table and pans on EVERY
navigation drag: it has nothing to rotate or roll and zooms on the wheel, so
under the defaults a middle drag pans with any keys held, as it always did. The cheat-sheet (View > Viewport Controls) is
written from the table and the keymap.

Beside the gestures: `nav_orbit_speed`, `nav_pan_speed`, `nav_zoom_speed`
(multipliers on `Camera`'s rates, 0.25-4, refused outside) and
`nav_invert_wheel` (the wheel zooms out away from you, in the viewport and
on the sheet). The remote `viewport_drag` takes `ctrl`/`shift`/`alt` and
`scroll` `ctrl`/`shift`, so a script can make every gesture.

Tests: `tests/test_mousenav.cpp` (every gesture round-trips; **the
defaults resolve every button and key combination as the hard-coded code
before the table did**, transcribed in the test; every preset is
clash-free, never binds a bare left and is its own name back; unknown
gestures, a bare left, a clash, an unknown preset and an out-of-range speed
are refused by name; the rows survive a save and a bad file); GUI
`viewport/every_preset_drives_its_gestures` (every gesture under every
preset does what the preset binds, from an empty spot, and an unbound one
does nothing), `viewport/wheel_inverts_and_speeds_scale`,
`assembly/right_click_menu_under_a_right_orbit_preset`,
`drawing/the_sheet_pans_on_navigation_drags`.

## Not built

Chord sequences (`Ctrl+K` then `S`), per-project keymaps (a project must not
redefine whoever opens it), rebinding Escape / Enter / Delete / arrows or
mouse clicks, keyboard presets ("SolidWorks keys"), and gestures with two
held keys.
