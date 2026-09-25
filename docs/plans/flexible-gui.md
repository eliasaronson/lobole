# Flexible sub-assemblies in the GUI

## The problem

A sub-assembly placement can be `flexible` (v129): its mechanism moves in the
parent's solve and studies. But that is reachable only from the RPC and the
SDK. The occurrence panel shows nothing about it, a placement's
configuration cannot be chosen there, an inner mate cannot be picked as a
study driver or a relation's side, and an exploded view moves a flexible
unit as one block.

## The shape of the fix

- **The occurrence panel.** A sub-assembly placement shows a "Flexible"
  checkbox, and a configuration picker listing that sub-assembly's
  configurations. Both are undoable edits through the same actions the RPC
  uses. A refusal (nested flexibility, nothing fixed inside) is shown where
  the click was, with the app's reason.
- **The tree.** A flexible placement expands to show its moving inner
  occurrences and their mates, read-only, addressed by path. Selecting one
  selects the leaf in the viewport.
- **Picking inner mates.** The motion study driver picker and the relation
  tool list inner mates of flexible placements, by path, as the RPC accepts
  them.
- **Direct manipulation.** Dragging a moving inner part in the viewport
  moves it within its mate and poses the unit, like dragging a top-level
  part within its mate.
- **Exploded views.** Explode steps may name an inner occurrence of a
  flexible placement by path. The auto-explode stays top-level.

## Files

- `src/ui/OccurrencePanel.cpp`, the assembly tree and relation tool UI,
  `src/ui/AssemblyActions.{h,cpp}`, `src/ui/GizmoInput.cpp`.
- The explode model if it needs a path (a format bump and golden if it is
  stored).
- `docs/assemblies/README.md`, `docs/ui/panels.md`.

## Verification

- GUI tests, as a person would do it: tick Flexible on a placement, pick its
  configuration, drive an inner mate from a study picked in the UI, drag an
  inner part, undo each.
- The headless and GUI suites.
