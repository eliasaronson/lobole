# Derived: another part's geometry, copied

A `Derived` feature carries a **copy** of another part's bodies inside this
part's file, so part B can be drawn on part A's faces — a jaw designed on a
servo's bolt pattern, a bracket on a camera's holes — without typing their
coordinates. It is a snapshot, not a live link: several parts sharing one
regeneration graph is where ordering, cycles and cross-machine nondeterminism
live, and a snapshot keeps each part a pure function of its own file.

Code: `src/core/features/DerivedFeature.{h,cpp}` (the feature and the
`derived::` helpers), `src/ui/DerivedActions.{h,cpp}` (the project-level verbs
that need other documents), `src/remote/RemoteModeling.cpp` (RPCs).

## The feature

`DerivedFeature` fields (JSON in `describe feature_types`):

- `source_part` — the part slot it copies from; `source_name` its name when
  taken, for messages once the source is gone.
- `bodies` (source `created_by` ids) and/or `body_names` — which bodies; both
  absent means all. The two filters narrow together. Names exist because one
  Import feature creates every body of a STEP (hundreds for a PCB), so a
  `created_by` filter cannot pick one out. A filter that selects nothing is
  refused, naming what the source has.
- `configuration` (v133) — the source configuration the copy is taken in, by
  id (`add_feature` also takes its name and stores the id). Absent: whichever
  the source has active. See [Configurations](#configurations).
- `placement {position, orientation}` — where the copy sits in this part;
  identity unless made from an in-context edit.
- `reference` (default true) — see below.
- `material` (every feature's) — given to the copy, it is every copied body's;
  empty, each body keeps its source body's. Material is out of the chain
  hash, so setting it is a cache hit: `Document`'s material stamp applies a
  Derived's own material (and restores the source's when it is cleared),
  since `Execute`, where the copy otherwise stamps it, does not run.
- `snapshot` — per body: its asset name, name, material, source `created_by`,
  the source's element map and display colour; plus `source_chain_hash`.

**Execute reads the snapshot and never the source document.** Each body's
B-Rep is a document asset `derived-<content hash>.brep` (binary BinTools),
stored where sketch reference images are — `Document`'s asset store,
`assets/` in the directory form, base64 under `assets` in the single file —
because undo and redo rebuild features from JSON and JSON never carries the
bytes. Content addressing lets two features copying the same body share one
blob and leaves the previous blob in place for undo. Everything else is in the
feature JSON, so the chain hash covers it and an update invalidates downstream
features. `CADShape::Volume` is cached by TShape identity, because every
mutating RPC digests every body's volume and a large unchanged snapshot must
not be re-measured each time. The snapshot is not baked geometry: baked geometry is a cache that
can be ignored or deleted, a snapshot is the truth. A file with a Derived
feature loads and regenerates with no source part present (golden
`v84-derived-servo`).

## Update and staleness

- **Update** (`derived::TakeSnapshot`, driven by `update_derived` or the
  tree/editor verb) resolves the source part, takes its current bodies at its
  own tip, applies the placement, replaces the snapshot and records the
  source's `TipChainHash`. It is one undo step.
- **Stale** = the source's current `TipChainHash` differs from the recorded
  one (`derived::IsStale`). Not `Version()`, which restarts at 0 on every
  load. The chain hash strips appearance and material, so recolouring the
  source is not a change, and hashes an Import's file content rather than its
  path, so saving (which moves staged import files into the project) is not
  one either. When the source is not loaded the answer is unknown (`null`),
  never "fresh". A copy whose link was broken is never stale.
- Nothing updates automatically.

## Names

The copy's elements are the source's under this feature:
`<derived id>/<source created_by>/<source name>`
(`DerivedFeature::DerivedName`). They survive an update exactly when the
source's own names do, so a sketch on a derived face follows the face through
an update that changes the source's dimensions; a name that no longer resolves
after an update is an ordinary feature error.

## Reference bodies (ghosts)

`reference: true` sets `Body::reference`, which every consumer reads. A ghost is
there to be drawn on — sketch-on-face, external geometry (all bodies of the
feature are searched), hole centres, a mate connector — and is skipped by
everything that treats a body as matter: mass properties and the BOM, STEP/STL
export (refused in words when only ghosts remain), CAM stock, the FEM mesh,
`Add` (never fuses into a ghost) and `Cut` (passes through), Boolean operands
(refused, naming `reference: false`), hole drilling, the assembly's placed
bodies (interference, the contact report, section caps, fastener fitting and
checks), drawing views, and the assembly renderer. Baked geometry carries the flag, so
a reloaded part does not weigh its ghost. In the part tab a ghost is drawn
translucent (alpha 0.35), with no outline, still pickable. A source's own
ghosts are never copied further.

`reference: false` makes the copy an ordinary body of this part — the way to
use one part as a cutting tool for another.

## Configurations

A part with a configuration per variant is a family, and a Derived with a
`configuration` takes one member of it: the chess set's pieces are one
`piece` part with a configuration per rank, and its rotary blanks are parts
that derive the member they need. (The set itself no longer derives: it
places `piece` with a configuration and a material per placement --
[assemblies](../assemblies/README.md#configurations-per-placement).) The
snapshot is taken from the source
regenerated under that configuration — the copy `ui::ConfiguredPartDocument`
keeps, the same one a drawing view in a non-active configuration reads — so
taking it never switches the source's own tab. Staleness compares against that
configured copy's `TipChainHash`, not the live one: the tip hash moves with
the configuration a part has active, and comparing against it called every
copy of a non-active member stale.

A configuration the source does not have (deleted, or a name that matches
none) is refused, naming the ones it has, and so is one in which a feature of
the source fails.

## Refusals

Deriving from this part itself, or from a part whose derived chain reaches
this part (`derived::WouldCycle`, naming the chain); from a part not in the
project; a `bodies`/`body_names` filter the source does not satisfy; a
`configuration` the source does not have, or one it fails to build in.
`derive_occurrence` outside an in-context edit, or on an occurrence that is not
a neighbour, is refused.

## Verbs

RPC:

- `add_feature {type: Derived, source_part, configuration?, bodies?,
  body_names?, placement?, reference?}` — the handler takes the snapshot
  through the project.
- `update_derived {feature_id | index | all: true}`.
- `derive_occurrence {occurrence, name?, reference?, bodies?, body_names?}` —
  during `edit_in_context` only; placement
  `OccMatrix(edited)⁻¹ · OccMatrix(neighbour)`, so the copy appears exactly
  where the ghosted neighbour is. A sub-assembly occurrence contributes its
  leaf parts, each at its own placement.
- `get_state` feature rows carry `stale: true | false | null`;
  `check_geometry` adds `stale_derived` when any copy is stale.

GUI: `###tb_derive` opens a preview whose tool window lists the candidate
parts as buttons (one candidate is taken at once). The tree row is tinted,
amber when stale, with **Update from source** and **Break link** (forgets the
source: a frozen copy) in its context menu. The editor has the source list, a
Reference checkbox and Update from source. There is no placement editor;
placement comes from the in-context neighbour or the RPC.

## Tests

`tests/test_derived.cpp` (regenerates with the source absent, content-addressed
assets, update clears stale, names and a sketch surviving an update, cycle
refusal, filters by id and by name, ghosts weightless and uncut, no ghost of a
ghost, projected faces and edges, a hole on a projected derived hole edge, a
moved import file is not a change, a mate connector on a derived face);
`tests/derived_smoke.py` (the RPC surface end to end, headless: refusals,
stale, update and undo, in-context placement, save/open, no clash between a
copy and its source, no ghost curves in a drawing view); GUI
`derived/tool_copies_the_other_part_as_a_ghost`,
`derived/source_list_takes_the_snapshot`,
`derived/a_ghost_is_not_drawn_in_assemblies`, and `defaults/derive` (refuses
until a source exists). Goldens `v84-derived-servo`, `v85-derived-by-name`,
`v133-derived-configuration` (a copy taken in a configuration the source does
not have active, and its stale check against that configuration).

## Not built

- Live update in dependency order. The stale flag plus update-all is the
  mechanism.
- Re-checking a copy's placement against the assembly after the neighbour
  moves.
- Deriving sketches across parts (a derived body's faces serve through
  external geometry).
- Two-way features across parts (a hole through three parts at once).
