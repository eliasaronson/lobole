# Four app fixes the watch found

Building the watch's keyless works (docs/modeling/watch.md) hit four defects
in the app. Each was worked around in the generator; this fixes them in the
app, where every other model meets them too.

## 1. A mate takes only one relation

`PropagateRelations` settled a MATE the first time any relation reached it,
so a second relation into the same mate — the watch's clutch, turned by the
stem and slid by the yoke — was accepted by `add_relation` and never
applied. A relation the document holds and the solve ignores is exactly what
the refusal rule forbids.

**Fix.** Settle per (mate, degree of freedom): a cylindrical mate's turn and
slide are two nodes, so each takes its own relation. What remains after that —
the same degree of freedom driven twice, by relations that disagree — is
reported, not skipped: after the solve (and the loop pass), every usable
relation's residual is checked, and one left unsatisfied carries an error
naming the value it wants and the value it got.

## 2. A relation applies only when something drives it

With no driven mate (a plain `solve_assembly`), or after the driven chain,
`PropagateRelations` seeded every relation's A-mate at once and marked each
one settled. A mate that is both a follower and a driver was therefore never
written: a chain was applied at its ends only. The watch's stem side sat off
its relations' phases until something drove the stem.

**Fix.** Seed ONE root at a time — the A of the first relation neither of
whose ends is settled — propagate its whole component, then the next. The
driven mate still goes first, so its chain wins any race.

## 3. A configuration can only hide parts

An assembly configuration suppresses occurrences and nothing else. It cannot
suppress a mate or a relation, cannot hold a mate's offset or rotation, and
a mate or relation touching a suppressed occurrence still drives it. So a
watch's "crown pulled out" could not be a configuration.

**Fix.** `Configuration` gains `suppressedMates`, `suppressedRelations` and
`mateOverrides` (mate id, optional rotation, optional offset). They are an
OVERLAY applied inside `Solve`, never written into the mates, so the authored
values survive and switching back restores them:

- a suppressed mate, or one touching a suppressed occurrence, is not solved
  (it sits out like an unusable mate, without an error); likewise a
  suppressed relation, or one coupling a mate that is out;
- an overridden degree of freedom is substituted where the solve reads the
  mate, pinned in the loop pass, pre-settled in relation propagation (so its
  relations drive FROM it), and never written back;
- `set_mate` on an overridden degree of freedom is refused, naming the
  configuration (the override is edited with `set_configuration`), and the
  gizmo drag leaves it alone;
- activating a configuration, and editing the active one, re-solves.

`set_configuration` on an assembly accepts `suppressed_mates`,
`suppressed_relations` and `mate_overrides`, and REFUSES the part-only keys it
used to ignore (`parameter_overrides`, `suppressed_features`); the part
handler refuses the assembly-only ones. `list_configurations` reports the new
fields. The occurrence panel's configuration section lists mates and
relations to suppress.

Format **v101**: the three fields are written when non-empty (older files
load unchanged). A golden in `tests/golden/assembly/`, its load test, and a
row in `docs/versioning/format.md`.

## 4. A circular pattern of an adding extrude fails

A Pattern repeats the BODY its source created, and falls back to the source's
cutter for a cut (R14). An extrude that ADDS to an existing body creates no
body and keeps no tool, so patterning it failed with "source body not found".

**Fix.** An extrude keeps its tool for Add as well as Cut
(`Feature::ToolShape`/`ToolMap`/`ToolAdds`, renamed from the cut-only names),
and the pattern repeats the tool in the source's own mode — each copy fused
like the original.

## Files

`src/core/AssemblySolve.cpp`, `src/core/AssemblyLoopSolve.*`,
`src/core/Configuration.h`, `src/core/Occurrence.{h,cpp}`,
`src/core/io/SerializeConfigurations.cpp`,
`src/core/io/SerializationInternal.h`, `src/remote/RemoteConfigurations.cpp`,
`src/remote/RemoteAssembly.cpp`, `src/ui/OccurrencePanel.cpp`,
`src/ui/GizmoInput.cpp`, `tools/designer_mcp.py`, `src/core/Feature.h`,
`src/core/features/{ExtrudeFeature,PatternFeature}.*`; docs
`docs/configurations/plan.md`, `docs/assemblies/relations-plan.md`,
`docs/versioning/format.md`.

## Verification

- `test_assembly_relations.cpp`: a cylindrical mate turned by one relation
  and slid by another takes both; a plain solve applies a three-mate chain
  end to end; two relations that disagree on one degree of freedom leave an
  error on the relation.
- A configuration test: suppress a mate, a relation, an occurrence; override
  an offset; activate and deactivate — the authored values come back; a
  `set_mate` on the overridden degree of freedom is refused; v101 round-trip
  and its golden.
- `test_pattern.cpp`: a circular pattern of an adding extrude adds every
  copy's volume.
- The full headless and GUI suites.
