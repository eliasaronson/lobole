# One GPU kernel for a tool swept over a grid

Most of CAM's heavy work is one pattern: a cutter shape (`TipHeight`) swept
over a grid of cells.

- **Planar verification and playback** carve a height map
  (`CarveSweep`, CamVerify.h; `StockSim`, CamSim.h). The chess board's verify
  took about 35 s.
- **Rotary verification and playback** carve a radius map
  (`CarveSweepRotary`, CamRotary.h), already on every core by bands
  (docs/cam/rotary.md#on-every-core): the knights blank's check went from
  about 50 s to 7.
- **Surfacing generation** builds the cutter-location surface: the highest
  point under the cutter at each cell (`RotaryCL` for the rotary,
  `CamDropCutter.h` for planar). It was 85% of a rotary generate.
- **Rest masks and leftover maps** compare two such grids.

All of them are data-parallel over cells. The app already has headless Vulkan
compute in `src/femgpu/` (`ComputeContext`: device choice, storage buffers,
SPIR-V pipelines, dispatch), which the topology solver runs on. One kernel
family there could serve all of these.

## What step 1 measured

The rotary carve, with its rapid checks, as one float32 compute shader
(branch `gpu-rotary-carve-prototype`: `src/shaders/rotary_carve.comp`, and the
hidden `[rotary_gpu]` bench in femgpu_tests). One thread per cell. The host
bins the moves per axis bin and theta tile in file order, and the kernel runs
as a single dispatch. Knights Turn: 156,409 moves, 9,151 rapids.

| | 0.5 mm (the panel) | 0.25 mm (the RPC default) |
|---|---|---|
| CPU walk, 22 cores | 0.98 s | about 6 s |
| RTX 4060, total with binning and transfers | about 0.1 s | about 0.3 s |
| Arc iGPU kernel | – | 1.15 s |
| llvmpipe, 22 threads | 1.5 s | 5.5 s |

- **Where it is used most, the gain is modest.** At the panel's 0.5 mm the
  rest of `InspectRotary` (parse, blank, sampling, compare) is 0.47 s, so the
  whole check would go from 1.45 s to about 0.55 s. The large gain, 6 s to
  0.3 s, is at 0.25 mm, which is RPC and agent use.
- **The verdicts agree, the maps nearly do.** Gouge and leftover are
  identical. Map cells differ by at most 4e-4 mm at 0.25 mm. At 0.5 mm, two
  cells differ by up to 0.22 mm where a ray runs along a ball's wall, and
  replaying that cell in float on the host reproduces the GPU's value.
- **Rapids did not agree.** At 0.25 mm the GPU counted 2 rapids through
  stock that the CPU does not. The toolpath lies on the grid's own lattice,
  so a cutter's wall lands EXACTLY on cell centres, and which side a cell
  falls is decided by the last bit of rounding. Shrinking every cutter by
  1e-4 mm on both sides made the rapids identical at both resolutions, and
  under a 16,513-rapid stress test.
- **The Arc iGPU's faster trig** puts cells up to 1.1e-3 mm off. That is
  under the verdict thresholds but over this plan's 1e-4 mm, so the
  tolerance would depend on the device.
- **CI gains nothing:** llvmpipe is 1 to 1.5 times slower than the CPU walk.

**Conclusion: the rotary check alone does not earn a second
implementation.** Two findings carry on:

- **The CPU reference had the same tie**, and now decides it: the rotary
  walk and the planar carve reach `R - kCutterWallTolerance` (1e-4 mm), so a
  cell on the wall is outside by a decision
  ([simulation.md](../cam/simulation.md#the-cutters-wall)). A kernel uses the
  same reach.
- **The kernel shape carries over.** Per-tile, file-order lists and one
  dispatch are what made it fast (310 ms to 45 ms). The candidates that could
  earn a kernel are the ones not yet measured.

## What step 1b measured: the planar verify

The planar carve with its rapid checks, swept-cell marks and bite volumes, as
the same kind of kernel (branch `gpu-planar-carve-prototype`:
`src/shaders/planar_carve.comp`, the hidden `[planar_gpu]` bench). The chess
board's check is no longer about 35 s. On today's code a whole `Inspect` is
0.2 to 0.35 s at the panel's 0.4 mm and 0.4 to 0.9 s at 0.25 mm. The carve is
30 to 60% of that. Every check also inspects the other setup
(`OtherSetupsOf`).

| carve, Top / Face | 0.25 mm | 0.4 mm (the panel) |
|---|---|---|
| CPU, 1 thread | 255 / 325 ms | 90 / 137 ms |
| CPU, bands of rows on 22 cores | 48 / 62 ms | 27 / 33 ms |
| RTX 4060, total | 14 / 15 ms | 7 / 7 ms |
| llvmpipe, total | about 55 ms | – |

- **The maps agree** to 2.5e-5 mm on every device. So do the swept cells and
  every verdict field.
- **The rapids agree** with the wall tolerance, including on a stress file of
  3,222 rapids through stock: the same count, lines and deepest cells. Without
  the tolerance, one rapid's deepest cell moved by 1.9 mm, the same tie the
  rotary check showed.
- **Uncached readback was most of a transfer.** Reading through the storage
  buffer's own mapping cost 170 ms for 8 MB; a cached readback copy costs
  5 ms. The rotary numbers above carried that cost unseen.

**The GPU saves 20 to 50 ms in a check of 0.2 to 1.3 s, and the band split
takes the carve most of the way with one implementation.** Neither verify
earns the library. The CPU work that remained on the planar check is done:
the carve runs in bands like the rotary one, `SampleModel` and the compare
run on every core, and each other setup is inspected once
([simulation.md](../cam/simulation.md#on-every-core)).

## What comes next, if anything

The CL surfaces are the one candidate not yet measured (`RotaryCL` and the
planar drop-cutter, most of a surfacing generate). Step 2 and on stand only if
they show a gain the CPU cannot reach.

## Constraints the design has to meet

- **`designer_core` stays GPU-free.** It is unit-tested and scripted
  headlessly. The kernels live in the compute library (renamed from `fem_gpu`
  to a general `gpu` one), and the app layer chooses GPU or CPU. The CPU path
  stays the reference.
- **Single precision.** The development GPU (RTX 4060 laptop) runs doubles at
  1/64 of float speed. The grids are already stored as `float`, but today's
  arithmetic is double. A GPU answer agrees with the CPU to a tolerance
  (about 1e-4 mm, well under the verdict thresholds of about 0.01 mm), not to
  the bit.
- **One source for a picture and its verdict.** The playback's last frame is
  the verdict's stock. Whichever path computes one computes the other, in the
  same run.
- **Order where the result needs it.** A rapid is checked against the stock
  as it stands at that point in the file. The kernel therefore processes
  moves in file order per cell tile, as the CPU band split does per band. Cuts
  are a `min` (and CL surfaces a `max`), which are order-free.
- **CI renders on llvmpipe.** The GPU path must work there, however slowly,
  or be skipped by `ping`'s `features`. The CPU path is what CI trusts, and an
  agreement test compares the two on the same inputs wherever a GPU runs.

## Steps, if a candidate earns them

1. **The library.** Generalize `fem_gpu` into the shared compute library.
   Add a `SweepKernel` interface (tool profile, move list, grid, mode: carve,
   CL max, or check) with a CPU implementation behind the same interface.
2. **The candidate that earned it** on that library, with agreement tests.
   Then the others, one at a time, each measured first.

## Verification

At each step, an agreement test (GPU against CPU within tolerance, including
the rapid count and its lines), the CAM suites on both paths, timings on the
chess set, and CI green on llvmpipe.
