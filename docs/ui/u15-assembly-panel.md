# U15 — the assembly Model panel holds structure, not modes

## The problem

An assembly tab's Model dock is one scrolling column of five unrelated
things: the occurrence tree, Mates, Relations, Configurations, Exploded
views, Motion, and a Bill of materials header. At the dock's shipping width
the reader meets "Base (all shown)", "None", "Add study" stacked one under
the next with nothing to say which of them is a list of document objects and
which is a thing you are *doing right now*. Three of the seven are neither
read nor edited in the panel — they are driven in the viewport, with a
slider or a gizmo, while the panel they live in scrolls them off screen.

The part tab already answers this and the assembly tab does not: part
configurations have their own dock tab behind Model (`kPanelConfigurations`),
but `DrawConfigurationsPanel` returns early when there is no `state.document`,
so on an assembly the tab is empty and the same capability was re-implemented
inline in the occurrence panel.

## The rule this round applies

**A section that is the document's structure stays docked. A section that is
a viewport mode floats.**

- Structure — the occurrence tree, Mates, Relations, the BOM: read, selected
  from, and edited in place. It stays in the Model dock.
- Structure, wrong dock — Configurations: a named variant of the document,
  browsed and switched exactly as a part's configurations are. It moves to
  the Configurations dock tab that already exists.
- Viewport modes — Exploded views and Motion: both pose the occurrences and
  are driven by a scrub slider or a drag gizmo while you watch the model.
  They become floating tool windows opened from the assembly toolbar, the
  shape every other mode in this app already has (Section, Measure, the CAM
  verbs, Fastener).

The tool-window slot is one-at-a-time by design (`BeginToolWindowFrame`), and
here that is a feature rather than a constraint: an exploded view and a motion
study both place occurrences, and holding both open at once has no meaning.

## What gets built

**1. Configurations move to their own tab.** `DrawConfigurationsPanel` grows
an assembly arm holding what `DrawConfigsSection` does today — the Base/named
selector, the name field, the per-occurrence / per-mate / per-relation
suppression checklists and the mate-DOF holds. `DrawUI` calls it outside the
`state.document` branch so the tab is present on an assembly tab.
`DrawConfigsSection` is deleted from `OccurrencePanel.cpp`.

**2. Exploded views become a tool window.** A new `state.explodeToolOpen`,
an `assembly.explode` command with its keymap row, and a `###tb_asm_explode`
toolbar button; `DrawToolWindow` dispatches to `DrawExplodeToolWindow`.
Contents are today's section: the None/named list, Add, the name field, the
Factor slider, Auto-explode, Delete, and the "drag the gizmo" hint, plus a
Close button — the Section tool's rule rather than Esc, and for its reason.
Closing is not clearing the view: `state.activeExplodedView` stays what it
was, because the explode is a display state the render path composes and a
person who closes the window is done editing, not done looking.

**3. Motion becomes a tool window.** `state.motionToolOpen`, an
`assembly.motion` command, a `###tb_asm_motion` button, dispatch in
`DrawToolWindow`, a keymap row. Contents are today's section: the study list, Add (with its
refusal text), the driver combo, the range line, the `WhyNot` refusal, the
Pose scrub, the Run/Delete row, the in-flight progress bar and the validation
verdict. The scrub stays *in* this window rather than becoming a second
bottom-centre transport like CAM's: motion has no play, the window is small,
and a second floating slot buys nothing until it does.

**4. What is left in the Model dock** is the occurrence tree, Mates,
Relations and the Bill of materials header — a list of what the assembly *is*,
and nothing that needs the viewport to make sense.

## Files

- `src/ui/OccurrencePanel.cpp` — `DrawConfigsSection` deleted;
  `DrawExplodedSection` / `DrawMotionSection` become
  `DrawExplodeToolWindow` / `DrawMotionToolWindow` behind
  `BeginToolWindowFrame`, declared in `OccurrencePanel.h` beside
  `DrawMateToolWindow`.
- `src/ui/ConfigurationsPanel.cpp` — the assembly arm.
- `src/ui/PreviewFeature.cpp` — two more branches in `DrawToolWindow`.
- `src/ui/AppState.h` — `explodeToolOpen`, `motionToolOpen`.
- `src/ui/Commands.cpp` — `assembly.explode`, `assembly.motion`, each
  enabled only on an assembly tab and each closing the other.
- `src/core/io/Keymap.cpp` — a row per command, so the palette can name them
  and a chord can reach them (X and M, in the Assembly context).
- `src/ui/Toolbar.cpp` — two buttons on the assembly bar, highlighted while
  open like `###tb_section`.
- `src/ui/UI.cpp` — `DrawConfigurationsPanel` outside the part-only branch.

## Verification

- The existing assembly GUI tests address these rows by their `###` ids
  (`###explode_view_<id>`, `###motion_t`, `###cfg_mate_sup_<id>`, …). Every id
  is kept, so a test's path changes only in its window prefix — from the Model
  panel to `//<Title>###ToolWindow/` or the Configurations panel. Each of
  those tests is repointed, which is also the check that the row is still
  reachable where it moved to.
- New GUI checks inside the two tests that already cover these surfaces: the
  toolbar button opens the window; opening Motion while Exploded is open
  replaces it rather than stacking; Close leaves the active exploded view
  applied. The configuration test already asserts the solve followed each
  click, and now does it from the Configurations tab.
- `gui_smoke`'s no-window-overlaps-the-viewport assertion (U4) covers the two
  new windows for free, since they go through `BeginToolWindowFrame`.
- `tools/gui-tests-xvfb.sh` in full, plus `tools/test-fast.sh`.
- No format change: nothing here touches serialization, so no `kVersion`
  bump and no golden.

## Follow-on — the mate list is a list

Found while looking at what was left in the Model dock: the mate rows were
the last list in the app still selecting one at a time, through
`SelectMate` rather than the shared gesture, inside a `PushID(int)` scope
that no test could address.

- The rows take `ListRowClicked` with `Kind::Mate` (`###mate_row_<id>`), so
  Ctrl and Shift work there as they do everywhere else.
- They also take the new **`clickClears`** option: clicking the row that is
  the whole selection clears it. The selected-mate editor opens inside the
  panel and has nothing to close it with, so the row you just clicked is
  where a person aims to put it away. Opt-in, not the universal rule — most
  lists auto-select what they just created, and clicking that row to confirm
  it must not make it vanish.
- **Delete reaches mates.** `DeleteSelection` took occurrences only on an
  assembly tab, so the key did nothing with a mate picked and the row's own
  button was the one way out. Mates and occurrences now go together in one
  snapshot, and the row's button is that same verb with a counted label.
- Test: `assembly/a_mate_row_closes_and_deletes`.

Relations are the remaining list that Delete does not reach.

## A third floating mode: Simulation (S1, 2026-09-16)

The rule this round set — structure stays docked, viewport modes float —
took its third tenant: a study on the assembly
([../simulation/assembly-studies-plan.md](../simulation/assembly-studies-plan.md)).
`###tb_asm_simulation` toggles `state.simulationToolOpen`, drawn from the
same one-at-a-time slot as explode and motion (`DrawSimulationToolWindow`,
StudyPanel.cpp), mutually exclusive with them; Close leaves simulation mode.
Its body is the part tab's Studies panel over `ui::StudyHost`, plus a
participants table — every top-level occurrence with a role combo and, on a
rigid row, "Clamp here". Test: `sim/an_assembly_study_clamps_where_the_support_touches`.
