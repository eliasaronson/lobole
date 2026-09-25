# Fixes from the PES-on-OM615 dogfood

## The problem

Mounting the injection pump on the OM615 took three agents and turned up
twelve places where the app got in the way. Three of them let a wrong model
through:

- **A cut can leave a solid inside out and call it valid.** In the intake
  flange, a slot's round ends coincided with the branch bores' cylinders. On
  one side the boolean removed nothing ("cut removed no material"). Drawn
  from the other face, it turned the bores inside out: a section is solid
  where the bores are, about 5000 mm² each, and BRepCheck still passes.
- **Motion studies never check parts that are rigid to each other.** The
  pump passed four exact motion studies with three standing clashes: its
  clamp plates overlapped, their screws sat in the holders, and a banjo sat
  in the primer pillar. A study reports a count of skipped pairs and nothing
  else.
- **`app.part(name)` opens an existing part of that name.** A second
  generator building into the same project writes its features into the
  first one's part. It bit twice: the camshaft and the oil filler cap.

Then a missing capability:

- **Sub-assemblies are rigid.** A pump placed as a sub-assembly cannot be
  driven by the engine's crank, so it went in flat: about 70 occurrences and
  37 tabs, grouped only by a folder. Its load configurations could not come
  with it, so the rack was held by a `set_mate` instead.

And eight that cost time without making anything wrong:

- **`fit_fastener` on a split rim** says "pick the RIM … a cylindrical face
  has an axis but no direction". The caller had picked a circular edge; the
  rim had been split into three arcs by the faces around it.
