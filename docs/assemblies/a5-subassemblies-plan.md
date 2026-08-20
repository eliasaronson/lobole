# A5 — sub-assemblies (assemblies in assemblies) — plan

> Status: **A5 COMPLETE 2026-08-20** — A5.1-A5.4 on the 19th (4ce026a, 7d01be6, 4485c45,
> 5278440). What remains is A5.5, the dock restructure, which is an
> acceptance exercise on a real model rather than code — and the deferrals
> listed at the bottom. Reviewed 2026-08-19 against the code (the walker
> inventory below was checked with grep, not memory) and extended: six more
> geometry walkers, one mechanism promoted out of "risks" because it does not
> exist yet, and the three open design questions answered. Written 2026-08-18,
> triggered by
> the Marsliden dock: Elias wants a "Gangway" assembly and a "Dock"
> assembly with their own BOMs, composed into one top-level assembly
> (plus the ladder) with a combined BOM. Today an occurrence can ONLY
> reference a part — `PartIdByName`/`PartDocById` both filter
> `kind == DocKind::Part` (AppActions.cpp:1131/1902) — so this is a new
> workstream, the largest single feature since the V validation round.

## Why

Structure and scoped BOMs. A dock, its gangway, and a bought ladder are
three units with independent build lists; today they must live flat in
one assembly, so there is exactly one BOM and no unit boundaries. With
sub-assemblies the per-unit BOM is free — open that assembly's tab —
and the top-level BOM aggregates the whole tree. The same boundary pays
off in the contact gate: "is the gangway attached to the dock" becomes a
question about ONE node, not thirty.

## Design

**v1 = RIGID sub-assemblies.** A sub-assembly occurrence is one placed,
solid unit. Its internal occurrence transforms are whatever its own
document stores (the A3 contract: a file re-solves to identical
placements, so flatten never triggers a solve). Mates, exploded views,
and the drag gizmo keep operating between TOP-LEVEL occurrences only.

**Reference model.** `Occurrence::partId` already stores a PartSlot
stable id, and assemblies are slots too — the field's SHAPE does not
change; its resolution WIDENS. New serialized field
`ref_kind: "part" | "assembly"` (loader defaults `"part"`, so every
existing file reads unchanged) — explicit, rather than inferring from
whatever the id happens to resolve to, so a dangling reference still
knows what it was. Resolution helpers gain assembly-aware variants
(`SlotDocById` returning kind, or `AssemblyDocById` beside
`PartDocById`); `add_occurrence part="Gangway"` resolves parts first,
then assemblies, and its not-found error lists BOTH vocabularies
(error-style.md rules apply).

**Core: one flatten, every walker ports to it.**

```cpp
// core/AssemblySolve.h (beside PartResolver)
struct FlatInstance {
  uint64_t partId;         // always a PART at the leaves
  const Document* part;
  glm::mat4 world;         // composed down the tree
  std::string path;        // "Gangway/Board 03" — display + addressing
  uint64_t rootOccurrenceId; // the TOP-LEVEL occurrence this belongs to
};
using AssemblyResolver =
    std::function<const AssemblyDocument*(uint64_t slotId)>;
std::expected<std::vector<FlatInstance>, std::string>
Flatten(const AssemblyDocument& top, const PartResolver& partById,
        const AssemblyResolver& asmById, int maxDepth = 8);
```

Cycle guard: the recursion carries the set of assembly slot ids on the
current path; revisiting one fails with the cycle spelled out
("Gangway -> Dock -> Gangway"). maxDepth is the runaway backstop.

**Walker ports** (the bulk of the work — every place that loops
`GetOccurrences()` + `PartDocById` + `OccurrenceMatrix` for GEOMETRY
moves onto `Flatten`):

1. Render sync — AssemblyActions.cpp SharedBody cache. Instancing
   survives: FlatInstance keeps partId, so six barrels in two nested
   assemblies still share one mesh asset.
2. BOM — ComputeBom (AssemblyActions.cpp:166): aggregate by partId
   across the flat list; a sub-assembly placed twice doubles its parts'
   quantities. Per-unit BOM = that assembly's own tab, unchanged code.
3. Mass — assembly::MassProperties (AssemblySolve.cpp).
4. contact_report / interference / compare_geometry occurrence paths
   (RemoteInspection.cpp): each TOP-LEVEL occurrence's body set is its
   flattened subtree, so a sub-assembly is one contact node — the
   desired semantics for "is the gangway attached".
