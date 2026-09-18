# Two checks the CAM side is missing: cutter reach, and a mesh fine enough to check against

Two small, independent fixes found while costing 3-axis surfacing. Neither is
surfacing work; both are wrong today.

## 1. The designed surface is coarser than the grid it is sampled onto

`CADShape::GenerateMesh(double deflection)` takes a deflection and throws it
away. `CADKernel.cpp:788-807` overwrites it unconditionally whenever the
bounding box is non-void:

```cpp
if (diag > Precision::Confusion())
  linDeflection = diag * 0.002;   // 0.2% of the bounding diagonal
```

The argument survives only for a degenerate shape. So `GenerateMesh(0.05)` at
`CamVerify.cpp:373` produces **0.4 mm** faceting on a 200 mm part.

That number matters because `SampleModel` builds the *designed* surface the
verifier compares the machined height map against, on a grid whose default
resolution is **0.25 mm** (`CamVerify.h:86`). The reference surface is
coarser than the grid measuring it, and `Verdict::maxGouge` is read in
hundredths. Every gouge and leftover figure on a large part carries that
faceting as noise.

The scaling rule itself is right — it is what keeps a large model from
tessellating into millions of triangles, and the angular term next to it is
what stopped small cylinders reading as n-gons. What is wrong is that it is an
*override* rather than a *default*.

**The fix, part one.** Make the parameter a request and the scaling rule the
fallback: `0` (the new default) derives from the bounding diagonal exactly as
today; a positive value is honoured. Every existing caller passes `0.05` or
`0.1` — values that have never taken effect and were never chosen for a reason
— so each becomes an explicit `GenerateMesh()` and keeps today's behaviour to
the triangle.

**The fix, part two, and it is not optional.** A triangulation lives on the
shared underlying `TShape`, so it is visible through *every* copy of a shape —
including the ones the document and the renderer hold. Meshing a `Body` at the
verifier's tolerance therefore re-meshes the document behind the renderer's
back: a cylinder answering 248 triangles answers 888 from then on, through a
handle that never asked for anything. That is the same hazard as the bounding
box which disagreed with itself once a shape had been meshed — the one that
reported twelve legitimate rapids as cutting through material.

So a caller wanting a tolerance of its own asks `GenerateMeshDetached`, which
deep-copies first and keeps the side effect to itself. `SampleModel` is such a
caller, and it derives its tolerance from the grid it is filling (`like.res`)
rather than naming a literal, per the defaults rule: the reference surface is
meshed to a quarter of a cell, below what the grid can resolve.

Files: `src/cad/CADKernel.h` (both the real and the stub declaration),
`src/cad/CADKernel.cpp`, and the call sites in `MeshCollide.cpp`,
`GlbAnimation.cpp`, `FastenerTool.cpp`, `TessellationCache.cpp`,
`ViewActions.cpp`, `RendererSync.cpp`, `CamVerify.cpp`, plus the tests that
pass a value.

Verified by: a kernel test that a stated deflection changes the mesh and keeps
scaling, and that saying nothing still derives — measured on FRESH shapes and
inside the chord-bound regime, because the angular deflection (0.2 rad) floors
every circle at ~32 segments and a request coarser than about 0.005·r cannot
be observed at all. A second test that a detached mesh leaves a sharing copy
untouched, and that the undetached call still moves it — the hazard pinned, so
changing it is a decision rather than a surprise. Then
`tests/render_pixel_smoke.py`, which must be **byte-identical** — if a renderer
mesh moved, a call site was converted wrongly or something meshed a shared
shape.

## 2. Nothing checks that the cutter is long enough for the cut

`Tool::fluteLength` (`CamJob.h:49`, "mm, the usable depth") is written and read
by `SerializeCam.cpp`, drawn as the shank of the tool glyph in
`CamStockMesh.cpp:281`, and compared against nothing, anywhere. `DepthPasses`
takes no `Tool` at all, so it cannot check reach even in principle. The
verifier models only the cutting end: `CarveSweep` uses the radius and
`TipHeight`, both clamped to the tip, so a holder buried in the workpiece
removes no material in the simulation and the job reports `Safe()`.

It is also unreachable. `set_setup`'s tool schema omits `flute_length`, the
setter never assigns it, `ToolJson` never emits it, and no GUI widget touches
it — so every tool in every job is carrying the default 20.0 mm regardless of
what is in the spindle. **Exposing the field and adding the check are one
change, not two**: a check against a number nobody can set is a check against
a fiction.

**The fix.** Two halves, both small.

- Expose `flute_length` — the schema line, the setter, `ToolJson` (which also
  silently drops `chipload_min`/`chipload_max` today; they go back in beside
  it), and a field in the tool row in `CamPanel.cpp` next to the diameter.
- Add a reach `Finding` in `CamFeeds.cpp`, beside the chip-room one at
  `:245-266`. That check already computes the exact quantity — `deepest =
  max(deepest, op.topZ - op.bottomZ)` — and compares it to `2 x diameter` for
  chip evacuation. The reach finding compares the same depth to
  `fluteLength`, and unlike chip room it applies to **every** op type that
  cuts at depth, not just `Profile` and `Drill`: a pocket or an adaptive pass
  buries a short cutter just as well.

`FeedsFor` already renders into the GUI verdict list (`CamTool.cpp:1175`) and
is already reachable over RPC (`RemoteCam.cpp:1627`), so the finding reaches
both a person and an agent with no new plumbing. `flute_length` is already
serialized, so **no format bump and no golden**.

The finding says the depth, the cutter's usable length, and that the holder
reaches the work — with the arithmetic beside it, like every other finding.

Files: `src/core/cam/CamFeeds.cpp`, `src/remote/RemoteCam.cpp`,
`src/ui/CamPanel.cpp`, `tests/test_cam_machine.cpp`.

Verified by: a case in `test_cam_machine.cpp` where a 20 mm cutter asked for a
30 mm deep pocket produces the finding and a 40 mm cutter does not; the units
gate (`tests/rpc_units_gate.py`) over the new parameter.

## Not this

The full shank-and-holder envelope collision designed at
`docs/cam/simulation.md:199-202` — generalising `TipHeight` to a radius-above-
tip profile for the whole assembly and reporting a cell standing above it with
its G-code line. That is the real version of check 2 and wants the holder
geometry the `Tool` record does not carry. Fixture and clamp collision stays
excluded: no fixture solid exists in the model.

`Verdict::outsideTravel` checks X and Y only (`CamVerify.cpp:687-700`), so a Z
move past the machine's travel is unflagged. Real, adjacent, and its own fix.
