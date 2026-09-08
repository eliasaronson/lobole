# OM615 reference dimensions and limits

The reference target is the early **200 D/8, OM615.913**, rather than a mixture
of early and late 200 D/220 D specifications. This is a working parametric
assembly, not a production or manufacturing model.

## Sources inspected

- [Workshop manual for Mercedes-Benz 200/220/230 & 250 series](https://documents.cdn.ifixit.com/SWZXI3IJg11tfP3v.pdf),
  printed pp.23–24 (PDF pages 21–22): the diesel columns, not the adjacent
  petrol-engine columns. The supplied scan gives 87 × 83.6 mm, 1988 cc,
  21:1 compression and 1-3-4-2 firing order for the 200 D/8.
- [Proxmann technical catalog](https://www.proxmann.com/upload/file/catalog/proxmann_teknik_katalog.pdf),
  printed p.67 (PDF page 80), piston 71160800, specifically OM615.913/.915.
  The adjacent 71160900 entry is the longer-stroke 2.2 L engine and has a
  different compression height. The drawings and table were visually checked.
  Its printed `BD -63,3` entry is suspect and is **not** used as a bowl depth.

## Corrections made

All dimensions below are millimetres. The generator measures finished B-Rep
extents for the piston, pin, valve heads and rings; it does not merely print
the input constants. Results are written to `reference-dimensions.json`.

| Dimension | Previous model | Revised | Evidence |
|---|---:|---:|---|
| Piston compression height | 47 | 52.9 | Proxmann 71160800 |
| Overall piston height | 77 | 91.4 | Same drawing |
| Gudgeon pin diameter × length | 30 × 72 | 26 × 72 | Same drawing |
| Nominal ring widths | 2.5 / 2.5 / 4 | 3 / 2 / 4 | Same entry |
| Intake valve head diameter | 36 | 38.8 | Midpoint of manual's 38.70–38.90 |
| Exhaust valve head diameter | 32 | 33.2 | Midpoint of manual's 33.10–33.30 |
| Valve stem diameter | 9 | 9.92 | Within both diesel ranges on p.24 |
| Valve spring wire diameter | 4 | 3.8 | Diesel entry on p.24 |

Previously the drawn rings were another 0.6 mm narrower than their nominal
widths. They now have their catalog widths; a modeled 0.05 mm clearance on
each groove flank provides the axial fit. Seat insert outside diameters are
derived to fit around the corrected valve heads without crossing the bore
or each other; those outside diameters are not sourced factory dimensions.

The piston-height correction also exposed a cut robustness problem: the
block slab reaches z=242.9, whereas a sketch at that coordinate was stored
at 242.89999389648438. A drill starting there left a microscopic roof over
the hole. Head-bolt drills now start 1 mm outside the casting and extend
1 mm farther, preserving the intended blind-hole floor.

## Casting and joint revision

The supplied front photograph and Daimler sectional drawing are visual
references, not scaled production drawings. The block now has drafted upper
walls and radiused crankcase shoulders; the head has drafted walls with local
machined port pads. The sump section has rounded lower corners. The timing
cover crown follows the cam-chain envelope with a circular arc, and the cam
cover has concentric rounded shoulders with a 6 mm transverse wall.

The former cam-cover clearance holes broke through the outside edge. Integral
through-roof bolt towers now surround eight M6 clearance holes, with matching
gasket holes and blind threaded bosses in the head. The heads are accessible
from above; the CAD fastener probe selects 55 mm screws. This bolt pattern is a modeled joint, not
a sourced factory pattern. Piston skirts retain the catalog height on their
thrust faces and have derived fore/aft reliefs for the modeled counterweights.
These reliefs are also assumptions rather than a factory piston profile.
A modeled 0.5 mm valve-tip edge chamfer clears the wider stem at peak lift
while retaining the nominal stem diameter and valve length; it is not a
sourced factory detail.

Manifold runners now use analytic circular elbows, with a tangent normal to
the gasket face and a bend radius exceeding the outside tube radius. The
collector spans all four runners. Additions must preserve existing volume;
bore and stud cuts cannot remove more than their tools. These checks exposed
missing manifold material that an interference check alone had missed.

At the elbow-development checkpoint, the standalone exhaust was one valid
B-Rep, 693,841 mm³, and built in 5.2 s on the test machine. The final
collectors were subsequently extended beyond the complete outer-runner
envelopes to eliminate protrusions through their end caps. The preceding spline version built in 31.4 s but lost
most of its material, so these timings compare different resulting geometry.

These changes improve casting form and joint construction. They do not
establish mold parting, core withdrawal, machining access, casting allowances,
or production tolerances. Physical material assignments still use the CAD
library's steel/aluminum approximations; appearance is not a material certificate.

## What the model still does not establish

- The 21:1 figure identifies the reference engine. The model does **not**
  demonstrate that compression ratio. Rod length, the 3.2 mm crown-to-deck
  gap, bowl shape and prechamber volume remain modeling assumptions.
- Bearing, piston and pin fits still use the existing exaggerated clearance
  convention. Exact clash checking permits smaller fits, but reducing all
  of them requires sourced tolerances and a separate validation pass.
- The head casting height, valve placement, port routing, rocker arrangement
  and retainer construction need further reconciliation with the drawings.
  In particular, the manual's diesel valve drawing (Fig. A:19) shows cap and
  lock nuts; the current collet construction is an approximation.
- Injection pump, fuel lines and several fasteners remain absent. Zero
  interferences proves clearance of the modeled parts, not completeness.

Keep these distinctions when presenting or extending the model. A dimension
derived to clear an adjacent part is not a factory measurement.

## Verified revision

The final run builds **77 parts / 249 occurrences** in 65.7 s. All 77 parts
pass the B-Rep validity check and contain exactly one solid body. Its complete
standing sweep checks 823 exact candidates (30,053 cleared by bounds) in
13.12 s with zero clashes. The 49-pose, 720° exact motion study checks 20,429
pairs in 166.06 s, with zero clashes and all poses solved. Measured stroke
is 83.600 mm for all four cylinders, all eight valve lifts are 8.0 mm, and
the measured firing order is 1-3-4-2. These are sampled motion checks.

Total generation, validation, saving and GLB export took 247.56 s. The saved
project is `/tmp/om615-revised/om615.dsgn`; its animation is `om615.glb` in the
same directory. `reference-dimensions.json` records actual extents, piston
compression height and stem diameters. `motion-report.json` preserves the
motion result. These generated files are local artifacts, not tracked sources.

The CAD arc-path fix and exact-standing SDK gate were checked with the
1,119-test headless suite and the SDK smoke test, both passing.

## Workshop-manual follow-up (September 2026)

The later photographs include a 220 D. This generator retains the early
OM615.913 **2.0 L** bore and stroke; those photographs guide the external
arrangement, not a change to 2.2 L dimensions.

The supplied *Mercedes-Benz Service Manual Engines 615, 616, 617.91.pdf*
was inspected by bookmark and selected rendered pages, rather than sending
its entire scan through context. PDF page numbers below are one-based:

| Reference | Used for |
|---|---|
| 05.4-118/1, PDF 220 | Machined head height 84.8–85.0 mm (84.0 mm is the refacing limit, not the nominal height) |
| 05-100, PDF 178; 05-235, PDF 257 | Valve arrangement and single-side rocker architecture |
| 07-137, PDF 371 | Stepped nozzle holder, wrenching sections and leak-off connections |
| 07.1.4-230/1, PDF 424 | 27 mm injector socket; inclined injector/prechamber installation and sealing plate |
| 14-470, PDF 489 | Intake arches above the exhaust, on the same side of the head |
| 07-200, PDF 387–389 | Injection-pump installation context, without a dimensioned mounting pattern |

The earlier 155 mm head height confused the cam-axis datum with the machined
head height. The head is now 85 mm tall, with raised cam and rocker supports.
Both valve rows and both manifolds are on +X; the modeled order is
E–I / I–E / E–I / I–E, feeding two paired intake mouths and four exhaust
ports. One rocker shaft serves all eight rockers. The outer cam bearings
were repositioned to clear the end retainers.

The upper cover is one hollow B-Rep enclosing both the rocker gear and the
cam-chain crown, with a lofted transition and a continuous gasket outline.
The lower timing housing meets it at the head datum. Low outboard mounting
ears provide vertical screw access; the previous tall bolt towers would
have required screws longer than the current standard-parts table supplies.
Head screw heads sit in counterbores below the cover gasket.

The injectors now have turned shoulders, two wrenching sections, an axial
fuel inlet and two small upward leak-off nipples. Inclined stepped
prechambers extend through the head to the fire face. Four separate nozzle
sealing plates are included. The injection-pump mounting boss has a 60 mm
blind register, three mounting holes and an `injection_pump_mount` frame.
The pump, timing-device connection and fuel pipes remain absent.

Only the head height and 27 mm wrench size above are newly sourced numeric
dimensions. Injector lengths, prechamber steps/angle, ports, cover attachment
and the entire pump interface are **modeling estimates**. In particular, the
pump register and bolt pattern must not be used to order or machine a mating
pump. The manifold envelopes are closer to the photographs, but are not
reconstructions of factory casting/core drawings. The valve caps/retainers,
water/oil routing, casting process and manufacturing tolerances still need
further work. A valid solid and a clean interference report cannot certify
fluid separation or machinability.

`reference-dimensions.json` now measures the machined head datums, injector
wrench flats and pump register from the B-Rep and records their evidence
status. It also checks the shared manifold-side boundary. Imported catalogs
currently support `bearing-v1`; engine dimensions are not disguised as bearing
records, and no manufacturer dataset or manual scan is bundled with this
engine. The existing standard-parts fitter still supplies the actual screws.

### Follow-up validation

The final generator builds **78 parts / 250 occurrences** in 85.21 s.
The standing sweep checks 806 exact candidates (30,319 cleared by bounds)
in 14.60 s, with no clashes. The 49-pose, 720° exact motion study checks
19,327 pairs in 208.69 s, with no clashes, 83.600 mm stroke, eight 8 mm
valve lifts and firing order 1-3-4-2. All 78 parts passed B-Rep validity
checks. This validates the nominal sampled mechanism, not production fits.

The final project is `/tmp/om615-workshop/om615.dsgn`. The output directory contains
`standing-report.json`, `motion-report.json`, `reference-dimensions.json`,
`timings.json` and `rpc-timings.json`. Full generation, validation and saving
with `--no-export` took 311.80 s (5 min 12 s). The first complete revised run
before the edge-query cleanup took 327.26 s, with a 99.64 s build. The original
model's same-session baseline was 287.22 s total / 73.04 s build. Thus the
cleanup helps, but the more detailed model and corrected fitting are still
slower overall than the original. These are individual runs, not a statistical
benchmark; kernel warm-up and system load also affect wall time.

The final headless regression suite passed **1,140 / 1,140 tests**. Focused
coverage includes translated oblique rounded profiles, truncated/inside-start
axis rays, insufficient stocked screw length, and fastener action behavior.
The saved engine was also inspected from both manifold and injection sides,
including an enlarged injector view. Review images and `geometry-report.json`
are beside the final project.
