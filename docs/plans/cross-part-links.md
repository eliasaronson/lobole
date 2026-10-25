# Cross-part links: sketches, and copies that keep up

## The problem

A `Derived` feature copies another part's bodies into this one
([derived](../modeling/derived.md)), so part B is drawn on part A's faces.
Two things keep it from being the way multi-part design is done:

- **Only bodies cross.** A shared outline that exists only as a sketch in A
  -- a gasket, a lid's edge, a bolt circle drawn before either part has a
  solid -- cannot be linked from B. A linked paste across parts is meant to
  fall back to a plain copy that never follows A again, and does worse: the
  clipboard records the source sketch's feature id but not its part
  (`CopySketchSelection`, `src/ui/SketchTools.cpp`), and feature ids are
  per-part counters, so a linked paste into B binds to B's OWN sketch of that
  id when one exists earlier in B's history.
- **A copy keeps up only when someone presses Update.** After A changes, B's
  copy is stale -- an amber tree row, `stale: true` in `get_state`,
  `stale_derived` in `check_geometry` -- and B is built from the old A until
  someone notices. Every reader of B (the assembly, a drawing, a CAM job, an
  export, a requirement check) takes the old geometry without a word. And the
  staleness is not transitive: when C copies B and B copies A, C reads fresh
  while B is stale.

The freshness stamp is also fragile: `TipChainHash` is `std::hash` over each
source feature's JSON, so it differs between standard libraries, and a build
that adds a key to a feature's JSON marks every copy of it stale.

## The shape of the fix

### A Derived carries sketches

`Derived` gains `sketches` (source sketch ids; `sketch_names` as
`body_names` is to `bodies`). A copied sketch is a snapshot like a body: its
SOLVED entities, after the source's own `Execute` (expressions, constraints,
its external geometry), stored in the feature's snapshot with the sketch's
plane, in the source part's coordinates; `placement` applies to it as to a
body. Regeneration stays data-only: B is still a pure function of its own
file.

In B a copied sketch is reference geometry, never a profile on its own:

