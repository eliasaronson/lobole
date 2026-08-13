# U9 — GUI parity for script-only features (design)

> Status: **SHIPPED 2026-08-13** — one round
> (C1 `8762fef`, C2 `e58b535`, C3 `475358d`, C4 `7a083fa`, C5 `d4d7606`,
> C6 `7785857`, acceptance-run fixes `dd3608f`, docs this commit).
> gui suite 99/99; headless 571/571.
> Decisions recorded at sign-off: (a) the Hole tool picks a FACE first and
> then takes centre clicks, creating the point-sketch itself (both the sketch
> and the Hole appear in the tree); (b) scope includes the remote-driving
> fixes, since "drive the GUI like a human" is a first-class use of it.
> Parent: [plan.md](plan.md) U9.

## Problem (GUI-only dogfood, 2026-08-13)

Every earlier dogfooding round drove the app over the script API. This one
modelled a part with nothing but clicks and screenshots — the interface the
project's own goal points at ("the LLM uses the same interface as humans").
It did not get far:

- **Hole, Shell, Draft, Split and Refine had no creation path in the GUI.**
  They appear in `src/ui/` only in `FeatureEditor.cpp`, which *edits* a
  feature that already exists. Hole matters most, because it is **not** sugar
  for an extruded cut: its tool solid is built from primitives
  (`CreateCylinder` + `CreateCone`), so counterbore/countersink cannot be
  expressed as a cut at all, and it stamps its feature id into every element
  it creates (`NamePrimitiveElements(tool, GetId())`), which is what makes
  drawing `hole_callout` text work.
- **Draft could not have worked even with a button**: `DraftFeature::Execute`
  rejects faces whose `ref.featureId != bodyId`, and no UI code ever wrote
  `bodyId`.
- **Extrude was reachable only from inside a sketch session**
  (`###tb_sk_extrude`, Shift+E). Finish the sketch with ✓ and nothing in the
  interface could extrude it.
- **The onboarding text named two affordances that do not exist**: "Create >
  Box" (that menu was deleted when the toolbar took over) and "Double-click a
  plane" (plane picking has always been arm-then-click). The assembly panel
  told a GUI user to call `add_occurrence` — an RPC — while the button for it
  sat on the toolbar.
- **The automation surface could not stand in for a human**: no hover, no way
  to enumerate widgets, `"ui action failed (see app log)"` with nothing ever
  written to that log, and `viewport_click` fractions measured against the
  whole OS window while `describe` called them viewport coordinates.

## The machinery every new tool reuses

`BeginPreviewFeature(state, feature)` (`PreviewFeature.cpp`) does all of it:
adds the feature to the Document (so the preview is live and pickable),
`SeedSlotsFromSelection`, `SelectFeature`, `ArmNextUnfilledSlot`.
`AcceptPendingFeature` re-adds it through an `AddFeatureCommand`, so accept is
one undo step. **A new tool is therefore a `Create*` action ending in one
`BeginPreviewFeature` call**, plus rows in three switches.

`PickSlot` (`FeatureEditor.h`) is **append-only** — tests address slot rows as
`###slot<enum value>`. The three switches (`SlotSpecsFor`, `SlotFilled`,
`ApplySlotItem`) have no `default:`, so a new enumerator is a deliberate
`-Wswitch` build break: that break *is* the per-tool checklist.

## The Hole state machine

A sketch session and a feature preview cannot coexist — `DrawToolWindow`
gives a session absolute priority — so the tool is staged:

```
[idle] --tb_hole--------------------------> A: face pick
       --tb_hole + planar face selected---> B            (A skipped)
A --click planar face--> B      (BeginSketchOnFace, tool forced to Point)
A --click curved face--> A      (stays armed + actionMsg; nothing created)
B --click--> B                  (Point tool places centres)
B --Enter / ✓ / Shift+H--> C    (BeginSketchHole; refuses with 0 points)
B --Esc--> [idle]               (CancelHoleSession — tiered, below)
C --Accept--> committed (one undo step)      C --Esc--> the sketch is kept
```

