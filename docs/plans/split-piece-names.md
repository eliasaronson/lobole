# Split pieces named by what bounds them

## The problem

When one operation turns a face or edge into several, `PlaceAll`
(`src/core/ElementHistory.cpp`) names the pieces `<name>;M<tag>.0`, `.1`, …
in **result-index order**: the order in which OpenCASCADE's builder made
them. Nothing about a piece is in its name, so anything that changes that
order, or adds a piece, moves every reference to a piece onto another piece.
It does that silently: the name still resolves.

Reproduced on a headless instance. A 40×20×10 block (extrude 2 over rectangle
0..40 × 0..20) gets a 4 mm deep slot across it (cut 4, x = 12..16). The top
splits into `2/cap1;M4.0` (the right piece, centroid x = 28) and
`2/cap1;M4.1` (left, x = 6). A sketch is attached to `2/cap1;M4.0` and a
fillet rounds its front edge, `2/g2;U3;M4.0`. Then the slot sketch is edited:

| edit to the cut | sketch lands on | fillet lands on | reported |
|---|---|---|---|
| slot moved to x = 28..32 | right piece (x = 36) | its front edge | nothing |
| second slot at x = 36..38 | **a 2 mm sliver, x = 38..40** | **the sliver's 2 mm edge** | nothing |
| second slot at x = 26..28 | the piece x = 28..40, not 16..26 | the same | nothing |
| base rectangle drawn from the opposite corner | **the left piece (x = 6)** | the left piece's back edge | nothing |

`.0` turns out to be whichever piece holds the first unsplit edge of the base
wire: a moved cutter keeps it, but a second cutter, or a base sketch whose
wire starts elsewhere, hands `.0` to another piece. The second row is the
worst case: a sketch drawn on a 24 mm wide face now sits on a 2 mm sliver, and
the regeneration reports success. `docs/modeling/toponaming.md#limits` names
this as "the one place stability can leak"; its own rule, "never a silent snap
to a neighbouring face", is what it breaks.

Edges inherit it twice: an edge split by the cutter gets `.i` the same way,
and every U-pass edge of a piece face (`<piece>;U<k>`) carries the piece's
ordinal.

## The shape of it

### 1. A piece is named by the faces only it touches

Pieces of one name are told apart by their neighbours. For each group of
results that one input became (a `Modified` or `Generated` list longer than
one), after the history pass and before the fallback passes:

- a **face** piece's bounds are the names of the faces sharing an edge with
  it (`TopExp::MapShapesAndAncestors`, edge → faces);
- an **edge** piece's bounds are the faces touching its end vertices that do
  not contain the edge itself (vertex → faces), so the top edge of the front
  wall is told apart by the slot wall and the side wall at its ends, the
  same faces that tell the top pieces apart.

Each neighbour enters by its **stem**: its name with every piece qualifier
removed, so a piece is never qualified by another piece's qualifier and
names nest one level at most. A piece's qualifier is its bounds minus those
every piece of the group shares, sorted, joined with `|`:

```
2/cap1;M4[2/g3|4/g3;M4]      the right piece: the block's right wall and the slot's wall
2/cap1;M4[2/g5|4/g5;M4]      the left piece
2/g2;U3;M4[2/g3|4/g3;M4]     the right half of the front wall's top edge
```

A moved or resized cutter keeps those names exactly: the same faces still
bound the same pieces. Faces not yet named when it runs (the L pass comes
later) do not count.

**Pieces nothing tells apart keep an ordinal.** When two pieces have the same
qualifier (a cylinder cut by a slot through its axis: both halves of the wall
touch both slot walls and both caps), they are numbered `.0`, `.1` in result
order as today, and recorded as **ordinal**. A last pass makes every name in
the map unique the same way, so two faces never share one.

The qualifier is the step's suffix, `;M<tag>[…]` or `;G<tag>[…]`, where `.i`
sits today. `[`, `]` and `|` appear nowhere else in the grammar; the content
of a bracket is opaque to every reader.

### 2. Lookup: exact, then the same piece, then refuse

`ElementMap` keeps, for each piece face and edge, its stem, its full bounds
and whether it is ordinal (`pieces` / `edgePieces`). `Resolve` and
`ResolveEdge` grow two steps behind the exact match, so all 82 callers get
them without change:

1. **exact** — the name.
2. **the same piece** — a stored `stem[a|b]` finds the one current face whose
   stem is `stem` and whose bounds include `a` and `b`. That is the piece
   the reference meant, whatever the other pieces now are, so it resolves
   silently. If the stem itself now names one whole face (the cutter no
   longer splits it), that face is the piece grown to the whole; it resolves
   too.
3. **a legacy ordinal** — a stored `stem.<i>` with no exact match takes the
   i-th piece of `stem` in result order, the rule the build that saved it
   used, and the regeneration warns that the reference is by position and to
   re-pick it.

