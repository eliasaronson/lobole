# Fixing what the action matrix found

## The problem

The action matrix (docs/modeling/README.md#action-matrix) ran 30,363 ordinary
actions over 196 designs. 4,549 fail, three of them inside OpenCASCADE in ways
that can take the app down, and not one is refused: every action that cannot
apply is accepted and left in the tree as a broken feature. The six classes
below cover most of the fails and all of the crashes and hangs. Each names
where it happens and the shape of the fix; the matrix is the test for all of
them, by the baseline moving from fail to pass or to refuse.

### 1. A fillet that never returns

Fillet r0.125 or Chamfer on the bore edge `120/p2/bore;U3` of fuzz/30 does
not return (`tests/action_matrix/open/fillet-hang.json`). Every RPC and the
GUI preview run `Feature::Execute` on the main thread with no deadline, so
the app freezes as soon as the edge is picked, and a document holding the
fillet would freeze on open.

The hang is in `BRepFilletAPI_MakeFillet::Add` →
`ChFi3d_Builder::PerformElement`, which walks the tangent-continuous chain
from the picked edge with no visited set. It stops when the chain closes at
its start, meets a sharp corner or adds nothing; a chain that runs into a
closed tangent loop that does not pass through the start goes round forever,
growing the spine. OCC 7.9 ignores the progress range on blends and draft
(`Build(const Message_ProgressRange&)` drops it), so a deadline cannot stop
it; interrupting it from another thread lands inside `malloc` and deadlocks
the next allocation.

Fix: **an OCC patch.** PerformElement stops when the next edge is already
on the spine, and the chain is refused with OCC's own error, which
`EdgeBlendNamed` turns into "the edges tangent to this one run into a loop
that does not come back to it; pick an edge on the loop, or one past a sharp
corner". A check of our own before `mk.Add` would have to repeat OCC's
tangency test and could disagree with it; the patch is the clean fix.
`mk.Add` also moves inside the signal scope that today covers only `Build`.

### 2. Faults caught inside OpenCASCADE

Draft on 11 faces (reference/38, fuzz 13, 24, 31, 38) dereferences address 0
inside OCC, and Fillet on one end-cap edge of reference/25-modelled-thread
faults at a non-null address. `OCC_CATCH_SIGNALS` (src/cad/OccSignals.cpp)
turns both into an ordinary feature error, and nothing else happens. The
process is not trustworthy afterwards: the long jump skipped destructors,
the non-null fault may have followed a stray write, and Draft and Fillet run
on `body->shape` itself, whose TShapes the regen cache shares with the
previous feature's bodies. A kernel run that died part-way can leave the
cached input half-written (p-curves, tolerances), and undo then serves it.

Fix:
- **Find the faulting frame** of both repros under `build-asan` (or gdb on a
  debug OCC). A candidate for Draft is `Draft_Modification_1.cxx`, which
  dereferences `FirstCurve` before its null check. A null dereference is
  OCC's bug whatever the input, so the fix is the patch that makes it throw;
  a refusal of our own is added only where the input is one we should never
  have passed.
- **Draft and Fillet run on a copy** (`BRepBuilderAPI_Copy`), as Shell
  already does, so a fault damages only throwaway topology.
- **A caught fault is a process event.** `occsig::FaultCount()` counts them;
  the document's regen cache is cleared; the failed feature's `regen` entry
  carries `kernel_fault: true`; `ping` reports the count; the GUI shows one
  notice recommending save and restart.

### 3. Nothing is refused

`add_feature` checks only `Feature::WhyIncomplete()`, which sees no bodies,
and then commits whatever it was given. The GUI is stricter (a preview whose
own execution failed cannot be accepted, `PreviewFeature.cpp`), but that rule
is not shared code. So the matrix's commonest fails are inputs that a cheap
look at the tip bodies would have refused:

- **Pattern or Mirror of a source that made no body and has no tool**, for
  example a body whose newest feature is a Shell or Fillet: accepted, then
  "source body (feature N) not found".
- **An element named by its `made_by` id instead of `created_by`**: accepted,
  then "the body the edges belong to no longer exists".
- **A name that resolves on none of the bodies with that id, or on two.**
- Checks that already run inside Execute and cost nothing: a draft face
  perpendicular to the pull, a shell thicker than the body allows, refs on
  two bodies, a blend edge with one face, and a blend edge between tangent faces.

Fix: one virtual, `Feature::WhyRefusedOn(const std::vector<Body>& tip, const
Document&) const`, empty by default, implemented by Fillet, Chamfer, Draft,
Shell and Pattern/Mirror from the checks above (moved, not copied, out of
Execute). `add_feature` and `modify_feature` call it after `WhyIncomplete`
and refuse with the document unchanged; `PreviewFeature`'s accept path calls
it too, so the GUI and the RPC refuse the same things. Each refusal names
what would be accepted: the right `created_by` id, the body to pattern, the
largest shell thickness. A full dry run (execute on a copy and refuse on any
failure) is not part of this: it doubles the cost of every add that succeeds.

### 4. Shell: under half pass

Shell passes 756 of 1723 times. The failures go by body, not by face: 57
designs fail every shell and 100 pass every one. The all-fail bodies are
thin-walled or already hollow (19/20/21 shelled boxes, 09 thin extrude, the
sheet-metal parts, the hinge box), rounded on every edge (14), or boolean
results (37 union, 39 intersect, v137 text). Shell's default and the matrix's
5% both come from the body's bbox, which on a thin-walled body is not the
wall, so the inward offset meets the opposite wall while the bbox guard
passes it. "Had no effect" (ShellFeature.cpp) is reported both when OCC
returned the input and when the volume grew, which hides which happened. The
offset tolerance is the literal `1.0e-3`.

Fix:
- **The default comes from the thinnest wall under the open face**: a ray
  from the face's `inside_point` along its inward normal to the next face,
  the least over the open faces, then a tenth rounded 1/2/5 as
  `SizeFromBody` does. `SizeFromWall` sits beside it in CreateActions.cpp;
  the matrix uses the same derivation.
- **A thickness at or over that wall is refused** before the kernel runs,
  naming the wall (an item-3 check).
- The two "no effect" cases get their own messages with both volumes.
- The offset tolerance is derived from the thickness.

What is left failing after that is OCC's offset on blends and booleans; the
baseline records it rather than this plan chasing it.

### 5. Draft: Standard_OutOfRange

150 drafts throw `Standard_OutOfRange` from `NCollection_Sequence::Value`.
Our Draft indexes no OCC sequence, so it is in the kernel, most likely
`BRepOffsetAPI_DraftAngle::CorrectWires`, which indexes per-edge parameter
lists without a bounds check when a cylinder's seam passes through a vertex
of another wire.

Fix: name the frame with a replay under `catch throw`. If it is the
unchecked index, the OCC patch skips the empty entry and the draft is built;
if the input is one OCC cannot draft, the exception is a fair refusal and
only its message changes.

### 6. Bodies that share a `created_by`

Pattern copies (`#1`, `#2`, …) and split pieces keep the id of the feature
that made them. Fillet, Draft, Shell, Split, Refine, Boolean, Thread and Rib
take the first body with that id and look for the name only there, so an
edge on any other copy "no longer exists". Sketch-on-face and frames already
try every body with the id, which is why they pass on the same designs.

Fix: one `topo::FindOwningBody(bodies, ref, kind, &why)` returns the one body
with that id whose map resolves the name; two is a refusal naming both, none
is `UnresolvedReason`. Every ref of one feature must land on the same body.
Draft and Shell take their body from their face refs. Every feature above
uses it, and item 3's checks are built on it. This is the "short of a
sub-ordinal" fix that docs/known-bugs.md describes for split pieces.

### 7. A full sweep that fits a merge

A full sweep is 2,000–3,000 s on one instance, too slow to run on every
merge. It is two costs. The baseline's `slow` table holds 39 actions that
take about 660 s together, 470 s of it Shell on gear flanks (5–42 s each,
and every one fails). The other ~30,300 actions average 50–70 ms, and that
is the matrix's own round trips: each action is the call, the checks on
its answer, an `undo`, then a full `get_state` to compare every body
against the part before it. On top of that, every shard builds every
generator design, because a generator's actions are split across shards by
hash.

Fix:
- **The Shell refusal of item 4** takes most of the 470 s: a thickness at
  or over the wall is refused before the kernel runs.
- **`undo` answers with the bodies' digest**, the same `digest` that
  `add_feature` already returns, so the check after an undo compares it
  with the part's digest from before the action. The full `get_state` goes
  out of the per-action path and is read only when the digests differ, to
  say how.
- **A generator design is built once per run**, saved to the run's scratch
  directory and opened by each shard, instead of being generated in every
  one.
- **`tools/action_matrix.py run --profile`** writes the time per action and
  per RPC, so the next slow class is found from a measurement instead of a
  guess. This plan measures before and after each change above and keeps
  only what moves the time.
- **Where it runs:** the sample stays in the headless suite (every push);
  the full `-C action_matrix` run joins `tools/merge-branch.sh --full` and
  the release checklist. CI keeps the sample only.

## OCC bug or our use of it

Each kernel failure (items 1, 2 and 5) starts with one test: the input body
exported to `.brep` and the same OCC call made on it in a few lines of plain
OCC, outside the app. If it fails there, it is OCC's, and it is fixed by a
patch in `nix/patches/`. If it fails only inside the app, the cause is state
we left on the shape (p-curves written by an earlier boolean, grown
tolerances), and the fix is ours: the copy before Draft and Fillet, or
whatever the difference turns out to be. The calls themselves are OCC's only
API for each job (`BRepFilletAPI_MakeFillet`, `BRepFilletAPI_MakeChamfer`,
`BRepOffsetAPI_DraftAngle`, `BRepOffsetAPI_MakeThickSolid`), but Shell uses
`MakeThickSolidByJoin` with every option at its default (arc joins, no
intersection mode); item 4 measures `Intersection = true` and the
intersection join on the matrix's Shell rows and keeps whichever passes more
without a pass becoming a fail.

A patch goes into every build that ships OCC, as the shell patch does:
`nix/occt-overlay.nix`, `packaging/appimage/deps.sh` (and the AppImage
deps cache key in `.github/workflows/release.yml`) and
`packaging/windows/occ-cross.nix`. CI's apt OCC stays unpatched; the
matrix sample already skips the actions that hang there. Each patch is
written as an upstream report with its `.brep` reproducer, ready to send.

## Order

6 and 3 first, as one change: they share `FindOwningBody` and the
`Feature.h` edit, which is one rebuild. Then 1, 4, 2 and 5, then 7, whose
Shell saving depends on 4. Items 1, 2 and 5
each start with a replay that names the frame; if a replay contradicts the
cause given here, the fix follows the replay.

## Files

- `src/core/Feature.h`: `WhyRefusedOn`.
- `src/core/ElementHistory.{h,cpp}`: `FindOwningBody`, `mk.Add` inside the
  signal scope.
- `src/core/features/{Fillet,Chamfer,Draft,Shell,Pattern,Mirror,Split,Refine,Boolean,Thread,Rib}Feature.cpp`:
  `FindOwningBody`, `WhyRefusedOn`, the copy before Draft and Fillet.
- `src/cad/OccSignals.{h,cpp}`: the fault count.
- `src/core/Document.cpp`: clearing the regen cache after a fault.
- `src/remote/RemoteModeling.cpp`: `WhyRefusedOn` in `add_feature` and
  `modify_feature`; `kernel_fault` in `regen`; `ping`.
- `src/ui/PreviewFeature.cpp`: `WhyRefusedOn` on accept; the fault notice.
- `src/ui/CreateActions.cpp`: `SizeFromWall` for Shell.
- `nix/patches/occt-*.patch`, `nix/occt-overlay.nix`,
  `packaging/appimage/deps.sh`, `packaging/windows/occ-cross.nix`,
  `.github/workflows/release.yml`: the PerformElement stop, and the Draft
  and Fillet fixes their frames call for.
- `src/core/features/ShellFeature.cpp`: the join and intersection options.
- `tools/action_matrix.py`: Shell's thickness from the wall; the digest
  check after undo; generator designs built once; `--profile`.
- `src/remote/RemoteDispatch.cpp`: `undo` returns `digest`.
- `tools/merge-branch.sh`: the full matrix under `--full`; the release
  checklist in `docs/packaging/README.md`.
- `tests/action_matrix/{baseline.json,open/}`, `docs/known-bugs.md`: the
  repros and known rows that the fixes retire.

## Verification

- Each repro in `tests/action_matrix/open/` replays to a refusal or a pass,
  and its `known` row and known-bugs entry go.
- A Catch2 case per item, written to fail first: the fuzz/30 fillet refused
  under a ctest timeout; a feature whose kernel call faults leaves the
  earlier bodies identical (volume, bbox, tolerances) after undo; a
  made_by id refused with the revision unchanged
  (`refused_calls_smoke`); fillet an edge of pattern copy `#1`; the Shell
  default on a thin-walled body is under its wall.
- `reference/41-split-then-fillet` stops being a known failure.
- The matrix is re-blessed once at the end. The ratchet accepts fail → pass
  and fail → refuse; any pass → fail or pass → refuse is read case by case
  and either explained in the commit or fixed. The commit states the before
  and after counts per action.
- A full sweep's wall time, before and after item 7, from `--profile`;
  its outcomes identical to the sweep without the changes.
- The `defaults/*` GUI suite, the headless suite, the GUI suite, the full
  `-C action_matrix` run, and `tools/ci-local.sh --asan` (it touches the
  signal path).
