# Settings — a home for preferences, and a rule for where things live

> Status: written 2026-08-17 as a design because GPU device selection had
> nowhere to live; **built 2026-08-20**, with the gaps found on review folded
> in below (they are marked *added on review*).

## Why now

Nothing in this app remembers what a *person* prefers. Render quality is
session state that `set_render_options` mutates and forgets; `showPlanes` and
friends are flags on `AppState`; the dock layout lives in `imgui.ini`; and
project-level choices like `commit_geometry` correctly live in the project.
The gap is the middle: preferences that belong to the user, survive a restart,
and apply to every project.

GPU selection was the forcing function. Rendering *used* to call
`ImGui_ImplVulkanH_SelectPhysicalDevice` (prefers discrete) while FEM compute
ranked its own (needs `VK_EXT_shader_atomic_float`); neither was controllable,
and on a dual-GPU machine the right answer is the user's, not ours. Both now
go through one shared rule (`common/DevicePreference.h`) that honours the
preference — see the two findings at the end of "The window".

## The rule: three buckets

1. **Project settings** — belong to the model and travel with it in git.
   `commit_geometry`, sheet defaults, the material assigned to a body. These
   stay in the project's `index.json`. *Not this doc.*
2. **User preferences** — belong to the person, survive restarts, apply to
   every project. GPU device, render quality, navigation style, autosave
   interval. *This doc.*
3. **Session state** — belongs to the moment. Dock layout, camera, armed
   tools, the section plane. Stays exactly where it is; never persisted here.

The test for bucket 2: *would the user be annoyed to set it again tomorrow,
and would they expect the same answer in every project?* If a project could
reasonably disagree, it belongs in bucket 1.

## Storage

`$XDG_CONFIG_HOME/designer/settings.json`, falling back to
`~/.config/designer/settings.json` — a sibling of the `onshape.env` already
there. **Config, not state**: `~/.local/state/designer` (where the autosave
lives) is for things the app can regenerate; this is for things the user
chose. The path helper mirrors `Autosaver::StateDir()`.

```json
{ "settings_format": 1, "gpu_device": "", "shadows": true, "ssao": "half", … }
```

Rules that matter more than the schema:

- **Written with `WriteTextAtomic`** (already in `Serialization.cpp`). An
  interrupted save must never leave a truncated file that silently resets
  someone's preferences.
- **Unknown keys are preserved on write, not dropped.** An older build must
  not delete a newer build's settings just by running once. This is the
  opposite of the recipe format, where an unknown key is a hard error —
  because there, silence would corrupt geometry; here, silence would corrupt
  the user's choices.
- **Absent file = defaults**, no complaint. **Corrupt file = defaults**, a log
  line, and the bad file renamed `.bak` rather than overwritten — the one
  copy of a hand-edited config is not ours to destroy.
- **No live reload.** Read at startup, write on change (debounced ~1 s so a
  slider drag is one write).
- *(added on review)* **A save re-reads and merges first.** Preserving unknown
  keys covers an old build meeting a new file; it does nothing about two
  instances running at once, which is the normal case here — a windowed
  `dev-watch` on 8799 while agents drive `headless-preview` on scratch ports.
  Without a merge the last writer wins and an agent silently reverts the
  human's preferences. Save therefore re-reads the file, overwrites only the
  keys this instance actually changed, and writes that.
- *(added on review)* **A newer `settings_format` is read, not refused.** The
  recipe format refuses a future version because guessing would corrupt
  geometry; here the whole design is that unknown keys survive and missing
  ones default, so refusing would only lock a user out of their own
  preferences. The number is for humans reading the file: nothing in the
  current design requires it to move, and a bump means "we changed what an
  existing key MEANS", which is the only case worth a migration.
- *(added on review)* **No `kVersion` bump, no golden file.** This is not the
  document format. The repo's reflex — a format change bumps `kVersion` and
  adds a golden pair — does not apply, and saying so here saves the next
  person the question.

