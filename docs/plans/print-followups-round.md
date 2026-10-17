# Print follow-ups: the counter, the planetary, the seal, and four small gaps

## The problem

After the printed-features round, seven items from the showcase builds are
still open:

- **The counter is still the design that fused.** generators/counter/ was
  never rebuilt to the rules its print taught: every piece on the bed, 45°
  faces over other parts, measured air. `check_print`'s stacking item fails
  it, and its README says so.
- **The planetary does not build on a measured P1S profile.** Built at
  70 mm on K5 (air 0.4 mm), its meshing herringbone flanks leave 0.205 mm of
  vertical air between gears, and 12 pairs fail stacking. Its plate is also
  built around the origin, so `bed_fit` finds the sun and the ring off the
  bed and the sun in the keep-out zone. Its README marks its own quick-start
  command as failing.
- **The jar sealed with no gasket.** The lid's skirt lands flat on the jar's
  shoulder, and that PLA ring held water upside down. Nothing in Loböle
  models a contact as a sealing face, or judges whether a contact ring is
  closed all the way round and pressed.
- **The `intermittent` relation knows two laws**, uniform and Geneva. The
  clock's escape wheel steps uniformly, so the swing had to be large enough
  for a uniform step to fit behind the impulse face. A table law would let
  the wheel follow the impulse face.
- **`mass_properties` on an assembly sums solid masses**, while a part
  reports its printed mass.
- **The stacking item reads about 0.02 mm low** where a sample sits on a face
  boundary: the triangle a ray meets names the face, so near an edge it can
  name the wrong one.
- **`--fresh` refuses an unsaved scratch project.** Generators that reset
  between runs fall back to `new_project`.

## The shape of the fix

1. **The counter, rebuilt.** Every wheel and transfer starts on the bed. The
   stacked layout is replaced by one where no part prints over another, or
   prints over it only through 45° faces with the profile's air. It uses
   teardrop holes and tapered extrudes where they help.
   - The digits, the Geneva carry and the `intermittent` relation stay.
   - `check_print` passes every item, stacking included, with no waiver.
   - A negative control: the old stacked layout fails stacking.
   - The README says what changed from the fused print, and why.
   - If four digits cannot fit the bed under these rules, the generator
     says so and builds the most digits that fit.
2. **The planetary, on the bed and clear of its neighbours.**
   - The plate is placed inside the profile's bed, clear of its keep-out
     zones.
   - The meshing flanks leave the profile's air: the tooth's helix angle,
     the gap a side, or a stepped flank, whichever keeps the herringbone's
     45° self-support. The generator says which it chose and why.
   - `check_print` passes every item on K5.
   - A registered test runs it on a seeded P1S profile, not only on the
     built-in table.
3. **A seal is a requirement on a contact.**
   `measure: {kind: seal, a, b, motion_study?}` reports the contact between
   two placed bodies as rings: whether a contact path closes all the way
   round an axis, its least width, and its least interference or pressure.
   It passes when a ring closes at least the required width.
   - The jar states two: the gasket's, and the skirt-on-shoulder ring.
   - A test: a lid with a notch in its skirt fails.
4. **`intermittent` gains `law: table`**: a monotone table of
   (driver fraction, step fraction) over the window, refused if it is not
   monotone or does not end at 1. The clock's escape wheel follows its
   impulse face through it, and the clock's swing is re-solved. The
   generator reports the new swing.
5. **Assembly printed mass.** `mass_properties` on an assembly sums each
   occurrence's printed mass, centre and inertia when its part is printed,
   and its solid values otherwise, and says which. A test: an assembly of a
   printed and an unprinted part.
6. **Stacking at face boundaries.** Where the triangle a ray meets lies
   within a deflection of a face boundary, the air is taken off every face
   that touches that boundary, keeping the least. The hinge box's cone reads
   its modelled 0.6 mm (0.4 under K5) to within a micron.
7. **`--fresh` closes a scratch project.** It refuses a project with unsaved
   work only when that project has a path. An untitled scratch project is
   discarded, and the call says so.

## Files

- `generators/counter/`, `generators/planetary/` (and its test.json).
- `src/remote/RemoteRequirements.cpp` and `src/core/Measure*.cpp` (seal),
  `generators/jar/`.
- `src/core/Occurrence.*` and `AssemblyRelations.cpp` (the table law), its
  serialization, `generators/clock/`.
- `src/remote/RemoteInspection.cpp` (assembly mass).
- `src/core/PrintCheck.cpp` (stacking).
- `generators/_lib.py` (`--fresh`).
- Docs for each, RPC rows, `vNEXT` for the table law.

## Verified by

- a test for each item that fails before it;
- the counter, planetary, jar and clock generators, with their print jobs re-exported
  and sliced clean in the Bambu CLI;
- the headless, GUI and generators suites.
