# A freewheel relation

## The problem

Every relation is an equation: dof_b = ratio × dof_a + phase, or a cam's
table. None can DRIVE one way and SLIP the other, which is what a one-way
clutch, a ratchet and a click do. So the watch's hand-winding turns the rotor
backwards through the whole reduction train — in a real automatic, a one-way
wheel lets the crown wind the mainspring while the rotor stays put, and the
rotor turning the "wrong" way winds nothing.

A freewheel's position depends on HISTORY: where the output is after the input
went back and forth is not a function of where the input is now. Every other
relation is a function, which is why a solve can be run any number of times
and why a document re-solves to the same pose on load. That is the design
question: where the history lives, and how it stays reproducible.

## The shape of the fix

**One number of state per freewheel, in the document.** A freewheel holds

    b = ratio × a + phase + lead

and `lead` is the only state: how far the output has run AHEAD of the input's
drive line, in b's unit. It only ever grows in the direction the freewheel
`drives` (`positive` or `negative` for b), and propagation keeps the equation
exact by updating it:

- **From a** (the input moved): if the drive line puts b further along in the
  drive direction, b follows — engaged. If not, the input is going back: b
  stays where it is — slipping.
- **From b** (something else moved the output): forward in the drive
  direction, b overruns and a stays; backward, the clutch engages and drags
  a back with it — a lone clutch does exactly that (spin a bicycle's wheel
  backwards and the pedals turn). It does NOT drag when a is reachable from
  b through the other relations, because then dragging feeds itself: see
  [reverser-plan.md](reverser-plan.md). Held, it reports instead.
- **`lead` is reconciled once, after the walk**, from where the two ends
  actually finished: the gap between the output and the drive line is slip
  and `lead` takes it up. A gap the other way is two drivers disagreeing and
  is reported. Doing it here rather than in the walk is what lets two
  freewheels drive one output — a reverser ([reverser-plan.md](reverser-plan.md))
  — and a slipping freewheel leaves its output UNSETTLED for the driving
  path to move.

This is a roller clutch: it re-engages the moment the input turns forward
again, wherever that is. A ratchet's dead band (re-engaging only at the next
tooth) is not in this round.

Why this holds together:

- **Idempotent.** The rule reads b's stored value and `lead`, and after it
  runs the equation holds exactly, so running it again with the same inputs
  changes nothing. Propagation runs at least twice per action (set_mate
  propagates and then solves; a drag solves every frame) and again on load,
  tab switches and configuration switches — every one of those finds the
  equation satisfied and leaves `lead` alone.
- **Saved and undone with the pose.** `lead` is a relation field, serialized
  with the document. Undo is a whole-document snapshot
  (`SnapshotAssembly`), so the same undo that restores the mates restores
  `lead`; no undo path is new.
- **Reproducible studies.** A motion study runs on a COPY of the assembly
  and starts from the document's `lead` every time, so running it twice gives
  the same samples. A scrub or `leave_at` walks the live document, as it does
  for the mates, and moves `lead` with them.
- **The residual check needs nothing new**: `RelationValue` returns
  ratio × a + phase + lead, which is what b is after propagation.

**Not inside a loop.** A loop is closed by a least-squares solve over
equations; a clutch that may slip is an inequality, which that solver cannot
hold. A freewheel whose mates are in a mate loop is reported in its own
`error` and left out of the loop solve, never silently treated as always
engaged.

**The surface.** `add_relation(type: "freewheel", a, b, ratio, drives)` —
rotation to rotation; `drives` is required (the direction is the whole point
of the thing, and a default would be a plausible wrong answer). `lead` starts
as the gap between the two parts as they stand, so a clutch engages where it
is made and adding one moves nothing; shows in `list_relations`, and `set_relation {lead}` can put it back
(re-engaging a clutch by hand). The relations panel shows it read-only. The
SDK gets `Asm.freewheel()`.

**Format: v104** — the `freewheel` kind and its `drives` and `lead` fields.
Golden `tests/golden/assembly/v104-freewheel.json`.

**Watch.** The reduction wheel is split in two, as a real one-way reduction
wheel is built: the wheel the rotor pinion drives rides loose on the staff
(a bore clear of it), and the pinion that drives the ratchet is fixed to the
staff. A freewheel joins them, driving in the winding direction. Then:

- a quarter turn of the crown winds the ratchet and the rotor does not move;
- the rotor turned forward winds the ratchet by the train's ratio;
- the rotor turned back winds nothing (winding in one direction, as the
  Valjoux 7750 does).

**Not in this round**, each for a stated reason:

- **Breguet teeth** on the clutch and winding pinion. A freewheel there
  would let the crown slip backwards, but square dogs cannot slip, and
  sawtooth dogs ride over each other only because the clutch LIFTS along
  the stem against its spring. That lift is a motion driven by the slip,
  which nothing models, and without it a slipping pose puts the teeth
  through each other. So the rotor still turns the crown through the
  keyless works, as it does today; the doc says so.
- **A reverser** (winding in both directions).
- **A ratchet's dead band.**

## Files

- `src/core/Occurrence.h` — `RelationKind::Freewheel`; `Relation::lead`,
  `Relation::drives`.
- `src/core/AssemblySolve.cpp` — `RelationValue`, the freewheel rule in
  `PropagateRelations` (both directions), validation, the loop exclusion
  where loop rows are built.
- `src/core/io/SerializeAssembly.cpp` — the kind string and fields;
  `SerializationInternal.h` v104; `docs/versioning/format.md`.
- `src/remote/RemoteAssembly.cpp` — `add_relation`'s freewheel case,
  `set_relation {lead}`, the describe rows.
- `src/ui/OccurrencePanel.cpp` — `lead` in the relations panel.
- `tools/designer_sdk.py` — `Asm.freewheel()`.
- `generators/watch/` — the split reduction wheel, the freewheel, the
  winding checks, the mesh table.
- `docs/modeling/watch.md`, `docs/remaining-watch-items-plan.md`.

## Verification

- `tests/test_assembly_relations.cpp`: input forward engages; input back
  slips and `lead` grows by exactly the slip; output driven forward
  overruns with the input still; output driven back drags the input;
  `drives: negative` mirrors all four; propagating twice changes nothing;
  a freewheel in a mate loop reports its error and the loop still solves;
  v104 round trip and the golden.
- `tests/assembly_relations_smoke.py`: over RPC, the same four cases;
  `drives` omitted or misspelled is refused naming both; `set_mate` then
  `undo` restores `lead`; a motion study leaves `lead` as it was.
- The watch: the three winding checks above, the exact sweep (the loose
  wheel's bore against its staff) and every clearance still VERIFIED.
- Headless and GUI suites; `benchmarks/runners/reference.py`.
