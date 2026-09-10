# Validating motion froze the app before the run started

Measured 2026-09-10 on the SO-101 PG-52 gripper
(`scratch/so101-pg52-gripper`, 10 parts / 16 occurrences, 25 samples, exact
clash), RTX 4060 Laptop.

## The report

"Validate motion freezes the app when I press it."

It is a fair description and it is not what the code looks like. The run has
been a background job since G2: `motion::Run` takes a cancel predicate and a
progress callback and writes a fraction per sample. Nothing about that shape
should freeze a frame.

## Attribution

Two different problems wearing one symptom, split by timing the RPC that
starts a run (`run_motion_study {async: true}`) — the handler runs on the main
thread, so its latency *is* the stall — and then polling `ping` throughout the
run to see whether the app answers.

| phase | wall | main thread |
|---|---|---|
| `StartMotionRun` before the worker exists | 2.6 s quiet, 7–12 s under load | **blocked, no frame drawn** |
| the run itself | 8–11 s | responsive, worst round-trip 0.06 s |

So half of it was a real freeze and half was a working app with nothing on
screen to say so — the panel greyed the buttons out and said nothing else. The
second half was a UI gap, closed separately by drawing the fraction the job
was already publishing, plus a Cancel (83be3e3c, `OccurrencePanel.cpp`). The
first half is this document.

`tools/profile.sh --app`, driving twelve starts and cancelling each after one
sample so the prep dominates. Measured on 83be3e3c, so the per-occurrence
duplicate copies were already gone — this is what remained after that:

```
54.55%  ui::CopySubtree
 └ 54.55%  Document::Document(const Document&)
    └ 54.54%  Document::Regenerate()
       ├ 51.51%  BooleanFeature::Execute
       │  ├ 36.13%  topo::BooleanNamed  → BRepAlgoAPI_Cut → BOPAlgo_*
       │  └ 15.32%  CADShape::Volume    → BRepGProp::VolumeProperties
       └ ...
```

`CopySubtree` copies every document under the assembly so the worker shares
nothing with the tab — right, and not the problem. The problem is that
`Document`'s copy constructor ends in `Regenerate()`, which **executes the
feature list**: real OCCT booleans, re-cutting cuts that were already cut, to
arrive at bodies the source document was holding the whole time. Two thirds
of the profile is one `BRepAlgoAPI_Cut` after another.

(The 15% under `CADShape::Volume` is not an independent cost. `BooleanFeature`
asks for the result's volume to refuse an empty solid and to advise on a
subtract that removed nothing; the volume cache is keyed on the shape, and
every re-execution produces a *new* shape, so every copy pays the Gauss
integration again. Stop re-executing and it goes away with the rest.)

## The fix

`Document::CopyWithGeometry` (core/Document.h): a copy that hands the new
document the source's own bodies as **baked geometry**, so the tip-hash
comparison an opened project already goes through decides whether they still
describe the feature list. If they do, nothing executes. If they do not — the
source was mid-edit, the hash moved, anything — `Regenerate` ignores the blob
and rebuilds exactly as the plain copy does. The failure mode is slow, never
wrong. Datums still execute, because a datum's output is not a body; that is
the same rule `docs/versioning/baked-geometry.md` sets for a baked open.

| | prep | motion verdict |
|---|---|---|
| plain copy | 6.94 s | clean, 575 pair checks, 25 samples |
| `CopyWithGeometry` | **0.011 s** | byte-identical sample table |

631x, measured back to back on one machine under the same load. The verdict
comparison is the part that matters: the whole point of a copy is that the run
sees the same geometry, and the 25-row sample table and every result field are
identical.

## The trap this hid behind

The first version of the fix made things **worse** — prep went from 3.4 s to
10.7 s — and the profile still showed `BooleanFeature::Execute` at 68%.

`CopyWithGeometry` returned a `Document` by value, and the call site was
`make_unique<Document>(Document::CopyWithGeometry(*pd))`. `Document` declares
a copy constructor, so it has **no implicit move constructor**: the rvalue
bound to `const Document&` and `make_unique` ran the plain copy — a second,
full regeneration — on top of the cheap one. Nothing about that is visible at
the call site, and the cheap copy's own logging still said it had adopted the
bake, because the *second* copy is the one that rebuilt.

So `CopyWithGeometry` returns `std::unique_ptr<Document>`, which makes the
mistake unspellable rather than merely documented.

## Gate

`tests/test_document_copy.cpp`, "A geometry-reusing copy is the same document,
built by executing nothing": same saved bytes, same body volumes as a plain
copy, `LastRegenStats().executed == 0` where the plain copy's is non-zero, and
an edit afterwards still rebuilds from the recipe. Without that `executed`
assertion the two copies are indistinguishable and a regression is silent.

## Not fixed

`CopySubtree` is still synchronous on the main thread. It no longer costs
anything worth seeing, so moving it to the worker would buy 11 ms and cost the
guarantee that the documents cannot be edited underneath it. The other
consumers of `Document`'s copy constructor still pay the full rebuild; whether
any of them also only wants to *read* geometry has not been looked at.

`motion_status` still reports no fraction, though the job publishes one per
sample — an RPC client polling a long run sees `running: true` and nothing
else, which is the same gap the GUI had.