Anything else does not resolve, as now. When the reason is a split — several
current faces have the stem and none or more than one is bounded by all of
the stored names — the error says so, from one helper,
`topo::UnresolvedReason(map, name, kind)`:

> attached face `2/cap1;M4[2/g3|4/g3;M4]` was split by Slot (4) into
> `…[2/g3|4/g7;M4]` and `…[4/g3;M4|4/g5;M4]`; pick the one you meant

That is the second row of the table: today's silent jump to the sliver
becomes an error on the sketch and on the fillet, naming both candidates. It
does not pick the nearer piece: a reference that no longer names one face is
a feature error, which is the rule `toponaming.md` already states.

The 45 sites that build "… no longer exists" today call the helper instead,
and `tests/test_conventions.cpp` fails on that literal anywhere else.

### 3. One reader of the grammar

The qualifier contains `;`, `/`, `,` and `#` (a neighbour may be an L face or
a pattern copy), so every place that scans a name must skip brackets. Those
are `DecodeElementName` (`src/core/ElementOrigin.cpp`), `NameBefore`
(`Literals.cpp`), the instance strip in `PatternFeature.cpp`, and the two
copies of the descent parser: `DescendsByModification` (`Document.cpp`, face
appearances) and `DescentOf` (`AssemblySolve.cpp`, a fastener's seat). They
get one bracket-aware step tokenizer in `ElementOrigin`, and the two descent
parsers become one `topo::DescentOf` there, reading `.<i>` and `[…]` alike
as a split. A face appearance still follows every piece; a seat still fails
on a split.

`DecodeElementName` reports a piece's qualifier, and `list_elements` reports
`piece: {of, bounded_by, ordinal}` for each piece face and edge, with
`ordinal: true` meaning the name is by position and will not survive a
reorder. Its `MethodDoc` row says so.

### Saved documents

A name is saved in every reference, so this is a format change: raise
`kVersionPending`, add a golden `tests/golden/vNEXT-split-piece-names.json`
(+ `.expect.json`) holding a sketch on a qualified piece and a fillet on a
qualified edge, and a `vNEXT` row in format.md's Pending changes. No
migration: a saved `.i` name keeps the meaning it was saved with through
lookup step 3, warns, and is replaced by the qualified name when re-picked.
The documents under `scratch/` that hold such names come back from their
generators.

## Files

- `src/core/ElementHistory.cpp` — `PlaceAll` records groups instead of
  writing `.i`; a `QualifyPieces` pass in `PropagateNames` after the seams
  and before `FallbackPasses`; the uniqueness pass. Every named boolean,
  blend and `NamesThroughHistory` goes through it.
- `src/core/ElementHistory.h` — `UnresolvedReason`; the header comment's
  grammar.
- `src/core/ElementMap.h` — `pieces` / `edgePieces`, lookup steps 2 and 3.
- `src/core/ElementOrigin.{h,cpp}` — the tokenizer, the qualifier in
  `ElementOrigin`, `DescentOf`.
- `src/core/Document.cpp`, `src/core/AssemblySolve.cpp`,
  `src/core/Literals.cpp`, `src/core/features/PatternFeature.cpp` — read
  names through the tokenizer.
- The 45 "no longer exists" sites (features, `FemStudy`, `MotionStudy`,
  `RemoteCam`, `SketchTools`, `DocumentSlots`, `ConflictsPanel`, …) —
  `UnresolvedReason`.
- `src/remote/RemoteInspection.cpp` — `list_elements`' `piece` field and its
  row.
- `src/core/io/SerializationInternal.h`, the golden, `docs/versioning/format.md`.
- Docs: `docs/modeling/toponaming.md` (grammar, operations, resolution,
  limits), `docs/rendering/materials.md` and `docs/parts/README.md` where they
  quote `;M<tag>.<i>`.

## Verification

`tests/test_toponaming.cpp`, each a mutation test on the block above with a
sketch on the right top piece and a fillet on its front edge, asserting by
centroid where each lands:

- the slot moved, resized, and joined by a second slot on the far side: both
  stay on the right piece, by name, with no warning;
- a second slot through the right piece: both features fail, and the error
  names the cut and both pieces (today both land on the sliver);
- the base rectangle drawn from the other corner: the sketch stays on the
  piece bounded by the slot wall it was bounded by, or fails, and never
  lands on the left piece;
- a cylinder split by a slot through its axis: both wall pieces are ordinal,
  and `list_elements` says so;
- every name in each map is unique;
- a stored `2/cap1;M4.0` resolves to the piece it named before, with the
  warning.

The existing "A slot cut splits a cap into disambiguated .0/.1 pieces" case
becomes the qualified-name case. `tests/test_element_origin.cpp` decodes
qualified names whose neighbours carry `,`, `#` and `;`. The seat-split case
in `tests/test_actions.cpp` runs on a qualified split. `test_determinism`
checks the golden corpus still names identically across regenerations, and
`test_golden` and `format_version_gate` check the bump. Then the headless
suite and `tools/test-fast.sh`.
