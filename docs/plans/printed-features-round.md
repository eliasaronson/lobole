# Features the printed designs asked for

## The problem

Each showcase generator worked round a modelling feature that a printed
part needs and Loböle does not have:

- **Horizontal holes print with a sagging roof.** The engine's pressed pins
  in horizontal holes clashed with the flat bridged roof the generator first
  gave them, so it modelled teardrops by hand. Every printed design with a
  horizontal pin or bolt hole needs the same thing.
- **No taper on an extrude.** A cam lobe or a boss that prints standing
  needs its underside drafted to 45°. The engine's camshaft became a bar with
  eight pressed lobes because a one-piece cam would overhang under every
  lobe. The Draft feature drafts existing faces, and an extrude cannot be
  made tapered.
- **No TPU process.** The print allowance tables carry PLA and PETG. The
  engine's belt and springs and the jar's gasket stated every limit by hand,
  and `check_print` could not run `bed_fit` on a TPU plate.
- **No strength check for printed pins.** The Strandbeest's pins, the
  clock's arbors and the engine's wrist pins print standing, so they bend
  across their layer lines. The FEM can judge that already (the hook uses
  `layer_normal` and `interlayerMPa`), but nothing applies it to a pin.
- **FEM loads whole faces only.** The clock added a 0.1 mm step on an arbor
  so that a drum seat would be its own face to load.

## The shape of the fix

1. **A teardrop hole.** The Hole feature gains `printed: teardrop` for a
   hole whose axis lies across the print direction.
   - The hole's section above the axis rises to a point at 45° (or a flat
     at the top, `teardrop_flat`, at the profile's bridge limit).
   - The point is placed by the print direction of the part's print
     process, and the hole is refused on a part with no print process.
   - A vertical hole is refused with `teardrop`, naming why.
   - `check_print`'s overhang item passes a teardrop hole.
   - A pin in it clears the round part of the hole, and the clash check
     sees no overlap at the point.
2. **A tapered extrude.** Extrude gains `taper_deg`: the walls lean in (or
   out) by that angle along the extrude. It is OCC's `BRepOffsetAPI_DraftAngle`
   or a lofted prism, whichever holds the profile's edges and their names.
   - Topology naming must keep each wall's identity, as for a straight
     extrude.
   - Refused past the angle at which the profile collapses, naming that
     angle.
   - GUI field, defaults suite, golden (`vNEXT`).
3. **A TPU print process.** A built-in FDM TPU 95A table in the print
   allowance, from published practice: Bambu's TPU guidance and community
   gaps, each row's source stated and marked to confirm with a coupon. It
   has its own `stl_deflection`.
   - `check_print`'s items are judged for TPU as for PLA: overhang, the
     bridge limit (shorter for TPU), shells and clearance.
   - A TPU plate passes `bed_fit` under the PLA profile's printer (the bed
     is the printer's, not the material's).
   - The engine and the jar use it instead of their hand limits.
4. **Pin strength across layers.** `_lib.py` gains `pin_strength(d, part,
   face, load_n, …)`. It runs a static study of a pin as printed: the load
   on its face, held at its seat, using `layer_normal` and `interlayerMPa`.
   It reports the factor of safety against the interlayer strength.
   - The Strandbeest's crank stub under the hand crank's torque, the clock's
     barrel arbor under the weight, and the engine's wrist pin under a hand
     crank's push each state a factor in their receipts.
   - A factor under 2 stops the run, as the hook's prediction would.
5. **Loads on part of a face.** A load or a fixture takes
   `region: {face, …}`: a band of a face between two planes, or within a
   radius of a point. It is applied to the mesh's facets inside it.
   - The clock's 0.1 mm seat step goes.
   - A test: a cantilever loaded on half its end face against its hand
     solution.

## Files

- `src/core/features/Hole*` (teardrop), `src/core/features/Extrude*`
  (taper) and their serialization, goldens, the GUI tool sessions.
- `src/core/PrintAllowance.*` (TPU table), `src/core/PrintCheck.cpp`.
- `src/core/fem/` loads, `src/remote/RemoteSimulation.cpp` (region).
- `generators/_lib.py`, `generators/{strandbeest,clock,engine,jar}/`.
- Docs: docs/modeling, docs/tolerances, docs/fem, the RPC rows.

## Verified by

- **Unit tests:**
  - a teardrop hole's section;
  - the overhang pass;
  - a taper's volume against its frustum;
  - topology names across a taper edit;
  - the TPU table;
  - a region load's reaction against its hand value.
- **Generators:**
  - the engine with teardrop holes and a one-piece tapered camshaft, if
    the taper allows it (else said why);
  - the Strandbeest, clock and engine with pin factors in their receipts.
- **The headless, GUI and generators suites.**
