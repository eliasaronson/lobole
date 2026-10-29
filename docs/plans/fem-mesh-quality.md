# FEM mesh quality: refinements that hold, and a peak that has settled

## The problem

A study's mesh decides how much its peak stress can be trusted, and three
things about ours were measured against closed forms, handbook factors, a
converged mesh and gmsh. All of it ran on the current build through RPCs,
with Netgen's python module and gmsh on the exported STEP beside it.

**What is already good.** Netgen meshes as well as gmsh does at a
comparable node count. On the five parts, the worst tet quality was within
±0.1 of gmsh's in either direction, and gmsh's default optimiser left
slivers on the thin plates. Second-order tets with midside nodes on the true
surface give bending within 1 %, even with ONE element through the
thickness:

| cantilever plate 60×20×t, auto size 4.2 mm | t = 2 | t = 1 | t = 0.8 |
|---|---|---|---|
| elements through the thickness | 1 | 1 | 1 |
| tip deflection vs a converged mesh | −0.8 % | −0.8 % | −0.9 % |
| bending stress at mid-span vs a converged mesh | −0.1 % | −0.6 % | −0.1 % |

So a thin wall needs no special sizing for a static study. Buckling is not
measured here; it is checked where it is built
([calculix-bridge.md](calculix-bridge.md)).

**1. A face refinement makes slivers, and one gave a fake peak with no
warning.** Plate 100×40×5 with a Ø10 hole in tension, global size at auto
(7.2 mm), a refinement on the hole face. The converged peak is 24.55 MPa,
and Heywood's Kt gives 24.22:

| hole refinement | worst tet quality | hole peak |
|---|---|---|
| 0.5 mm | 0.123 | 23.94 |
| 0.4 mm | 0.06 | 22.82 |
| 0.3 mm | 0.014 | **36.79**: +50 %, at a point 30° off the load axis, repeatable |
| 0.25 mm | 0.006 | 23.34 |
| 0.2 mm | | "volume meshing failed" |

`FemMesher.cpp` sets a refinement with the legacy
`OCCGeometry::SetFaceMaxH`. That array sizes the face's own triangles but not
its edges or the volume behind it, so a fine face meets coarse tets head on.
Netgen 6.2 sizes from `OCCGeometry::GetProperties(face).maxh` (what its
python `face.maxh` sets), which grades the size into the edges and the
volume. The same refinements through that route:

| hole face size | legacy (ours) | `GetProperties(face).maxh` |
|---|---|---|
| 0.3 mm | 7 088 tets, quality 0.014 | 22.5 k tets, quality 0.187 |
| 0.2 mm | fails | 44.8 k tets, quality 0.127 |
| 0.15 mm | fails | 84.9 k tets, quality 0.252 |

With `grading` 0.2 instead of the default 0.3, quality rises to 0.35–0.37.

**2. A peak at a hole or fillet is about 5 % low at the auto size, and
nothing says so.**

| part | at auto size | converged | handbook |
|---|---|---|---|
| hole plate | 23.41 MPa (−4.6 %) | 24.55 | 24.22 (Heywood) |
| shoulder fillet r 2 on a 26→20 bar | 19.83 MPa (−4.8 %) | 20.83 | 20.48 (Peterson, ±3–5 %) |

Shrinking the GLOBAL size does not find this. Netgen's curvature sizing
already sets the size at the hole, so the steps 5, 3, 2, 1.5, 1 mm moved the
peak by under 2 % each while it stayed 5 % low. The answer arrived only at
0.75 mm over the whole part, with 353 k nodes and taking 40 s. Refining only
the face the peak sits on converged in two or three steps, at 2.6 k–7.5 k
nodes and under 1 s. A convergence check therefore has to refine where the
peak is, not everywhere.

**3. The "away from the support edge" value drifts with the mesh.** A
peak on a clamp's or pin band's edge is a singularity, and it grows without
limit as the mesh refines. `FindSupportEdgePeak` reports the largest value
farther than ONE ELEMENT from the edge, which is meant to be the trustworthy
number. But one element shrinks as the mesh refines, so the value creeps
toward the singularity with it:

| case | mesh step | peak (singular) | "away" |
|---|---|---|---|
| clamped plate, t 2 | auto → 0.4 mm | 40.9 → 48.9 | 40.7 → 44.7 |
| camshaft crank end (pin supports) | 2.0 → 1.0 mm | 99.8 → 134.1 | 42.4 → 58.8 |
| handle pin | 1.25 → 0.63 mm | 212 → 269 | 103 → 189 |

Printed parts also report a stress across the layers, which governs PLA. It
has no "away" value at all, and it moved +65 % to +121 % on the same steps.
The woodcutter crank's interlayer factor of safety went from 1.08 to 0.65:
the verdict was set by the mesh at a pin band's end.

## The shape of the fix

### Refinements through Netgen's shape properties

In `fem::Mesh` (src/core/fem/FemMesher.cpp), each `settings.faceSizes` entry
sets `OCCGeometry::GetProperties(face).maxh` on the face from `faceMap`, in
place of `SetFaceMaxH`. When any refinement is set, `mp.grading` drops
from Netgen's 0.3 to 0.2.

Netgen keeps those properties in a STATIC table keyed by shape
(`global_shape_property_indices`), shared by every mesh in the process. That
table needs two rules:
- `fem::Mesh` holds one mutex from setting the properties until the volume
  mesh is done. Two study jobs meshing at once would otherwise size each
  other's faces. Our faces are deep copies, so their keys never collide, but
  the table itself is not thread-safe.
