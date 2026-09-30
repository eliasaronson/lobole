# CAM in the GUI: the features the chess round built, and a panel like the others

The chess round added a part's configuration to a job part, the Engrave
operation and one operation at several rotary indices. None of the three
can be driven from the GUI:

- A job part's configuration is set only by `machine_part` /
  `add_job_part`. The panel's Parts list shows a name, and "Add part" adds
  the active configuration, so the chess undersides job (one piece part, a
  configuration per rank) cannot be built by hand.
- Engrave is in an operation's type list with Width and Depth, but its
  target is a sketch entity, and the job tab draws no sketches to pick.
- Several indices are an "at A" text box of typed angles. The app already
  knows which index turns a floor to the spindle (`IndexFrameFor`,
  `SolveIndexA`), and the GUI does not use it.

The CAM panel also breaks the rule every other panel keeps
(docs/ui/panels.md): a docked panel holds the document's structure as rows,
and editing happens in the floating tool window. The part and assembly
panels are one Model tree: plain `SectionHeader` groups with counts, and
`ListRow`s with a glyph, a name and a dim tail. Create verbs live on the top
toolbar, the rest in a row's right-click menu, and settings open in the tool
window. The CAM panel has none of that:

- a setup combo with loose "+" and "Flip" text buttons;
- four sections holding inline forms (Parts with placement numbers, Stock
  and safety);
- a status line;
- the operations of the one visible setup, and an "Add operation" text
  button.

Setups are a combo rather than a level of the tree, so a job's structure,
setup by setup, is never visible at once.

## 1. The CAM panel is a Model tree

```
CAM
Parts 2                                   (SectionHeader; + on hover)
  [part] board                     oak    (tail: configuration, else material)
Setups 2                                  (SectionHeader; + on hover)
  v Top                            [ok]   (status glyph as the tail)
      [pocket] Dark squares      T1 6 mm
      [profile] Square corners   T6 2 mm
      [chamfer] Frame chamfer   T8 12 mm
      [lock] Vise                          (fixtures, under their setup)
  > Face after epoxy               [!]
Tools 8                                   (SectionHeader)
  [flat] T1  6 mm flat 2F           6 mm
```

- **Setups are rows**, each a disclosure over its operations and fixtures.
  The expanded, selected setup is the active one, which replaces the combo.
  Its right-click menu holds Rename, Flip (today's button), Duplicate and
  Delete, plus Add operation (today's `AddOperationType`) and Add fixture.
  Double-click opens the setup's settings in the tool window: machine, held
  in, stock and safety, the rotary. That is today's "Stock and safety" form
  and the setup-level fields, moved rather than rewritten.
- **The status line becomes the setup row's tail glyph**: not generated,
  ok, warnings, failed, pending. A click on it opens the Generate window, as
  the line does today.
- **Parts rows** keep the missing-part red. The right-click menu holds
  Configuration (a submenu of the part's configurations, `###cam_part_cfg_*`),
  Place and Remove. Double-click edits the placement in the tool window.
  "Add part" moves to the Parts header's `+` and offers each part, with a
  submenu of its configurations when it has them.
- **Tools** stay as they are, already `ListRow`s, under a header of their
  own.
- **One gesture set.** Rows go through `ListRow` and the shared list
  selection (docs/ui/selection.md), with the same click, double-click,
  right-click and Delete as the feature list. The operations keep drag to
  reorder, within their setup.

Widget ids change, and the GUI tests that open the combo or the sections
move with them. `cam/panel_widgets_have_distinct_ids` keeps its gate.

## 2. A job part's configuration

The Parts row's Configuration submenu and the add menu's submenu set
`JobPart.configuration` through one `ui::SetJobPartConfiguration`, in one
undo step. It re-derives what depends on the geometry, as a re-clamp does
(`RederiveDepths`). A configuration the part no longer has shows red with
the reason, like a missing part. The same function serves the RPC's
`set_job_part {configuration}`, if there is no such door yet.

## 3. Engrave from the viewport

- The job tab draws each job part's **sketches that hold text or open
  curves**, laid out through the part's placement in the setup frame, as
  thin lines in the sketch colour. Pickable, not editable.
- An **Engrave verb** (`job.engrave`) on the toolbar, in the 2.5D group:
  pick a label or a curve and the op is derived (`DeriveOperation` for
  engrave: width from the cap height, depth from the V-bit). A pick of a
  second label adds a target to the same op, as a pocket's floors do.
- Without a V-bit in the setup, the verb says so and offers the tools that
  are, as a pocket verb refuses a tool that cannot reach.

## 4. Indices from picks

- On a setup held in the rotary, a verb whose pick is a face that does not
  face the spindle at the setup's index **solves the index** for it
  (`SolveIndexA` on the setup-frame normal) instead of refusing it as a
  wall. A second pick at another index adds that index to the same op.
  Four crenel floors, picked, give one op at `[0, 90, 180, 270]`.
- In the op's settings the indices are **chips**: `A0  A90  A180  A270`,
  each with an x. The typed field stays behind a small "edit" for an angle
  nobody can pick.

## 5. Board generation: section once per height

72% of the board's top setup (92 s) is `SectionLoops`, an OCC boolean of
the whole board at a Z. The corner pass has 32 wall targets and sections
the board again for each, at the same depths, and the pocket does the same
for its floors. A per-generate memo in `GenerateContext`, keyed by (framed
bodies, Z), sections each height once. The bodies are the framed ones for
this setup, so the memo never outlives a generate, and a changed model is
a new generate.

## Files

- **1 and 2:** `src/ui/CamPanel.cpp` (and a new `CamTree.cpp`, if the
  panel is split), `src/ui/JobActions.{h,cpp}`, `src/app/guitests/GuiTestsCam.cpp`.
- **3 and 4:** `src/ui/CamTool.cpp` (the verbs and picks), the job tab's
  overlay (`SceneOverlays` / `SketchOverlay`), `ui::JobSketchCurves`, the op
  settings' index chips in `DrawCamOpSettings`.
- **5:** `src/core/cam/CamStrategies.cpp`, `CamGeometry.cpp`,
  `CamToolpath.h` (the context).

1 and 2 are one agent, 3 and 4 another, and 5 a third. The overlap is
`DrawCamOpSettings` (the chips), which the second agent owns.

## Docs

Fold into docs/ui/panels.md#cam-job-tab-the-cam-panel (the tree),
docs/cam/gui.md (verbs, picks, chips), docs/cam/operations.md#engrave (the
verb), and docs/cam/rotary.md#one-operation-at-several-indices (picked
indices).

## Verification

- **GUI tests:**
  - build the chess undersides job by hand: add the piece part twice in two
    configurations and cut one with a pocket;
  - engrave a picked label and generate it with no refusal;
  - pick four crenel floors on a rotary blank, getting one op whose chips
    are the four indices;
  - the panel shows two setups expanded at once, and the ids gate passes.
- **Speed:** the board's top setup regenerates in under a third of its
  92 s, and the moves are unchanged against the unmemoized run (the same
  test the outline cache has).
- The headless and GUI suites, the CAM-off build, and screenshots of the
  panel beside the part and assembly panels, looked at.
