# Internal gears and epicyclic relations

## The problem

A relation couples two mates' degrees of freedom: dof_b = ratio · dof_a +
phase (src/core/Occurrence.h `Relation`, src/core/AssemblySolve.cpp
`PropagateRelations`). Two things a geared mechanism needs do not fit.

**The sign of an internal mesh.** A `gear` relation given teeth derives
−za/zb, because external gears counter-rotate. An internal pair (a planet
in its ring) turns the same way. Today that takes an explicit signed
`ratio`, which overrides the teeth unchecked, or a `chain` relation, whose
name says something else.

**Three members.** A mate's DOF is measured relative to its parent. A
planet's revolute sits on the carrier, so its angle is relative to the
carrier, and the correct coupling involves three members:

- Willis: (ω_sun − ω_carrier) / (ω_ring − ω_carrier) = −Zr/Zs;
- a planet on its carrier: ω_planet,rel = −(Zs/Zp)(ω_sun − ω_carrier).

`generators/planetary/build.py` gets one fixed case right by deriving bare
ratios by hand with the ring held. A differential, a hub gear, or any case
where more than one member moves cannot be expressed.

Every relation today is binary:

- **A mate tree** solves by propagation: the first relation to reach a DOF
  settles it.
- **Inside a closed loop**, equality relations become least-squares rows
  (AssemblyLoopSolve.cpp).
- **A DOF that two relations both drive** is not refused. The second is
  reported "not satisfied" after the solve if they disagree.

## The shape of the fix

**`internal: true` on a `gear` relation** with teeth gives +za/zb. Giving
both `internal` and a `ratio` is refused, as teeth and ratio are today.

**A linear relation over several mates:** `type: "linear_n"`.

- It takes `terms: [{mate, dof, coefficient}, …]` and a constant:
  Σ cᵢ·dofᵢ = k.
- It has two or more terms. Two terms is today's linear relation.
- **Loop solver:** it becomes one row with a coefficient per member, all of
  whose members must lie in one looped component (today's filter, extended
  from two members).
- **Propagation:** it becomes a hyperedge.
  - When all members but one are settled, the last is solved for.
  - When fewer are settled, it waits.
  - A new root is chosen from its unsettled members in mate order, holding
    them where they stand, as a binary relation's unsettled `a` is held
    today.
  - The post-solve residual check covers it like any other relation.
- **`type: "epicyclic"`** is the form a person writes: `{sun, ring,
  carrier}` mates and `teeth_sun`, `teeth_ring`. It is stored as the
  linear relation Willis gives, with its tooth counts kept so the UI and
  `list_relations` show what was written.
- A planet's own spin on the carrier is a second linear relation: ω_planet
  − (−Zs/Zp)(ω_sun − ω_carrier) = 0. `epicyclic` can add it per planet
  when given `planets: [{mate, teeth}]`.

**Everywhere a relation is touched:**

- `add_relation` / `set_relation` / `list_relations`, and the help rows;
- configuration suppression (`RelationSuppressed`);
- flexible-placement remapping;
- the freewheel cycle search, which walks every member;
- the Occurrence panel's relation editor;
- the SDK helper.

The new kinds bump `kVersion`, with a golden and a format.md row. An older
build refuses the kind.

**The planetary moves onto it:** a real carrier mate driven through
`epicyclic`, no hand-derived ratios, and the same motion studies.

## UI

Today relations are listed and edited in the GUI but cannot be created
there (src/ui/OccurrencePanel.cpp, `DrawRelationsSection`). An epicyclic
relation nobody can add without a script is half a feature, so this plan
adds creation.

**Creating a relation:**

- The Relations section gains `Add relation`. It opens the existing "Edit
  relation" tool window in create mode, with a **Kind** combo first.
- The combo lists only what the window can make: Gear, Internal gear,
  Linear and Epicyclic. The other kinds stay script-only, as now.
- **Gear / Internal gear:**
  - the Drives and Driven combos (`DrawRelationSides`, as editing has);
  - "Teeth (drives)" and "Teeth (driven)" with `ExprInputDouble`;
  - the ratio, shown read-only and derived;
  - Phase.
- **Linear:** the same sides, with Ratio and Phase.
- **Epicyclic:**
  - Sun, Ring and Carrier combos over the mates that have a rotation
    (`DofMateChoices`);
  - "Teeth (sun)" and "Teeth (ring)";
  - a Planets list: `ListRow`s of a mate combo and its teeth, with
    `Add planet`.
- **Validation:** a mate used twice, or a missing member, is refused in
  the window's red line (`asmEditRefusal`), and Accept is disabled.

**Editing** a gear relation shows its teeth now that they are stored. It
shows the epicyclic form's members and teeth, not its raw coefficients.

**List rows:**

- The tail reads "gear 14:13", "internal 13:40" or "epicyclic 14/40".
- A "not satisfied" relation keeps today's red glyph, tooltip and editor
  line.

**GUI tests:**

- add a gear relation with teeth and see the derived ratio;
- add an internal one;
- add an epicyclic relation with planets, turn the sun's mate, and see the
  carrier follow;
- the refused cases.

## Files

- `src/core/Occurrence.h` (`Relation`, terms), `src/core/AssemblySolve.cpp`
  (propagation, residual check, freewheel search),
  `src/core/AssemblyLoopSolve.{h,cpp}` (rows).
- `src/core/io/SerializeAssembly.cpp`, `SerializationInternal.h`, a golden
  in `tests/golden/assembly/`, docs/versioning/format.md.
- `src/remote/RemoteAssembly.cpp`, `src/ui/OccurrencePanel.*`,
  `tools/designer_sdk.py`.
- `tests/test_assembly_relations.cpp`, an assembly smoke.
- `generators/planetary/build.py`, docs/assemblies/relations.md.

## Verification

- **`internal`:** a gear pair with it turns the same way at za/zb.
- **`epicyclic` with each member held in turn:**
  - ring held: carrier = Zs/(Zs+Zr) of the sun;
  - carrier held: ring = −Zs/Zr of the sun;
  - sun held: carrier = Zr/(Zs+Zr) of the ring.
- **A differential:** driving two members sets the third by Willis; driving
  one leaves the others held where they stand, and says so.
- **In a closed loop:** the same relation solves as rows, to the same
  answer as propagation.
- **Conflict:** two epicyclic relations that disagree report "not
  satisfied", as binary ones do.
- **Serialization and refusal:** the golden at the new version; an unknown
  kind refused by an old build.
- **The planetary:** the planets orbit at 1/(1 + Zr/Zs) of the sun, and the
  motion studies pass with the epicyclic relation in place of the bare
  ratios.
