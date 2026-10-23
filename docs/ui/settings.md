# Settings: user preferences and where things live

## Three buckets

1. **Project settings** belong to the model and travel with it in git
   (`commit_geometry`, sheet defaults, a body's material, the document's unit
   system). They live in the project, not here.
2. **User preferences** belong to the person, survive a restart and apply to
   every project. They live in `settings.json`, described here.
3. **Session state** belongs to the moment: the dock layout (`imgui.ini`), the
   camera, armed tools, the section plane, View > Show Planes. Never written
   to `settings.json`.

The test for bucket 2: would the user be annoyed to set it again tomorrow, and
would they expect the same answer in every project? If a project could
reasonably disagree, it is bucket 1. The same rule puts the recent-projects
list and the autosave in the **state** directory, not the config one: the app
writes them and can regenerate them; the user did not choose them.

## Storage

`$XDG_CONFIG_HOME/lobole/settings.json`, falling back to
`~/.config/lobole/settings.json`; on Windows `%LOCALAPPDATA%\Lobole\config`.
All three per-user directories (config, state, cache) resolve through
`RuntimePaths::User{Config,State,Cache}Dir`, which on Windows gives
`%LOCALAPPDATA%\Lobole\{config,state,cache}` rather than `%TEMP%`, which the
OS sweeps.

```json
{ "settings_format": 1, "gpu_device": "", "shadows": true, "ssao": "half", … }
```

- **Written atomically** (`DocumentIO::WriteTextAtomic`): an interrupted save
  never leaves a truncated file that silently resets someone's preferences.
- **Unknown keys are preserved**, kept verbatim in `Settings::unknown`, so an
  older build cannot delete a newer build's settings by running once. (The
  document format does the opposite — an unknown key there is an error —
  because there silence would corrupt geometry.)
