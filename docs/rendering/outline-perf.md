# Outline/wireframe renderer performance — plan

Status: **EXECUTED 2026-08-11** (P0–P3 in one pass; P4 not needed). Measured:

- Benchmark (`designer_tests "[.outline-perf]"`, 212 × 1.5k-tri meshes): old
  per-frame pass **67 ms**; new adjacency build 0.24 ms/mesh once at load,
  full reclassify of all 212 meshes **0.80 ms** (~84×).
- Real repro (Waveshare STEP via the remote harness, 131 outline meshes):
  silhouette pass **0.81 ms** total — and it now runs only on camera settle,
  not per frame. Zero `vkDeviceWaitIdle` during interaction.
- Suites: 555 ctest cases green, `gui_smoke` (83/83 ImGui tests) +
  `remote_smoke` green on Xvfb.

Implementation notes / deviations from the plan below:

- New pure-CPU module `src/render/OutlineEdges.{h,cpp}` (in `designer_core`,
  unit-tested in `tests/test_outline_edges.cpp` against a port of the old
  map-based extractor).
- Settled views now keep **feature edges and all boundary edges visible** and
  add silhouettes on top ([static prefix | silhouette suffix] buffer layout,
  one contiguous draw). The old code *replaced* the wireframe with
  silhouettes + front-facing boundary after the first camera move, dropping
  feature edges entirely — the new behavior is additive, depth-tested, and
  matches the load-time look.
- Degenerate (zero-area) triangles: the old extractor computed NaN normals
  for them, so their internal edges never passed any dot test; the new build
  excludes them explicitly for parity (and finite normals).
- `OutlineSettings.silhouetteSettleMs` (120) and `.liveSilhouettes` exist as
  the tuning knob/escape hatch but are not yet surfaced in the View menu.
- Drive-by: `Logger` now flushes every message — block-buffered stdout was
  swallowing log tails on crash/kill under the remote harness.

## Motivation

Repro: import `Waveshare_bus_serie_adapter.step` (a PCB assembly: **212
solids, 5,733 faces, 10.7 MB**). Orbiting the camera lags hard — even with
the dGPU at full clocks, because the bottleneck is CPU-side in
`OutlineRenderer`. Any assembly with many bodies or dense tessellation hits
it; single simple parts hide it.

## Root cause

All in `src/render/OutlineRenderer.cpp`:

1. **Every camera move re-flags every mesh.** `UpdateCamera` (line ~734)
   marks all meshes `needsEdgeUpdate` when the camera moves/rotates by more
   than a sliver — i.e. every frame of an orbit drag.
2. **The per-view recompute is brutally slow.** `UpdateMeshEdgesForView`
   (line ~863) runs per mesh per frame and, for *every triangle*: three
   `mat4` vertex transforms, a normalize, then inserts three edges into a
   `std::map<pair<u32,u32>, std::vector<pair<vec3,bool>>>` — a red-black
   tree with a heap-allocated vector per edge, rebuilt from scratch each
   frame. For 212 bodies this is hundreds of ms per frame.
3. **Mid-frame GPU stalls.** When the silhouette edge count changes (it
   does, continuously, while orbiting), the edge index buffer is destroyed
   and reallocated with a `vkDeviceWaitIdle` (line ~968), and `CopyBuffer`
   does another queue wait-idle. The comment there already flags this and
   TODOs a per-frame-in-flight ring.
4. **Latent staleness bug.** The re-upload is gated on
   `visibleEdges.size() != mesh.edgeIndexCount` (line ~956) — a view change
   that yields the *same number* of different edges draws stale edges.
5. `ExtractEdges` (static feature edges, run once at `AddMesh`) uses the
   same `std::map` pattern — tolerable at load, but the adjacency it builds
   is thrown away instead of being kept for the per-view pass.

Asymmetry worth knowing: `RenderOutlines` (EDGES/SILHOUETTE modes) draws the
full mesh through the outline shader and never reads `edgeIndexBuffer`. Only
**WIREFRAME** — the default mode set in `main.cpp` — draws it. All of the
above cost exists to give the wireframe its view-dependent limb silhouettes
on top of the static feature edges.

## Design

Per-view silhouette extraction is fundamentally two flat passes: a facing
test per triangle, then a comparison per edge. Everything view-independent
(unique edges, triangle adjacency, per-triangle geometry) is computable
**once at mesh load**. And mesh transforms are rigid, so instead of
transforming every vertex to world space, transform the **camera into
mesh-local space** once per mesh.

**Precomputed at `AddMesh`/`UpdateMesh`** (mesh-local space, contiguous
arrays on `OutlineMesh`):

- Per-triangle normal + centroid (one pass over the index buffer).
- Unique edge list: `{v0, v1, triA, triB (or INVALID), flags}` where flags
  mark *boundary* (one adjacent triangle) and *feature* (dihedral
  `dot < 0.5`, keeping the current cos 60° threshold). Build with a
  sort-based pass or `unordered_map` + `reserve` — this replaces the
  `std::map` in `ExtractEdges` too; static feature edges and adjacency come
  out of the same build.

