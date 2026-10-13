# A regeneration cache that remembers more than one history

## The problem

The prefix cache (`Document::m_cache`, `RegenCacheEntry`) keys each feature
on the chain hash of everything up to and including it, which is the right
key. But it is a VECTOR holding exactly one history: an edit at feature k
`resize`s it to k and rebuilds the tail, so the bodies the old tail made are
thrown away. Every way back to a state the document was in a moment ago
then pays for the whole tail again:

- setting a value back to what it was,
- undo, and redo,
- removing a feature and undoing the removal, or dragging one and back
  (the chain hashes after it move, then return).

Measured on the scratch generator outputs (`--headless`, wall time of the
RPC, `get_state.regen` for the counts; each row repeated, spread ±1 s):

| part | features | edit at | edit | set back | undo | redo | executed each |
|---|---|---|---|---|---|---|---|
| OM615 cylinder head | 389 | 3 | 20.7 s | 20.1 s | 21.3 s | 25.1 s | 386 |
| OM615 cylinder head | 389 | 195 | 13.8 s | 12.8 s | 13.2 s | 13.3 s | 194 |
| OM615 cylinder head | 389 | 319 | 0.24 s | 0.28 s | 0.24 s | 0.23 s | 70 |
| OM615 block | 228 | 5 | 5.0 s | 5.4 s | 5.0 s | 5.0 s | 223 |
| OM615 block | 228 | 114 | 3.8 s | 3.9 s | 4.0 s | 4.0 s | 114 |
| planetary print | 39 | 6 | 10.4 s | 10.1 s | 9.9 s | 9.9 s | 33 |

A full regeneration of the head is 19.2 s, so an edit near the top costs a
full rebuild, and so does every step back from it: trying a value and
returning, or one undo and one redo, is three of them, a minute on the head.
All of the set back / undo / redo cost is recomputing geometry the document
already built.

What is already free and stays so: a preview followed by OK, and undo/redo
of an ADDED feature. Both append at the tail, a removal of the tail is a
full hit (`divergence == upTo` keeps the entries beyond), and the re-add
finds its entry. A recolour or material change is out of the hash.

## The shape of it

**Keep every recent entry, keyed by its chain hash.** `m_cache` becomes an
LRU map `chain hash → entry`, plus the vector of hashes of the CURRENT chain
(what `CachedBodiesAfter(index)` and the baked-placeholder logic read).
Regenerate walks the features as now, but a feature whose chain hash is in
the map is a hit wherever it is, not only inside the unbroken prefix. Once
the chain diverges it never re-converges by itself (the hash folds
everything before it), so in practice a run is: hits up to k, then either
all hits (a state seen before) or all misses. The key is unchanged, so
everything it already covers stays covered: the evaluated expressions
baked into the feature JSON, configuration suppression, the sheet-metal
seed, the print fingerprint, an import's content hash. Nothing new needs
hashing, because a hit still means "identical inputs all the way down".

**An entry carries what Execute left on the feature, not only the bodies.**
The prefix cache gets away with keeping only bodies because a cached
feature's object still holds the state of its last execution, which WAS
this one. A hit from another branch breaks that: feature k+5's `m_tool`
(the prism a later Pattern reads), a Frame's resolved placement, a Curve's
edge, a sheet feature's bends, and the error, warning and
`errorAfterEarlierFailure` it raised are those of the branch just left. So
an entry stores, besides `bodiesAfter`, a `Clone()` of the feature taken
right after it executed, and a hit restores from it through a new
`Feature::CopyDerivedState(const Feature& executed)`. It is PURE virtual,
like `Clone` and `VisitFeatureRefs`, so a new feature type has to say what
its Execute retains; the base copies error, warning and unresolved face
looks. A suppressed or rolled-back feature's entry holds no clone.

**Memory is bounded by entry count, per document.** Bodies are shared
`TopoDS_Shape`s, so an entry costs what that feature made and nothing it
passed through. The head's full 389-entry chain is ~90 MB resident
(173 MB after open, 262 MB after the first rebuild), and each alternative
history kept costs up to that again. Cap the map at `max(256, 3 × feature
count)` entries — the current chain plus about two alternatives for the
whole list, more for edits low in it — evicting least recently used, never
an entry of the current chain. `ClearRegenCache` clears it as now.

**OpenCASCADE.** A boolean writes p-curves onto its arguments, and a hit
hands a cached body to the next feature as an argument: that is already
what the prefix cache does, on one thread, so nothing changes. The
restored bodies are the same `TShape`s the earlier branch had, so the
tessellation cache (keyed on `CADShape::GeometryId`) can serve their meshes
too.

**Not keyed on body identity.** A per-body key would let an edit to one
body skip later features on another. Every multi-feature OM615 part is one
body, and in the multi-body ones the Add/Cut/Hole target is chosen by
bounding-box overlap across ALL bodies (`ApplyBodyMode`), so those features
read every body and would miss anyway. In the planetary print (5 bodies),
editing the sun's knob re-ran 33 features in 11.5 s, of which the four
gear branches it does not touch are ~2.3 s each; the engrave cuts in them
are implicit-target and would still re-run.

## Files

- `src/core/Document.h`, `src/core/Document.cpp` — the LRU map and the
  current-chain vector; the divergence loop becomes a per-feature lookup;
  restore of derived state on a hit; the bake path seeds placeholders into
  the vector as now. `LastRegenStats` unchanged.
- `src/core/Feature.h` — `CopyDerivedState`, base half for error/warning.
- every feature class (`src/core/features/*.h`, sketch, sheet, datums,
  `Primitives.h`, `TemplateFeature`) — its override.
- `docs/architecture.md#the-document-model-and-regeneration` — the
  "Incremental" bullet; `docs/versioning/README.md` where it describes the
  prefix cache after a bake.

## Verification

- `tests/test_regen_cache.cpp`, **`cached_evaluation_matches_from_scratch`**:
  for every document in the golden corpus (the set `test_determinism.cpp`
  walks), run a fixed, seeded sequence — modify an
  early, a middle and a late numeric field; set each back; undo and redo
  through the stack; remove a feature and undo; move one and back — and
  after EVERY step compare against a clone regenerated with
  `ClearRegenCache()`: body count, names, volumes, topology counts, element
  maps, and every feature's error, warning and retained state (a Pattern of
  a cut reads `m_tool`, so a stale one shows as different geometry).
- The same test asserts the counts: set back, undo, redo, remove-and-undo
  and move-and-back execute 0 features; an edit executes exactly the tail.
- A python smoke does the same sequence, cheaper, over generator outputs
  (gear, slider crank, planetary print) built in a private scratch dir, as
  the generator tests already do.
- A cap test: a long edit sequence on a 40-feature part never exceeds the
  cap, and the current chain is never evicted.
- Re-measure the table above with the same RPC script: every set back /
  undo / redo row is under 0.5 s on the head.
- The headless suite and `tools/test-fast.sh`; `--asan` once, since entries
  now outlive the chain that made them.
