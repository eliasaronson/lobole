# compare_geometry: ask for the metrics you read

## The problem

`compare_geometry` runs **three OpenCASCADE booleans** — `Common(A,B)`,
`Cut(A,B)`, `Cut(B,A)` — to produce IoU, dice, precision, recall and the
missing/extra diff solids. There is no way to ask for less.

The smart-speaker generator's `verify.py` calls it three times per run and
reads **one field**:

```python
r = app.compare_geometry(golden_path=str(path))
ratio = r.get("volume_ratio", 0)
```

`volume_ratio` is vol(A) ÷ vol(B). It needs none of those booleans. Measured
on that project: **13.19 s for the ring brace, 8.71 s for the brace ladder,
0.03 s for the port tube** — 22 s of a 56 s generator run, 39% of it, spent
on four metrics nobody reads. The second call costs the same as the first, so
it is not a missing cache, and it is not the STEP read: the port tube reads a
file of the same kind in 30 ms.

It is the fillets. The ring brace carries 24 tori against 49 cylinders, the
brace ladder 32, and a torus-against-cylinder intersection has no closed form
— the profile is `__sincos_fma`, `GeomInt_...WLApprox`, `SVD_Decompose`,
`ElSLib::TorusD0`: walking lines and least-squares fits.

## What to build

**A `metrics` selector.** `"all"` (the default, today's behaviour) or
`"volume"`, which skips the booleans and returns everything that does not
need them:

- `volume_ratio`, `volume_reference`, `volume_candidate`
- `surface_reference`, `surface_candidate`
- `center_offset`, `bbox_iou`
- the **face and edge census**

and **omits** `iou`, `dice`, `precision`, `recall`, `volume_common`,
`volume_missing`, `volume_extra` rather than reporting them as zero. A zero
IoU means "these do not overlap at all", which is the opposite of "not
measured"; the fields are absent and `metrics` in the response says which
mode produced it.

An unknown `metrics` value is **refused**, naming the two that exist.
`materialize`, `missing_path` and `extra_path` are refused with
`metrics: "volume"` — they are the diff solids, which is exactly what was
skipped.

**The verdict strings** follow the same rule: in volume mode they can speak
about size, placement and topology, and must not claim a shape match, which
only the overlap can support.

## What it costs

A caller that reads `volume_ratio` gets an answer in milliseconds instead of
seconds. A caller that wants to know two shapes are the SAME asks for `all`
and pays for it — and should, because volume is a weak oracle: the benchmark
round found a frustum faked from stacked discs matching a real one on bbox,
volume and centre of mass (docs/benchmark-construction-plan.md). What
separates them there is the face census — `cone:1 plane:2` against
`cylinder:6 plane:7` — which the cheap mode still returns.

So the cheap mode is "size, placement and topology", not "volume alone", and
that is a materially stronger check than the one field the generator reads
today.

## The generator

`verify.py` uses `metrics: "volume"` and **prints which mode it ran**, so a
weaker check never looks like the stronger one. `--deep` asks for `all`,
for when a part has been re-drawn and the question is whether it is the same
shape rather than the same size.

## Files

- `src/core/Measure.{h,cpp}` — `CompareBodySets` takes a flag for the
  overlap; `SetCompare` says whether it holds one.
- `src/remote/RemoteInspection.cpp` — the `metrics` parameter, the refusals,
  the conditional fields and verdicts, the describe row.
- `generators/smart_speaker/verify.py` — cheap by default, `--deep` for the
  full comparison, and it says which it used.

## What the first run corrected

The cheap mode's verdict first called a differing census "DIFFERENT face/edge
census", which reads as a finding. It is not: the built ring brace comes back
**95 faces / 223 edges against the export's 45 / 171 — at an IoU of 0.991**. A
STEP round trip splits faces. The census is therefore *stated*, with a note
that it is not a verdict, and `--deep` is what answers the question it raises.

Measured end to end: `compare_geometry` 20.47 s -> **0.68 s** over the three
calls (225 ms each), and the generator run **55.8 s -> 38.9 s**. `--deep`
costs what it always did.

## Verification

- `tests/test_measure.cpp`: two bodies compared with and without the
  overlap — the volumes, census and centres agree between the two modes, and
  the cheap one reports no overlap rather than zero.
- An RPC smoke: `metrics: "volume"` omits `iou` and carries `volume_ratio`;
  an unknown `metrics` is refused naming both; `materialize` with
  `metrics: "volume"` is refused.
- The generator run, end to end: 55.8 s before, expected ~34.
