# U13 — Chrome that fits: overflow, elision, units, geometry (2026-08-24)

> Status: **SHIPPED 2026-08-24**, all six phases (dc738e0, f3d9eef, f9dc6d4,
> f9860ee, 4774803, de7713f). The open decision below was settled
> document-level; format v64. What the work found that this plan did not
> predict is in "What changed on contact" at the end — read that before
> trusting the per-phase sections, several of which describe a problem that
> turned out to be a different one.

A screenshot audit of the running app on real projects (`scratch/marsliden-dock.json`,
17 documents) found nothing wrong with the *theme* and a lot wrong with the
*fit*. `Theme.cpp` is disciplined — one value ramp, one accent, real DPI
scaling. What it is applied to does not respect its own constraints: text
clips instead of eliding, two bars overflow without saying so, and numbers are
printed at a precision the geometry does not have.

This is the same class as U9/U10 (dogfooding residue), and it gets the same
treatment: fix the instance, then make the fix the only way to do it, then
gate it.

## Findings

| # | Finding | Evidence | Site |
|---|---|---|---|
| 1 | Two commands share one glyph in one bar | Split and Section both `ICON_FA_SCISSORS` | `Toolbar.cpp:196`, `:270` |
| 2 | Draft uses the universal overflow chevron | `ICON_FA_ANGLES_RIGHT` (`»`) mid-bar | `Toolbar.cpp:268` |
| 3 | Two near-identical grids, unrelated meanings | Datum Plane `TABLE_CELLS_LARGE` vs Toggle Planes `TABLE_CELLS` | `Toolbar.cpp:241`, `:279` |
| 4 | Document tab bar overflows silently | plain `Button`+`SameLine` in a `NoScrollbar` window; at 17 docs the `+` is off-screen | `UI.cpp:443-655` |
| 5 | Panel tabs unreadable at the default split | `0.17` = 218 px; six tabs render `Mo…`, `His…`, `CA…` | `DockLayout.cpp:186`, `PanelTitles.h` |
| 6 | Text clips, never elides | `Gangway [assembly] (fixed`, `1305294.05 r`, `None. Use the mate tools in the to` | `FeatureTreePanel.cpp:479` and peers |
| 7 | The elision fix exists twice, privately | `MassText()`/`CellTooltip()` (BOM bug), `VolumeText()` — neither reachable from the tree | `OccurrencePanel.cpp:362`, `:373`; `AppActions.cpp:2061` |
| 8 | Fixed decimals on 7-digit numbers | `1305294.05 mm³` claims 10 nl precision from a tessellated solid | `FeatureTreePanel.cpp:479`, `:62` |
| 9 | A comment describes behaviour the code lacks | `VolumeText` promises `"2 100 mm³" / "0.34 cm³"`, body is `%.0f mm³` | `AppActions.cpp:2061` |
| 10 | Window geometry never persists | `glfwCreateWindow(1280, 720, …)`, every launch | `main.cpp:1208` |
| 11 | Tool window has no elevation | 6%-white border, no shadow, flat on the 3D scene | `PreviewFeature.cpp:109` |
| 12 | Preferences scrolls on open | height `em * 28` is under its own content | `SettingsWindow.cpp:383` |

Not findings, checked and deliberate: the Preferences window's font-unit
sizing and its rejection of `AlwaysAutoResize` (`SettingsWindow.cpp:370-379`),
`ControlWidth()`'s cap on combo width, and `Cylinder = ICON_FA_DATABASE`
(the glyph is a cylinder drum whatever Font Awesome calls it).

---

## U13a — Icon collisions

Findings 1-3. One file, no new concepts.

| Command | Now | Proposed | Why |
|---|---|---|---|
| `part.split` | `SCISSORS` | `TABLE_COLUMNS` | a rect divided in two; Section keeps scissors as the more-used cut |
| `part.draft` | `ANGLES_RIGHT` | `MOUNTAIN` | a tapered face, and stops impersonating an overflow control |
| `view.toggle_planes` | `TABLE_CELLS` | `EYE` / `EYE_SLASH` by state | a visibility toggle should look like one, and show what it will do |

All three replacements verified present in `deps/fonts/IconsFontAwesome6.h`
and unused in `src/`. `EYE` is already `sketch.toggle_glyphs`
(`Toolbar.cpp:503`) — that is not a collision but the consistency we want: one
glyph, one *meaning*, across bars. The bug is one glyph meaning two things in
**one** bar.

