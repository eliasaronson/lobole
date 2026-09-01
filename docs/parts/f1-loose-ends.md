# Three things F1 left — plan

> Status: **BUILT 2026-09-01**, all three (format v72). Written after F1
> phases 1-7 and
> hole-standards shipped, prompted by using it: *"I get surprised by the parts
> and fasteners being movable after having placed them."* Every mechanism
> named below was checked against the source.

## First: does the assembly solver (A1) gate any of this?

**No, and it is worth saying why rather than just asserting it.**

A1 is about **closed kinematic loops** — a four-bar linkage, a slider-crank, a
toggle clamp. `core/AssemblySolve.h` places by direct frame alignment
propagated from the anchors, which solves mate TREES correctly and reports
loops as errors. The gap is the propagation, not the mate types.

A fastener is the most tree-like thing in a model: a leaf with **zero degrees
of freedom** relative to the one part it bears on. There is no loop to close
and nothing to iterate. The current solver already places a rigid leaf
exactly right.

And on the representation: the review's own account of what A1 would replace
is *"the numeric core"* — the six mate types, connector frames, stored DOF
poses and the mate graph are listed as the parts that already exist and would
survive. So mates authored now would not need rewriting either.

**So this is not gated.** More than that: the design below **bypasses the
solver entirely**, which makes it immune to whatever A1 turns out to be.

## 1. A fastener should be ATTACHED, not mated — and not free

### The surprise, and where it comes from

`GizmoOccurrence` hands a drag gizmo to *the selected, non-fixed occurrence*.
`AddFastener` places with `fixed = false`. So a placed screw is selectable and
draggable, and you can pull it out of its hole with nothing objecting.

It is worse than untidy. The screw does not follow its plate either — the
solver's own comment says *"unmated occurrences have no world entry and are
untouched"* — so today a fastener is neither held nor carried. It just sits
at a remembered transform.

### Why not mates

A mate is for KINEMATICS, and a screw has none. Using the mate system for
this costs:

- **Up to four Mates rows per joint.** The bolted dock is six joints, so 24
  rows; a real dock with fifty bolts is two hundred. The Mates list stops
  being readable, and it is the list you use for the mechanism.
- **Mates MOVE things.** Placement is by frame alignment, so a fastened mate
  relocates the fastener to satisfy the mate — potentially away from where the
  fit computed it. Agreeing with the derivation rather than overriding it is
  real work for no benefit.
- **A mate implies a solve.** Two hundred rigid leaves is two hundred nodes in
  a graph that exists to answer a question fasteners do not ask.

#### And what that argument got wrong (corrected 2026-09-01)

Every bullet above is true **of the hardware**, and all three still hold: the
screw, its washers and its nut are attached, not mated, for exactly those
reasons. What the argument missed is that it answers the wrong question. It
asks how the SCREW should be held, and never asks what the screw is FOR.

A bolt joins two parts to each other. Attaching the head end to the near part
and the nut end to the far one left the two parts as free of each other as
they had ever been — so the first thing tried after this shipped was rotating
one bar, and the joint tore in half with the screw riding one side and the nut
the other. Reported the same day, twice: *"Did we not build the mate for the
fasteners?"*, and then *"I also don't see the mates in the mate list"* —
which is the second half of it. The Mates list is where a person looks to see
what holds an assembly together, and a bolted assembly's was empty.

So placing a fastener now ALSO adds one **fastened mate between the two parts
it clamps** — the part the head bears on to the part the axis leaves last.
Three things make that cheap rather than the cost the bullets feared:

- **One mate per INTERFACE, not per bolt.** A joint through three parts is two
  interfaces, and a second bolt across an interface a first already joined
  would close a loop, which this solver reports as an error rather than
  solving. `assembly::MatesConnect` asks whether the two are already connected
  through the mate graph and adds nothing when they are. A fifty-bolt dock is
  not two hundred rows; it is one row per pair of parts that touch.
  `fit_fastener` returns `mate: 0` to say so, and the tool window tallies
  *"8 placed, 1 joined"* — the difference being the interesting number.

  Mating only the two ENDS of a joint was the first attempt and was the
  reported bug one layer in: the alternator's stator is clamped between two
  housings and was joined to neither, so dragging a housing left it behind.
  Consecutive pairs are a chain, so the fix is still a tree.
- **The mate MOVES NOTHING.** The bullet above is right that a mate placed by
  frame alignment relocates things — so the DOFs are measured rather than
  assumed. Both connector frames are resolved through the same
  `ConnectorLocalFrame` the solver uses, and flip/rotation/offset/dx/dy come
  out of the transform between them. The solve that follows a placement is a
  no-op, which is the whole requirement: the parts are already where the
  person put them, and a mate that "corrects" them breaks the model it was
  meant to record. Gated by a test that solves and compares the full relative
  transform, not just a position.
- **It anchors on faces the traversal already walked.** Where the axis LEAVES
  one part and where it ENTERS the next — the two faces that touch, which is
  the interface a person would have picked by hand. Found by normal and
  distance along the axis: no new picking, no new geometry, and nothing stored
  that the fastener did not already know.

The mate reads as what it is in the list: `Bolted`, fastened, *Bar A's bottom
face to Bar B's top face*. If a person deletes it, the joint is free again;
that is the right escape hatch and it costs nothing to have.

### The proposal: an attachment

