# Plan: a printed screw-top jar with a TPU gasket

A generator, `generators/jar/`, that makes a screw-top jar for a Bambu Lab
P1S: a PLA jar and lid with modelled threads, and a TPU95A gasket ring in
the lid that the lid squeezes when it is screwed down. It is filled with
water and turned upside down, so the seal is the point. It is the first
thread printed from Loböle, and the first part in two materials.

## What it builds

Every size derives from `--inner-diameter` (60 mm) and `--volume` (200 ml)
and from the print process (`--profile`, else `--process fdm --material
pla`). Each part is modelled the way it prints:

- **jar**, upright: one revolved section. A floor, a straight bore (the inner
  diameter, so it cleans out), a body as wide as the lid, a flat **shoulder**
  the lid stops on, a threaded **neck** above it, and a short **lip**
  narrower than the thread's root that carries the sealing rim. The rim is
  chamfered on the outside to a narrow flat two lines wide, which is what
  presses into the gasket.
- **lid**, top down: a disc and a skirt, an internal thread in the skirt,
  and an annular **gasket groove** in the ceiling.
- **gasket**, flat: a rectangular ring, ten layers thick, pressed into the
  groove.

The **thread** is a single-start, right-hand thread with **45° flanks**
(90° included) at a coarse pitch (a fifteenth of the inner diameter,
rounded to half a millimetre: 4 mm at 60). A 60° ISO flank overhangs 60°
from vertical, both the jar's printed upright and the lid's printed top
down, whichever flank carries the load; at 45° every flank is at the
printable limit without support. The neck is threaded over two pitches, the
lid over its whole skirt.

The **print allowance** is applied to the thread fit: the neck is a custom
external thread on a face at the basic major less the `external_thread`
allowance, the lid's a custom internal thread on a bore at the basic minor
plus the `internal_thread` allowance (`print_allowance`). The groove takes
the `hole` and `shaft` allowances; the gasket, TPU, takes none (below).

**The squeeze is set by a hard stop**, not by torque: the lid's skirt lands
on the jar's shoulder. At that height the gasket (thickness t, in a groove
of depth g) is pressed by the rim by s = 25 % of t, and the rim still
clears the ceiling by t − g − s. A stop is what makes "hand-tight" a
dimension.

## The app changes it needs

1. **Thread flank angle.** `ThreadFeature` cuts only the 60° basic profile.
   Add `flank_angle_deg` (the half angle, default 30; written only when not
   30) for a CUSTOM thread: the groove, the overlay depth, a custom thread's
   major from a bore, and `PrintedThreadFor` take the depth from it
   (`threads::BasicDepth(pitch, flank)`, 5H/8 with H = P / (2 tan α)). A
   table designation with another angle is refused (the table's rows are
   60°), and so is an angle outside 15-60°. The body's `ThreadMark` carries
   it, and the CAM thread mill refuses a mark that is not 60° rather than
   milling a 60° profile into it. GUI: a flank-angle field in the Thread
   editor, shown for a custom designation. RPC describe row; format bump.
2. **A screw relation backed by a custom thread.** A thread-backed `screw`
   relation refuses any Thread that is not a table row, and every printed
   thread is a custom one (`PrintedThreadFor` says to model it so). Take the
   custom thread's own pitch. It also ignores the hand: a left-hand thread
   advances the other way, so the ratio takes the hand's sign.
3. **A `tpu` material** (TPU 95A as printed: density, modulus from Bambu
   Lab's TPU 95A datasheet), its slug in `designer_mcp.py`'s
   `MATERIAL_SLUGS`, and the materials doc. No TPU print-allowance table:
   the built-in tables refuse a material without sourced numbers, and none
   for TPU's hole and shaft sizes were found. The gasket's fits are elastic
   (it is pressed into its groove), so it is modelled at size and the README
   says so.

Format: `kVersion` + 1 for `flank_angle_deg` and the `tpu` slug, a golden
at that version, a row in docs/versioning/format.md.

## The checks (every run)

- `check_geometry` is `verified` on all three parts.
- **Thread fit, as modelled**: the radial clearances at crest and root and
  the flank clearance follow from the allowances; read back from the lid's
  `print` report.
- **The assembly** `jar`: the jar fixed; the lid on a revolute mate from
  the jar's shoulder frame to the lid's stop frame (frames built on those
  faces, so a longer skirt moves the frame); the gasket fastened in the
  groove. The angle at which the lid's thread meets the jar's at the stop
  is FOUND, not computed from the kernel's helix phase: the exact
  interference of lid and jar is swept over a turn and the middle of the
  clean window taken. At that angle lid and jar must not interfere at all.
- **The squeeze**, measured on the solved assembly: the rim's flat and the
  gasket's underside in world coordinates give s, and the exact
  interference volume of gasket and jar must match the rim's section above
  that depth (two independent readings). s / t must lie in 20-30 %.
- **Unscrewing**: a screw relation backed by the lid's thread couples the
  lid's turn to its rise; a motion study turns the lid back two turns from
  the point the gasket lets go, mesh clash at a fifth of the thread
  clearance, and must be clean.
- **Negative controls**, each must FAIL its check: a lid whose skirt is
  longer by the squeeze plus a layer (it stops short) reads a squeeze under
  20 %; the lid turned half a pitch out of phase at the stop interferes
  with the jar.
- **Printability**: every triangle of each exported STL (the part in its
  print orientation, what the slicer sees) is checked against 45° from
  vertical: a facet facing down more steeply than that, off the bed, fails
  the run.
- STL: three files, `jar.stl` and `lid.stl` (PLA) and `gasket.stl` (TPU),
  each one watertight shell in print orientation, chords at the profile's
  deflection.

## Files

- `generators/jar/build.py`, `generators/jar/README.md` (incl. printing on
  the P1S: TPU95A settings, the gasket's infill, the order of assembly).
- `generators/README.md` index row and generator-test row;
  `tests/generator_runs.py`, `tests/CMakeLists.txt` (`generator_jar`).
- `src/core/ThreadStandards.{h,cpp}` (`BasicDepth`),
  `src/core/features/ThreadFeature.{h,cpp}`, `src/core/Feature.h`
  (`ThreadMark::flankDeg`), `src/core/io/SerializeFeatures.cpp`,
  `src/ui/ToleranceActions.cpp`, `src/core/cam/CamThreadMill.cpp`,
  the Thread editor, `src/core/AssemblySolve.cpp`,
  `src/cad/CADMaterials.{h,cpp}`, `tools/designer_mcp.py`,
  `tools/designer_sdk.py` (`Part.thread` flank).
- Tests: `tests/test_thread.cpp` (a 45° custom thread by Pappus volume;
  refusals), the relation test for a custom and a left-hand thread-backed
  screw, `tests/test_materials.cpp` (tpu), golden `v172-thread-flank`.
- Docs: docs/modeling/holes-and-threads.md, docs/assemblies/relations.md,
  docs/rendering/materials.md, docs/versioning/format.md.

## Verification

`tools/build.sh`; `tools/ctest -LE gui -j$(nproc)`; the GUI suite (the
Thread editor changes); `python3 generators/jar/build.py --port <scratch>`
end to end with `--profile "K5 PLA on P1S"`; `tools/ctest -C generators -R
generator_jar`.
