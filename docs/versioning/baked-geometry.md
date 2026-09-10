# Baked geometry — the built bodies beside the recipe

> Status: **SHIPPED 2026-08-14** (`7c47886` bulk load, `5392888` the blob).
> A 148-feature part: **open 259 s → 0.04 s**, save 0.12 s, project 2.9 MB of
> which 1.4 MB is geometry. Benchmark: `scratch/scale_part.py`.
> A second producer since 2026-09-10 (`7721ef46`): a Document COPY offers the
> source's bodies the same way — see "It is not only for loading".

## Why

A `.dsgn` project is a *recipe* — a feature list re-executed on every open.
That is what makes it diffable and git-friendly, and it is why the scale round
(`docs/performance/scale-round.md`) found a part that saved in 0.4 s and
opened in 259 s. Every open, every clone, every checkout of an old revision
paid a full rebuild.

Commercial CAD files all store the resulting B-Rep alongside the history for
this reason. This does the same, with one property they mostly do not have:
the stored geometry is *checkable*, so it can never be believed when it is
wrong.

## What is stored

```
<project>.dsgn/
  index.json          the recipe's envelope (unchanged)
  features/           one JSON per feature (unchanged)
  geometry/
    index.json        geometry_format, chain_hash, occ_version, and per body
                      its name / material / created_by / element map
    0.brep, 1.brep …  one binary B-Rep per body (BinTools)
  .gitattributes      geometry/*.brep binary -merge -diff
```

The **element map** rides along because face and edge names are transient in
memory — rebuilt on every regeneration — and a drawing dimension or an
assembly mate that references `2/face3;M5` must still resolve after a load
that executed nothing.

The **chain hash** is `Document::TipChainHash()`: the same hash the
regeneration prefix cache keys its entries on, folded over every feature's
serialized JSON plus the active configuration's effective suppression. It is
not a second hashing path invented for this — reusing it is what makes the two
caches agree by construction.

## The rule that makes it safe

On load, the recipe is read first and the blob is *offered* to the document
(`Document::AdoptBakedGeometry`). The single regeneration that follows
compares the hash it computes against the offered one:

- **match** → install the bodies, execute nothing;
- **anything else** → ignore the blob and rebuild.

"Anything else" covers every way a blob can go wrong: absent, truncated,
written by a future format, hash from another model, a feature file edited by
hand or by a merge. Each degrades in silence to exactly the behaviour the app
had before this existed. A stale blob cannot make an open *wrong* — only
faster.

Two consequences worth knowing:

- **A model that failed to regenerate is not baked.** A blob is a promise that
  this is what the recipe produces, and a failed feature means it is not.
- **A different OCC version loads the blob anyway**, with a line in the log.
  Rebuilding would be the riskier choice: face ordering can shift between
  kernel versions, which moves the element names that dimensions and mates
  reference. The geometry the author saw is the geometry you get.

### It is not only for loading

`Document::CopyWithGeometry` offers a copy the *source document's* bodies —
the same claim ("these bodies describe this feature list") arrived at a
different way, and checked by the same comparison, with the same degradation
to a plain rebuild when it does not hold. It took the motion study's snapshot
of an assembly from 6.9 s of re-executed booleans to 11 ms
(docs/performance/motion-prep-copy.md). A second producer for a mechanism
written for loading, and it needed no change to the mechanism.

## Only the tip

The blob stores the *final* bodies, not the bodies after every feature. The
per-feature snapshots are what would make an early-history edit fast too, but
on disk they are N whole B-Reps — roughly 5 MB × 148 for the benchmark part —
for a benefit only the person currently editing can use.

So: **the first edit after opening rebuilds the model**, which is what opening
itself used to cost. Everything that is not an edit — look, measure, section,
export, place in an assembly, project a drawing — is now instant.

The blob does seed the prefix cache with hash-only placeholder entries
(`RegenCacheEntry::bodiesValid == false`) so a *second* regeneration is free:
without that, any later version bump found an empty cache and rebuilt
everything the blob had just saved. A divergence that would resume from a
placeholder has no bodies to resume from, so it falls back to a full rebuild.

## Git

The app creates and commits the project repo itself (`GitVault`), so it is the
app's job to say how derived data should be treated there.

**Committed by default.** A clone or an old checkout opens instantly, and the
geometry is pinned against kernel drift. Saving writes `.gitattributes`
marking the B-Reps `binary -merge -diff`: git will not try to diff them, and a
conflict cannot interleave two shapes into garbage — the resolution for
derived data is "take either side, the hash decides whether it survives".

**`commit_geometry: false`** in the project's `index.json` turns it off: the
blob is deleted, a `.gitignore` keeps `geometry/` out of the repo, and opening
a fresh clone rebuilds. For anyone who would rather keep the repo small than
open it fast.

An unchanged save rewrites nothing — not the recipe, not the blob — so
repeated saves add nothing to history. Git's content addressing does the rest:
only genuine geometry changes cost anything.

## Not done, and why

- **Baking drawing HLR renders.** Same argument, smaller prize (472 ms for a
  twelve-view sheet), and it wants this machinery to settle first.
- **Persisting the prefix cache.** Would make the first edit fast too; N
  B-Reps on disk for one person's benefit. Revisit if the first-edit cost is
  what people actually complain about.
- **Git LFS.** The right answer for a team with large models, the wrong
  dependency to impose on a solo user. The opt-out covers "keep my repo
  small" for now.