`sketch.trim` keeps `SCISSORS` (`Toolbar.cpp:422`) for the same reason — a
different bar, and cutting is what it does.

**Gate:** `icons/no_duplicate_glyph_per_toolbar` — walk the drawn toolbar
buttons per context (the `##ToolbarRow` window enumeration already exists at
`GuiTests.cpp:5336`), take each label's text before `###`, assert the set is
unique. A future button that reuses a glyph fails at test time instead of
at dogfood time.

**Cost:** ~1 h including the gate.

---

## U13b — Overflow that admits it exists

Findings 4-5. Two bars, one principle: a strip that cannot show everything
must say so.

**Document tab bar** (`UI.cpp:443`). Put the tab strip in a child sized
`avail.x - plusWidth` with `ImGuiWindowFlags_HorizontalScrollbar`; draw `+`
after `EndChild()` + `SameLine()`. Two results: `+` is always reachable, and
the scrollbar makes the overflow *discoverable* — today the bar does scroll,
but with `NoScrollbar` nothing indicates the other 14 documents exist.
`PartTabsBarHeight()` grows by `style.ScrollbarSize` only while overflowed,
measured from the previous frame — the same trick `##ToolbarRow` uses at
`DockLayout.cpp:56-60`.

**Panel tabs** (`PanelTitles.h`). Drop the words: tab = icon, tooltip = name,
and the panel draws its own name as a heading in its body (it has none today,
so nothing is lost). Not a width-driven swap — at 218 px "Configurations"
alone does not fit, so a dynamic rule would sit in icon-only mode permanently
while adding hysteresis at the threshold. The `###id` split already in
`PanelTitles.h` is what makes this safe: every `//Model/...` ref,
`focus_panel("Variables")` call and dock lookup addresses the id, not the
label.

**Gates:** `chrome/plus_button_reachable` — open a project with 17 documents,
assert the `+##addmenu` item rect lies inside the viewport.
`chrome/panel_tabs_not_truncated` — assert no left-dock tab reports
`label_truncated` at the default split.

**Cost:** ~half a day.

---

## U13c — Elevation and fit

Findings 11-12. Small, independent, no new interfaces.

**Tool window** (`PreviewFeature.cpp:109`): `WindowBg = kSurface` instead of
`kBg` and `Border` at ~0.14 alpha, for that window only. ImGui ships no shadow
primitive (`ImGuiCol_BorderShadow` is vestigial and set transparent at
`Theme.cpp:37`); the usual workarounds are nested rounded rects with decaying
alpha, a pre-blurred 9-slice, or a value step. The value step costs three
lines and stays consistent with a palette already built out of value steps. If
it reads as insufficient, the draw-list version is ~15 lines and can follow.

**Accept/Cancel** (same window): Cancel is red today, and red is also the
error colour for failed features in the tree. Cancel is not destructive.
Accept → `kAccent`, Cancel → `kSurface`; red stays for errors and deletes.

**Preferences** (`SettingsWindow.cpp:383`): raise the height constant from
`em * 28` until the default content fits without a scrollbar (the 90%/85%
viewport caps already there keep it honest on a small screen). Nothing else in
that window changes — the font-unit sizing, the centring, and the
`AlwaysAutoResize` refusal are all deliberate and documented in place.

**Cost:** ~2 h.

---

## U13d — TextFit: one way to draw text that might not fit

Findings 6-7. The fix has already been written twice, privately, in response
to the BOM bug — and the feature tree's Bodies list, which shows the same
failure, reaches neither. Promote, do not invent.

New `src/ui/TextFit.h`:

- `TextEllipsis(const char* text, float maxWidth = 0)` — measure, truncate,
  append `…`, and register a hover tooltip carrying the full string.
- `SelectableEllipsis(...)` — the same for tree and list rows.

Both wrap ImGui's own `RenderTextEllipsis` (`deps/imgui/imgui_internal.h:3716`)
— the function its tab bars already use, so the look matches the rest of the
UI for free.

Then sweep the call sites: `FeatureTreePanel.cpp:479` and `:62`,
`OccurrencePanel` (adopting `TextFit` and deleting its local `CellTooltip`),
the Mates and Configurations lists, and the Bodies rows.

**Gate:** `chrome/no_clipped_label_without_tooltip` — the remote layer already
computes `label_truncated` (`RemoteServer.cpp:519`), so a GUI test can walk the
left panel and assert that anything truncated has a tooltip. Same shape as
`defaults/*` and `keys/every_toolbar_button_is_a_command`.

