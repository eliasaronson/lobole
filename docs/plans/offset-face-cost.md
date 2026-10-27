# Offset Face cost on a large body

## The problem

On the SO-101 wrist bracket (`Wrist_Roll_Pitch_SO101.step`, 291 faces,
imported with `keep_origin`) every planar Offset Face tried — `1/b1/face192`
by ±0.5, `1/b1/face194` by 0.3, the inner plate faces 192 and 197 by 1.11 —
holds the main thread 50 to 60 s and is then refused "… leaves N separate
pieces of the body". Two causes:

1. **The cells are built from the whole body.** A planar face with no
   tangent neighbour is offset by Replace Face's cells
   (`directedit::ReplaceOnto`, `src/core/features/ReplaceFaceFeature.cpp`):
   `BOPAlgo_MakerVolume` over EVERY face of the body, plus the neighbours
   extended by the body's diagonal and the target surface bounded to twice
   the diagonal around the body's centre. That is ~25 s of intersections on
   291 faces, and it is also the wrong answer: the target plane, that large,
   runs on through every other wall of the body that crosses it and cuts it
   into cells, each with an untouched face's probe in it, so the edit is
   refused for "pieces" the edit itself never made.
2. **An edit is built more than once.** `RefuseFailingDirectEdit`
   (`src/remote/RemoteModeling.cpp`) adds the edit to a copy of the document,
   which regenerates, and then calls `Regenerate()` on the copy again. The
   first execution records the imported face's fingerprint into the
   feature, which is part of its chain hash, so the second regeneration
   misses and builds it again. The same miss hits every accepted edit:
   built on the copy, built again by the commit, and once more by whatever
   regenerates next.
3. **The checks before the builder scale with faces × edges.**
   `directedit::FreeformNeighbour` describes every neighbour in full
   (`Measure::DescribeFaceAt`: UV bounds over every edge, a projection, the
   area), once per shared edge. On a refined STL (the Voron fan grill:
   Refine to 2937 faces, then a Delete Face of 916) the planar facets carry
   no p-curves, so each description projects every edge, and rolling the
   tree's bar back and forward held the main thread 62.7 s before the heal
   began. `RestoreUntouched` likewise describes each renamed face in full,
   walking the shape to its index each time.

There is no builder thread to have bypassed: `directedit::BuilderBudgetS` is
CPU time of the thread that runs the builder, and regeneration runs on the
main thread on every path (RPC, editor preview, undo, open —
docs/architecture.md#threading). Moving one direct edit off the frame means
regenerating off the frame, which is not this change. With the cost of the
edit down to the neighbourhood of the face, the stall goes with it.

## The shape of the fix

1. **Build the change, not the body.** `ReplaceOnto` builds the cells from
   the replaced face, its neighbours extended across the edges they share
   with it, and the target surface — nothing else of the body. The cells
   that hold a piece of the replaced face and a piece of the target are the
   volume between the old face and the new one: a cell on the replaced
   face's outer side is material to add, one on its inner side material to
   take away (read from the piece's orientation in the cell against the
   face's in the body, no point classification). The result is the body
   fused with the one set and cut by the other (`topo::BooleanNamed`, under
   the same deadline). A neighbour and the strip the fuse adds to it lie on
   one surface: they are merged across the replaced face's old edges only
   (`ShapeUpgrade_UnifySameDomain` through BooleanNamed's refine hook, every
   edge and vertex of a face the edit does not touch kept), so the neighbour
   is one face, as before. Naming goes through the booleans' and the
   merge's history, as before: the target's piece is `<face>;M<id>`, a
   neighbour `<name>;M<id>`, an untouched face keeps its name
   (`RestoreUntouched`). The existing checks stay: an untouched face the
   edit changes is refused ("cuts …, which it should leave alone"), a target
   no extended neighbour reaches is named, and `CheckResult` judges the
   whole. What the cells are built from no longer depends on the size of
   the body, so the cost does not either, and a wall the target plane
   crosses elsewhere is never cut.
2. **Build an edit once.** `RefuseFailingDirectEdit` reads the error the
   copy's own regeneration (in `AddFeature` / `ReplaceFeature`) left,
   instead of regenerating the copy again. `Document::Regenerate` keys a
   feature whose evaluation changed what it is keyed on (a fingerprint
   recorded) afresh, the evaluation filed under both keys and every later
   link rehashed, so the next regeneration hits. An edit that builds on the
   copy is not built again by the commit: the document adopts the copy's
   evaluations (`Document::AdoptEvaluations`, the same chain hashes), and a
   hit that restores a recorded fingerprint is re-keyed the same way.
3. **Check what the check needs.** `FreeformNeighbour` looks at each
   neighbour once, and at its surface's type only; `RestoreUntouched`
   indexes the faces once and compares type, area and centroid.

Files: `src/core/features/ReplaceFaceFeature.cpp`, `src/core/DirectEdit.cpp`,
`src/core/Document.{h,cpp}`, `src/remote/RemoteModeling.cpp`,
`tests/test_direct_edit.cpp`, `tests/direct_edit_step_smoke.py`,
docs/modeling/README.md#direct-edit, docs/architecture.md (the regen
cache), docs/known-bugs.md.

## Verification

- A cost check (`std::clock()`, docs/ui/testing.md#tests-that-time-things):
  a planar Offset Face on a block carrying a few hundred faces elsewhere
  (a grid of holes far from the face) costs under a bound set against the
  slowest core, which the whole-body build exceeds many times over; shown to
  fail with a CPU spin planted in the measured interval.
- A plate whose top's plane, extended, runs through a separate wall of the
  same body: the top offset builds (it was refused "separate pieces").
- A refused direct edit through `add_feature` executes once (its failure is
  logged once) and a kept one is not executed by the commit
  (`get_state`'s `regen.executed` is 0), in `direct_edit_step_smoke`; an
  edit that records a fingerprint, and what follows it, hit on the next
  regeneration; a document adopting its copy's evaluation executes nothing.
- A cost check on `FreeformNeighbour` over a faceted prism (2000 side
  facets, two 2000-edge ends, no p-curves).
- `test_direct_edit` unchanged; `direct_edit_step_smoke`'s per-edit
  outcomes (kept with the same volume, or refused) compared before and
  after; the action matrix's direct-edit rows; test-fast, the GUI suite and
  `pygui`.
- The bracket's offsets re-run, and the fan grill's regeneration: time and
  outcome before and after.