5. render_sheet footer walk (RemoteServer.cpp BuildSheetFooter).
6. export_visual / STEP-STL assembly export (RemoteProject/AppActions).
7. Assembly drawing views (HLR walk in core/drawing).
8. GUI occurrence panel (OccurrencePanel.cpp): a sub-assembly occurrence
   renders as ONE row with a kind tag and an "Open" that activates its
   tab; no tree expansion in v1.
9. **Offline path-traced render** — `SyncRayTracingScene`
   (AppActions.cpp:2982) walks occurrences itself; a nested assembly would
   otherwise ray-trace only its top-level parts. The assembly Render button
   is recent, so this is user-visible from day one.
10. **In-context ghosts** — `SyncInContextGhosts` (AssemblyActions.cpp:1138,
    1153, 1167). Editing INTO a sub-assembly stays a non-goal, but whether a
    sub-assembly's CONTENTS appear as ghosts around the part you are editing
    is a separate question, and the answer is yes: ghosts come from the
    flattened tree, so the context looks like the assembly does.
11. **Explode** — `AutoExplode` (:889) and `ApplyExplodeTransforms` (:940).
    "Exploded views operate between top-level occurrences only" hides two
    requirements: a top-level occurrence's explode offset must move its WHOLE
    subtree, and AutoExplode's spacing math needs subtree bounds rather than
    per-part bounds.
12. **Scene bounds** — `VisibleSceneBounds` (AppActions.cpp:2060), which
    backs camera fit and (since the section tool) the section's default
    offset and drag step. Bounding only top-level parts would put a new
    section's cut outside a nested model.
13. **Section caps** — `SyncAssemblyRenderers` transforms each cut plane into
    an occurrence's part space; with nesting it needs `FlatInstance::world`,
    not `OccurrenceMatrix`. (Postdates the original plan.)

Non-goals for the walkers: configurations reaching INTO a sub-assembly
(a top config can suppress the whole sub-assembly occurrence, nothing
finer); connector face-anchors on sub-assembly occurrences (origin
anchor only in v1 — face anchors refuse with a decision-complete error
naming the restriction); in-context editing INTO a sub-assembly (open
its own tab instead — already works).

**Staleness must be built, not ported** (was a "risk", is a mechanism).
`AssemblyPartsStale` (AssemblyActions.cpp:152) stamps and compares the
versions of referenced PARTS through `PartDocById`, which returns null for an
assembly-kind reference — so it `continue`s, never reports stale, and the
re-solve never runs. `SyncDocumentRenderers` then gates the whole resync on
`state.assembly->Version()` (AppActions.cpp:2800), and nothing bumps the top
assembly's version when a SUB-assembly's document changes. Left alone, editing
a part inside the Gangway leaves the top tab showing yesterday's geometry —
correct-looking output, quietly wrong, the same failure class as the section
cutter aimed at the origin. A5.1 therefore stamps versions TRANSITIVELY: for
each occurrence, the referenced document's own version plus, for an assembly,
every version in its subtree.

**Guards.** `delete_part` ("refused while referenced by an assembly")
extends to assemblies referenced by other assemblies — same check, both
kinds. Adding an occurrence that would create a cycle is refused at
add_occurrence time by a dry-run Flatten, not discovered later at
render time.

**Format: v55.** `ref_kind` on occurrences + kVersion bump. **No deviation
after all** (the draft reserved one): `ref_kind` is a field on an occurrence,
so it IS expressible in one document — the reference not resolving inside a
lone file does not matter, because a golden pins the SCHEMA, not the
resolution. `tests/golden/assembly/v55-subassembly.json` is the classic frozen
golden, in the assembly subdirectory the harness already has, plus a companion
test asserting a pre-v55 golden still reads as part references (the
compatibility claim, checked rather than asserted). The multi-document
behaviour — save, reopen, flatten, BOM — is the nested smoke's job in A5.4;
that is coverage the golden cannot give, not a replacement for it.

## Phases

- **A5.1 core — SHIPPED**: ref_kind + serialization, resolution helpers, Flatten
  with cycle/depth guards, kVersion 55. Headless-testable alone.
- **A5.2 walker ports — SHIPPED** (all thirteen, including the six the
  review added): the eight sites above, each verified as it
  lands (render by screenshot, the rest by smoke assertions).
- **A5.3 surface — SHIPPED**: add_occurrence assembly resolution + candidate
  lists in errors, list_occurrences/get_state carry ref_kind and path,
  delete/cycle guards, describe rows, GUI row + Open.