**Cost:** ~1 day, most of it the sweep.

---

## U13e — Units: one system, chosen once

Findings 8-9. CAD convention, and the right answer: the document carries a
unit system, chosen once, and everything displays in it. Fusion, SolidWorks
and Onshape all work this way; SolidWorks additionally lets length, mass and
angle be set independently, but each is then *fixed*, never per-value.

New `core/Units.h`: a `UnitSystem` (length, mass, angle) plus
`FormatLength / FormatVolume / FormatMass / FormatAngle` reading from it.
`MassText()`'s g/kg split moves here as the mass formatter.

**Invariant:** display units never touch stored values. Everything remains mm
internally; `Units` is a presentation layer. Anything else turns a display
preference into a geometry change.

Two rules do most of the work whichever units are chosen:

- **Thousands separators.** `10 080 000` is readable; `10080000` is not.
- **Significant figures, not fixed decimals.** 4-5 sig figs is honest and
  never overflows a column; `%.2f` on a 7-digit number is false precision.

Derived units get their own fixed choice — mm for length but grams for a screw
is normal; choosing mm³ and then printing 10 080 000 of them is not, so mm
parts default to cm³ or L for volume.

Fix finding 9 in passing: either make `VolumeText` do what its comment
promises or delete the comment. It currently describes behaviour the body does
not have.

**Format:** a document-level unit system is a `kVersion` bump — v63 → **v64**
plus one golden (`tests/golden/v64-units.json/.expect.json`), per the rule in
`CLAUDE.md`. See the open decision below.

**Gates:** unit tests on the formatters in `designer_tests` (sig figs,
separators, the g/kg and mm³/cm³ boundaries) — no GPU needed.

**Cost:** ~1-1.5 days including the format bump, golden, and call-site sweep.

---

## U13f — Window geometry survives a restart

Finding 10. Last because it touches `main.cpp` and needs a carve-out done
right.

Store `window_w/h/x/y/maximized` in `settings.json` (the one-table field
system already handles this shape), restore at create, write debounced on
move/resize. First run — nothing saved — derives from
`glfwGetMonitorWorkarea` at ~80%, centred, clamped to a floor. Work area, not
`glfwGetVideoMode`: the latter includes taskbars and docks.

**Validate on restore.** A monitor unplugged since the last run leaves the
saved rect off-screen; clamp it into some *current* monitor's work area before
applying. This is the same reasoning `SettingsWindow.cpp:374` gives for
centring Preferences rather than remembering its position.

**The carve-out that matters:** `--hidden` and `--run-gui-tests` keep the fixed
1280×720. `GuiTests.cpp:5303` is a test whose whole premise is the toolbar
overflowing *at 1280×720*, and `gui-tests-xvfb.sh` / `headless-preview.sh` both
create that screen. A host-derived size there would make layout tests depend on
the developer's monitor.

**Gate:** the clamp is a pure function — unit-test it in `designer_tests`
against a synthetic monitor list (saved rect fully off-screen, partially
off-screen, monitor smaller than saved size).

**Cost:** ~half a day.

---

## Order, and why

**U13a → U13b → U13c → U13d → U13e → U13f.**

- **U13a first**: one file, mechanical, and it comes with a gate that stops
  the next button from reintroducing it. Cheapest possible proof the
  workstream is moving.
- **U13b second**: the only *reachability* bug in the set — a document you
  cannot click is worse than one that reads badly.
- **U13c** is a style pass with no new interfaces; it can slot anywhere, and
  sits here because it is short.
- **U13d before U13e**: elision fixes clipped *names* too, which units never
  will (`Gangway  [assembly]  (fixed`). Units then reduce how often elision
  has to fire.
- **U13e** is the largest and the only one carrying a format bump, so it wants
  a clear run.
- **U13f last**: smallest user-visible surface per hour spent, and the test
  carve-out means it should not be rushed alongside anything else.

U13a-c can ship as one commit each in an afternoon. U13d-f are day-scale.

## Gates added

| Gate | Suite | Guards |
|---|---|---|
| `icons/no_duplicate_glyph_per_toolbar` | gui | U13a |
| `chrome/plus_button_reachable` | gui | U13b |
| `chrome/panel_tabs_not_truncated` | gui | U13b |
| `chrome/no_clipped_label_without_tooltip` | gui | U13d |
| `units/format_*` | headless | U13e |
| `window/geometry_clamp` | headless | U13f |

Four GUI tests against a suite that runs 150 in 80 s — within budget.

## Out of scope

