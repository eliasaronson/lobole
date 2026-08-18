# A5 — sub-assemblies (assemblies in assemblies) — plan

> Status: **draft, awaiting sign-off.** Written 2026-08-18, triggered by
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

Non-goals for the walkers: configurations reaching INTO a sub-assembly
(a top config can suppress the whole sub-assembly occurrence, nothing
finer); connector face-anchors on sub-assembly occurrences (origin
anchor only in v1 — face anchors refuse with a decision-complete error
naming the restriction); in-context editing INTO a sub-assembly (open
its own tab instead — already works).

**Guards.** `delete_part` ("refused while referenced by an assembly")
extends to assemblies referenced by other assemblies — same check, both
kinds. Adding an occurrence that would create a cycle is refused at
add_occurrence time by a dry-run Flatten, not discovered later at
render time.

**Format: v55.** `ref_kind` on occurrences + kVersion bump. DEVIATION
from the golden-file policy, recorded here: the golden corpus is
single-document files, and a nested assembly is only meaningful as a
multi-doc project (the reference must resolve). So v55's regression
anchor is a PROJECT fixture — `tests/fixtures/nested-dock/` (small:
one part, one sub-assembly, one top) — exercised by the new smoke's
save -> reopen -> flatten -> BOM round-trip, instead of a
`tests/golden/v55-*.json` pair the harness cannot host. If a
single-doc-expressible v55 change lands later, it adds the classic
golden then.

## Phases

- **A5.1 core**: ref_kind + serialization, resolution helpers, Flatten
  with cycle/depth guards, kVersion 55. Headless-testable alone.
- **A5.2 walker ports**: the eight sites above, each verified as it
  lands (render by screenshot, the rest by smoke assertions).
- **A5.3 surface**: add_occurrence assembly resolution + candidate
  lists in errors, list_occurrences/get_state carry ref_kind and path,
  delete/cycle guards, describe rows, GUI row + Open.
- **A5.4 tests**: nested_assembly_smoke (build sub + top, combined BOM
  quantities, contact through a sub-assembly, cycle refusal, dangling
  ref honesty, save/reload) + the nested-dock fixture.
- **A5.5 acceptance — restructure the dock**: `build_dock.py` splits
  into "Dock" (frame/deck/barrels/cradle/moorings) and "Gangway"
  (stringers/noggins/boards/roller carriage) assemblies composed in a
  top-level "Marsliden dock" with the ladder and the two HINGES — the
  hinges deliberately live in the TOP assembly because they span the
  Dock/Gangway boundary, which makes them the cross-boundary contact
  test. Every existing gate must stay green: touching list, clearances,
  one component, 0 clashes/near-misses, blue barrels, render_sheet
  clean. Plus new asserts: Dock-tab BOM and Gangway-tab BOM row counts,
  and the top BOM equal to their sum plus ladder + hinges.

## Risks / open questions

- The render-sync port is the risky one (instancing cache, outline
  renderer, face picking all hang off the occurrence walk) — do it
  early in A5.2 with GUI tests + screenshots, not last.
- Occurrence-id-keyed features (exploded steps, mates, selection)
  continue to see top-level ids only — audit for places that assume
  every id resolves to a part doc.
- Solve on the active assembly must NOT recurse into subs (rigid), but
  a stale sub (edited after the top was saved) renders with its NEW
  solved state — that is the live-reference behavior parts already
  have, so it is consistent, just worth a doc sentence.
- Path addressing ("Gangway/Board 03") is DISPLAY-only in v1; RPCs
  address top-level occurrences. If inner addressing is ever needed,
  the path string is the natural key — deferred.