- **A5.4 tests — SHIPPED** (tests/nested_assembly_smoke.py + five Flatten
  unit tests + two goldens; the fixture directory proved unnecessary once the
  golden turned out to be expressible): nested_assembly_smoke (build sub + top, combined BOM
  quantities, contact through a sub-assembly, cycle refusal, dangling
  ref honesty, save/reload) + the nested-dock fixture.
- **A5.5 acceptance — DONE 2026-08-20** as a new script and a new project
  (`scratch/restructure_dock_nested.py` -> `scratch/marsliden-dock.json`),
  leaving build_dock.py and floating-dock.json untouched. Written as a
  RESTRUCTURING of the flat project rather than a second copy of the build
  script, so equivalence is provable rather than hoped for: every part
  document and world placement comes from the flat source.
  **Results:** Dock 49 parts / 261.0 kg, Gangway 24 / 82.0 kg (in its OWN
  coordinates, occurrence at x=-2410, so the composed transform is what puts
  it back), top 76 items / 385.2 kg — identical to the flat build to the
  gram. Both hinges touch BOTH units; Dock vs Gangway clear at exactly
  10.0 mm, the articulation gap, through the composition. It found the last
  two unported walkers (contact_report, named-pair interference).
  **It also surfaced something pre-existing:** the flat project carries six
  sub-0.14 mm³ overlaps at lap joints (End right/Post right 1 and 2, three
  mooring/frame pairs, End right/Ladder). They are NOT from the restructure —
  the flat source reports the identical set — so the script asserts
  equivalence with the source instead of an absolute zero the source never
  met. Worth a look: build_dock.py's own `clashes == []` gate would fail on
  the saved project today.

  The original A5.5 sketch (edit build_dock.py in place): `build_dock.py` splits
  into "Dock" (frame/deck/barrels/cradle/moorings) and "Gangway"
  (stringers/noggins/boards/roller carriage) assemblies composed in a
  top-level "Marsliden dock" with the ladder and the two HINGES — the
  hinges deliberately live in the TOP assembly because they span the
  Dock/Gangway boundary, which makes them the cross-boundary contact
  test. Every existing gate must stay green: touching list, clearances,
  one component, 0 clashes/near-misses, blue barrels, render_sheet
  clean. Plus new asserts: Dock-tab BOM and Gangway-tab BOM row counts,
  and the top BOM equal to their sum plus ladder + hinges.

## The three questions the first draft left open

**Which configuration does a placed sub-assembly use? Its own active one.**
A sub-assembly is a live reference, exactly like a part: the top shows
whatever that document currently is. The consequence has to be said out loud
because it is surprising — flipping a configuration in the Gangway tab
silently changes the top assembly's geometry AND its BOM. Per-occurrence
configuration selection stays deferred (it already was, one level down, in
C0-C4); when it lands it answers both levels at once.

**Interference and contact treat a subtree as ONE node.** That is the point
for "is the gangway attached to the dock", and the cost is that a clash
BETWEEN two parts inside the Gangway is invisible from the top tab. Deliberate
— and the report says where to look instead, rather than leaving the user to
conclude the model is clean.

**Drawing dimensions attach to the sub-assembly, not to its insides.**
Dimension refs are occurrence-qualified (`DrawingDocument.h:70`) and picked
geometry is keyed by occurrence id (`DrawingRender.h:30`); a leaf inside a
subtree has an id that lives in the SUB's document, which the top's refs
cannot resolve. So v1 projects the flattened geometry but stamps every
projected body with its TOP-LEVEL occurrence id: a sub-assembly dimensions as
one unit, and no ref can dangle. Dimensioning an inner part means opening
that assembly's own drawing — or the path key, deferred with the rest of
inner addressing.

## Risks / open questions

- The render-sync port is the risky one (instancing cache, outline
  renderer, face picking all hang off the occurrence walk) — do it
  early in A5.2 with GUI tests + screenshots, not last.
- Occurrence-id-keyed features (exploded steps, mates, selection)
  continue to see top-level ids only — audit for places that assume
  every id resolves to a part doc.
- `tools/designer_mcp.py` carries its own copy of the RPC surface and
  `mcp_smoke` cross-checks it against `describe`, so ref_kind/path land there
  too or that test fails.
- Solve on the active assembly must NOT recurse into subs (rigid), but
  a stale sub (edited after the top was saved) renders with its NEW
  solved state — that is the live-reference behavior parts already
  have, so it is consistent, just worth a doc sentence.
- Path addressing ("Gangway/Board 03") is DISPLAY-only in v1; RPCs
  address top-level occurrences. If inner addressing is ever needed,
  the path string is the natural key — deferred.