Deliberately not in U13, each its own decision:

- **A light theme.** The palette is dark-only by construction; a second one is
  a workstream, not a polish item.
- **Toolbar grouping with captions.** 32 unlabelled icons in one bar is a real
  legibility problem, but the fix is an information-architecture change
  (groups, or a ribbon), not a swap.
- **Sketch mode dimming the model**, and the unhaloed red/green U/V axis
  labels over bright geometry. Real, but they belong with a sketch-visuals
  pass.
- **The default planes** rendering as three large translucent quads that mud
  together on an empty document.
- **`Rib = ICON_FA_BARS_STAGGERED`** — weak, no better candidate in the set.

## Open decision

**Where does the unit system live?**

- **Document-level (recommended).** Matches every CAD tool: a part authored in
  inches displays in inches for whoever opens it. Costs a `kVersion` bump to
  v64 and one golden — routine here.
- **App-level in `settings.json`.** Ships a day sooner, no format bump. But it
  makes display units a property of *the reader*, which is wrong the moment a
  project is shared — and F9 shipped collaboration, so projects are shared.

Recommendation: document-level. Flagging it because it is a format commitment,
not because the trade is close.

---

## What changed on contact

Six phases shipped as planned. Five things were not what the plan said they
were, and they are the part worth keeping.

**The part-tabs bar was never the height it asked for.** U13b was scoped as
"pin the + and add a scrollbar". Reserving room for the scrollbar produced a
bar 12 px taller showing nothing, which turned out to be two separate bugs
underneath. `PartTabsBarHeight()` returned a number the window never got:
ImGui clamps every non-child window to `style.WindowMinSize` (180x60 in
Theme.cpp, there so a dock node cannot be dragged to a sliver), and it does
so AFTER any size constraint, so `SetNextWindowSizeConstraints` cannot get
under it either. The bar asked for 36 px, was given 60, and `BeginDockspace`
went on reserving 36 — the bottom ~24 px of every docked panel has been
sitting underneath the tab bar. And the 36 itself was short of one button
row; the tabs simply drew past it, which was harmless until a child had to
fit a scrollbar too.

**`GetScrollMaxX()` reads zero in a strip whose height depends on it.** It
comes from the previous frame's `ContentSize`, and the height depended on the
answer, so the two deadlocked: no extra height, therefore a squeezed strip,
therefore nothing that ever reported an overflow. Measuring the run from the
cursor is a fact about the current frame and has no such loop.

**`label_truncated` is not a clipping signal.** The U13d gate was designed
around it. It reports that the test engine's 32-byte `DebugLabel` buffer was
cut — correlated with long labels, unrelated to whether anything was clipped
on screen. The gate that replaced it asserts the user-visible contract
instead: a row too long for the panel is still addressable by id, and still
readable on hover.

**Two of the Preferences findings were already deliberate.** The window sizes
in FONT units, caps at 90%/85% of the viewport, centres on every appearing
rather than FirstUseEver, and explicitly rejects `AlwaysAutoResize` — with
the reason written in place (the blurbs wrap to the content region, so
content-driven width ran off the screen). `ControlWidth()` caps the combos on
purpose too. All that survived was one height constant. Reading before
planning would have caught it; reading before EDITING did.

**Elevation by value step was not available.** One step up the surface ramp
lands exactly on kSurface, which is what every input, frame and button in the
tool window already is — the fields would have dissolved into the window
behind them. It got a drawn shadow instead. The same collision explains why
the part-tabs scrollbar was invisible on arrival: `ImGuiCol_ScrollbarGrab`
was kSurface, the same value as the buttons directly above it. Moving the
grab one step up the ramp fixes every scrollbar beside a control.

## Follow-ups this round did not take

- **`free_port()` is racy, in ~15 test scripts.** It binds port 0, reads the
  port, CLOSES the socket, and the app binds it later — so a parallel `ctest
  -j` can hand the same port to two tests. `fem_smoke` failed once this way
  during U13e and passed alone immediately after. Pre-existing, unrelated to
  this workstream, and not in the known-flakes list; the fix is a retry
  around `wait_for` rather than a better port picker.
- **Leftover scratch instances.** Four `--headless` instances were squatting
  8795-8798 from earlier sessions, and one GUI instance on 8917 had been
  pegging a core since the day before. `headless-preview.sh --stop` exists;
  nothing reaps an instance whose session ended.
- The toolbar still wraps two icons onto a second row at 1280 px, and the
  "Out of scope" list above is otherwise untouched.