- its plane is a sketch plane;
- each entity is an edge named `<derivedId>/<srcSketchId>/<entityId>`, which
  a sketch in B projects as external geometry (`ExternalGeom`, the path a
  derived body's edges already take), solver-fixed and violet as a linked
  copy is. Entity ids are the source's own and stable, so a projection
  survives an update that keeps them.

The GUI: the Derive tool lists a part's sketches beside its bodies, and
**Sketch on it** opens a sketch on a copied sketch's plane with every entity
projected. A linked paste from another part makes (or reuses) a `Derived` of
that sketch and projects the pasted entities, so it follows A. The clipboard
records the source's part, which ends the id collision: a linked paste
resolves the source by part and feature, never by feature alone.

A cycle -- A's sketch copied into B while B's body is copied into A -- is
refused at creation and at update. `derived::WouldCycle` walks body and
sketch edges alike.

### A stable stamp

A copy records the CONTENT of what it copied: an `inputhash` key
(`src/core/InputHash.h`, the stamp requirements and studies use) over the
copied bodies' exact B-Rep and the copied sketches' solved entities, in place
of the source's chain hash. A copy is stale exactly when taking it again
would change what it holds, on any platform and across builds. The key is
cached per source `Version` in the session, so a check on an unchanged source
hashes nothing.

### Copies that keep up

A link has `follow` (default true). A following link is brought up to date
at fixed **gates**, never in the middle of an edit:

1. a part's tab is shown, and a project is opened;
2. before any RPC that reads geometry -- the dispatcher's existing flush
   before a non-`Deferrable` call (`RemoteServer::HandleSemantic`), which
   `batch` already relies on;
3. before a save, every export (STEP, STL, 3MF, a print job, a drawing's
   SVG/PDF, the post), a requirements check, a design sweep, and a CAM
   generate.

One function, `ui::RefreshLinks(state)`, does it for the whole project: it
orders the parts by their links (body and sketch edges), and updates each
part's stale following links in that order, so C sees B after B has taken A.
In the GUI the part being edited cannot change under the person editing
it -- only a script edits another part meanwhile -- and gate 1 runs when they
next show a tab.

**Nothing may miss a gate.** Every resolver that hands out a part for
another document to read (`SlotDoc`, `PartDocById`, the drawing and job
resolvers, folded onto those two) asserts, in debug and test builds, that the
part has no stale following link. The GUI suite and the smokes then fail any
path that reads a part without passing a gate, today's and tomorrow's.
`test_conventions.cpp` already fails a new part resolver that bypasses them.

**What an update is.** One command on the updated part's OWN stack
(`CommandsFor`, as a batch uses for a part that is not active): one undo step
per part per refresh, however many links it updated, and the part is dirty.
Undoing it leaves the link stale, and the next gate takes it again, so to
keep an older copy a person sets `follow` off (the tree's context menu and
the editor: **Follow source**) or breaks the link. A source that is absent,
or rolled back (its rollback bar set), is not taken: the link stays stale,
reported as today, with the reason.

**Reporting.** An update that breaks a feature in the part it updated is
applied, never held back, and said where it happened: the RPC that passed the
gate carries `links_updated: [{part, features, broken}]` beside its own
`regen`; the GUI shows one notice ("Bracket updated from Plate: Hole 2
failed"). `stale` becomes transitive -- a link whose source is itself stale
reads stale -- in `get_state`, `check_geometry` and the tree.

`update_derived {all: true}` becomes one undo step, and it stops listing the
links it skipped as updated.

## Not in this plan

- Feature templates keep their explicit **Update from template**; the gates
  could take them too, later, through the same function.
- A design sweep still rebuilds each part from its file and does not follow a
  swept variable through a link into a deriving part; its docs say so.

## Files

- `src/core/features/DerivedFeature.{h,cpp}`: sketches in the snapshot, the
  content key, `follow`, the cycle walk over both edge kinds.
- `src/core/features/SketchFeature.cpp`: projecting a copied sketch's
  entities.
- `src/ui/DerivedActions.{h,cpp}`: `RefreshLinks`, the order, the update as
  a command on the part's own stack.
- `src/ui/DocumentSlots.cpp`, `AssemblyActions.cpp`, `DrawingActions.cpp`,
  `JobActions.cpp`: the resolvers' assertion; the drawing and job resolvers
  folded onto `SlotDoc`/`PartDocById`.
- `src/remote/RemoteDispatch.cpp`: gate 2; `RemoteModeling.cpp`:
  `update_derived`, `links_updated`; the export and save paths: gate 3.
- `src/ui/SketchTools.cpp`, `AppState.h`: the clipboard's source part, the
  linked paste across parts.
- `src/ui/FeatureTreePanel.cpp`, `FeatureEditor.cpp`, the Derive tool:
  sketches, **Follow source**, the notice.
- `src/core/io/SerializeFeatures.cpp`: `sketches`, `sketch_names`,
  `follow`, the content key -- a format change (`vNEXT`), with a golden.
- `docs/modeling/derived.md`, `docs/sketching/README.md`,
  `docs/scripting/rpc.md`, the `describe` rows.

## Verification

- A sketch in A copied into B and projected into a sketch of B that is
  extruded: edit A's sketch, show B's tab, and B's extrude follows; undo in B
  leaves it stale and the next gate takes it again; with `follow` off it
  stays stale and says so.
- A chain A -> B -> C: an edit in A reaches C through one gate, B first.
- Over RPC: edit A, then `export_step` of B exports the updated B and the
  response lists the update; a broken downstream feature is in it.
- A linked paste from A into B whose own history holds a sketch of the same
  id links to A's sketch (fails on main).
- A cycle through a sketch link is refused at creation and at update.
- The freshness stamp is unchanged by a build that adds a JSON key to a
  source feature (a test feeds an extra key) and changed by an edit.
- The resolver assertion fires on a planted read past the gates; the GUI
  suite, the headless suite and `tools/ctest -L pygui` pass with it on.
- The golden for the new keys; `tools/ctest -L golden`, the reader-fuzz
  sample.
