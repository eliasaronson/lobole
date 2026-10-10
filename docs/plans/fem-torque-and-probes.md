# FEM: torque loads and local stress probes

## The problem

**Torque has to be faked.** A study can apply only fixed, force and pressure
loads. To load the planetary's sun with a hand's torque, the generator
picked one tooth flank per planet with a twist-corrected selection heuristic
and put a tangential force on each. A shaft, a key, a knob or a gear under
torque all need the same workaround. A load at a point away from the face
(a lever's end, a bracket's hole) cannot be stated at all.

**The answer a study gives is the global maximum.** On the sun it sat in
the sharp inside corner where the knob meets the gear face. That stress
grows as the mesh is refined (2.7 MPa at 0.6 module, 3.6 at 0.4), so it is
a property of the mesh, not the part. The quantity a designer wants (the
tooth root, a fillet, a bore's edge) cannot be read. `probe_study` returns
a face's rigid motion and no stress, although `FemResults::vonMises` holds a
value per node and each boundary triangle knows its B-rep face.

**The units gate misses these units.** `tests/rpc_units_gate.py` does not
know `torque` or `moment` as dimensional stems, and has no `n_mm` suffix, so
a bare `torque` parameter would pass unnoticed.

## The shape of the fix

### Torque

- `LoadType::Torque` and `Load::axis {origin, direction}`.
  - A load on a cylindrical face takes the face's own axis by default.
  - Anywhere else the axis must be given, and is refused when it is not.
- Applied as a traction t = (T/J)·(â × (x − o)), with J = ∫ r² dA over the
  face. That is exactly the torque T about the axis and no net force on a
  symmetric face.
- `AssembleFaceLoads` (FemSolverInternal.h) gains the Gauss-point position
  it does not compute today. An assembly study splits a torque over
  imprinted pieces by J, not by area.
- RPC: `add_load {type: "torque", torque_nmm, axis_origin?, axis_dir?}`.

### Remote load

- `LoadType::Remote`: a force F at a point p carried by a face, as an
  RBE3-style distribution.
- t(x) = F/A + (I⁻¹ M) × (x − c), with M = (p − c) × F and I the face's
  area second-moment tensor about its centroid c.
- RPC: `add_load {type: "remote", force_n, direction, point_mm}`.

### Both loads

- **Serialization** writes the axis and point; a build that does not know
  the types refuses them. `kVersion` is bumped with a golden and a
  format.md row.
- **Input hash:** `FemInputKey` hashes the serialized study, so it covers
  the new loads unchanged.
- **GUI:** the Study panel's tools and rows, and a viewport glyph (a
  circular arrow about the axis; an arrow at the remote point tied to its
  face).

### Local stress

`probe_study` gains, per face: `von_mises_max_mpa` (with its location) and
`von_mises_mean_mpa` (area-weighted over the face's triangles), for any
static part study. The data is already there.

### Requirements and the gate

- **Requirements:** a `study_probe` measure (`von_mises_max` or
  `von_mises_mean` on a named face), so a requirement can hold a tooth root
  rather than the global peak.
- **Gate:** `rpc_units_gate.py` learns `torque` and `moment`, and the `n_mm`
  suffix.

### The planetary moves onto them

- One torque load on the knob's flats replaces the per-tooth flanks.
- The ring and planets are held by the fixed faces the mesh reaches.
- The receipt reports the stress probed at a sun tooth's root fillet face
  next to the global peak, saying which is which.

## UI

### Study panel load tools

`DrawLoads` (src/ui/StudyPanel.cpp) gains two arm buttons beside Fixed,
Force and Pressure. They arm like the others.

- **Torque:**
  - armed: "Torque (N·mm)" via `ExprInputDouble`;
  - a click on a cylindrical face takes its axis (and says so in a muted
    line);
  - a click on any other face needs an axis. An "Axis" combo with the
    choices `DrawAxisRefCombo` offers Revolve: X/Y/Z, datum axes,
    construction lines, custom. A click with no axis is refused in the
    panel's red `placeRefusal` line.
- **Remote:**
  - armed: "Force (N)", "Direction" (`ExprInputDouble3`) and "At (mm)"
    (`ExprInputDouble3`);
  - the point defaults to the clicked face's centroid pushed out along its
    normal by a tenth of the body's bounding diagonal, so the default is
    a lever arm the part can show;
  - after placing, the point can be edited in the load's row.
- **Defaults:** the magnitudes seed the way `SimulationArmTool` seeds
  Force (from the part's weight). A torque's seed is that force times the
  clicked face's radius.
- **Rows:** `ListRow`s like the rest: "Torque  <face>" with "390 N·mm" as
  the tail; "Remote  <face>" with "60 N".

### Glyphs

- **Torque:** a circular arrow about the axis, drawn with the rotate
  handle's arc and an arrowhead, in a new BC colour.
- **Remote:** an `ArrowGlyph` at the point, with thin lines to the face's
  centroid.

### Probing

- **Results** (`DrawResults`) gain a `Probe` arm button.
- **Hover:** with it armed, hovering a face shows its max and mean von
  Mises in a cursor label, the Measure tool's `DrawCursorLabel` look.
- **Click:** pins the face to a "Probes" list under the results. Each row
  reads "<face>  max 2.1  mean 1.4 MPa". The row menu offers Remove, and
  "Add requirement", which makes the `study_probe` requirement.
- **Marker:** the pinned faces' max locations get a small marker in the
  viewport.

### GUI tests

- place a torque on a cylinder;
- refuse a torque on a flat face without an axis, then place it with one;
- place a remote load;
- probe a face, pin it, and turn the pin into a requirement.

## Files

- `src/core/fem/FemStudy.{h,cpp}`: types, validation, building FaceLoads.
- `src/core/fem/FemSolver.h`, `FemSolverInternal.h`: traction assembly.
- `src/core/fem/AssemblyStudy.cpp`: splitting by J.
- `src/core/io/SerializeStudies.cpp`, `SerializationInternal.h`, a golden,
  docs/versioning/format.md.
- `src/remote/RemoteSimulation.cpp` (`add_load`, `probe_study`),
  `src/remote/RemoteRequirements.cpp`.
- `src/ui/AppState.h`, `src/ui/StudyPanel.cpp`, the viewport glyphs,
  `src/app/guitests/GuiTestsSimulation.cpp`.
- `tests/rpc_units_gate.py`, `tests/test_fem.cpp`, `tests/fem_smoke.py`.
- `generators/planetary/build.py`, its README; docs/simulation/README.md.

## Verification

- **Torque on a shaft's end face, the other end fixed:**
  - the twist angle matches TL/GJ;
  - the surface shear stress matches Tr/J, within the tet10 tolerances the
    beam tests use.
- **Net loads:** a torque on a symmetric face gives no net force; the
  traction sums to T about the axis to 1e-9.
- **Remote force at an offset:** the reactions at the fixed face equal F
  and the moment (p − c) × F.
- **Probe on the cantilever:**
  - the root face's max equals the analytical Mc/I within the existing
    tolerance;
  - the max and mean on a face the peak is not on are smaller than the
    global maximum.
- **Refusals:** a torque on a non-cylindrical face without an axis; a remote
  load without a point.
- **Serialization:** the golden at the new version.
- **GUI suite:** placing a torque load.
- **The planetary `--study`:** a root-fillet stress that changes by a few
  percent between 0.6 and 0.4 module, where the corner peak changes by a
  third.