## Shape in code

One `struct Settings` with its defaults in the struct itself and exactly one
live copy at `AppState::settings`. Nothing else caches a copy — two sources of
truth for a preference is how "it reverted when I opened a second project"
bugs happen.

*(added on review)* It lives in **`src/core/io/Settings.{h,cpp}`**, not
`src/ui/`, for one reason: `designer_tests` links `designer_core` alone, so
anything in `src/ui/` cannot be unit-tested. The store is pure IO and belongs
in core anyway; only the window is UI. (Same call as `ProjectSaveTarget` the
same day.)

Worth the small effort up front: drive the field list, the JSON round trip and
the window rows from **one table** (name, member, default, tier, blurb), so
adding a setting is one line rather than three edits that can drift. A dozen
settings do not need reflection machinery, but they do need a single list.

## Applying: three tiers, and the window says which

- **Live** — the next frame reads the field: shadows, SSAO, shadow map, font
  size. No Apply button anywhere in this window. (Font size is live but not
  free: the atlas is rebuilt between frames, so the change ASKS for a rebuild
  rather than just moving the number — see the findings below.)
- **Next time** — autosave interval (rearms the timer on the next tick).
- **Restart** — the *render* GPU. The swapchain and every renderer are built
  at startup, so the row states it and shows the device currently in use
  beside the one chosen, making a pending change visible rather than a lie.
  Note the asymmetry worth documenting on that row: the FEM **compute**
  context is recreated per run, so the same setting takes effect there
  immediately.

## The window

Edit ▸ Preferences (Ctrl+,) opens one floating window, `###SettingsWindow` —
not a dock tab; it is not part of the workspace. Collapsing-header sections in
one scroll, generated from the table's `section` column, so a section exists
exactly when a setting is in it. **Shipped: Appearance** (font size),
**Performance** (GPU, shadows, SSAO, shadow map) and **General** (autosave,
**terminal panel**).
The design's Viewport & Navigation, Simulation and Advanced sections have no
tenants yet and are therefore absent rather than empty.

Three properties it must have, all learned the hard way this month:

- **Every control carries a stable `###id`, and no `PushID` scopes.** A hidden
  label or an ID scope leaves a widget reachable only by the numeric id
  `ui_items` prints — which is exactly what made the Hole type combo, the Loft
  section combo and the study material combo unreachable to scripts and tests
  (docs/ui/gui-coverage.md).
- **Each row explains itself** in one muted line: what it costs, what it
  affects. A settings window that does not explain its settings moves the
  confusion rather than removing it.
- **A reset that does not need the JSON.** Shipped as one "Reset all to
  defaults" at the foot of the window, beside the config file's path; the
  design's per-section resets wait for a window big enough to need them.
  Resetting keeps unknown keys — they belong to another build, not to us.
- *(learned building it)* **The View menu's copies are gone.** Render quality
  and font size lived there as session flags that reset every launch; they are
  preferences now, and one control per setting is the only way the two ends
  cannot drift. What stays in View is genuinely session state: Show Planes,
  Show Element Names, Viewport Controls.
- *(learned building it)* **A restart-tier row must not promise what a restart
  cannot deliver.** Choosing a GPU that cannot present to a window aborted
  startup outright ("no WSI support on physical device 0") — recoverable only
  by hand-editing settings.json. Startup now scores such devices ineligible,
  falls back, and records WHY, and the row shows that reason instead of
  "restart to use it". Note `glfwGetPhysicalDevicePresentationSupport` is NOT
  sufficient: it asks the X server, which said yes for a device Mesa then
  refused. Only `vkGetPhysicalDeviceSurfaceSupportKHR` against a real surface
  is trustworthy, so selection creates a throwaway one.
