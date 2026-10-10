# Body materials after a Boolean, bodies by name, and mesh clash resolution

## The problem

**A Boolean probably drops the body's material.** Material and appearance
are stored on the feature that created a body and stamped onto bodies after
each regeneration, keyed on `createdBy` (src/core/Document.cpp, the
material stamp).

- A Boolean erases its operands and makes a new body owned by itself
  (BooleanFeature.cpp), so the stamp then writes the Boolean's own
  material.
- That is empty unless someone set it, and it overwrites the "inherit the
  target's material" assignment the Boolean makes.
- So a part whose body was given a material, then unioned, likely comes
  out with none. Nothing tests it.

**Addressing a body is awkward.** `set_material` and `set_appearance` take
a creating feature id or a body index.

- Both change as features are added. The planetary generator set
  materials on ids that a later Boolean had replaced, and had to look
  bodies up by name instead.
- A resolver that takes body names already exists (`ResolvePartBodies`,
  RemoteHelpers.h) for requirements, drawings and sections.

**The fast clash check is coarse next to small gaps.** The mesh clash mode
of motion studies tessellates each body at 0.2 % of its own diagonal (0.2
mm on the planetary's ring). With print-in-place gaps of 0.1 mm the
triangles are coarser than the gap.

- It passed, and the worst sample is re-checked exactly, but the negative
  control that proved it catches a misplaced planet was run at 0.25.
- There is no way to ask for a finer mesh. `Mesh::Build`'s deflection
  argument only changes the number it reports, not the tessellation.

## The shape of the fix

**Material after a Boolean:**

- A test first: a body given a material, then unioned, then subtracted
  from, keeps its material and its density in mass properties.
- Then fix the stamp so a Boolean's result keeps its target's material
  unless the Boolean sets its own.
- The same check for body pattern, mirror and split, which also re-own
  bodies.

**`body_name` on `set_material` and `set_appearance`:**

- Resolved by `ResolvePartBodies`.
- A name that matches several bodies is refused, naming them.
- A name that matches none is refused, listing the bodies there are.
- The write still lands on the creating feature, as today. The docs say
  so: a later Boolean renames the body.

**Mesh clash resolution follows the requested deflection:**

- `collide::Mesh::Build` tessellates at the deflection it is given.
- `add_motion_study` takes `clash_deflection_mm`; it defaults to today's
  0.2 % of each body's diagonal.
- The study's `deflection_mm` reports what was actually used.

**The planetary asks for a clash deflection under its gap,** and its
negative controls (a planet half a tooth out, and 0.4 mm out) become part
of the generator's run at the current gap.

## UI

- **Bodies by name need no UI change.** The feature tree already lists
  bodies by name, and sets a body's material from its right-click menu.
  That menu writes the creating feature too, so the Boolean fix is what
  makes it stick.
- **The clash deflection is script-only.** The Motion window's Validate
  always checks exactly (src/ui/OccurrencePanel.cpp), so a mesh
  deflection has nothing to set there.

## Files

- `src/core/Document.cpp` (the stamp), `src/core/features/BooleanFeature.cpp`,
  `src/core/BodyMode.cpp`, `tests/test_materials.cpp` or a body-mode test.
- `src/remote/RemoteModeling.cpp` (`set_material`, `set_appearance` rows and
  handlers).
- `src/core/MeshCollide.{h,cpp}`, `src/core/MotionStudy.cpp`,
  `src/remote/RemoteMotion.cpp`, the motion tests.
- `generators/planetary/build.py`, docs/assemblies/README.md (motion
  studies), docs/modeling/README.md (materials), docs/scripting/rpc.md.

## Verification

- **The material test:** fails before the fix, passes after, for union,
  subtract, pattern, mirror and split.
- **`body_name`:** sets the material of the named body; refuses an
  ambiguous name and an unknown one.
- **Mesh clash:**
  - a finer `clash_deflection_mm` reports that deflection;
  - on a pair 0.1 mm apart it reads them as clear;
  - on a pair 0.05 mm overlapping it reads a clash.
- **The planetary:** its negative controls fail at 0.1 mm a side, and its
  real run passes.
- **Suites:** headless and GUI.
