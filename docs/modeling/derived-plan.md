# Derived — geometry from another part (A2)

> Status: **D1-D4 DONE 2026-09-03** (`0beba3a8` core, `e82961eb`
> surface, `b3933741` the D3 proofs, then the gripper:
> [derived-gripper-dogfood.md](derived-gripper-dogfood.md)). See "As built"
> at the end. The capability review's A2
> ([../capability-review-2026-08-31.md](../capability-review-2026-08-31.md))
> as a design: a **binder feature with a snapshot**, not a shared regeneration
> graph. From: *"we already have variable changes as cross-cutting
> dependencies — is A2 the same risk?"* and *"could you build the SO-101
> gripper?"* — the second is this plan's acceptance test.

## Why now, and what it is not

Top-down design is the casualty of A2: you cannot draw part B on part A's
face. Every workaround this project has used is a number carried by hand —
a project variable for a hole centre, a measured bolt pattern typed into the
next part. The gripper exercise is the shape of the demand: a jaw designed
around a servo's bolt pattern and a camera's holes, both of which exist
only as other documents' geometry.

This is **not** a Part Studio. Several parts sharing one regeneration graph
is where ordering, cycles and cross-machine nondeterminism live, and the
review was right to refuse it. It is a feature that carries a **copy** of
another part's geometry inside this part's file, the way the Import feature
carries a STEP's, and is **updated on request**, the way a drawing is.

## What exists, and is reused

| precedent | what it proves | reused for |
|---|---|---|
| project variables (`RegenContext::symbols`) | a part regenerates from an input outside its document; an edit there dirties every reader; two machines agree from a shared text input | the mental model: derived geometry is one more external input, resolved before the pass |
| Import feature (`SeedGeometry`, baked bodies) | a feature whose geometry comes from elsewhere, re-read on demand, baked so the first regen is a load | the snapshot: same sidecar, same load path |
| baked geometry (`geometry/`, chain hash, element map on disk) | a stored B-Rep is checkable and its names survive a load that executed nothing | the snapshot's storage and its element names |
| A4 propagation (`{partId → Version()}`, `SubtreeVersions`) | staleness by version stamp, reported not auto-resolved | "source moved since this was taken" |
| A5 `WouldCycle` | refusing a reference that would loop, at creation | a part deriving from a part that derives from it |
| in-context edit (`InContextEdit`, ghosted neighbours at `OccMatrix(edited)⁻¹·OccMatrix(other)`) | the neighbour's placement relative to the part being edited | where a derived copy lands when made from an in-context edit |
| linked sketch copies (`sourceSketchId`, "Break link") | a linked copy's UX: a tint, a break-link verb, a stale marker | the tree row and its verbs |
| ElementMap naming (`extrude42/side/entity17`, suffixes on splits) | names composed from why an element exists | derived names: the source's name under the derived feature's id |

## The design

### The feature

```
Derived {
  source_part: <part slot id>        // the document it copies from
  bodies?: [<created_by ids>]        // which of its bodies; absent = all
  placement?: {position, orientation} // where the copy sits in THIS part;
                                      // identity unless made in context
  reference: true                    // default: a ghost, not this part
  snapshot: geometry/<n>.brep + element map + source_version + source_chain_hash
}
```

- **Execute** loads the snapshot. It never resolves the source document, so
  regeneration of a part with a Derived feature is a pure function of that
  part's own file — determinism, checkpoints, rollback, merge and the golden
  corpus see one more feature with data in it, which the format already
  handles for Import and for baked bodies.
