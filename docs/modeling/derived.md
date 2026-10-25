# Derived: another part's geometry, copied

A `Derived` feature carries a **copy** of another part's bodies or sketches
inside this part's file, so part B can be drawn on part A — a jaw designed on
a servo's bolt pattern, a bracket on a camera's holes, a gasket on a lid's
outline — without typing their coordinates. Regeneration reads the copy and
never the other part: several parts sharing one regeneration graph is where
ordering, cycles and cross-machine nondeterminism live, and a snapshot keeps
each part a pure function of its own file. Keeping the copy up with its
source is the project's job, done at fixed gates
([below](#copies-that-keep-up)), never in the middle of an edit.

Code: `src/core/features/DerivedFeature.{h,cpp}` (the feature and the
`derived::` helpers), `src/ui/DerivedActions.{h,cpp}` (the project-level verbs
that need other documents), `src/remote/RemoteModeling.cpp` (RPCs).

## The feature

`DerivedFeature` fields (JSON in `describe feature_types`):

- `source_part` — the part slot it copies from; `source_name` its name when
  taken, for messages once the source is gone.
- `bodies` (source `created_by` ids) and/or `body_names` — which bodies; both
  absent means all, unless the copy names a sketch. The two filters narrow together. Names exist because one
  Import feature creates every body of a STEP (hundreds for a PCB), so a
  `created_by` filter cannot pick one out. A filter that selects nothing is
  refused, naming what the source has.
- `configuration` (v133) — the source configuration the copy is taken in, by
  id (`add_feature` also takes its name and stores the id). Absent: whichever
  the source has active. See [Configurations](#configurations).
- `sketches` (source sketch ids) and/or `sketch_names` (v193) — which
  sketches to copy ([Sketches](#sketches)); the two narrow together like the
  body filters, and a copy that names a sketch copies only the bodies it
  names too. A sketch the source does not have is refused naming the ones it
  has, and so is one that failed or is suppressed there.
- `follow` (v193, default true) — the copy is retaken at the gates
  ([Copies that keep up](#copies-that-keep-up)); false, it stays as taken
  until Update.
- `placement {position, orientation}` — where the copy sits in this part;
  identity unless made from an in-context edit. It moves copied sketches'
  planes as it moves bodies.
- `reference` (default true) — see below.
- `material` (every feature's) — given to the copy, it is every copied body's;
  empty, each body keeps its source body's. Material is out of the chain
  hash, so setting it is a cache hit: `Document`'s material stamp applies a
  Derived's own material (and restores the source's when it is cleared),
  since `Execute`, where the copy otherwise stamps it, does not run.
- `snapshot` — per body: its asset name, name, material, source `created_by`,
  the source's element map and display colour, and the size bands the
  source stamped on it (`fit_bands`, [below](#size-bands-on-a-copy)); per
  sketch (`snapshot.sketches`): its source id, name, plane and solved
  entities; plus `source_key`, the [content key](#update-and-staleness).

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

- **Update** (`derived::TakeSnapshot`, driven by a gate, `update_derived` or
  the tree/editor verb) resolves the source part, takes its current bodies
  and sketches at its own tip, replaces the snapshot and records its
  **content key**. It is one undo step on the part's own history; `update_derived
  {all: true}` is one step for every link of the part, and lists only the
  ones it took (`skipped` names the rest with their reasons).
- **The content key** (`derived::Take`, `derived::StoredKey`) is an
  `inputhash` key (`core/InputHash.h`, the stamp studies and requirements
  use) over what the copy holds: each body's exact B-Rep as inputhash digests
  a shape -- read back as a project reads it, without OCC's meshes and flags,
  so a source reopened from its baked geometry keys as the one regenerated
  -- with its name, material, producer, element names, colour and size
  bands; each sketch's plane and solved entities, field by field. FNV, never
  `std::hash` or a JSON dump, so it is the same on every platform and is not
  moved by a build that adds a key to a feature's JSON. A file from before
  v193 records none (its `source_chain_hash` is read and dropped); the key
  is then computed from the snapshot itself.
- **Stale** = taking the copy again would give another key -- or would be
  refused, the source having lost what it copies -- or its source is itself
  behind: stale is **transitive**, so when C copies B and B copies A, C reads
  stale while B is behind A. What taking it now would give is cached by the
  source's stamp (instance, `Version`, `SaveStamp`, configurations revision),
  so a check on a source that has not moved hashes nothing. A source change
  that does not change what was copied -- a feature that adds nothing the
  copy takes, an Import's file moved but not changed -- is not one; a
  material, colour or tolerance on a copied body is. When the source is not
  loaded the answer is unknown (`null`), never "fresh". A copy whose link was
  broken is never stale.

## Copies that keep up

A copy with `follow` (the default) is brought up to date at fixed **gates**,
never in the middle of an edit:

1. a part's tab is shown (`SetActivePart`), and a project is opened
   (`AdoptProject`, before anything reads a part; the parts it changed read
   unsaved, as an edit would leave them);
2. before every RPC whose answer reads geometry -- every method not flagged
   `Deferrable`, at the top of the dispatcher (before a request parks), and
   before each such call inside a `batch`;
3. before a save, every export (`ExportActiveDocument`, `ExportStep`, a print
   job, a drawing's SVG/PDF), a requirements check, a design sweep and a CAM
   generate (`RefreshCamToolpaths`).

`ui::RefreshLinks` (`src/ui/DerivedActions.cpp`) does it for the whole
project: it orders the parts by their following links, and updates each
part's stale ones in that order, so C sees B after B has taken A, all in one
gate. Each part's updates are **one command on that part's own undo stack**
(a stashed part's, as a batch uses for a part that is not active), and the
part is dirty. Undone, the copy is stale again and the next gate takes it
again -- to keep an older copy, turn **Follow source** off (the tree's
context menu, the editor) or break the link.

What is not taken stays stale and says why (`stale_reason` in `get_state`,
the editor's line): a source that is absent, a source that is rolled back
(its rollback bar or a sketch edit's view rollback set: what it gives then
is not the part), a source that refuses the copy (it lost the body or sketch
copied), and both sides of a cycle. The active part is not updated while a
sketch is open on it or a tool's preview feature is pending -- the session
holds its features -- and is taken at the next gate.

In the GUI the part being edited cannot change under the person editing it:
only a script edits another part meanwhile, and gate 1 runs when they next
show a tab.

### Reporting

An update that breaks a feature in the part it updated is applied, never
held back, and said where it happened. The RPC that passed the gate carries
`links_updated: [{part, part_id, features, broken: [{feature_id, name,
error}]}]` beside its own `regen` -- any answer may carry it
(`tools/rpc_contract.py` adds it to every typed result) -- and a `batch`
collects them. The GUI shows one notice in the menu bar ("Gasket updated from
Plate: On outline failed", `AppState::linksNotice`). `stale` is transitive in
`get_state` (with `stale_reason` and `follow`), `check_geometry`'s
`stale_derived` and the tree.

### Nothing may miss a gate

Every resolver that hands a part to another document to read --
`ui::PartDocById`, and the drawing and job resolvers, which are now that
function -- calls `ui::AuditPartRead`. While the audit is on it checks that
the part, and every part upstream of it, has no following link a gate would
take now, and counts one that does: a path that reads a part without passing
a gate. The GUI suite turns it on and fails the test with a miss
(`LinkAuditMisses`, beside the job-edit audit); every smoke sets
`LOBOLE_LINK_AUDIT=1` (`tests/smoke_app.py`), which makes a miss abort the
app with the part and the link named. Off, it costs nothing. The gate's own
reads, and the existence and cycle checks that read no geometry, go through
a raw slot lookup instead.

## Sketches

A copied sketch is the source sketch's SOLVED entities, after the source's
own `Execute` (expressions, constraints, its external geometry), with its
plane, both in the source part's coordinates; the placement moves the plane.
In this part it is reference geometry, never a profile on its own and never a
body:

- a sketch stands on its plane with `plane_ref` = the Derived's id and
  `plane_ref_sketch` = the source sketch id (0 when the copy holds one), and
  follows it through an update;
- each entity is named `<derived id>/<source sketch id>/<entity id>`
  (`DerivedFeature::SketchEntityName`), which a sketch here projects as
  external geometry (`project_geometry`, kind edge). It is put onto the plane
  as it is drawn there (`sketch::ProjectEntity`): exactly, type and curve,
  when the planes are parallel -- facing the other way, an arc runs the
  other way round and stays the same points -- else its shadow, as a body's
  edge projects. Unlike a body's edges it keeps its construction flag, so a
  profile line is still a profile line and the sketch extrudes; it is
  solver-fixed and drawn violet, as a linked copy is. Entity ids are the
  source's own, so a projection survives an update that keeps them; one that
  goes fails the sketch, naming it. Text is not projected.

**Sketch on it** (the Derived editor, one button per copied sketch) opens a
new sketch on a copied sketch's plane with every entity projected. The Derive
tool's editor lists the source's sketches beside the parts, as checkboxes.

A **linked paste** (`Ctrl+Shift+V`) of entities copied in another part makes
(or reuses, when one before the sketch already copies that sketch in place) a
Derived of the source sketch, inserted before the sketch being edited, and
projects the pasted entities: they follow the other part. The clipboard
records the source's part as well as its sketch (`sketchClipboardSourcePart`):
feature ids are per-part counters, and a paste that looked the sketch up by
id alone bound to this part's own sketch of that id.

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
taking it never switches the source's own tab. Staleness takes from that
configured copy too, not the live part: what the live part gives moves with
the configuration it has active, and comparing against it called every copy
of a non-active member stale.

A configuration the source does not have (deleted, or a name that matches
none) is refused, naming the ones it has, and so is one in which a feature of
the source fails.

## Size bands on a copy

A copy carries the size bands its source stamped on each body it copies
(`DerivedBody::fitBands`): an ISO class or explicit deviations on a round
face, a printed press, a bearing's rings
([press fits](../tolerances/README.md#press-fits)). They are taken with the
snapshot, as the SOURCE read them -- its tolerances under its own print
process, so a printed pin derived into a part that is not printed is still
the press its profile bands -- and `fitband::Stamp` puts them on the copy
under the copy's own face names, beside any tolerance the host sets on the
copy's faces. So a press on a source part is a press on the copy an
assembly places, not a clash. They are part of the snapshot: a tolerance set
on the source after it was taken reaches the copy like any other change:
the bands are in the content key, so once the source's bodies carry it the
copy reads stale, and a following copy is retaken at the next gate.

## Refusals

Deriving from this part itself, or from a part whose derived chain reaches
this part (`derived::WouldCycle`, naming the chain; a link copying a sketch
is an edge like one copying a body, and the walk is made at creation and
again at every update, since a file merged from elsewhere can carry a cycle
whole); from a part not in the
project; a `bodies`/`body_names` filter the source does not satisfy; a
`configuration` the source does not have, or one it fails to build in.
`derive_occurrence` outside an in-context edit, or on an occurrence that is not
a neighbour, is refused, and so is one that would take the part past 50 000
features ([../versioning/README.md](../versioning/README.md#what-a-load-refuses)).

## Verbs

RPC:

- `add_feature {type: Derived, source_part, configuration?, bodies?,
  body_names?, sketches?, sketch_names?, follow?, placement?, reference?}` —
  the handler takes the snapshot through the project.
- `update_derived {feature_id | index | all: true}` -- `all` is one undo
  step; `updated` lists what it took, `skipped` the rest with reasons.
- `set_derived_follow {feature_id, follow}`.
- `project_geometry {sketch_id, feature_id: <derived id>, element:
  "<derived id>/<sketch id>/<entity id>"}` projects a copied sketch entity
  (kind defaults to edge for one).
- `derive_occurrence {occurrence, name?, reference?, bodies?, body_names?}` —
  during `edit_in_context` only; placement
  `OccMatrix(edited)⁻¹ · OccMatrix(neighbour)`, so the copy appears exactly
  where the ghosted neighbour is. A sub-assembly occurrence contributes its
  leaf parts, each at its own placement.
- `get_state` feature rows carry `stale: true | false | null`, `follow`,
  and `stale_reason` when stale; `check_geometry` adds `stale_derived` when
  any copy is stale; any answer may carry `links_updated`
  ([Reporting](#reporting)).

GUI: `###tb_derive` opens a preview whose tool window lists the candidate
parts as buttons (one candidate is taken at once). The tree row is tinted,
amber when stale, with **Update from source**, **Follow source** (a check
mark) and **Break link** (forgets the source: a frozen copy) in its context
menu. The editor has the source list, the source's sketches, a Reference
checkbox, Follow source, **Sketch on <name>** per copied sketch and Update
from source. There is no placement editor;
placement comes from the in-context neighbour or the RPC.

## Tests

`tests/test_derived.cpp` (regenerates with the source absent, content-addressed
assets, update clears stale, names and a sketch surviving an update, cycle
refusal, filters by id and by name, ghosts weightless and uncut, no ghost of a
ghost, projected faces and edges, a hole on a projected derived hole edge, a
moved import file is not a change, a mate connector on a derived face; a
copied sketch projected and padded with the source absent, the content key
unmoved by an ignored JSON key or a feature that adds nothing and moved by
an edit, a missing sketch named, an arc put onto a plane facing the other
way); `tests/test_links.cpp` (a linked paste from another part links to
that part's sketch, the gates carrying an edit over with undo and follow off,
a chain A -> B -> C in one gate and transitive stale, a cycle through a
sketch link refused at creation and update, update all as one step, the
audit counting a planted read);
`tests/derived_smoke.py` (the RPC surface end to end, headless: refusals,
stale and update with follow off, undo, in-context placement, save/open, no clash between a
copy and its source, no ghost curves in a drawing view; a sketch copied and
padded in another part, `export_step` of it answering `links_updated`, and a
broken downstream feature named in it); GUI
`derived/tool_copies_the_other_part_as_a_ghost`,
`derived/source_list_takes_the_snapshot`,
`derived/a_ghost_is_not_drawn_in_assemblies`, and `defaults/derive` (refuses
until a source exists). Goldens `v84-derived-servo`, `v85-derived-by-name`,
`v133-derived-configuration` (a copy taken in a configuration the source does
not have active, and its stale check against that configuration),
`v182-derived-fit-bands` (a printed pin's press band on its copy),
`v193-derived-sketch` (a copied sketch, follow off, a sketch on it padded).
`tests/test_press_fits.cpp`, "A Derived copy of a printed pin keeps its
press": a pin printed under a profile, derived into a part that is not
printed, is relieved against its printed cap, before and after a round trip.

## Not built

- Re-checking a copy's placement against the assembly after the neighbour
  moves.
- Two-way features across parts (a hole through three parts at once).
- Feature templates keep their explicit **Update from template**; the gates
  could take them too, through the same function.
- A design sweep rebuilds each part from its file and does not follow a swept
  variable through a link into a deriving part.
- A copied sketch is not drawn in the 3D view of the part that copies it;
  it is seen through a sketch that stands on it or projects it.
