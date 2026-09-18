# The manufacturing surface — what is missing

A survey, not a plan: the gaps a manufacturing-facing audit found, each with
the evidence that makes it real, so that a later round can pick one up without
re-deriving why it matters. Written after the sheet-metal and grill rounds,
when pipe bending, fasteners and sheet metal had each shipped and the question
was what they do not yet reach.

The ones that became work have a plan of their own and are named here only to
say where they went.

## The signal worth trusting

A gap that several rounds each worked around SEPARATELY is a gap; a gap one
round noticed is a wish. Ranked by that:

**1. Workholding does not exist.** Five documents, four rounds, and the CAM
docs mark it "Wanted" in so many words. `docs/cam/reach-and-fidelity-plan.md`
— "Fixture and clamp collision stays excluded: no fixture solid exists in the
model"; `docs/cam/dogfood-round.md` leaves "check the tool actually clears the
clamps" to the operator's eyes; `docs/cam/simulation.md` excludes machine and
fixture collision as "a different model". The stated prerequisite is small: a
solid the verifier can see.

**2. A press or interference fit has no representation.** Four rounds, four
different workarounds. The watch modelled the cannon pinion's friction fit as
a 1:1 relation ("that relation IS the friction fit",
`docs/modeling/automatic-watch-plan.md`); the air-cooled engine OPENED real
clearances to 1.5–3 mm because tighter than the collision mesh's own
deflection reads as contact; OM615 kept an "exaggerated clearance convention";
the grill simply assumes them. And `check()` has no way to be told a contact
is deliberate (`docs/modeling/aircooled-dogfood.md`).

**3. A weld is not a thing in the model.** Three rounds. `grep weld src/`
finds mesh-vertex welding in FEM and a measure readout — nothing else. The
bicycle solved it in the generator (`bike_shop.Weldment`: solid rods unioned,
bores drilled after); the grill expresses ~14 welded joins as flush mates
indistinguishable from bolted ones; the dock wrote the rule down as a
convention you must remember — "intra-part overlaps are welds … never assert
clashes == [] at body scope on welded-look parts" (`docs/dogfooding.md`). A
weld is currently a clash you have to know to ignore. Partly addressed by the
tube round (`docs/tube/plan.md`) for tube frames; a weld as a first-class
record — length, size, symbol, a weld table — is still open.

**4. Nothing has ever been physically made.** `docs/roadmap.md` — "**Nothing
has cut a real part yet** — that is the check simulation cannot stand in for."
Three documents say it. Every number downstream of the post is unvalidated
against a machine and a caliper: the 0.4 % cut-length overcount, slug
retention, the Z-zero convention, and now the bend allowances a blank is cut
to. One real part, measured, would find more than a week of features.

## The shop's paperwork

**The drawing cannot say what a shop annotates.** No balloons or on-sheet
parts list, no section or detail views, no revision block, no GD&T or datum
symbols, no weld or surface-finish symbol; `TitleBlock` carries no material,
finish or general-tolerance field. All deferred together in
`docs/drawings/plan.md` on the grounds that assembly drawings should mature
first — they have, across four dogfoods since.

**STEP goes out naked.** `CADKernel::SaveSTEP` is a bare `STEPControl_Writer`
loop: no names, no colours, no assembly structure, no PMI. The IMPORT side
already uses XCAF, so the app reads more than it writes. This is the cheapest
item in this file by a wide margin.

**A bought part loses its name on import** (`docs/modeling/derived-*`), so a
BOM cannot name a vendor part.

## Procurement, which does not exist at all

A material carries appearance, density and FEM constants
(`src/cad/CADMaterials.h`) — and no cost, no stock form, no temper, no finish,
no spec or certificate. The BOM therefore ends at `total_mass_g`: no price, no
supplier, no part number, no make/buy flag, no lead time. Nothing packs
members into stock lengths, so "how many six-metre sticks do I buy" is
unanswerable, and nesting blanks on a sheet is deferred as its own workstream
(`docs/cam/stock-and-origin.md`).

Surface finish, roughness, plating and heat treat appear in NO document — not
even as a deferral.

## Standards and threads

**Seven ISO standards, geometry only.** No property class (8.8/10.9/A2-70), no
torque, no preload — `check_fasteners` checks length and engagement, never
load. Missing families, in the order the rounds wanted them: threaded and
clinch inserts, rivets, self-tappers, dowel pins, circlips, spring washers.
Bearings exist only as a user-imported catalogue, the opposite arrangement
from fasteners, while four rounds have hand-modelled one.

**A thread cannot be fully specified.** No class (`threadClass` is display
only), no imperial, no left hand, no modelled helix. A tapped hole is now
first-class in every way except that its callout cannot state a class — and
CAM cannot cut it (below).

## CAM cannot make what the modeller designs

Six operation types, milling only. No canned cycles, so no tapping and no
thread milling: a hole the app DERIVES as tapped has no operation that taps
it. No turning, no laser, plasma, waterjet or additive post. No cutter
compensation, no cycle-time estimate.

## Still-open defects in shipped features

These are faults, not wishes, and they rank ahead of anything above.

- A circular corner relief on a fold that is not square leaves the flat
  pattern non-planar — 1.67 mm of blank on 1 mm sheet at 60°
  (`docs/grill-dogfood-plan.md`, finding 9).
- `Verdict::outsideTravel` checks X and Y only, so a Z move past the machine's
  travel is unflagged (`docs/cam/reach-and-fidelity-plan.md`).
- The ~0.4 % cut-length overcount at the ramp seam, and so the cutting time
  derived from it (`docs/cam/gui.md`).
- `add_feature` refuses `cb_depth` on a simple hole naming one field rather
  than the pair, so the fix is a guess the first time (`docs/dogfooding.md`).
- The FPV build guide still carries the superseded, more generous clearance
  table that `fpv-drone.md` retired — a stale shop-facing number.