**Per view update** (only for meshes actually flagged):

1. `camLocal = inverse(mesh.transform) * cameraPosition` — once per mesh.
2. Pass 1: `facing[t] = dot(normal[t], camLocal - centroid[t]) > 0` into a
   reused scratch bitset.
3. Pass 2: emit edge `(a,b)` into a reused scratch index vector if
   *feature*, or *boundary* with `facing[triA]`, or
   `facing[triA] != facing[triB]` (true silhouette).

Zero steady-state allocations, O(T + E) flat loops, no per-vertex matrix
math. Expected ≥50–100× over the map-based version; this is the same
classification the current code computes, so visuals are unchanged.

**Buffers:** allocate each mesh's edge index buffer once at worst case
(`2 × edgeCount × 4` bytes), `HOST_VISIBLE | COHERENT`,
`MAX_FRAMES_IN_FLIGHT` copies; write directly into the current frame's copy
and bind that one in `RenderWireframes` (per-frame `edgeIndexCount`). No
staging, no `CopyBuffer`, no realloc, no `vkDeviceWaitIdle` — which also
structurally removes the in-flight use-after-free / `VK_ERROR_DEVICE_LOST`
class the line-958 comment defends against. Always write when recomputed
(kills bug #4). If worst-case ×2 memory ever bites on huge assemblies, fall
back to one shared ring buffer with per-mesh offsets — note it, don't build
it yet.

**Interaction policy:** while the camera is moving, skip the silhouette
append and draw only the (precomputed, always-correct) feature+boundary
edges — standard CAD-viewer behavior. When the camera settles (~120 ms
without significant delta), run one silhouette pass. With phase 1 done that
pass should be a few ms for the whole assembly; if it still spikes, amortize
round-robin K meshes/frame.

## Phases

**P0 — instrument + baseline (~0.25 session).** Scoped timer around
`UpdateMeshEdgesForView` + a per-frame total (LOG_DEBUG-gated). Record
baseline with the Waveshare STEP; paste numbers into this doc. Keep the
timer — it's the acceptance gauge.

**P1 — precomputed adjacency + flat classify (~1 session).** The design
above minus buffer changes: build adjacency at `AddMesh`/`UpdateMesh`
(replacing `ExtractEdges`' internals), rewrite `UpdateMeshEdgesForView` as
the two flat passes with scratch reuse. Behavior-identical output.

**P2 — persistent per-frame-in-flight edge buffers (~0.5 session).** As
designed. Deletes the wait-idle path, fixes the staleness bug, retires the
defensive comment. Assert/log that no wait-idle happens during interaction.

**P3 — interaction policy (~0.5 session).** Motion → static edges only;
settle → one silhouette pass. Expose the settle delay (and an
"always-live silhouettes" escape hatch) on `OutlineSettings`.

**P4 — stretch, only if P1–P3 miss the target.** GPU compute classify into
an index buffer + indirect draw; merge many tiny bodies' edge lists into
shared buffers to cut the 212 bind/draw pairs; align with
[instancing-design.md](instancing-design.md) (outline meshes are currently
per-occurrence copies, not asset/instance).

## Acceptance

- Waveshare assembly orbits at 60 fps in wireframe mode **on the Arc iGPU**
  (i.e. no dGPU required; also covers the power-capped-dGPU case).
- Settle-time silhouette pass < ~5 ms total for the 212-body assembly.
- Zero `vkDeviceWaitIdle` during camera interaction.
- No visual change on settled views vs. today (same classification rules).

## Tests

- Unit: adjacency builder on a cube (edge/boundary/feature counts exact)
  and a tessellated cylinder; silhouette classify with a fixed camera
  (cylinder limb edges flip as the camera crosses the axis plane).
- Soak/regression: the two-extruded-parts rotate scenario from the line-958
  comment (formerly `VK_ERROR_DEVICE_LOST`) — structurally fixed by P2, keep
  as a GUI soak test.
- Perf: P0 instrumentation + the Waveshare file as the manual fixture (or a
  generated many-body equivalent checked into `tests/` if we want it in CI).

## Risks

- Silhouettes freeze during drag (P3): mitigated — feature/boundary edges
  (the bulk of a CAD wireframe) remain correct at all times; matches
  mainstream CAD viewers. Escape hatch in settings.
- Worst-case buffer memory ×`MAX_FRAMES_IN_FLIGHT`: acceptable for realistic
  assemblies; shared-ring fallback noted above.
- `UpdateMesh` (regen/feature-edit path) must rebuild adjacency — same code
  path as `AddMesh`, covered by the unit tests.

Total effort through P3: ~2–2.5 sessions.
