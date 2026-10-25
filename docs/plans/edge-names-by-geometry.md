# Edge names by geometry

## The problem

A U name (`<face>;U<k>`) names a face's unnamed boundary edges by their
position in `TopExp::MapShapes(face, TopAbs_EDGE)` (`FallbackPasses`,
src/core/ElementHistory.cpp). That order is the order OpenCASCADE's blend
builder hands the face back, which follows maps keyed by shape address, so
it moves with the process's allocation history: the same design in a fresh
instance and in one that has just built another names a fillet face's
edges differently. A Fillet saved as picking `100/g1;G110;U0` can find no
such edge, or a different one, when the document is opened in another
process (docs/known-bugs.md, "A blend face's edge names depend on what the
process did before"; `tools/fuzz.py` reports it as `leak`, seed 41 after
seed 37 on generator 1).

A saved document is opened in a different process every time, so this is a
reopen bug as much as a fuzz finding. It is cheapest to fix now: the fix
renames U edges in saved documents, and there are no users' files yet.

## The shape of the fix

**Number a face's unnamed edges in an order the geometry fixes.** For each
named face, take its unique boundary edges (deduplicated as today, so a
seam counts once) and sort them by a key computed from the edge alone:

1. the edge's midpoint (the point at its parameter midpoint), each
   coordinate quantized to a tolerance well above the kernel's noise
   (Precision::Confusion() × 100), compared x, then y, then z;
2. then its length;
3. then its curve type and its two end points, for the edges the first two
   cannot tell apart.

Two edges equal on all of these are coincident geometry. That cannot happen
on a valid face. If it does, the pass names neither, rather than guessing.

`U<k>` is then the k-th edge in that order. The L pass, which names an
unnamed face from its sorted boundary-edge names, is unchanged but now sees
stable input.

**The format moves.** A saved U name means a different edge, so this is a
format change: `vNEXT`, a golden, and its row in format.md's pending table.
No migration: the old numbering was never stable, so there is nothing
reliable to map from. Goldens and fixtures that cite a U name are
regenerated, and the action matrix baseline is re-blessed, with every
changed row read before the bless (a name changing is expected; an outcome
changing is not).

## Files

- `src/core/ElementHistory.cpp` (`FallbackPasses`).
- `src/core/io/SerializationInternal.h` (`kVersionPending`),
  `docs/versioning/format.md`, `tests/golden/`.
- `tests/action_matrix/baseline.json`, any test or fixture that spells a U
  name (grep `;U[0-9]`).
- `docs/known-bugs.md` (the entry goes), the modeling docs on element names.

## Verification

- The fuzz `leak` case: seed 41 after seed 37 on generator 1, replayed, names
  alike in both orders. It fails before the change.
- A unit test builds the same filleted body twice in one process with
  unrelated shapes allocated and freed between, and once after a different
  build, and asserts every U name maps to the same edge geometry each time.
  Fails before.
- A document with a Fillet on a U edge, saved, then opened in a fresh
  instance: the Fillet finds its edge and the volume matches.
- `fuzz_corpus_smoke`, the golden tests, the action matrix (full, its
  outcomes unchanged apart from names), the headless suite, the GUI suite.