- **A save re-reads the file and merges**, overwriting only the keys this
  instance changed. Two instances at once is the normal case (the human's
  windowed instance plus agents' scratch instances); without the merge the
  last writer would revert the other's preferences.
- **Absent file = defaults.** **Corrupt file = defaults**, a log line, and the
  bad file set aside as `settings.json.bad-<YYYYmmdd-HHMMSS>` (never over an
  earlier one: a `-2` is added) rather than overwritten; the windowed app
  says so once, in a modal naming where it was kept (`settings::Load`'s
  note).
- **A newer `settings_format` is read, not refused.** Unknown keys survive and
  missing ones default, so refusing would only lock a user out of their own
  preferences. The number moves only if an existing key changes meaning.
- **No live reload.** Read at startup; written on change, debounced 1 s
  (`kSaveDebounceSeconds`) so a slider drag is one write.
- Not the document format: no `kVersion` bump and no golden when a setting is
  added.

## Shape in code

`struct settings::Settings` in `src/core/io/Settings.{h,cpp}` — in core, not
`src/ui/`, because `designer_tests` links `designer_core` alone and the store
must be unit-testable. Defaults live in the struct. Exactly one live copy
exists, `AppState::settings`; nothing else caches one.

One table, `kFields`, drives the field list, the JSON round trip and the
window rows, so adding a setting is one row. A row's `section` is the
Preferences page it is drawn on; a section string nobody has used yet is a new
page, with no change to the window. The window no longer needs a section's
rows together, but the table should read the way it draws, and a test holds
that.

Two members are deliberately not `kFields` rows: `keys` (the keymap
overrides, [keybindings.md](keybindings.md)) and `window` (the main window's
last geometry — remembered state nobody wants to type, restored through
`FitToMonitors` in `core/io/WindowGeometry.h`, which drops a rect no current
monitor can show).

## Tiers: how a change arrives

Each row says which tier it is, so a pending change is visible:

- **Live** — the next frame reads it.
- **Next time** — the next save, new part or next run of the thing.

Nothing waits for a restart: the one setting that did, the render GPU, now
rebuilds the device in place ([the GPU choice](#the-gpu-choice)), and
`test_settings.cpp` checks every row is one of the two.

## The current settings

| Section | JSON key | Tier | Notes |
|---|---|---|---|
| Appearance | `ui_font_scale` | Live | 85 / 100 / 120 / 140 %; the font atlas is rebuilt between frames |
| Appearance | `lock_lights_to_model` | Live | off: the light rig follows the camera; on: fixed in the world where it stands at the home view ([rendering](../rendering/README.md#the-light-rig)) |
| Navigation | `zoom_to_cursor` | Live | on: the wheel zooms toward what the pointer is over; off: toward the centre of the view ([viewport](README.md#viewport)) |
| Performance | `gpu_device` | Live | `""` = automatic (prefers discrete); a change that moves the first rung rebuilds the device in place |
| Performance | `shadows` | Live | |
| Performance | `ssao` | Live | `off`, `half`, `full` |
| Performance | `idle_throttle` | Live | rest at ~10 Hz when nothing is happening (docs/rendering) |
| Performance | `shadow_map` | Live | 1024 or 2048 |
| Sharing | `git_user_name`, `git_user_email` | Next time | who a commit is by; empty = git's own configuration |
| General | `terminal_panel` | Live | off by default ([terminal.md](terminal.md)) |
| General | `autosave_interval` | Next time | seconds, 0 disables |

Render quality and font size are not in the View menu: one control per
setting is the only way two copies cannot drift. What stays in View is session
state (Show Planes, Show Element Names, Viewport Controls).

## The GPU choice

Rendering and FEM compute pick their device through one rule
(`common/DevicePreference.h`) that honours `gpu_device`. A device that cannot
present to the window is scored ineligible and skipped, with the reason
recorded, and the row shows that reason under "Rendering on: <device>".
Only `vkGetPhysicalDeviceSurfaceSupportKHR` against a real surface is
trustworthy (`glfwGetPhysicalDevicePresentationSupport` asks the X server,
which can say yes for a device the driver then refuses), so selection creates
a throwaway surface.
The preferred device is the first rung of
[the device ladder](../rendering/README.md#the-device-ladder): one that fails
to start falls to the next, and the window says which and why.

A change is Live: at the next frame boundary, when the ladder's first rung is
another device than the one in use, the device is
[rebuilt in place](../rendering/README.md#rebuilding-the-device) onto it --
the document, the undo history and the open tabs carry on. A path-traced
render, a motion video or a test-engine capture holds the device; the switch
waits for it, and the row says "Switches when the render finishes".
`set_settings {gpu_device}` answers once the switch has run, with `renderer`
saying where it landed. The FEM compute context is created per run, so the
same choice applies there on the next run.

## Safe mode

A preference can be what crashes a start — a `gpu_device` whose driver dies,
a font scale — and so can a layout that trips a docking assert in
`imgui.ini`. Safe mode starts the window without either:

- **default settings**, never written: `settings::SetSessionOnly` makes every
  `Save` succeed without touching the disk, so Preferences and `set_settings`
  work on as usual and last the session;
- **the default dock layout**, in a scratch `<state>/safe-mode-imgui.ini`, so
  the user's `imgui.ini` is neither read nor written;
- no GPU preference (the defaults have none) and **no ray tracing**;
- **no file opened** from the command line; the crash notice still offers
  the autosaves.

It is asked for with `--safe-mode` or `LOBOLE_SAFE_MODE=1` (`0` or `1`; any
other value is refused), and entered **by itself** when the last two
windowed starts died before their first frame
([crashes and stalls](../architecture.md#when-the-renderer-cannot-start)).
A banner over the viewport says why ("the last 2 starts crashed before the
window drew"), where the user's own files are, and offers two ways out:
**Restart normally**, and **Set them aside and restart**, which renames
`settings.json` and `imgui.ini` to `*.bad-<stamp>` (kept, not deleted) and
starts again on the defaults. A `Safe mode` notice sits at the right of the
menu bar, `ping` answers `safe_mode: true`, and the path-traced still says
it is off in safe mode. Never under `--gui-tests`, which already run on
defaults, and refused with `--headless` and `--offscreen`, which have no
layout.

## The window

Edit > Preferences (`Ctrl+,`) opens one floating window,
`Preferences###SettingsWindow` — not a dock tab; it is not part of the
workspace. It shows one page at a time: a category list on the left, the
selected page on the right under its title, and a footer under both.

**The pages** come from `ui::PreferenceCategories()`, the window's only
statement of their order:

- one per table section, in the order `kFields` first names it — Appearance,
  Performance, Sharing, General — each listing its rows;
- then the hand-written pages in `kHandPages` (SettingsWindow.cpp): **Keyboard**
  ([keybindings.md](keybindings.md)), **Machines**, the user's CAM machine
  profiles, and **Print profiles**, the user's printing allowances.

Each row in the list is a glyph and the name (`###cat_<name>`). A section's
glyph comes from a small table by name; an unknown section gets the generic
gear rather than none. The page shown is `AppState::settingsCategory`,
session state: the window reopens on the page it was left on until the app
closes. `ui::OpenPreferences(state, "Machines")` opens it on a page, to the
front — the CAM panel's `Preferences › Machines` buttons and the Print
process section's `Preferences › Print profiles` use it.

**Search.** A field above the category list (`###set_search`) filters by row
label and blurb. While it holds text the page side shows the matching rows of
every table page, each group under its page's name, and the rows are the real
controls. A library page matches by item name, and a match opens that item on
its page (`###find_<page>_<name>`); a hand-written page whose name matches is
offered as a row that opens it (`###find_page_<name>`), so "keyboard" is not a
dead end.

**The footer.** `Reset <page> to defaults` (`###set_reset_category`,
`settings::ResetSection`) on a table page — it leaves every other page, the
keymap and the unknown keys alone. A library page has none: its items are the
user's data, not preferences with a default. Then "Reset all to defaults"
(`###set_reset_all`, which keeps unknown keys) and the config file's path.

### Library pages

A library is a list of the user's own things, each stored as its own file and
written on every edit — there is no Save button to forget. Its page puts the
list on the left (New, Duplicate and Delete as icon buttons over it, the rows
as `ListRow`s) and the selected item's form on the right, so a long form (a
machine's tool table) scrolls in its own pane instead of pushing the list off
screen. `DrawLibraryPage` in SettingsWindow.cpp lays it out from a
`LibraryPage`: an id prefix (`mach` gives `###mach_new`, `###mach_dup`,
`###mach_delete`, `###mach_item_<name>`), the blurb, the names, the selection,
and callbacks for create, duplicate, delete and the form.

Adding one is two things: a draw function that fills a `LibraryPage`, and a
`kHandPages` row naming it, its glyph, and `items`/`open` so search finds its
items. There are two:

- **Machines** (`mach`): stored in `~/.config/lobole/machines`, one file per
  machine; a CAM setup picks one and keeps a copy
  ([cam/gui.md](../cam/gui.md#preferences--machines)).
- **Print profiles** (`prof`, `DrawPrintProfilesPage`): stored in
  `~/.config/lobole/print_profiles`, one file per profile; a printed part
  picks one and keeps a copy
  ([print profiles](../tolerances/README.md#print-profiles)). New starts from
  the built-in table of the selected profile's pair. The form is name,
  process and material, conditions, the STL chord deviation, the layer height
  and the air over another part (`z_gap_mm`), the bed's x, y and z
  (`bed_mm`, `###prof_bed_x`/`_y`/`_z`, three zeros = not stated; each an
  `ExprInputDouble`) and the rows as a stretched table, never wider than the pane — kind, from,
  to (0 = and up), allowance and source, each field a flat numbered id
  (`###prof_row_allow_<i>`) so a script reaches one without the table's id
  scope.

### Layout rules

- Every control has a stable `###id` and there are no `PushID` scopes, so
  scripts and tests can address every row ([testing.md](testing.md)). The
  panes are child windows, and a child window starts its own id scope, so
  each pane hashes its items from the window's seed (`BeginPane`): a control
  is `//###SettingsWindow/###set_ssao` whichever pane, page or search result
  draws it. The one exception is a machine's tool rows, `$$<T number>/###libdia`.
- Each row explains itself in one muted line: what it costs, what it affects.
- Every width is measured (`CalcTextSize` plus the style's padding, or the
  font size), never a pixel literal, because the window can change its own
  font size. The category list and a library's list are as wide as their
  longest name, within a cap.
- A form puts each label above its control, so a long label wraps with the
  window rather than running off it.
- Sized in font units, capped to the viewport, centred when it appears, and
  not `AlwaysAutoResize` (the blurbs wrap to the content region, so
  content-driven width ran off the screen). It fits a 1280 × 720 viewport at
  the default font size and at 140 %, and no pane scrolls sideways at 100 %
  or 150 % (`settings/each_page_shows_its_own_rows` checks every page).

## The script surface

`get_settings` returns the whole struct; `set_settings {key: value, …}`
validates, applies, persists immediately and returns the new state.
`set_render_options` remains as a thin alias over the render fields. Keys use
the JSON names above.

## Tests

- Headless: defaults → set → save → load is identical; unknown keys survive a
  round trip; a corrupt file falls back and is set aside, twice in a second
  without one replacing the other; a session-only save writes nothing.
- `ResetAppForTest` resets `settings` to defaults before every GUI test, so
  the developer's own config never decides whether a test passes.
- Nothing a test runs may write the developer's real preferences, and
  `set_settings` persists immediately, so any test that touches one is a
  writer. The GUI runner gives each shard its own `XDG_CONFIG_HOME`; the
  Python smokes launch through `tests/smoke_app.py`'s `launch_app()`, which
  sets it; and `tests/config_isolation_lint.py` fails a script that spawns
  the binary itself.
- Headless: every table row is on exactly one page and no page is empty
  (`every preference is on exactly one page`); a fixture table with a new
  section gets a new page (`a new section is a new preferences page`);
  `a section resets alone`.
- GUI: `settings/the_gpu_row_switches_live`,
  `app/device_rebuild_keeps_the_session` (the switch onto another device and
  back, where one can show the window),
  `settings/a_change_reaches_the_renderer`,
  `settings/each_page_shows_its_own_rows` (each page's rows and no others, and
  no sideways scroll at 100 % and 150 %), `settings/search_finds_rows_on_any_page`,
  `settings/a_page_resets_alone`, `cam/the_machine_button_opens_on_machines`
  (and search opening a machine), `cam/machines_are_kept_in_preferences`,
  `tolerance/print_profiles_are_kept_in_preferences` (new, duplicate, a
  rename and a refused one, a row edit read back from disk, delete).

## Not built

Per-project overrides of user preferences (a project must not force a GPU on
whoever opens it), synced settings, live reload of the file. Search does not
reach into the Keyboard page's bindings; its own filter does that. The remote
port is a launch flag (`--remote`), not a preference: one that silently
disagreed with the command line would be worse than neither.
