# A sketch's projection of an imported edge is checked against what was picked

## Problem

A sketch's `external` reference to an imported part names its face or edge
by position: `1/b1/face145;U1` is the second unnamed edge of the 146th face
the file listed. A changed file read again renumbers its faces, so the same
name lands on whatever the new file puts there. Projecting the four hole
edges of a servo horn and pointing the Import at a revised servo (holes
turned 20 degrees) failed one reference "no longer exists" and resolved
another to an unrelated 0.87 mm line elsewhere on the servo, which the
sketch projected without a word. That is a substitution, which the project's
rule forbids: an input that no longer means what it meant is refused,
naming it.

Direct edits already guard their imported faces with a fingerprint
(docs/modeling/README.md#direct-edit, rule 5). Sketch projections have no
such guard.

## Fix

1. **Fingerprint every projected element whose name is the file's own
   geometry**: the seed is an `Import`, the role positional, with no Derived
   prefix and no M/G step after it (an element an upstream edit modified is
   moved by its parameters on purpose, and a print would refuse that). The
   print is {shape, size, centroid}: for an edge its curve kind (line /
   circle / arc / other), its length and the centre of mass of the curve
   (seam-independent: a circle's is its centre); for a face its surface kind,
   area and centroid -- the direct edits' `FaceFingerprint` vocabulary. It is
   taken the first time the sketch projects the element, and carries the
   name it was taken for, so a client that edits `element` in a spec and
   sends the old print back gets a fresh print, not the old element.
2. **On regeneration the element under the name must still match the
   print** (1e-6 relative on size, 1e-6 of the body's diagonal on the
   centroid, as the direct edits). When it does not, or the name no longer
   resolves, the import's bodies are searched for the ONE element of the
   same kind whose own name is file geometry and which matches the print:
   - one match: the reference follows it (its name is rewritten; it is the
     same geometry, renumbered);
   - none, or several: the sketch fails, naming every lost reference in one
     error: "projected edge `1/b1/face145;U1` (a circle 7.85 mm long at
     (x, y, z)) is no longer in the imported file; re-pick it". Never the
     element that now holds the index.
3. Every lost reference is reported, not only the first.
4. The print is saved with the reference (`fingerprint` in a sketch's
   `external` entry): a format change, so `vNEXT` with its golden.

## Files

- `src/core/sketch/ExternalPrint.{h,cpp}` (new): the print, taking it,
  matching it, finding the element that matches, describing it.
- `src/core/features/SketchFeature.{h,cpp}`: `ExternalGeom::print`;
  `RegenerateExternal` checks, follows or collects the lost.
- `src/core/io/SerializeFeatures.cpp`: write/read `fingerprint`; the
  sketch `describe` row.
- `src/core/io/SerializationInternal.h`, `tests/golden/vNEXT-*.json`,
  `docs/versioning/format.md`: the format bump.
- `tests/test_external.cpp`: the regression tests.
- Docs: docs/sketch/README.md (projection), docs/interchange/README.md
  (editing an import), docs/known-bugs.md (entry removed).

## Verification

- Unit tests, each through a real Import of a BREP file rewritten between
  regenerations:
  - the same part with its faces renumbered (built in another order): every
    projection follows to the same geometry, under its new name;
  - the holes moved: the sketch fails naming every projected hole edge,
    and no projected entity sits on an unrelated edge;
  - the print round-trips through the file, and a print taken for another
    element name is dropped, not applied.
  The moved-holes test fails before the fix (one reference resolves to
  another edge).
- `tools/test-fast.sh`, `./tools/gui-tests-xvfb.sh`,
  `tools/ctest -L pygui -j1`.
- End to end: the a3 adapter (`scratch/imported-edits/a3_swap.py`) on a
  scratch instance: the Import pointed at the v2 servo; the Hole sketch
  either follows or names all four lost edges.
