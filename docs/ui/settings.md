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

`$XDG_CONFIG_HOME/designer/settings.json`, falling back to
`~/.config/designer/settings.json`; on Windows `%LOCALAPPDATA%\Designer\config`.
All three per-user directories (config, state, cache) resolve through
`RuntimePaths::User{Config,State,Cache}Dir`, which on Windows gives
`%LOCALAPPDATA%\Designer\{config,state,cache}` rather than `%TEMP%`, which the
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
  bad file renamed to `.bak` rather than overwritten.
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
window rows, so adding a setting is one row. Rows are grouped by section and
the grouping is load-bearing (the window draws one header per section); a
test holds it.

Two members are deliberately not `kFields` rows: `keys` (the keymap
overrides, [keybindings.md](keybindings.md)) and `window` (the main window's
last geometry — remembered state nobody wants to type, restored through
`FitToMonitors` in `core/io/WindowGeometry.h`, which drops a rect no current
monitor can show).

## Tiers: how a change arrives

Each row says which tier it is, so a pending change is visible:

- **Live** — the next frame reads it.
- **Next time** — the next save, new part or next run of the thing.
- **Restart** — the render GPU: the swapchain and renderers are built once at
  startup. The row shows the device in use beside the one chosen. The FEM
  compute context is created per run, so the same choice applies there
  immediately.

## The current settings

| Section | JSON key | Tier | Notes |
|---|---|---|---|
| Appearance | `ui_font_scale` | Live | 85 / 100 / 120 / 140 %; the font atlas is rebuilt between frames |
| Appearance | `lock_lights_to_model` | Live | off: the light rig follows the camera; on: fixed in the world where it stands at the home view ([rendering](../rendering/README.md#the-light-rig)) |
| Performance | `gpu_device` | Restart | `""` = automatic (prefers discrete) |
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
present to the window is scored ineligible at startup and skipped, with the
reason recorded, and the row shows that reason instead of "restart to use it".
Only `vkGetPhysicalDeviceSurfaceSupportKHR` against a real surface is
trustworthy (`glfwGetPhysicalDevicePresentationSupport` asks the X server,
which can say yes for a device the driver then refuses), so selection creates
a throwaway surface.

## The window

Edit > Preferences (`Ctrl+,`) opens one floating window,
`Preferences###SettingsWindow` — not a dock tab; it is not part of the
workspace. One collapsing header per section, generated from the table, then
two that are not table rows: **Keyboard** ([keybindings.md](keybindings.md))
and **Machines** — the user's CAM machine profiles, a list of structs each
stored as its own file in `~/.config/designer/machines`, written on every
edit; a CAM setup picks one and keeps a copy. At the foot: "Reset all to defaults"
(`###set_reset_all`, which keeps unknown keys) beside the config file's path.

- Every control has a stable `###id` and there are no `PushID` scopes, so
  scripts and tests can address every row ([testing.md](testing.md)).
- Each row explains itself in one muted line: what it costs, what it affects.
- Every width is measured (`CalcTextSize` plus the style's padding), never a
  pixel literal, because the window can change its own font size.
- Sized in font units, capped to the viewport, centred when it appears, and
  not `AlwaysAutoResize` (the blurbs wrap to the content region, so
  content-driven width ran off the screen).

## The script surface

`get_settings` returns the whole struct; `set_settings {key: value, …}`
validates, applies, persists immediately and returns the new state.
`set_render_options` remains as a thin alias over the render fields. Keys use
the JSON names above.

## Tests

- Headless: defaults → set → save → load is identical; unknown keys survive a
  round trip; a corrupt file falls back and leaves a `.bak`.
- `ResetAppForTest` resets `settings` to defaults before every GUI test, so
  the developer's own config never decides whether a test passes.
- Nothing a test runs may write the developer's real preferences, and
  `set_settings` persists immediately, so any test that touches one is a
  writer. The GUI runner gives each shard its own `XDG_CONFIG_HOME`; the
  Python smokes launch through `tests/smoke_app.py`'s `launch_app()`, which
  sets it; and `tests/config_isolation_lint.py` fails a script that spawns
  the binary itself.
- GUI: `settings/a_restart_row_says_restart`,
  `settings/a_change_reaches_the_renderer`.

## Not built

Per-project overrides of user preferences (a project must not force a GPU on
whoever opens it), synced settings, live reload of the file, a search field,
per-section resets. The remote port is a launch flag (`--remote`), not a
preference: one that silently disagreed with the command line would be worse
than neither.