- **`bent_path`** follows a clear first error ("straight is 20 mm, bends
  need 27.3") with a false second one ("pipe path must be an open sketch
  chain or a 3D curve").
- **`describe(method=…)` ignores its filter** and returns the whole catalogue.
  `help` is unknown over the Python remote, although the MCP instructions
  point to it.
- **A standing-sweep clash does not say where it is.** Finding it took both
  occurrences' bounding boxes.
- **A study cannot leave the assembly at a chosen driver value.**
  `leave_at` takes a sample index, "start" or "last". Posing a crank at −24°
  took eight `set_mate` and solve steps to keep the slider-cranks on their
  branch.
- **`list_mates` reads wrong.** `type` is "Mate" on every row, and the mate
  kind is in `mate_type`.
- **`set_camera` accepts an up vector nearly parallel to the view** and
  returns an arbitrarily rolled view.
- **`Plane.yz(x)` always faces +X**, so a feature on the +X side of a part
  extrudes a negative distance.

## The shape of the fix

### Correctness

1. **A boolean must not change volume the wrong way.** A cut cannot add
   material, and every result must have positive volume. After every body
   boolean (cut, add, intersect), measure the result:
   - A result with negative volume, or with a closed shell oriented inward,
     is refused.
   - A cut whose result is larger than its input is refused.
   - An add whose result is smaller than its input is already refused
     (BodyMode.cpp).

   Each refusal names the feature and the two volumes. Reproduce the intake
   flange case first: the old om615_top.py slot, drawn 1 mm past the pad
   face, from the other face. It becomes a regression test.
2. **A motion study checks rigid pairs once.** Pairs rigid to each other
   cannot change their relation, so checking them at one pose is complete.
   The first sample checks every pair, rigid ones included. Later samples
   skip them as now. A clash found there is reported like any other, marked
   `rigid: true`. The skipped count stays, and the study's result says the
   rigid pairs were checked at sample 0.
3. **Opening a part by name is a choice, not a default.** `app.part(name)`
   makes a new part, and refuses when that name exists and this client did
   not create it in this session. The refusal names
   `app.part(name, create=False)` to open it, or `reuse=True` for a
   generator that means to rebuild its own part in place. Every generator
   and smoke in the repo is updated to say which it means.

### Flexible sub-assemblies

4. **A sub-assembly can move in its parent.** A sub-assembly occurrence gets
   a `flexible` flag. When set, the parent's solve and motion studies take in
   the sub-assembly's own occurrences, mates and relations, instanced for
   that occurrence and addressed by path. A parent mate or relation can then
   name an inner mate by path: an engine's chain relation drives the pump's
   camshaft mate inside the pump occurrence. A flexible occurrence also names
   which of its assembly's configurations it runs under, so the pump comes
   with its rack held at full load. A rigid occurrence behaves as today. The
   OM615 then places the pump as one flexible occurrence instead of 70 flat
   ones. This is the proof, and every OM615 check must still pass.

   **The state lives on the parent's occurrence.** A sub-assembly document is
   shared by every placement of it, and the resolvers hand it out `const`. If
   the inner poses were written there, two pumps would fight over them and
   the parent's undo would not see them. So `Occurrence` gains three things,
   all saved in the file so it re-solves the same way:
   - `flexible`;
   - `innerPoses`: inner path to position and orientation;
   - `innerMateDofs`: inner path and mate id to its rotation and offset.

   `Flatten`'s placement of an inner occurrence (`PlacementOf`) reads
   `innerPoses` when the root is flexible. Undo snapshots the parent, so it
   covers them.

   **The solver itself does not change. It is wrapped.**
   `ExpandFlexible(parent)` copies the parent and, for each flexible
   occurrence:
   - adds the inner occurrences with synthetic ids from a range disjoint
     from the document's counter, and maps each one to (root, inner path);
   - copies the inner mates and relations, with their occurrence and mate
     ids remapped;
   - filters them by the occurrence's own `configurationId`, through new
     `MateSuppressedIn`/`RelationSuppressedIn`, and pins that
     configuration's mate overrides as held DOFs.

   An inner occurrence that is fixed in its own assembly is not a new body.
   It folds into the root, and a mate on it becomes a mate on the root with
   that `innerPath`, which `ConnectorLocalFrame` already composes. A parent
   connector whose path enters a flexible unit at a moving part is re-pointed
   at that part's synthetic occurrence. `CollapseFlexible` writes the root
   transforms, the inner poses, the inner mate DOFs and the errors back, with
   errors reported by path. `Solve` skips all of this when nothing is
   flexible. `motion::Run` expands once, on its copy, before its first
   `PropagateRelations`, and runs every sample on the expanded document.

   **Naming an inner mate.** A mate is named by `{path, mate}`. The empty
   path means this document. That form is used by:
   - `Relation`'s two sides;
   - a motion study's driver;
   - `set_mate`.

   It is saved as `mate_a_path`/`mate_b_path`/`driver_path`, a format bump
   with a golden. `add_relation` takes `a_path`/`b_path`, and
   `add_motion_study` takes `driver_path`; both validate the walk the way
   `add_mate` validates `inner_path`. Two cases are REFUSED, naming what is
   accepted:
   - a parent configuration that holds or suppresses an inner mate: the
     sub's own configuration does that;
   - a freewheel relation inside a flexible unit: its history would be
     per-instance state.

   Only one level is flexible. A flexible occurrence inside a flexible
   sub-assembly is refused, following the precedent `ApplyAttachments` set.

   **What sees the moving parts:**
   - **A study's samples** carry a pose for each moving inner leaf, keyed
     by path next to the top-level ids, and mate values keyed by path.
   - **Clash checks** key bodies by leaf (root plus inner path) for
     flexible units, so a plunger is checked against its barrel and the
     rigid-pair rule compares leaf transforms. A plunger is never "rigid" to
     the block just because the pump's root is bolted to it.
   - **GLB motion export** writes one node per moving leaf.
   - **Live playback** pushes per-leaf transforms.
   - **A clearance assertion's `checkedKey`** hashes the inner poses.
   - **`solve_assembly`'s `placed_by` counts** are reported per path.
   - **Exploded views** still move the unit as one block. That is
     documented, not changed.

   **Found by the survey, fixed on the way:** an origin anchor with an
   `inner_path` returns the unit's origin, because `ConnectorLocalFrame`
   answers the origin case before walking the path.

### Friction

5. **`fit_fastener` takes a split rim.** Collect every edge of the body on
   the picked edge's circle (same centre, axis and radius) and fit to the
   whole circle. Refuse only when the arcs do not close, and say so: "this
   rim is N arcs covering M°".
6. **`bent_path` reports one error.** Its first refusal ends the call; the
   pipe step does not run on the path it refused.
7. **`describe(method=…)` filters**, and `help` works from the Python remote
   the way the MCP server's does.
8. **A clash says where.** Every interference and study clash entry carries
   the overlap's centroid and bounding box in world coordinates.
9. **`leave_at` takes a driver value.** A number is a value of the driver.
   The study poses the assembly there by stepping from the nearest sample,
   as a sample does, so slider-cranks stay on their branch.
10. **`list_mates` names the kind `type`**, matching `add_mate(type=…)`.
    The `"Mate"` constant goes. Every caller in the repo and the benchmark
    tools is updated.
11. **`set_camera` refuses an up vector within 1° of the view direction**
    and names the angle.
12. **`Plane.yz`/`xz`/`xy` take `flip=True`** for the opposite-facing plane,
    so a feature on the far side extrudes a positive distance. The SDK
    docstring and the modeling docs say so.

## Files

- Correctness: `src/core/features/BodyMode.cpp` and the boolean path;
  `src/core/Assembly*` (motion study pair selection); `tools/designer_sdk.py`
  plus every generator in `generators/` and `tests/*.py` that calls
  `app.part`.
- Flexible sub-assemblies: `src/core/Assembly*`, `src/core/io/SerializeAssembly.cpp`,
  `src/remote/RemoteAssembly.cpp`, a format bump with its golden, and
  `generators/om615/`.
- Friction: the fastener fitting code, the `bent_path` feature,
  `RemoteDispatch.cpp`/the MCP server (`describe`, `help`),
  `RemoteView`/the camera, `RemoteAssembly.cpp` (clashes, `leave_at`,
  `list_mates`), and `tools/designer_sdk.py` (`Plane`).
- Docs: the area READMEs and the RPC `describe` rows for every changed
  behaviour; `docs/versioning/format.md` for the bump.

## Verification

- Each correctness fix starts from a failing test that reproduces the
  dogfood case: the inside-out flange, the pump's three standing clashes
  (found by a study, with no standing sweep), and a second generator's part
  of the same name.
- Each friction fix has a unit test or smoke asserting the new behaviour and
  the refusal text.
- The flexible sub-assembly has solver tests (a relation into an inner mate,
  a configuration held per occurrence), and the OM615 full stage passes with
  the pump as one flexible occurrence and the same numbers: spill timing 24°
  on all four elements.
- Headless suite, GUI suite, `ctest -L pygui`, `benchmarks/runners/reference.py`
  and `negative_controls.py`: the refusal texts and `list_mates` changed what
  an agent sees.
