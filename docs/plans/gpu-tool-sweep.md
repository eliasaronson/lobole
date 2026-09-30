# One GPU kernel for a tool swept over a grid

Most of CAM's heavy work is one pattern: a cutter shape (`TipHeight`) swept
over a grid of cells.

- **Planar verification and playback** carve a height map
  (`CarveSweep`, CamVerify.h; `StockSim`, CamSim.h). The chess board's verify
  took about 35 s.
- **Rotary verification and playback** carve a radius map
  (`CarveSweepRotary`, CamRotary.h). On every core by bands:
  docs/plans/rotary-verify-parallel.md.
- **Surfacing generation** builds the cutter-location surface: the highest
  point under the cutter at each cell (`RotaryCL` for the rotary,
  `CamDropCutter.h` for planar). It was 85% of a rotary generate.
- **Rest masks and leftover maps** compare two such grids.

All of them are data-parallel over cells. The app already has headless Vulkan
compute in `src/femgpu/` (`ComputeContext`: device choice, storage buffers,
SPIR-V pipelines, dispatch), which the topology solver runs on. One kernel
family there could serve all of these.

## What it would buy, and what is not known

These are estimates, not measurements. After the CPU band split, a rotary
check of one blank should take a few seconds on 22 cores, and a GPU might take
under one. Over a whole chess run that saves tens of seconds. The real gain is
interactive: verify and playback of a big turned or surfaced part becoming
near instant, and generation's CL surfaces with them. **Step 1 measures this
before anything else is built.**

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

## Steps

1. **Measure.** One kernel, the rotary carve with rapids, against the CPU band
   split, on the knights blank. Time it with uploads, and compare the map
   within tolerance. Stop here if the gain is not worth a second
   implementation.
2. **The library.** Generalize `fem_gpu` into the shared compute library. Add
   a `SweepKernel` interface (tool profile, move list, grid, mode: carve / CL
   max / check), and a CPU implementation behind the same interface.
3. **Rotary and planar verify and playback** on it, with the agreement tests.
4. **The CL surfaces** (`RotaryCL`, planar drop-cutter) on it.

## Verification

At each step, an agreement test (GPU against CPU within tolerance, including
the rapid count and its lines), the CAM suites on both paths, timings on the
chess set, and CI green on llvmpipe.
