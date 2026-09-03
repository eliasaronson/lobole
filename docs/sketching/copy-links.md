# Linked sketch copies (design + as-built)

> Status: **phase 1 SHIPPED 2026-07-19** (`700f1b4`, format v35) — the design
> below was signed off and built as written; only **phase 2 (cross-part
> links)** remains, and it was always scheduled after phase 1 proved the UX.
> Ctrl+C / Ctrl+V paste plain copies; **Ctrl+Shift+V pastes linked copies**
> that re-sync from their source until the link is broken.
>
> (This banner said "draft, awaiting sign-off" until 2026-08-21 — four weeks
> after the work landed. Found by auditing the plans for unfinished projects,
> which is exactly the kind of thing a stale status line makes impossible.)

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

## Sign-off questions — all three answered yes

1. Solver-fixed outputs (anchors, not free geometry) — **yes.**
2. Phase 1 = within-part only, cross-part pastes stay unlinked — **yes.**
3. `Ctrl+Shift+V` for paste-linked — **yes.**

## What shipped (phase 1, 2026-07-19)

Built as designed, with the phase boundary held:

- `DerivedKind::LinkedCopy` + `DerivedOp::sourceSketchId`. Same-sketch
  sources replay in `RegenerateDerived` using the existing id-stable instance
  mapping; **cross-sketch** sources replay in `SketchFeature::Execute`, where
  the source — required to be earlier in the history — has already executed
  (`ReplayLinkedCopyFrom`).
- Outputs are solver-**fixed** anchors: points, radii and arc angles are held
  out of the unknowns (`SketchSolver.cpp`), so the target's own constraints
  can reference them while the sync owns their position. They render violet.
- `SketchFeature::BreakLink(opIndex)` removes the op and keeps its outputs as
  free geometry with ids intact — one undo step, so downstream profile
  regions and constraints survive. Reachable per op from the Sketch panel.
- A vanished source sketch or entity **fails regeneration with a break-link
  hint** rather than desyncing silently.
- Cross-part pastes, and pastes whose source is later in the history, fall
  back to a plain unlinked paste with an `actionMsg` saying why.
- Format **v35** with a golden pair (a linked circle extruded through a
  second sketch); core tests cover sync, break-link independence, and the
  vanished-source error (`tests/test_sketch_feature.cpp`,
  `tests/test_sketch_solver.cpp`).

**Still phase 2:** cross-part links, for the reason stated above — regen
would have to resolve another part's document and dirty dependents, widening
its input surface. Unlinked cross-part paste remains the behaviour until then.


**2026-09-03:** the shape phase 2 would take is now written down in
[../capability-review-2026-08-31.md](../capability-review-2026-08-31.md) (A2,
"a binder, not a graph"): a feature carrying a baked copy of another part's
geometry plus the source version, updated explicitly — from which a linked
copy across parts is the within-part case on the copy. Still no demand.