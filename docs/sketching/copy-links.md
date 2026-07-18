# Linked sketch copies (design — awaiting sign-off)

> Status: **draft 2026-07-18**. The unlinked half is shipped: Ctrl+C copies
> the selected sketch entities, Ctrl+V pastes plain copies (fresh ids, small
> offset) into the same or any other sketch — including another part's,
> since the clipboard lives in AppState. This doc designs the LINKED variant
> the user asked for: pasted copies that keep tracking their source until
> the link is deliberately broken.

## The ask

"I always miss that copies aren't connected when copying sketches to a new
part or new sketch, so they don't sync when one is edited. We'd need a way
to remove the link too, so they can be edited independently."

## Design

### Mechanism: geometry-rewrite sync, not constraints

The sketcher's existing "linked" mirror/pattern use *constraints* (symmetry
ties inside one solve). That cannot span sketches — PlaneGCS solves one
sketch at a time — so linked copies sync by **rewriting geometry at
regeneration**, the way `plane_ref` re-derives a sketch plane from its
datum (and the way Onshape's *Use* / FreeCAD's external geometry work):

```
struct LinkedCopy {            // a new sketch derived-op kind
  uint64_t sourceSketchId;     // 0 = this sketch
  std::vector<uint32_t> sourceEntityIds;
  glm::vec2 offset;            // paste offset in target UV
  std::vector<uint32_t> outputIds; // the pasted entities in this sketch
};
```

- On regeneration (`RegenerateDerived` for same-sketch sources; the sketch
  feature's `Execute` for cross-sketch, after the source has executed), each
  output entity's geometry is refreshed from its source entity + offset.
  A vanished source entity errors the sketch with a break-link hint — the
  regen-failure UX, never a silent desync.
- Outputs are **solver-fixed**: they are anchors the target sketch's own
  constraints can reference, but the solver never moves them (their position
  is owned by the sync). This is what makes the semantics predictable —
  no cross-sketch constraint fights.
- Outputs render in a distinct tint (violet, alongside the new green =
  defined / blue = free coloring) so linked geometry is recognizably not
  editable in place.

### Breaking the link

Context action on any linked entity (and a per-op row in the Sketch panel):
**Break link** — deletes the `LinkedCopy` op and leaves the outputs as
ordinary free entities (ids kept, so downstream profile regions and
constraints survive). One undoable edit.

### Ordering + scope

- Same-sketch and cross-sketch within one part in phase 1. Cross-sketch
  requires the source sketch to be **earlier in the feature history** (the
  `plane_ref` rule) so regeneration stays a single forward pass.
- **Cross-part links are phase 2**: regeneration would need to resolve
  another part's document (the `PartDocById` machinery the assembly solver
  uses) and part-edit propagation would need to dirty dependent parts (the
  A4 stamp mechanism). Feasible, but it widens regen's input surface —
  designed separately once phase 1 proves the UX. Until then, cross-part
  pastes stay unlinked copies (today's shipped behavior).

### UI

- `Ctrl+V` — plain paste (shipped). `Ctrl+Shift+V` — paste linked.
- Paste-linked across parts falls back to plain paste with an actionMsg
  explaining why (until phase 2).
- Linked outputs: violet tint, "Break link" in a right-click context menu
  (the sketcher's first context menu — also a natural home for
  copy/paste/delete, which are keyboard-only today).

### Serialization

A new `derived_ops` kind `"linked_copy"` on the sketch feature (format
bump + golden when it lands, per policy).

## Sign-off questions

1. Solver-fixed outputs (anchors, not free geometry) — agreed?
2. Phase 1 = within-part only, cross-part pastes stay unlinked for now?
3. `Ctrl+Shift+V` for paste-linked?