- **Update** (`update_derived {feature}`, a tree verb, and "update all" on
  the part) resolves the source through `PartDocById`, takes its current
  bodies (regenerated, at the source's own tip), applies the placement,
  replaces the snapshot, records the source version and chain hash. It is a
  command, so it undoes, with the previous snapshot in the undo entry.
- **Staleness** is the source's `Version()` differing from the snapshot's,
  checked whenever both documents are loaded: a warning on the feature, the
  tree badge the drawings use, `stale: true` in `get_state`, and a line in
  `check_geometry`. Never auto-updated (the FEM rule).
- **`reference: true`** bodies are drawn ghosted like an in-context
  neighbour, excluded from mass properties, the BOM, export and CAM, and
  cannot be booleaned into. They exist to be **drawn on**: sketch-on-face,
  external geometry, hole centres, a mate connector. `reference: false`
  makes the copy an ordinary body of this part — the way to cut a part with
  another part as the tool (a mould from its casting), which the assembly
  plan lists as "assembly-level features" and which this covers for the
  one-way case.
- **Names.** A derived body's elements are the source's names under the
  derived feature's id: `<derivedId>/<source created_by>/<source name>`.
  They survive an update exactly when the source's own names do, which is
  the toponaming contract applied one document over; a reference that no
  longer resolves after an update is a feature error naming the element,
  as it is today within a part.

### Where a copy lands

From a part tab, a Derived copy sits at `placement` (identity by default,
editable). From an **in-context edit**, "derive this neighbour" takes the
occurrence under the cursor and sets the placement to
`OccMatrix(edited)⁻¹ · OccMatrix(neighbour)` — the copy appears exactly
where the ghost is, and stays there when the assembly is later re-solved,
because it is a copy. A stale placement (the assembly moved the neighbour)
is reported the same way a stale snapshot is, by comparing against the
assembly's current solve when that document is loaded.

### Refusals, in words

- deriving from this part, or from a part whose derived chain reaches this
  part (`WouldCycle` over derived edges);
- from a part that is not in the project (name the ones that are);
- from an assembly occurrence without a part inside it (a sub-assembly has
  no bodies of its own; name `inner_path`, as mates do);
- `bodies` naming a `created_by` the source does not have.

### Explicitly not in this plan

- **Live update** (auto-update when the source's version moves, in
  dependency order). It is the one thing that would add ordering back. The
  tools exist (`SubtreeVersions`, `WouldCycle`), and the stale badge plus
  "update all" is the honest 90%. Revisit when a real part has more than a
  handful of derived features and updating by hand is what people complain
  about — a complaint that has not been made yet.
- Deriving **sketches** (cross-part linked sketch copies, copy-links phase
  2). A derived body's faces give a sketch everything a linked sketch would,
  through external geometry; the sketch-level link stays deferred.
- Two-way features (a hole through three parts at once). A Derived with
  `reference: false` covers the one-way case; the two-way case is the
  assembly-level feature the assemblies plan defers.

## Phases

| phase | what | cost |
|---|---|---|
| **D1** core — **shipped** | `DerivedFeature`: snapshot load in `Execute`, `Update` against `PartDocById`, staleness by version + chain hash, names, `reference` flag honoured by mass/BOM/export/CAM/booleans; serialization v84 with the snapshot in `geometry/`; refusals | 1 session |
| **D2** surface — **shipped** | `add_feature {type: Derived}` / `update_derived` / `get_state.stale`; the tree row (tint, badge, Update, Break link → an Import-like frozen body); "derive this neighbour" from an in-context edit; the `defaults/*` gate for the tool | 1 session |
| **D3** proof — **shipped** | sketch-on-derived-face, external geometry from a derived edge, a hole pattern from derived circles, a mate connector on a derived face — each a test, none new code if D1's names are right | ½ session |
| **D4** acceptance | **the SO-101 gripper**: import the arm's STEP, derive the wrist servo into a new part, design the gripper body and moving jaw on its faces, derive the Pi Camera 3 STEP into the jaw and mount it, mates, interference and contact reports, a drawing. The dogfood round the roadmap keeps asking for, on the part this feature exists for | 1 session |

## Gates

- `test_derived.cpp`: a part derived from a box regenerates from its
  snapshot with the source document absent; update after the source changes
  replaces the geometry and clears the stale flag; the names on the copy are
  the source's under the derived id and a sketch-on-face reference survives
  an update that keeps the face; a cycle is refused; `reference` bodies are
  absent from mass and BOM.
- Golden `v84-derived-servo.json` with its `geometry/` snapshot — loads and
  regenerates with **no source part present**, which is the property that
  makes the corpus unit stay a document.
- T5's determinism property, unchanged: the snapshot is part of the input.
- `rpc_params_gate` for the new verbs; `regen_honesty_smoke` for an update
  that leaves a downstream feature broken.
- The gripper: a screenshot per part in the dogfood record, the contact
  report clash-free at the end, and the list of what broke on the way.

## As built (2026-09-03)

What shipped is the design above with these differences, each for a reason
found in the tree:

- **The snapshot is a document asset, not a `geometry/` file.** `geometry/`
  is the baked-geometry accelerator: keyed by the chain hash, ignored when
  stale, and deleted outright when the per-project bake setting is turned
  off. A snapshot is truth, not an accelerator, so it lives where the v49
  reference-image bytes do — `Document`'s asset store, `assets/derived-
  <content hash>.brep` in the directory form, base64 under `assets` in the
  single file. Content-addressed, so two features deriving the same servo
  share one blob, and an update leaves the previous blob in the store for
  undo. The single-file loader had to decode assets BEFORE its one
  regeneration; it decoded them after.
- **The stale stamp is the source's `TipChainHash`, not `Version()`.**
  `Version()` restarts at 0 on every load, so it cannot survive a session.
  The chain hash is what the prefix cache and the bake already key on, and
  it strips appearance, so recolouring the source is not "moved".
- **Names are `<derived id>/<source created_by>/<source name>`** exactly
  as planned; `test_derived` pins a sketch on a placed derived face following
  the face through an update that changes the source's height.
- **`Body::reference`** is the one flag every consumer reads. Skipped by:
  `MassOfBodies` (mass, BOM), STEP/STL/BREP export (refused in words when
  only ghosts remain), CAM job stock, the FEM mesh plan, `ApplyBodyMode`
  (Add never fuses into a ghost — with only ghosts the first solid becomes
  the part's own body; Cut passes through), Boolean operands (refused
  naming `reference: false`), the assembly's placed bodies (fastener check,
  interference, contact, section caps, fit, visual export) and the assembly
  render cache (in the assembly the real part is at its own occurrence),
  and baked geometry carries it so a reloaded jaw does not weigh its servo.
  A source's own ghosts are never derived further ("a ghost of a ghost").
- **Drawn as an in-context neighbour is**: ghost tint, alpha 0.35, no
  outline, still pickable. In the part tab only.
- **Verbs.** RPC: `add_feature {type: Derived, source_part, bodies?,
  placement?, reference?}` (the snapshot is taken in the handler, through
  the project), `update_derived {feature_id | index | all: true}`,
  `derive_occurrence {occurrence, name?, reference?, bodies?}` (in an
  in-context edit only; placement = `OccMatrix(edited)⁻¹ · OccMatrix
  (neighbour)`), `get_state` rows carry `stale: true | false | null` (null =
  source not loaded: unknown, never reported as fresh), `check_geometry`
  adds `stale_derived` when there is something to say. GUI: `###tb_derive`
  opens a preview whose window lists the candidates as buttons (a combo
  cannot be gated) — one candidate is taken at once, several ask; the tree
  row is tinted blue, amber when stale, with **Update from source** and
  **Break link** (the source is forgotten: a frozen copy, never stale) in
  its context menu; the editor has the same list, a Reference checkbox and
  an Update button. No placement editor in the GUI: placement comes from the
  neighbour or the RPC, which is where a real placement comes from.
- **A general fix found on the way**: the tool window's Accept button only
  disabled on unfilled pick SLOTS; a feature whose `WhyIncomplete` said no
  was refused by `AcceptPendingFeature` in silence. Accept now disables and
  says why, for every tool.
- **Gates**: `test_derived.cpp` (9 cases), golden `v84-derived-servo`
  (loads with no source part), `derived_smoke.py` (the RPC surface end to
  end, headless: refusals, stale, update + undo, the in-context placement,
  save/open), GUI `defaults/derive` (refuses until a source exists),
  `derived/tool_copies_the_other_part_as_a_ghost` (ghost instance, weightless,
  stale badge, Update verb, one undo step), `derived/source_list_takes_the_
  snapshot`.

- **D3 held**: external geometry projects a derived face (its four edges,
  where the placement put them) and a derived edge; a hole at a centre read
  off the derived boss drills the part's own body and not the ghost; a mate
  connector on a derived face resolves in the assembly at the copy's face.
  Three tests, zero new code — D1's names were right. The one thing learned:
  a vertical edge projects to a point, so a test has to pick a top edge by
  its midpoint, not the first name in the map.

**D4 done** (2026-09-03): [derived-gripper-dogfood.md](derived-gripper-dogfood.md) — the gripper built on ghosts of the real servo and camera STEP, every hole from a projected ghost edge, 19 findings, 8 fixed the same day.

