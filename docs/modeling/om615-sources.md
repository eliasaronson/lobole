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
