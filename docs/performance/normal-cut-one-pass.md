# A hole no longer unfolds the whole part

Measured 2026-09-18 while building the gas grill generator, whose thirteen
firebox holes are `NormalCut`s through a folded sheet.

## The finding

The generator's own RPC profile put the grill's build at **117 s**, of which
**`NormalCut` was 86.1 s over 30 calls — 2870 ms each**, and the firebox part
alone at 26.1 s. A `NormalCut` is a laser cut: a closed profile carried along
its sketch's normal and cut SQUARE TO THE METAL wherever it lands, which is
done by unfolding the part, laying the outline on the blank, cutting there and
folding back.

Reading the code said why a Ø7 bolt hole cost two and a half seconds:

- The outline is sampled every 0.25 mm, and **every sample built its own
  `IntCurvesFace_ShapeIntersector`** over the whole body — a fresh ray tree
  per sample, hundreds per hole.
- The part was **unfolded to decide where the outline lands**, before knowing
  whether it needed to be. For a hole square onto a panel — which is most
  holes — the flat pattern was then discarded three lines later, under a
  comment saying "nothing is unfolded at all".
- Both costs were paid **per region**. Thirteen holes drawn in one sketch
  unfolded the firebox thirteen times, and the sketch's second and third
  circles each rebuilt the tree the first had just built.

## The fix

One `CutShared` per cut: the ray tree built once, and the flat pattern behind
a lazy `Flat()` that is only forced when an outline actually crosses a bend.
Landing is decided from the sample hits themselves — they already name the
face each ray struck, and all samples on ONE planar face is a panel cut, no
unfold needed. `sheet::NormalCut` then takes **every profile of the sketch in
one call**, so the shared context is amortized over the whole feature rather
than rebuilt per region.

| | before | after |
|---|---|---|
| `NormalCut`, 30 calls | 86.1 s | **1.5 s** (2870 → 50 ms/call) |
| firebox part | 26.1 s | **3.0 s** |
| whole grill build | 117 s | **23.6 s** |

## Why it is safe

No behaviour moved: the same rays, the same landing rule, the same unfold when
one is needed. The gate is `tests/test_normal_cut.cpp` — twelve holes cut from
ONE sketch must give the same body, face for face, as twelve sketches cut one
at a time, and a "looks at" case asserts the panel path takes no flat pattern.
