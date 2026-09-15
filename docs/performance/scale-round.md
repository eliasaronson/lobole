# Scale round — what a big model costs (2026-08-14)

Every dogfooding round before this one used toy models: five-feature parts,
2 mm boxes, two-part assemblies, one-view drawings. They found *interaction*
bugs, because that is all a toy model can show. This round measured what the
app costs at sizes a real user would reach, on the machine it is developed on
(Release, `-O3 -DNDEBUG`; OCC 7.9.3 from nixpkgs, also Release).

The benchmarks are kept as `scratch/scale_part.py`, `scratch/scale_batching.py`
and `scratch/scale_assembly.py` — the numbers they print are the argument, and
re-running them is how the next round starts.

## What was measured

**A 148-feature plate** (400 × 300 × 12 with ~590 counterbored holes in 73
sketch+hole pairs, plus pockets):

| operation | before | after |
|---|---|---|
| add the 4th feature | 99 ms | — |
| add the 100th feature | 7.1 s | — |
| one Hole feature of 480 disjoint holes | 324 s | **7.9 s** |
| change one parameter (rebuild all) | 215 s | (see below) |
| edit the LAST feature (prefix cache) | 5.0 s | — |
| open_project | 259 s | — |
| check_geometry / list_elements | 10 s each | — |
| save_project | 0.4 s | — |
| export STL / STEP | 6.1 s / 2.8 s | — |

**A 50-occurrence assembly and a 12-view A2 drawing of it:**

| operation | before | after |
|---|---|---|
| add_occurrence (any count) | 2 ms | — |
| solve_assembly / BOM / auto_explode | 3 / 4 / 7 ms | — |
| open_project | 11 ms | — |
| mass_properties | 234 ms | — |
| **interference (all 1225 pairs)** | **7.5 s** | **282 ms** |
| update_drawing (12 views) | 5.7 s | **472 ms** |
| **export_drawing SVG / PDF** | **6.1 s / 6.3 s** | **19 ms / 33 ms** |

## What was wrong, and what it taught

**Hole cutters were fused one at a time** (`tool = tool.Union(hole)` in a
loop). `BRepAlgoAPI_Fuse` reprocesses the entire accumulated cutter on every
call, so an n-hole tool cost O(n²) — for solids that, being separate holes,
never touch. Disjoint tools now go into a compound; 480 holes went from 324 s
to 7.9 s.

This one is worth remembering for *how* it was found. The A/B that found it
cut the same 480 holes two ways — 60 Hole features of 8 points against one
feature of 480 — expecting the batched version to win because "the boolean
gets dearer as the solid gets more complex". It lost by 2.5×, and a
hypothesis losing that badly is what pointed at our own code instead of at the
kernel. A profiler would have said the same thing; the A/B needed no tooling
and took two minutes to write.

**Overlapping tools cannot be compounded.** The first version compounded
unconditionally and produced *no body at all* for a layout that spaced Ø6
holes 6 mm apart. A boolean against a compound whose pieces interfere with
each other fails outright, so overlapping holes still fuse. The benchmark's
own degenerate layout is what caught this — a tidier benchmark would have
shipped the bug.

**Interference ran a boolean per pair.** An all-pairs sweep is quadratic by
nature, but almost every pair in a real assembly is nowhere near its partner.
Bounding boxes are rejected first now: 7.5 s → 282 ms.

**Export re-projected the whole sheet.** `ExportActiveDrawing` called
`UpdateDrawingRenders` so the file could not be stale — reasonable intent,
except that HLR is the expensive part of a sheet and nothing had changed.
The cache now answers the staleness question properly (drawing version +
per-view source version), and export dropped from 6 s to 19 ms.

## What is still slow, and what it would take

**A parametric edit on a large part is the headline.** Changing the plate's
thickness on the 148-feature part re-runs everything downstream: 215 s. The
prefix cache does its job — editing the *last* feature costs 5 s, not 215 —
but an early edit is a full rebuild by definition, and a full rebuild is the
sum of ~600 boolean cuts against an increasingly complex solid. The hole fix
takes a large bite out of the constant; the shape of the curve is inherent to
the modeling approach.

**Opening a project rebuilds it from scratch (259 s).** ~~The single most
valuable thing left~~ — **DONE**: baked geometry (`Document`, keyed by the
chain hash the regen cache already computes) turned an open into a load.
Re-measured 2026-09-14 on a 22-part project: **0.14 s**. Saving takes 0.4 s
because the file is a recipe; opening no longer re-executes it.

**`check_geometry` and `list_elements` cost ~10 s** on the 148-feature part.
Also worth re-measuring before acting on: on a 41-feature part they are
**0.01 s each** today, so whatever is left scales with the part rather than
being a fixed cost.

Both walk every face and edge; neither is on a hot path today, but both would
want a cheaper mode (counts and validity without the full element dump).

## Method notes

- Timings are wall-clock over the JSON-RPC surface, which is what a user or a
  script actually waits on, including serialization.
- The first attempt at the part benchmark measured a "full regen" that took
  4 ms, because it re-posted identical feature JSON and the prefix cache
  correctly skipped everything. A rebuild benchmark has to *change* something.
- Two runs of the same benchmark on this machine vary by ~10%; the differences
  reported above are 10× to 300×, so they are not measurement noise.
