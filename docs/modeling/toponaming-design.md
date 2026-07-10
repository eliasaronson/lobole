# Topological naming — design memo (T1)

> Deliverable of [toponaming.md](toponaming.md) milestone **T1**. Validates the
> candidate "element maps seeded by our stable ids" design against the FreeCAD
> RealThunder scheme and the OCC history API, pins down the naming grammar,
> per-op propagation rules, `ElementRef` API, failure semantics, and
> serialization, and walks the three required mutation scenarios. Grounded in a
> read of FreeCAD `src/App/ElementMap.*` + `TopoShapeExpansion.cpp`, the OCC
> `BRep*` history headers, and our own `CADKernel.cpp` / `SketchProfile.cpp`.

## The core idea (validated)

FreeCAD's whole scheme reduces to **two identifiers per subshape**:

- **IndexedName** — the *volatile* address the kernel gives you: `Face2`,
  `Edge7`. For us this is already `faceIndex` — the `TopExp_Explorer` order in
  `CADShape::GenerateMesh` (`CADKernel.cpp:179-224`). Stable within one
  in-memory shape (fine for picking inside one regen), **not** stable across a
  regeneration after an edit. This is the identity we must stop persisting.
- **MappedName** — a *stable* string encoding *why the element exists* (its
  provenance). This is what references store; each regeneration re-resolves it
  to whatever `faceIndex` the element now occupies.

The element map is just `MappedName ↔ IndexedName`, rebuilt every regeneration.
Everything below is machinery to *generate* good MappedNames. We adopt the
irreducible core and deliberately drop FreeCAD's heavy parts (see
[Scope](#scope-what-we-steal-vs-skip)).

## Naming grammar (ours — human-readable, no hashing)

A MappedName is a plain `std::string`. Because our histories are shallow
(sketch → extrude/revolve → boolean → fillet, rarely deeper) we keep names
readable and skip FreeCAD's `StringHasher`/`#id` collapse until profiling or a
real collision forces it. Grammar:

```
name    := seed (';' step)*
seed    := tag '/g' entityId          ; a solid element born from sketch entity <entityId>
         | tag '/cap' ('0'|'1')       ; extrude/revolve end cap (fixed, hole-count independent)
         | tag '/seam' index          ; boolean intersection (section) element
step    := ('G'|'M') tag ('.' index)? ; Generated / Modified by feature <tag>, .index if 1→N split
         | 'U' localIndex             ; fallback: unnamed sub-element of a named parent
         | 'L'                        ; fallback: a face named from its (sorted) boundary edges
```

- `tag` = the owning feature's stable id (`Feature::GetId()`). Every segment
  carries it, so two features' elements never collide when combined (booleans,
  later assemblies) — FreeCAD's `;:H<tag>` namespacing, simplified.
- `entityId` = our stable sketch-entity id (`SketchFeature::nextEntityId`,
  monotonic, never reused). **This is the seed that makes everything stable** —
  the single most important thing we copy from FreeCAD (their sketch seeds
  `g7` = geometry id, not the volatile `Edge7`).
- A step *appends*; the historical prefix is never rewritten, so an element's
  name records its full derivation.

Examples (feature ids in brackets):

| Element | MappedName |
|---|---|
| Extrude[4] side face from sketch line id 7 | `4/g7` |
| Extrude[4] start / end cap | `4/cap0` / `4/cap1` |
| Fillet[5] blend face from that side face's edge | `4/g7;G5` |
| Same face after a hole is cut through it by Cut[6] | `4/g7;M6` |
| Boolean[6] seam face along the intersection | `6/seam0` |

## Per-op propagation rules