Three decisions are load-bearing:

**`SketchPurpose` is a parameter of `BeginSketchOn{Plane,Datum,Face}`, not
ambient state.** `BeginSketch` starts with `state.sketchEdit =
SketchEditState{}`, so a flag written by the caller *before* the call is wiped
— exactly across the A→B transition that needs it. Passing it in and applying
it after that reset is the only placement that survives. Keeping it inside
`SketchEditState` also means the twelve existing reset sites (tab switch, undo
of the sketch, opening another sketch, File/New, Shift+E …) end the Hole
session for free, with no new bookkeeping.

**Hole demands a genuinely planar face.** `FaceSelector`'s normal is an
average over the tessellation, so the existing sketch-on-face path will
happily build a tangent plane on a cylinder. `BeginHoleSketchOnFace` gates on
`CADShape::FacePlane` instead — the B-Rep answer — and a curved pick stays
armed with "that face is curved - pick a flat one". The pre-existing New
Sketch path is deliberately left alone.

**Cancel is tiered.** Escape at the bottom of the ladder discards the sketch
the tool created *only* when `HasUserGeometry(sketch)` is false — a new pure
query that ignores entities generated from `ExternalGeom::genIds`, since a
face-attached sketch's projected outline is not the user's work. Anything the
user drew is kept, and the discard is a `RemoveFeatureCommand`, so Ctrl+Z
brings it back.

`###tb_sk_hole` (Shift+H) drills an *existing* point sketch. It deliberately
does **not** set the purpose, which is what makes `purpose == Hole` a proof
that the session created its own sketch — the fact the tiered cancel relies
on.

## What each commit shipped

- **C1 `8762fef` — prerequisites.** A pending preview is added straight to the
  Document rather than through a command, so switching tabs stranded it in the
  stashed document forever, off the undo stack (pre-existing, affected every
  preview tool): `CancelPendingFeature` now runs before `StashActive`. Plus
  `###tb_sk_finish` on the sketch ✓ (it had no stable id, so the flow below
  could not be tested), and Escape while picking a plane resets the whole
  `SketchEditState` instead of leaking `reattachSketchId`.
- **C2 `e58b535` — Extrude/Revolve on the model toolbar.** `Candidates()`
  never emits `Kind::Feature`, so a sketch can never be a viewport pick; the
  action resolves it like `CreateRib` does (the selected sketch, else the most
  recent one with a closed profile). `BeginSketchExtrude/Revolve` became thin
  wrappers over the shared core.
- **C3 `475358d` — the Hole tool** (above).
- **C4 `7a083fa` — Shell / Draft / Split / Refine.** Draft's first face pick
  now names its body (and later picks on another body are refused); Shell
  follows the same rule for its open faces. Split had nothing at all: two new
  slots (`SplitBody`, `SplitKnifeFace` — the latter exposed only in Face-knife
  mode, the conditional-slot pattern `ExtrudeEndFace` uses), a parameter
  block, and a tree glyph. Its default knife is the XY plane offset to the
  target body's mid-height, because a base plane through the origin misses any
  body that is not at the origin and the tool would open already failing. The
  tool window now shows the pending feature's error in red and disables Accept
  (button and Enter) while the failure is the feature's own — "shell needs at
  least one open face" used to be visible only as a red row in the tree.
- **C5 `d4d7606` — honest text.** The empty-state hints name affordances that
  exist, and Help ▸ Keyboard Shortcuts (Ctrl+/) lists every binding the app
  has, including Ctrl+K, which was advertised nowhere, and the sketch keymap,
  which lived only in `SketchInteraction.cpp`. Its open flag lives in
  `AppState` rather than a function-local static, so the between-test reset
  can close it: a stray floating window silently eats the viewport clicks of
  every GUI test that runs after it (it did, five of them).
- **C6 `7785857` — remote driving.** Real failure reasons out of the test
  engine's own log; `key_press` reports the bad chord instead of returning
  false and being reported as "unknown method"; viewport fractions resolve
  against the 3D viewport rect (snapshotted each tick) instead of the OS
  window; new `item_hover` and `ui_items` (label, id, window, disabled, screen
  centre) so the GUI describes itself; the ref grammar documented in
  `describe` and in the MCP tool description.

