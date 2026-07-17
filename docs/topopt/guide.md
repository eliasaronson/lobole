# Topology optimization — guide

Generative design: given a design envelope, the supports it bolts to, and the
loads it carries, the optimizer removes material down to a target fraction
while keeping the part as stiff as possible. The result is an organic,
load-path-following shape you export for printing or **re-model clean geometry
against**.

Topology optimization is a kind of **Study** (like the FEM stress/modal
studies), so it lives in the Simulation workspace and reuses the same meshing,
materials, and boundary conditions.

## Quick start (GUI)

1. Model a **design envelope** — a solid blob that bounds where material may
   go (any modeled holes/cutouts are automatically kept out). A **bar** or
   bracket makes a far more interesting result than a cube. Assign it a
   material with stiffness data (e.g. Steel/Aluminum).
2. Enter the **Simulation** workspace (the toolbar). Add a study and set
   **Analysis → Topology**.
3. Arm **Fixed** and click the support face(s); arm **Force** (or **Pressure**)
   and click the loaded face(s). At least one of each is required.
   (Gravity/self-weight is deliberately ignored for topology.)
4. Optionally arm **Keep face** and click faces that must stay solid (bolt
   bosses, bearing seats), and set a **Mesh size** (mm) if the auto mesh is too
   coarse.
5. Set the **Volume fraction** (0.3 = keep 30% of the material) and **Run**.
   It runs on a worker thread — a progress bar shows the iteration count and
   you can **Cancel** (keeps the best shape found so far).
6. When it finishes, use **Display**: **Shape** shows the optimized grey shape;
   **Density** colormaps the density field. Drag **Threshold** to fatten/thin
   the shape (0 = auto, volume-matched).
7. **Shape guide (modeling overlay)**: tick this, exit Simulation mode, and the
   optimized shape appears translucent in the normal workspace so you can
   sketch/extrude clean parametric geometry against it.

## Getting a good result

- **Domain shape matters most.** A cube fixed and loaded on opposite faces has
  no room for a load path — you get a stubby sliver. Use a slender domain (a
  cantilever bar, an L-bracket) so the optimizer can form struts.
- **Volume fraction 0.4–0.5** gives a chunkier, cleaner truss than 0.3 on small
  parts. Lower fractions need finer meshes to resolve thin members.
- **Mesh size**: finer meshes give crisper shapes but each iteration is slower
  (the linear solve is the cost). Start coarse to explore, refine to finalize.
- The density field is a bit fuzzy at member boundaries (that is inherent to
  the method at v1); the **Threshold** slider and the STL smoothing clean it up.

## Export & re-model

- **Export STL** writes the optimized shape (iso-surface, Taubin-smoothed) as a
  binary STL for 3D printing — `export_topo_stl` over RPC/MCP.
- Topology output is a mesh, not a parametric B-Rep. The intended workflow for
  an editable part is the **shape guide**: keep the result as a translucent
  reference and re-model clean features over it. (Automatic mesh→feature-tree
  reconstruction is out of scope — see the design doc.)

## Headless / scripting (RPC + MCP)

Every step above works over the JSON-RPC / MCP surface, so an assistant can run
the whole loop without the GUI:

```
add_study {name}                              -> {id}
set_study {study, type:"topology", mesh_size,
           volume_fraction, filter_radius, max_iterations}
add_load  {study, type:"fixed",  face}
add_load  {study, type:"force",  face, magnitude, direction}
add_keep_face {study, face}                   (optional)
run_study {study}                             -> {started:true}   # async
study_status {study}                          -> {running, iteration, ...}  # poll
cancel_study {study}                          -> {cancelled}      (optional)
study_results {study}   -> {iterations, converged, volume_fraction, compliance}
set_result_display {study, field:"shape"|"density", threshold}
export_topo_stl {study, path, threshold?, smooth?}
set_topo_ghost {study, on}
```

A topology `run_study` returns immediately; poll `study_status` until
`running:false`, then read `study_results`. `tests/topo_smoke.py` drives this
end to end.

## What v1 does / doesn't

**Does:** single-body compliance minimization (SIMP + optimality criteria) on
tet10 meshes; fixed/force/pressure BCs; keep faces; volume-fraction target;
persisted result (reopens with the shape intact); async runs; iso-surface +
density display; smoothed STL export; shape-guide ghost.

**Doesn't (deferred):** stress-constrained TO, multiple load cases,
manufacturing constraints (draw direction, symmetry), assembly-level TO,
lattice/parameter optimization, and automatic parametric reconstruction. See
`docs/topopt/plan.md` for the rationale.