`Occurrence` gains **`attachedTo`** — the id of the occurrence it rides on —
and the relative transform is whatever the two poses already imply at the
moment of attaching. `assembly::Flatten` composes `world(parent) · relative`
exactly the way it already composes a sub-assembly's children, so this is a
shape the walker knows.

What falls out:

- **Moving the plate carries the screw.** The thing `check_fasteners` can
  currently only report after the fact.
- **No independent drag gizmo**, because the pose is derived — which is the
  reported surprise, fixed by making the model honest rather than by
  disabling a widget.
- **No solver involvement at all**, so A1 cannot disturb it and 200 fasteners
  cost 200 matrix multiplies rather than 200 constraint nodes.

Decisions this needs, made here rather than left to the implementer:

- **The parent is the part the head bears on** — the first segment of the
  derived stack. Not the far part: a nut rides the far side, so a nut attaches
  to the LAST part, which is what keeps a separating joint honest instead of
  stretching a rigid group across the gap.
- **Deleting the parent deletes what rides on it**, saying how many. A screw
  whose plate is gone is not a screw, and leaving orphans reintroduces exactly
  the floating hardware this fixes. (`WhyPartUndeletable` already refuses to
  delete a placed PART; this is the occurrence-level counterpart.)
- **Attachment is not permanent.** A context-menu "Detach" restores an
  independent occurrence, because a model is allowed to be wrong on purpose.
- **`check_fasteners` stays.** Attachment fixes POSITION. It does not fix
  LENGTH: a plate that gets thicker still leaves the screw too short, and that
  is still only reportable.

Format bump, golden, history row.

## 2. A countersunk screw in a countersunk hole — now nearly free

F1 refuses `iso10642` outright: the rim you pick on a plain bore is the bore's,
so the head would end up inside solid material. Two things shipped since have
almost dissolved that.

- `hole-standards` gives a hole a **recorded thread**, so the size no longer
  has to be inferred from whichever diameter you happened to click.
- `drawing::FindHoleForRef` — already used by `fit_fastener` — matches a
  picked circle against the hole's **bore, counterbore AND countersink**
  radii. So clicking the countersink's outer rim already resolves to the
  HoleFeature that made it.

Which means the information is all present: the thread from `fit`, the
countersink's depth and diameter from `csDepth`/`csDiameter`, and the
knowledge that this rim IS the countersink rather than the bore. What is left
is to use it: place the head in the countersink, measure the length overall
(`Profile::lengthOverHead`, already modelled), and refuse when the countersink
is too shallow for the head — which is a real check nobody can do by eye.

Small, and it removes a refusal rather than adding a feature.

## 3. A pick slot must be registered in two files

Recorded in [../known-bugs.md](../known-bugs.md) and unchanged: routing lives
in `SlotPickClicked`, liveness in `ViewportInput.cpp`'s per-frame chain, and a
sentinel missing from the second is disarmed the same frame it is armed — the
tool then ignores every click, with no error anywhere. All four sentinels are
correct today; the cost is paid by the fifth.

One registry of `{sentinel, alive(state), consume(state, item)}` replacing both
chains, plus a `test_conventions.cpp` scan asserting every `k*Slot` constant
appears in it. The same move that collapsed seven renderer lists to two after
three renderers shipped with a wire missing.

## What building it corrected

- **`position` was kept, not reinterpreted.** The tempting shape is to make
  `position`/`orientation` mean *relative to the parent* when `attachedTo` is
  set. That would need no derived state — and it would give one field two
  meanings, which is exactly what made `slotPick.featureId` need registering
  in two files. The relative placement is stored explicitly instead and the
  world placement is RECOMPUTED, so every existing reader of `position` (the
  drag gizmo, get_state, the RPC, exploded views, serialization) keeps working
  unchanged.
- **The relative transform is quaternion arithmetic, not a matrix round
  trip.** `Decompose` is file-local to the solver, and for a rigid placement
  `inv * (pos - parentPos)` and `inv * rot` are exact and need no
  decomposition at all.
- **`ApplyAttachments` had to be idempotent, and the golden asserts it.** A
  composition that reports movement every time it runs is a slow drift rather
  than a fix; the second call must move nothing.
- **The registry needed TWO assertions, not one.** Checking that every
  sentinel appears in the table catches a half-registered session. It does not
  catch someone re-introducing the second chain, which is the actual defect —
  so the gate also asserts `ViewportInput.cpp` compares no sentinel directly.
- **The countersunk case really had dissolved.** All three checks it needed
  were answerable from information already present: `FindHoleForRef` matching
  the countersink radius says the picked rim IS the countersink, `HoleKind`
  says the hole is one, and the standard's own `csh_height` against the hole's
  `csDepth` says whether the head fits in it. What was a blanket refusal is
  now three refusals that each name a fix.

## Order, and why

1. **The attachment (1).** It is the one reported from use, it is the largest,
   and it changes a format — so it wants to land before anything builds on the
   occurrence model.
2. **The pick-slot registry (3).** An hour, and it is insurance that gets more
   valuable the more tool sessions exist.
3. **The countersunk case (2).** Cheapest and least urgent: it removes a
   refusal, and no round has wanted a countersunk screw yet.

## Out of scope

- **A general rigid-group / sub-assembly-lite.** `attachedTo` is deliberately
  one parent and one level. Groups of arbitrary parts are a different feature
  with its own UI, and inventing it here would be building for a case nobody
  has asked for.
- **Attaching anything other than a fastener.** The field is general; the verb
  that sets it is the fastener tool. A general "attach this to that" gesture
  can come when something wants it.
