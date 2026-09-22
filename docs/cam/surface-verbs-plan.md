# Verbs for 3D and 4D finishing

## The problem

The job toolbar's verbs are how a person makes a CAM operation: one sentence
("Clear a pocket"), one click on the model, and everything else derived from
the tool and the face. The 3D and 4D operations generate and verify, but have
no verb that reaches them:

- **Finish a surface** (`SurfaceParallel`) and **Finish walls**
  (`SurfaceWaterline`) are written as verbs in `src/ui/CamTool.cpp`, with
  their prompts, their help, the shared face pick and their derivation. But
  the commit that added them (ece9e342) gave them no command, no key and no
  toolbar button. Nothing starts them, and no GUI test covers them.
- **Rotary finishing** (`SurfaceRotary`, parallel, spiral or helical) has a
  derivation (`cam::DeriveOperation`: no pick, a ball or flat nose, a machine
  with a 4th axis) and no verb. "Turn a face up" only indexes.
- **Roughing on the rotary** is a field of that op (a stepdown, which the
  strategy turns into layers from the blank inward), not a verb.

The only way in from the GUI is Add operation and then the op's type
dropdown. That skips the pick, so no outline and nothing derived.

## The shape

Four verbs on the job toolbar, each a command with a key:

| verb | command | key | pick | makes |
|---|---|---|---|---|
| Finish a surface | `job.surface` | R | a face, curved or tilted | `SurfaceParallel` |
| Finish walls | `job.walls` | W | a face | `SurfaceWaterline` |
| Finish on the rotary | `job.rotary_finish` | Shift+R | none | `SurfaceRotary` |
| Rough on the rotary | `job.rotary_rough` | Shift+O | none | `SurfaceRotary`, in layers |

- The two 3D verbs are the existing table rows, now wired: `verb(...)` in
  `Commands.cpp`, rows in the keymap, and buttons after Break an edge.
- The two rotary verbs show only when the machine has a 4th axis, beside Turn
  a face up (`HasRotaryAxis`). They take no pick, like "Cut out the whole
  part", and the window offers the tool and re-derives for another.
- **Rough on the rotary** is derived in core (`cam::DeriveRotaryRough`): the
  rotary finish's derivation, then a stepdown of half the cutter (the rule the
  2.5D ops use, which the rotary derivation deliberately does not apply to a
  finish) and a stock to leave of `DefaultFinishStock(tool)`, so the finish
  that follows has something even to cut. It is named "Rough on the rotary".
  The pattern is whatever the op editor says (parallel by default).
- The glyphs pass the toolbar-glyph gate (`test_conventions.cpp`).

## Files

- `src/ui/AppState.h`: two `CamVerb` values.
- `src/ui/CamTool.cpp`: two verb rows; the rough flag carried through
  `CreateVerbOperation` and `RederiveVerbOp`.
- `src/core/cam/CamDerive.{h,cpp}`: `DeriveRotaryRough`.
- `src/ui/Commands.cpp`, `src/core/io/Keymap.cpp`, `src/ui/Toolbar.cpp`:
  commands, keys, buttons.
- Tests: a unit test for `DeriveRotaryRough` (layers, leave, the refusals it
  inherits); GUI tests that Finish a surface makes a finish from a picked
  curved face, that both rotary verbs make their ops on a rotary machine, and
  that the rotary buttons are absent without one.
- Docs: docs/cam/gui.md's verb list and docs/cam/surfacing.md.

## Verified by

The three suites, the CAM-off build, and the new tests above.

## As built

As planned. `CreateVerbOperation` and `RederiveVerbOp` take the rough through
a `rotaryRough` flag on the verb row, so changing the tool in the verb's
window re-derives the layers and the skin for the new cutter. The unit test
cuts the rough from a blank 4 mm proud of a necked shaft and verifies the
posted file: no gouge, a skin left for the finish, and more than three
distinct cutting radii (layers). GUI tests:
`cam/the_surface_verbs_take_a_curved_face` (both 3D verbs from their buttons,
on a bar's round side, and no rotary buttons on a 3-axis machine) and
`cam/the_rotary_verbs_rough_then_finish`.

Not added: an RPC verb for the rough. A script sets `stepdown` and
`stock_to_leave` on a rotary op, which is what the verb derives.