Naming runs inside each solid feature's `Execute`, immediately after the OCC
maker builds the shape (**the maker must be kept alive** — see
[Prerequisites](#prerequisites)). For each output face (in `faceIndex`
explorer order, so the map keys match picking), we ask the maker what it came
from and compose the name. Rule of thumb: *survivor keeps its name; Modified
appends `M`; Generated appends `G`; unnamed → fallback pass.*

### Extrude (`BRepPrimAPI_MakePrism`) — **SAFE**
- Side face: `Generated(profileEdge)` → the swept face. Name `= tag/g<entityId>`
  where `entityId` is the sketch entity that produced `profileEdge`.
- Caps: **not** via `Modified` (prism doesn't override it); use `FirstShape()`
  / `LastShape()`. Name them with the fixed sentinels `cap0`/`cap1` so adding
  or removing holes in the sketch never renames a cap (FreeCAD's coplanar
  fixed-index trick, and a top user-pain fix).
- Side edges: mostly filled by the fallback passes (a side face names its
  boundary edges via `U`).

### Revolve (`BRepPrimAPI_MakeRevol`) — **SAFE**
- Same shape as prism: `Generated(profileEdge)` → side face `tag/g<entityId>`;
  the two planar ends of a *partial* revolve via `FirstShape`/`LastShape` →
  `cap0`/`cap1`. Full-360 has no caps.
- **Caveat:** `Generated` returns empty for non VERTEX/EDGE/FACE/SOLID inputs,
  and revolve creates **degenerate pole edges** at the axis with no generating
  parent (`MakeRevol::HasDegenerated/Degenerated`). Name poles from the axis
  (`tag/pole<n>`) or leave to the `L` fallback; never let them go unnamed.

### Boolean (`BRepAlgoAPI_Fuse/Cut/Common`) — **NEEDS CARE**
- A face untouched by the op **survives** → copy its incoming MappedName
  verbatim (identity preserved for free).
- **Gotcha:** faces created *along the intersection* come back as
  `Modified(inputFace)`, **not** `Generated`. A 1→N split (one face cut into
  pieces) → each piece gets `<sourceName>;M<tag>.<i>` with a positional `i`.
- Seam edges: `SectionEdges()` → `tag/seam<i>`.
- `IsDeleted(inputFace)` → that face and its splits are gone; any reference to
  its name now fails (→ reattach).
- **Our booleans currently discard the maker** (`CADKernel.cpp:84-112` takes
  `.Shape()` and drops the `BRepAlgoAPI_*`), so history is unavailable today —
  a hard prerequisite for T5.

### Fillet / chamfer (`BRepFilletAPI_MakeFillet/MakeChamfer`) — **NEEDS CARE**
- Blend face: `Generated(spineEdge)` → `<spineEdgeName>;G<tag>`. Reliable for
  simple edges; **fragile at corners/vertices and on tangent chains** —
  `Generated` at a vertex can be empty even though a corner patch exists.
- Adjacent support faces: `Modified(face)` → keep their names.
- The filleted edge is **removed** yet is the *argument* to `Generated` (OCC
  reports removed-but-generating). References to that edge fail → reattach.
- Check `StripeStatus`/`NbFaultyContours` and fall back to geometric matching
  where `Generated` is empty.

### The two fallback passes (essential, not optional)
OCC history is genuinely incomplete for lower-dimensional elements, so after
the history pass we name whatever is still unnamed (FreeCAD's `;:U`/`;:L`,
reduced to one pass each):
1. **Reverse (`U`)** — a named face names its unnamed edges/vertices:
   `<faceName>;U<localIdx>` (localIdx = deterministic position in the face's
   boundary). This is what gives fillet **edges** stable names to reference.
2. **Forward (`L`)** — an unnamed face is named from its sorted boundary-edge
   names + `;L`. Catches boolean seam faces and revolve caps the history missed.

### One code path: normalize onto `BRepTools_History`
The modern boolean family exposes `History()` directly; prism/revolve/fillet do
not (revolve's is private). `BRepTools_History` has a **template constructor**
that folds any maker's `Modified/Generated/IsDeleted` into the uniform object,
and `Merge()` composes histories across the feature chain. Building every op's
history into a `BRepTools_History` gives the propagator a single interface
instead of four per-maker quirk sets — do this from T2.

## `ElementRef` API

```cpp
// A persistent reference to a face/edge of a feature's output body.
struct ElementRef {
  uint64_t featureId = 0;   // the feature whose output map owns the name
  std::string name;         // a MappedName in that feature's element map
};

// Attached to each Body (alongside its CADShape): the map rebuilt every regen.
class ElementMap {
  // name -> faceIndex (the explorer-order id GenerateMesh/FaceSelector use)
  std::optional<uint32_t> Resolve(const std::string& name) const;
  const std::string* NameOf(uint32_t faceIndex) const;   // reverse, for "pick → store"
  // (edges get an analogous edgeIndex space when T5 needs them)
};
```

- **Pick → store:** the user picks `faceIndex` on a body → `NameOf` → store
  `ElementRef{ owningFeatureId, name }`.
- **Resolve (each regen):** `RegenContext` finds the referenced feature's body
  → `ElementMap::Resolve(name)` → concrete `faceIndex` (→ `TopoDS_Face` for
  geometry, or the pick range for highlighting).
- References resolve against **the feature they were picked at**, so adding
  downstream features never disturbs an existing reference; only editing the
  referenced feature or its ancestors exercises name stability (which the
  stable-id seeds provide).

## Failure semantics

Resolution that returns `nullopt` (the generating entity was deleted, a fillet
edge consumed, a face booleaned away) is **a first-class state, never a crash
or a silent guess**:
- The referencing feature enters a **per-feature error state** carrying the
  dangling `ElementRef`, and regeneration stops there (bodies reflect the good
  prefix — the existing `Document::Regenerate` contract).
- The UI offers a **reattach** action (re-pick the element). An optional
  geometric-similarity fallback (nearest face by surface type + centroid +
  normal + area) may *suggest* a target but must require confirmation.
- **Prerequisite:** `Feature` has no error field today; add one mirroring
  `Parameter::error` (`Document.h`), which already does exactly this for
  variables.

## Serialization

`ElementRef` serializes like every other id-reference we have
(`sketch_id`/`datum_axis_id`/`entity_id`): `{ "feature_id": N, "element":
"4/g7" }`. The `ElementMap` itself is **derived, never serialized** — it is
rebuilt on load by the regeneration, exactly like `DatumPlaneFeature`'s cached
plane. A format bump gates the first feature that stores an `ElementRef` (T4).

## Three scenario walkthroughs

**1. Reorder sketch entities.** Extrude[4] over sketch entities 7, 12, 19 →
side faces `4/g7`, `4/g12`, `4/g19`. Reordering the entities in the sketch's
list leaves their **ids** unchanged (ids are assigned at creation, never
reused). The names are identical; only the `faceIndex` each maps to may shift.
A reference to `4/g12` re-resolves to the new index. ✓ *This is the entire
payoff — names key on id, not explorer order.*

**2. Add a hole through a filleted face.** Box[4] → Fillet[5] an edge → blend
face `4/gE;G5`. A sketch-on-face reference picked here is `ElementRef{5,
"4/gE;G5"}`. Later, Cut[6] drills a hole through that filleted face. Because the
reference is anchored at **feature 5**, it keeps resolving against feature 5's
output regardless of feature 6. Within feature 6, the filleted face is
`Modified` → `4/gE;G5;M6` (with the hole), and the hole wall is a new face
named from the tool sketch + `6`. Editing feature 5's fillet radius doesn't
break the reference — the blend name keys on the spine edge's name, not a
radius or index. ✓

**3. Delete the referenced entity.** Sketch-on-face references `4/g12` (side
face from entity 12). Delete entity 12 from the sketch → Extrude[4]
re-executes, produces no side face named `4/g12`. `Resolve("4/g12")` →
`nullopt` → the dependent sketch enters the error state with a reattach prompt;
regeneration stops there. No silent snap to a neighbor. ✓ (Contrast today:
`faceIndex` would silently point at *whatever face now sits at that index* — the
classic toponaming bug.)

## Prerequisites (before T2 can prototype)

1. **Thread the sketch-entity id onto profile edges.** `Loop`/`LoopEdge`
   (`SketchProfile.h`) drop the entity id today, so extrude/revolve can't map
   `Generated(edge)` back to an entity. Add an `entityId` to `LoopEdge` and
   carry it through `MakeLoopWire`.
2. **Keep OCC makers alive.** `BuildPrism`/`BuildRevolve` return
   `CADShape(maker.Shape())` and drop the maker; booleans discard theirs. Build
   the `ElementMap` inside the builder (or return the maker/history alongside
   the shape).
3. **Per-feature error state** on `Feature` (mirror `Parameter::error`) for
   resolution-failure surfacing + reattach.
4. **`ElementMap` on `Body`** (name ↔ faceIndex), rebuilt each regen.
5. **Normalize op history onto `BRepTools_History`** for a single propagation
   path.

## Scope: what we steal vs skip

**Steal (irreducible):** the two-map indirection; seeding leaf names from our
stable sketch-entity ids; driving names off OCC `Modified`/`Generated`/
`IsDeleted`; the G/M markers with the source name as base; per-feature tag
namespacing; the `U`/`L` fallback passes; the fixed-sentinel cap-face trick.

**Skip (over-engineering at our scale):** `StringHasher`/`#id` name hashing
(store readable strings; add hashing only if names get pathological); multiple
names per element / `findAll`; child-element maps (`;:C`/`;:R`, only for
array/link features); the `data`/`postfix` zero-copy `QByteArray` split (plain
`std::string` append); external tags, negative-tag disambiguation, `;D`
duplicate randomization (add reactively on a real collision); encoding-version
migration (regenerate maps freely while the scheme is young).

## Revised T2–T5 path

- **T2 — prototype on extrude.** Prereqs 1, 2, 4. Seed side faces from entity
  ids + fixed caps; debug overlay shows names on hover. Mutation tests: move /
  add a sketch entity → the same side face keeps its name; delete it → name
  reports gone. *No history propagation needed — extrude generates directly.*
- **T3 — `ElementRef` plumbing.** Prereq 3. The ref type, serialization (format
  bump), resolution in `RegenContext`, error surfacing.
- **T4 — adopt sketch-on-face.** Replace the baked `SketchPlane` for
  face-attached sketches with an `ElementRef`; keep baked planes for
  datum/default-plane sketches (already stable by feature id). First
  user-visible payoff: edit a base extrude's height → the dependent
  sketch + its extrude follow.
- **T5 — booleans + fillet.** Prereq 5 done properly; the `U`/`L` passes; then
  M7 fillet consumes `ElementRef` edges. From here every new feature is born
  with an element map.

## Risks & open questions

- **Boolean history is currently thrown away** — the single biggest code change
  T5 needs; verify `Modified`+`SectionEdges`+`History()` on real cut/fuse
  cases before committing the propagation rule.
- **Fillet corners / revolve poles** report empty `Generated`; the `L` fallback
  + a geometric backup must cover them. Prototype each op and record which
  actually need the fallback.
- **1→N split disambiguation** uses a positional index, which is the one place
  stability leaks (FreeCAD hit the same wall). Acceptable for our depth; revisit
  if boolean-heavy models show reference drift.
- **Edge naming** is deferred to T5 (via the `U` pass); T2–T4 only need faces.
- Keep the guard from the plan: names good enough for *our* four consumers, not
  a general theory. T2's extrude-only scope enforces it.