## What the acceptance run then found (`dd3608f`)

Re-running the dogfood against the finished round — model a part with clicks
only — turned up four more ways the app claimed success while doing nothing.
They are the round's real payoff, because none of them are visible unless you
work the way a user works:

- **Shell could silently no-op.** `BRepOffsetAPI_MakeThickSolid` reports
  success and returns the *input* solid when the wall is too thick for the
  geometry (or the offset self-intersects around a hole). Opening a face
  always removes material, so ShellFeature now rejects a result that did not
  shrink, naming the thickness. A slab with a through hole "shelled" fine and
  changed nothing.
- **A tool with an unfilled required slot could be accepted.** An edge-less
  fillet regenerates happily — no error, so the C4 error line had nothing to
  show — and Accept committed a no-op feature. The tool window now says "Pick
  Edges to continue" and disables Accept; the slot specs already knew which
  slots are required.
- **Counterbore/countersink defaults are absolute** (Ø2 × 0.5), so switching a
  Ø0.5 hole to Countersink cut a crater four times the bore. Switching Type
  now sizes the head from the bore unless the user already set those numbers.
- **A scripted human could not see a refusal**: `item_click` on a disabled
  widget returned success, so the script sailed past a blocked Accept and
  every later step went to the wrong window (the Tool window keeps one id, so
  "//Hole###ToolWindow" happily addresses the *Fillet* session). Item actions
  now fail with "item is disabled: <ref>".
- **Hidden-label widgets were unreachable.** Every `LabeledCombo` submits as
  `"##Type"` — no visible label, therefore no path. `ui_items` already
  reported ids, so the item actions now accept `{id}`, and `item_combo` opens
  and picks in one action because the popup does not survive between RPCs.

## Deliberate non-additions

- **No `viewport_double_click`.** The dogfood asked for one only because the
  empty-state hint claimed double-clicking a plane starts a sketch. The hint
  was the bug.
- **No `HoleSketch` pick slot.** `Candidates()` cannot emit a sketch, so a
  slot for one could never be filled by a viewport click; the tool resolves
  its sketch the way `CreateRib` resolves its profile.
- **Split's pieces are still not individually addressable** (every piece is
  stamped with the split's id). Unchanged by this round; see
  `docs/interchange/feature-gaps-plan.md`.

## Tests

New GUI cases: `hole/face_pick_to_accept` (the whole flow, one undo step),
`hole/enter_opens_params_without_accepting` (pins the frame ordering that
keeps Enter from also accepting the params window it just opened),
`hole/escape_discards_empty_keeps_drawn` (both halves of the tiered cancel,
and that the discard is undoable), `hole/sk_hole_button_on_existing_points`,
`hole/preselected_face_skips_pick`, `hole/curved_face_refused_flat_accepted`,
`shell/open_face_pick_hollows_the_box` (which also asserts the error line and
the disabled Accept), `draft/face_pick_names_the_body`,
`split/default_plane_cuts_the_body_in_two`,
`refine/merges_the_seam_left_by_a_union`, `help/shortcuts_window_opens`,
`sketch/model_toolbar_extrudes_finished`, `preview/tab_switch_discards_pending`.
Headless: a `HasUserGeometry` unit test, and `remote_smoke` coverage for the
failure reason, the `key_press` chord error, `ui_items` and `item_hover`.

Two traps worth remembering. Primitives are placed by their **min corner**
(`BRepPrimAPI_MakeBox` from the origin, then translated), so a test that
guesses a face centre lands on a corner edge — which correctly outranks the
face in the unified pick, and the test then fails for a reason that has
nothing to do with what it is testing. Derive click points from
`GetBoundingBox`. And the floating tool window's id is `###ToolWindow` for
every tool, so "//Hole###ToolWindow" and "//Fillet###ToolWindow" address the
same window — a ref that *looks* specific will happily drive whichever session
is actually open.