- *(added on review)* **Every width is measured, never a pixel literal.**
  `CalcTextSize` plus the style's own padding — the style scales with the
  display's content scale *and* with the font-size row this very window owns.
  The History panel shipped with `-70.0f` and `58.0f` the same week and its
  buttons hung off the edge on a HiDPI screen; a window that is nothing but
  label-and-control rows, and that can change its own metrics, cannot afford
  that mistake.

No search field in v1; a dozen rows do not need one.

## The script surface

Two RPCs, not one per setting: `get_settings` returns the whole struct;
`set_settings {…}` validates, applies, persists, and returns the new state.
That keeps the assistant and the human on the same interface (roadmap goal 5)
and makes every setting testable headlessly.

`set_render_options` stays, as a thin alias over the same fields — it is used
by scripts and the MCP today, and it already mutates three of what become
settings.

*(added on review)* Both new methods need a row in the `describe` table beside
their dispatch chain, or the introspection test fails — the repo enforces
"one row per RPC".

## Tests

- **`ResetAppForTest` must reset `settings` to defaults.** Otherwise the
  developer's own config decides whether a GUI test passes, and CI and the
  desk disagree. (Precedent from this month: a function-local `static bool`
  for the shortcuts window leaked into five later tests.)
- **Tests point `XDG_CONFIG_HOME` at a scratch dir**, so no test run can write
  the developer's real preferences.
- *(added on review)* **Per SHARD, not per suite.** `gui-tests-xvfb.sh` now
  runs four app processes at once, and already gives each its own
  `XDG_STATE_HOME` because the recent-projects list is machine-global and
  broke `file/open_recent`. `XDG_CONFIG_HOME` needs the same treatment the
  moment settings exist, or the shards race on one `settings.json`.
- Headless: defaults → set → save → load → identical; unknown keys survive a
  round trip; a corrupt file falls back to defaults and leaves a `.bak`.
- GUI: `settings/toggle_persists_across_a_reload`, and one asserting a
  restart-tier row *says* it needs a restart instead of pretending to apply.

## First tenants

GPU device (render + compute), shadows, SSAO quality, shadow-map size,
**UI font size**, autosave interval. Deliberately short: the deliverable of v1
is the store, the rule and the window — not the list.

*(2026-08-20)* **Terminal panel** joined General, and is the first setting
whose job is to keep a feature *out* of the way: it is off by default, and
turning it on is what makes the panel and its right-hand dock node exist at
all ([terminal-panel.md](terminal-panel.md)). Live tier — it appears on the
next frame — and turning it off ends the running session rather than hiding
it, because a process kept alive behind a closed panel is one nobody can see
or stop.

*(added on review)* **Font size was the omission.** `View > Font Size` already
exists and writes `AppState::uiFontScale`, which nothing persists — it resets
to 1.0 on every launch. It passes this doc's own test more cleanly than
anything else in the app: an accessibility choice nobody wants to re-make
daily, and identical in every project. It is a first tenant, and the View
submenu now points here rather than owning it.

Navigation style is dropped from v1: there is no discrete flag to persist —
trackpad-friendliness is Alt+drag being accepted everywhere, not a mode. The
remote port is dropped too; it is a launch flag (`--remote`), and a preference
that silently disagrees with the command line would be worse than neither.

## Not in v1, and why

- **Keybindings.** Needs its own model (conflicts, chords, per-mode maps); a
  row of text fields would be worse than the current fixed set.
- **Per-project overrides of user preferences.** A project that forces a GPU
  on whoever opens it is wrong.
- **Synced/cloud settings.** No account system, and no reason to invent one.
- **Live reload of the file.** Watching config files earns a class of
  half-applied-state bugs for a benefit nobody asked for.

## Open question to settle when it is built

**Display units (mm/inch).** The model is always millimetres, so a unit is a
display choice — bucket 2. But a drawing's dimensions are part of the
document, so their units are bucket 1. The likely answer is "both, and they
are different settings", which is worth deciding deliberately rather than
discovering. Flagged for the units workstream, not for this window's v1.
