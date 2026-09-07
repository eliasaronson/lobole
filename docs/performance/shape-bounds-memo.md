# The section view was 18 fps, and none of it was the section

Measured 2026-09-07 on the air-cooled engine assembly
(`scratch/aircooled`, 33 leaf occurrences), RTX 4060 Laptop.

## The report

The Section tool open, the frame-time HUD reading 18.8 fps / 53 ms — and the
GPU idle by comparison:

```
cpu 50.50 ms   display 2.58 ms
gpu shadow 0.75  ssao 7.95  main 4.24  = 12.94 ms
update   6.83 ms  13%
ui      43.12 ms  81%
wait     0.04 ms   0%
record   0.55 ms   1%
present  2.54 ms   5%
```

`ui` is ImGui's NewFrame..Render — panels, picking, overlays
(`src/render/FrameTimer.h`). So: not the clip planes, not the cap fills, not
the extra shading. Something in the UI phase.

## Attribution

A windowed instance under Xvfb, same project, `frame_stats` across three
states (idle throttle off, so every frame is a real one):

| state | `ui` |
|---|---|
| no section, tool closed | 5.6 ms |
| section ON, tool window **closed** | 6.0 ms |
| section ON, tool window **open** | 27–31 ms |

(A different window size from the before/after table below, which is why the
absolutes differ; within a table every row is the same instance.)

The cut itself is free. The cost arrives with the tool *window*. `perf` on
that process (12 s, `cpu-clock`, dwarf call graph):

```
54.27%  ui::VisibleSceneBounds()
 └ 53.83%  CADShape::GetBoundingBox()
     ├ 14.2%  DrawToolWindow            PreviewFeature.cpp — "model spans …"
     ├ 13.5%  ComputeSectionHandleSpec  PreviewHandles.cpp — the arrow anchor
     ├ 13.4%  HasViewportHandle         PreviewHandles.cpp — the hit test
     └ 13.2%  BuildPreviewHandleOverlay PreviewHandles.cpp — the plane square
```

Four callers ask independently, each frame, for how big the model is.
`VisibleSceneBounds` flattens the assembly and calls `GetBoundingBox` on every
visible leaf body; that is `BRepBndLib::Add` with `useTriangulation=false`
(deliberately — the 4 µm CAM stock bug), so it walks the actual B-Rep, every
edge and `CurveOnSurface`, for every part. Uncached. **Half the application's
CPU was spent re-deriving a number that only a regeneration can change.**

The assembly flatten, suspected first, is noise: 0.4% of the same profile.

## The fix

Memoize on the shape — `CADShape` holds one slot per flavour
(conservative / tight), filled lazily, dropped by `SetShape`, which is now the
only thing that replaces `m_shape`.

Why on the shape and not in a cache beside it: an outside cache keyed on
`PlacementId` was tried once and removed, because that id is the TShape's
*address* and the allocator recycles it — a freed shape's box could be handed
to whatever landed there next (it surfaced as the determinism test failing
under a parallel run and passing alone). A slot living inside the object has
no key to collide with: it is reachable only from the object whose TShape it
describes, and that object is what keeps the TShape alive.

It cannot go stale, either. Nothing but `m_shape` feeds the answer, a shape is
immutable except through the five in-place transforms, and those go through
`SetShape` — gated by `test_conventions.cpp`, so a sixth cannot arrive quietly.
A regeneration builds *new* shapes, which start cold. That is exactly why the
comment this replaces ("caching does not help — a regeneration makes new
shapes") was right about a *build* and wrong about a *frame*: a build asks
about each shape once, a frame asks about the same shape four times, sixty
times a second.

Result. Same project, same window, same pinned config, the two binaries run
back to back (`ui` is the phase; `avg` is the whole frame):

| state | before `ui` | after `ui` | before avg | after avg |
|---|---|---|---|---|
| no section, tool closed | 4.06 ms | 3.83 ms | 30.63 ms | 29.35 ms |
| section ON, tool OPEN | 21.24 ms | **4.24 ms** | 48.87 ms | **30.44 ms** |
| tool closed, section stays | 4.26 ms | 3.50 ms | 30.59 ms | 28.40 ms |
| tool OPEN again | 19.59 ms | **3.43 ms** | 46.26 ms | **28.69 ms** |

The tool window now costs what any other small panel costs. `perf` over the
same state agrees — `VisibleSceneBounds` falls from 54.27% of process CPU to
0.62%, and `GetBoundingBox` no longer appears above 0.3%.

## Threading

Filling a slot writes to the shape, which puts it in the same class as
OpenCASCADE's own lazy caches — and the parallel clash sweep already has the
rule for those: shapes are warmed **single-threaded** before workers start
(`Measure::WorldBounds` over every entity in `RunSweep`, `MeshFor` over every
candidate in `motion::Run`). The memo is filled by those same warm passes.
Both sweeps also hand each worker its own `CADShape` copy, which carries the
memo with it, so a worker that does ask cold is asking about a shape only it
holds. `sweep_parallel_smoke.py` diffs a one-thread report against an
all-thread one byte for byte and is the gate on all of this.

## Why not compute the box on the GPU

Raised and rejected, for the record:

- **It answers a different question.** The GPU has the tessellation. This box
  deliberately does not (`useTriangulation=false`): a mesh-derived box is
  inflated by the deflection, so a part's size would change the first time
  anything drew it — the bug that reported twelve legitimate rapids as cutting
  through material. A GPU reduction reproduces exactly that box.
- **The readback costs more than the compute.** The reduction is microseconds;
  getting it back to the CPU inside the frame is a fence wait. The alternative
  is a 1–2 frame stale box, fine for placing an arrow and not fine for
  `FitView`, which has to be right on the frame you press it.
- **The work was redundant, not heavy.** A memo is 0 ms and exact; a GPU pass
  would be ~0.2 ms, approximate, and a new path in the untested Vulkan layer.
- **Most callers have no GPU.** `Measure`, `HoleFeature`, `RibFeature`,
  `DraftFeature` and CAM's stock derivation all call `GetBoundingBox` from
  `--headless`, batch RPCs and CI.

A GPU reduction is the right tool for bounds over data that genuinely changes
every frame — a deforming FEM field, motion-study playback, the CAM simulation
height map. Not for static B-Reps.

## Second place, untouched

`FaceSelector::PickFace` — the hover pick, which re-rays the scene every
frame — was 8.7% of the profile before and is 27.8% after, which is what
happens to second place when first place leaves. Real, unrelated to sections,
and not addressed here.
