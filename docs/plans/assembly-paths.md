# Assembly paths: insertion by sections, and sequences that say how parts go together

## The problem

`check_insertion` answers a simple question slowly and too narrowly.

- **Slow.** At every sample (and every turn it tries) it runs an exact,
  non-destructive OpenCASCADE boolean between the two solids. A herringbone
  planet against its ring takes 6–10 minutes, a counter wheel in its housing
  about 30 s, and the RPC holds the frame while it runs. The travel and press
  lengths are quantized to the sample step, and a shoulder the part seats on
  reads as a sliver of press.
- **Narrow.** A slide is one straight line along one mate's axis, onto the
  one part that mate joins. A fastened mate's Z need not be the part's axis
  (the engine's wrist pins and pulleys were swept the wrong way), and a split
  block, a bolted cap or a pin joining two parts cannot be stated at all, so
  the engine's crank, rods and pistons "fail" by a path nobody would use.
  The Strandbeest's generated assembly order (`check_order`) lives in one
  generator, and nothing checks a part against everything already placed.
- **Two bugs met on the way:** a project opened from file gives no bodies
  for a frames-only part (the planetary's carrier), so its pairs are refused.

## The shape of the fix

1. **Insertion by sections.** Along the slide axis, each part is cut into
   bands where its cross-section is constant (an extrusion, a bore, a tooth
   band is one band; a revolve or a chamfer is sliced finely), each band a 2D
   outline in the axis frame (from the B-rep's section, or the mesh's where
   the B-rep is not needed). For a pair of bands the slide offsets where
   they overlap form an interval found exactly from the band ends; whether
   they overlap is one 2D polygon intersection, the printed allowance a
   polygon offset first. Turns about the axis rotate outlines. The travel
   under interference, the press at the seat and the deepest overlap come
   out exact, with no step; a face meeting at a band boundary has zero area
   and is a contact, not a press. One exact boolean at the worst offset
   confirms the answer. The answer's shape (`check_insertion`'s returns and
   the `insertion` requirement) stays, plus what it confirmed.
   - Timings measured before and after on the counter wheel, a planetary
     planet in its ring and the clock's stage 2, in the docs.
   - The existing six insertion cases and `insertion_smoke` still pass
     unchanged; new cases for a shoulder (contact, not press), a press
     measured to its own length with no step, and a helical (herringbone)
     pair that must fail axially.
2. **Assembly sequences.** A document object on the assembly: ordered
   steps, each naming the occurrence it places and its path — a direction
   (default: the axis of the part's own cylinder that the mate seats, not
   the mate frame's Z), a distance from clear, and an optional turn about
   it (a bayonet, a screw's thread when the pair is threaded, which is then
   judged by the thread's own fit, not refused). Each step is checked
   against every occurrence placed before it, not only its mate partner.
   - An RPC to set, get and check a sequence; a requirement kind
     `assembly_sequence`; serialized (`vNEXT`), with its golden.
   - A step that cannot be checked (no path, no bodies) is refused naming
     why; a sequence that misses an occurrence says which.
   - The Strandbeest's `check_order` is rebuilt on it (same 25 steps); the
     clock states both stages' sequences (replacing the per-pair insertion
     requirements, which become its steps); the engine states its real
     order: crank into the lower block, caps down their bolt axes, pistons
     and rods joined by the wrist pin last, and so on.
   - Tests: a step blocked by an EARLIER part (not its mate partner) fails;
     a cap lowered along its bolts passes where a slide along the bore
     fails; a pin through two parts passes only after both are placed; a
     wrong order fails, the right one passes.
3. **Off the frame, and the carrier.** `check_insertion` and the sequence
   check run as jobs as the requirement checks do; the frame never waits on
   them. A frames-only part opened from file gives its frames as before
   and refuses insertion by name rather than silently; a test opens a
   saved planetary and checks its carrier's pairs are answered or refused
   by name, not dropped.
4. **The other generators, on the fast check.** The engine and the
   Strandbeest checked in full by their sequences, the planetary, jar and
   counter rescanned; anything that fails is fixed in its generator or
   reported with why it is right to fail. Engine jobs re-exported and
   sliced if its geometry changes.
5. **The same mistake elsewhere — measured first.** Profile
   `run_motion_study leave_at` on the labelled Strandbeest walker (~100 s a
   call) and `check_print` on a labelled plate; where the time is exact
   booleans in an inner loop that a mesh or section query answers, move it
   and keep one exact confirmation. Report each with before/after timings;
   anything that is not that pattern is reported, not rebuilt.

## Files

- `src/core/MeasureInsertion.{h,cpp}` (sections), a section helper beside
  it; `src/core/` assembly sequence model and its serialization
  (`src/core/io/SerializeAssembly.cpp`), `src/remote/RemoteAssembly.cpp`,
  `RemoteInspection.cpp`, `RemoteRequirements.cpp`; job plumbing as the
  requirement checks use.
- The carrier bug wherever opening drops a frames-only part's bodies.
- `generators/strandbeest/`, `generators/clock/`, `generators/engine/`,
  `generators/_lib.py`.
- Docs: `docs/inspect/README.md#insertion`, `docs/assemblies/`, the
  requirement kinds, `docs/versioning/format.md`.

## Verified by

- every new test failing before its change, the existing insertion tests
  unchanged and passing;
- timings before and after, written in the docs;
- the clock, Strandbeest, engine, planetary, jar and counter generator
  tests;
- jobs whose geometry changed re-exported and sliced clean in the Bambu
  CLI;
- the headless, GUI and generators suites.
