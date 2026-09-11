# A reverser: the rotor winds both ways

## The problem

The watch winds in ONE direction. The rotor's wheel drives the ratchet
through a freewheel, so turning it back winds nothing
([freewheel-plan.md](freewheel-plan.md)) — a Valjoux 7750 does exactly this,
but most automatics wind both ways, and the mechanism that does it is a
REVERSER: two one-way wheels fed in opposite senses, so whichever way the
rotor turns, one of them drives and the other slips.

Two freewheels driving ONE output is the case the solver does not handle.
Propagation walks relations and settles each (mate, DOF) node once, and a
freewheel acts only when one of its two ends is not settled yet. In a
reverser both ends of the slipping freewheel are already settled — its input
by the gear from the rotor, its output by the gear from the ratchet the
driving path just turned — so it is never reconciled, and the residual check
reports "not satisfied" on a mechanism that is behaving correctly.

## The shape of the fix

**The walk moves parts; one pass at the end reconciles the history.**
Today a freewheel updates `lead` inside the walk, wherever it is reached.
Instead:

- **Slipping does not settle the output.** Reached from its input, a
  freewheel drives its output only when the drive line is at or ahead of it;
  when it is behind (slipping) the output is LEFT for another relation to
  set — the ratchet's gear, in a reverser. Overrunning does not settle the
  input either, for the same reason.
- **After the walk, every freewheel is reconciled** from where its two ends
  actually finished: the gap between the output and the drive line is slip,
  and `lead` absorbs it. A gap the other way — the output BEHIND the line,
  where this freewheel should have been driving it — is two drivers
  disagreeing, and is reported on the relation.

This is order-independent, which is what the two-path case needs, and a
single-path freewheel behaves exactly as it does now: the same four cases,
the same idempotence (the reconcile pass finds a zero gap and does nothing).

**A freewheel in a cycle HOLDS rather than drags.** A lone clutch whose
output is pushed back engages and drags its input with it, which is what a
bicycle's pedals do. Inside a reverser that runs away: dragging one wheel
back turns the other one forward, which drives the output further the way
that caused the drag, and each solve multiplies it — the watch's ratchet
reached 5e10 degrees before this was understood. The real mechanism LOCKS
there, and a lock is a constraint rather than a drive, so the relation says
its output stands behind its line and moves nothing. A freewheel drags only
when its input cannot be reached from its output through the other
relations.

**Nothing may drive the winding train backwards**, for the same reason: the
clutches hold, and the pose would be one the mechanism cannot reach (a real
watch has a click there). The watch's mesh sweeps now run forward from where
each driver stands and leave it there, and the winding check turns the crown
a quarter turn from where it is rather than resetting it to zero.

**The watch gets a real reverser**, laid out as an ETA 2824's is:

- The reduction wheel becomes reverser wheel A, where it already stands.
- Reverser wheel B meshes wheel A — wheel to wheel, so it turns the other
  way — at (1.297, 6.130): 4.000 from A and 2.950 from the barrel, which are
  the two pitch distances. It clears the rotor pinion by 3.5 mm and the crown
  wheel by 4.6, and its pivot is still inside the barrel bridge.
- Each wheel rides loose on its own pinion through a freewheel, and BOTH
  pinions mesh the ratchet. There is no room between the barrel bridge and
  the rotor for another gear level, so the pinions drive the ratchet
  directly rather than through a common reduction wheel.
- Both freewheels drive the same way (the pinion sense that winds), so the
  rotor turning either way engages one and slips the other.

The A–B mesh is a wheel meshing another of ITS OWN tooth set, which the
cycloidal design has not had to do before: every pair so far is wheel to
pinion. `check_meshes` measures the contact ratio and the exact sweep checks
the flanks; if the shared profile cannot serve both meshes, the reverser
wheels take a tooth set of their own.

## Files

- `src/core/AssemblySolve.cpp` — the two settle rules and the reconcile pass.
- `generators/watch/watch_parts.py` — the second reverser wheel and pinion
  (the existing pair, renamed).
- `generators/watch/watch_spec.py` — wheel B's position, the A–B mesh pair.
- `generators/watch/watch.py` — the mates, the two freewheels, the mesh
  table, the winding checks.
- `docs/modeling/watch.md`, `docs/freewheel-plan.md` (the reconcile rule),
  `docs/remaining-watch-items-plan.md`.

## Verification

- `tests/test_assembly_relations.cpp`: two freewheels into one output — the
  input forward drives through one and slips the other, backward swaps them,
  and neither reports an error; the four single-path cases and the
  propagate-twice check still hold; a freewheel whose output is behind its
  line with the input settled is reported.
- Over RPC the two-path case is the watch's own reverser below; the
  relations smoke has three occurrences and would have to grow a mechanism
  to say less.
- The watch: the rotor turned either way winds the ratchet by the same
  amount, the crown still winds without moving the rotor, undo restores
  both freewheels, every mesh sweeps clean and the clearances hold.
- Headless and GUI suites; `benchmarks/runners/reference.py`.