- It removes its entries when it returns, on every path. Otherwise the table
  grows with every run and holds every meshed copy alive. Netgen has no
  remove call, so the entries are reset to default properties, and the
  indexed map is rebuilt without our shapes.

### A peak that has settled

A static study gains `converge`: `auto` (the default), `always` or `never`.
It is saved in the study, and `run_study` takes it as an override for one
run. Any other value is refused, naming the three.
- `auto` runs the loop when the first solve took less than the
  `converge_auto_s` preference (10 s by default; set in Preferences and through
  `set_settings`), and keeps refining while each step stays under it. A step
  that would run past it is not started, and the result says
  `stopped: "time"` with the time it took.
- `always` runs the loop whatever it costs.
- `never` skips it. The result says `convergence: "not checked"`, so a
  peak that was never checked does not look like one that was.

Taking the first solve's own time means no estimate is needed: the study that
just ran is the measurement. After the solve:
1. Take the governing peaks: von Mises and, for a printed material, across
   layers. A peak that `FindSupportEdgePeak` puts on a support's edge is a
   singularity and never settles, so its "away" value is used instead.
2. Find the B-Rep face each peak node lies on. If the node is interior,
   use the nearest boundary face.
3. Halve that face's element size, starting from the size its elements have
   now (the mean edge length of its facets), and re-mesh and re-solve.
4. Stop when every governing peak moves by less than 2 %, or after four
   refinements.

The result gains `convergence`: the steps (face, size, nodes, each peak),
`converged`, and the last change in percent. The factor of safety's receipt
says whether its peak converged. A requirement on an unconverged peak (loop
ran out, stopped for time, or not checked) is judged on the value it has,
and carries a `not_converged` warning with the reason. `check_requirements`
lists those requirements under their own heading, so a green result that
rests on an unsettled peak is visible. The refinements the loop
added belong to that run and are not written into the study.

This also catches problem 1's kind of fake. A peak that jumps by 50 % on
one step and back on the next does not converge, so it is flagged.

Cost: each step re-meshes and re-solves the body. On the hole and fillet
that was under 1 s a step. On the woodcutter crank's pin-supported studies
it is minutes per step, because a pin study solves on the CPU. That is why
the default is `auto`: it skips them with a stated reason instead of making
every run wait.

### "Away" measured in millimetres, not in elements

`FindSupportEdgePeak` measures "away" as a distance from the support's edge
nodes, in mm, rather than by tet adjacency. The distance is derived from
the support, never a literal:
- a pin: a quarter of its bore radius;
- a fixed face: a quarter of the face's smallest bounding-box side.

When a mesh is coarser than that, at least one element is used, as now.
The same rule gains an across-layers variant: `max_across_layers_away_mpa`,
and `factor_of_safety_away` reads the governing one of the two.

## Verification

- **Refinements.** The hole plate, with hole refinements of 0.3, 0.2 and
  0.15 mm at the auto global size:
  - each meshes;
  - worst quality ≥ 0.1;
  - the peak is within 1 % of 24.55.
  
  Before the change, 0.3 gives 36.79 and 0.2 fails to mesh, so this fails
  first.
- **Netgen's table.**
  - Two refined meshes run back to back give the same mesh as each run
    alone.
  - After a mesh, the property table holds no shape of ours.
  - Two study jobs meshing at once under `--tsan` give the same result.
- **Convergence.**
  - The hole plate and the fillet bar at the auto size with `converge`:
    `converged`, within 1 % of the converged values above and within the
    handbook's tolerance, in at most three steps. Tagged `[slow]` if the two
    together take over 10 s.
  - A clamped cantilever: the clamp's singular peak is excluded, the
    away value settles, and the result converges.
- **Away in mm.** The clamped plate at auto, 1.5, 0.75 and 0.4 mm: the
  away value moves under 3 % across the steps, where it now moves 10 %.
  The camshaft crank end's study at its generator size and half: the
  across-layers away value moves under 5 %.
- **The woodcutter.** Its leaned-on studies are re-run with `converge`
  (generators/woodcutter/woodcutter_fem.py), and the crank's factors of
  safety are re-stated from converged, away-from-edge values. It is not a
  test, because it runs for minutes.
- **Docs.** In docs/simulation/README.md:
  - "Meshing" gains the refinement route and the property-table rules;
  - "A peak on a support's edge" gains the distance rule;
  - a new "A peak that has settled" section.
  
  The `run_study`, `set_study` and `study_results` describe rows gain
  `converge`, `convergence` and `max_across_layers_away_mpa`.
  `docs/versioning/format.md` gains the study's `converge` key (`vNEXT`).

## Files

- `src/core/fem/FemMesher.{h,cpp}`: the properties route, grading, the
  mutex and cleanup.
- `src/core/fem/FemSolver.{h,cpp}`: `FindSupportEdgePeak` by distance,
  across-layers away.
- `src/core/fem/FemStudy.{h,cpp}`: `converge`, the loop, `convergence` in
  the results, the study input key.
- `src/core/io/SerializeStudies.cpp`, `SerializationInternal.h`, a
  `vNEXT` golden, format.md.
- `src/remote/RemoteSimulation.cpp`: parameters, result keys, describe rows.
- `src/core/Requirements.cpp`, `src/remote/RemoteRequirements.cpp`: "not
  converged".
- `src/ui/StudyPanel.cpp`: a Converge checkbox and the result line.
- `tests/test_fem_mesher.cpp`, `tests/test_fem_study.cpp` (or the files
  the existing mesher and support-edge tests live in).
