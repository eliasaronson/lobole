# Configurations — parametric part/assembly families — design

> Status: **SHIPPED (C0–C4, 2026-07-14)** — v1 complete. Part configs
> (override-regen + feature suppression) and assembly configs (occurrence
> suppression), format v31, RPC/MCP (in `remote/RemoteConfigurations.cpp`) +
> GUI panels. The one planned follow-on is per-occurrence part-config
> selection (§7). This doc is the design + as-built record. Deferred from
> assemblies A4 as "their own design"
> ([a4-design.md](../assemblies/a4-design.md):38); the roadmap's next
> workstream. Verdict: build it — the substrate is already here. A
> configuration is a *doc-level named object* (the FEM Study pattern, verbatim),
> its effect is *regenerate against an override symbol table + a suppression
> set* (the document already evaluates every feature field against
> `m_symbols` and the regen prefix-cache already hashes the evaluated values,
> so invalidation is free), and per-feature **suppression already exists**
> (`Feature::IsSuppressed`, v19). The honest cost is a mid-size workstream —
> smaller than drawings/FEM because there is no new geometry kernel and no new
> document kind, with the one genuine unknown (does override-regen compose
> correctly with suppression and the cache?) settled by a one-day spike.

## Why

Configurations are the last major missing *capability* — a table of named
variants of one part or assembly (the 6 sizes of a bracket, the metric and
imperial build, the "with-boss / without-boss" options), each a set of
parameter overrides and suppressed features, switchable in place. Every
mainstream CAD has them; this app does not.

It is also unusually on-brand. The project's differentiators are variables +
expressions + scripting + LLM-parity, all shipped; configurations are those
composed — "give me this bracket in M4/M5/M6, export each as a STEP" is one RPC
chain no mainstream CAD exposes headlessly. And because a configuration is
*just an override map applied during regeneration*, it is pure headless core:
CI-testable without a GPU, per the headless-core rule.

## What v1 is — and is not

**v1: part configurations first (named override sets = parameter values +
feature suppression, an active-configuration selector, serialization, GUI
panel, RPC/MCP), then assembly configurations (C4) — both committed scope.**
The part machine ships and gets used before the assembly phase builds on it;
they are sequenced, not one deferred.

- A **configuration** is a doc-level named object on a part `Document`
  (`Document.h:83-129`, the Study storage pattern): an id, a name, a set of
  **parameter overrides** (`name → expression`), and a set of **suppressed
  feature ids**. It is *not* a Feature and does not participate in the feature
  list — it is a sibling to the parameter table, exactly as a Study is.
- The document has an **active configuration** (persisted, like the rollback
  bar). Regeneration applies the active config's overrides; `id 0` = the
  **base** (the model as authored, no overrides), always present and valid.
- **Parameter overrides are expressions, not just literals** — a config may say
  `depth = width * 3`, reusing the exact `{name, expression}` shape parameters
  already serialize (`Serialization.cpp:1166`).
- **Feature suppression reuses the shipped flag** (`Feature::SetSuppressed`,
  honored at `Document.cpp:133`, serialized at `Serialization.cpp:378`).
  Effective suppression during regen = the feature's authored `m_suppressed`
  **OR** the active config suppresses it.
- **Surface**: a Configurations section in the Model dock (the StudyPanel
  list+add+select shape), an active-config selector, and
  `add_configuration` / `remove_configuration` / `list_configurations` /
  `set_configuration` (overrides + suppression) / `set_active_configuration`
  RPCs + MCP mirrors. Export/drawing/FEM naturally reflect the active config
  because they read the regenerated bodies.

**In scope, sequenced last (C4):**

- **Assembly configurations** — per-occurrence config selection (an occurrence
  shows a chosen config of its part) + occurrence suppression. `Occurrence`
  gains a config selector, and resolving an occurrence regenerates its part
  *at that config*. Occurrence suppression was always "arrives with
  configurations" (a4-design.md:107). Built on the finished part machine (§7),
  so it lands last but is committed, not deferred.

**Explicitly deferred (decisions, not gaps):**

- **Configured properties beyond parameters + suppression** — per-config
  material, per-config custom properties / part number, per-config
  dimensions that aren't already global parameters. v1 configures the two
  things that cover the 90% case (a value and a presence); the rest is
  additive later.
- **Configuration tables / design tables** (a spreadsheet driving many configs
  at once, à la SolidWorks) — v1 edits configs one at a time; a bulk
  table importer is a later convenience over the same model.
- **Implicit "edit the active config"** — in v1, editing a parameter with
  `set_variable` always edits the **base** expression; a configuration's
  overrides are edited only through the explicit configuration surface. This
  keeps "did I just change the base or this variant?" unambiguous. SolidWorks'
  configured-dimension behavior (an edit targets the active config) is a
  future enhancement, noted as decision 6.
- **Auto-generated "Default" snapshotting** — no magic first config that
  captures current values; base (id 0) *is* the authored model.

## 1. The model — a doc-level object (the Study pattern)

A configuration copies `fem::Study`'s shape and lifecycle verbatim
(`Document.h:83-129`, `Document.cpp:165-213`):

```cpp
// core/Configuration.h
struct Configuration {
  uint64_t id = 0;      // doc-unique, from Document::AllocateId(); never reused
  std::string name;
  // Parameter value overrides: parameter name -> expression (may reference
  // other parameters). Empty = no override for that parameter.
  std::vector<std::pair<std::string, std::string>> parameterOverrides;
  // Feature ids this configuration additionally suppresses (composed with the
  // feature's own authored m_suppressed).
  std::vector<uint64_t> suppressedFeatures;
};
```

Stored on `Document`, mirroring studies (`Document.h:183`):

```cpp
std::vector<Configuration> m_configurations;
uint64_t m_activeConfiguration = 0;   // 0 = base; persisted like m_rollback
```

with the Study accessor set (`GetConfigurations` / `FindConfiguration` /
`AddConfiguration(name)` returning a fresh-id `Configuration&` /
`RemoveConfiguration` / `RestoreConfiguration` for undo /
`ReplaceConfiguration` for modify — the `Document.cpp:165-213` template). Ids
come from the shared `AllocateId()` / `m_nextId` pool
(`Document.h:133,189`) so they never collide with feature or study ids.

**Not a new `DocKind`.** A configuration is edited by a panel/section within
the existing part tab, exactly as studies are a mode-gated panel and
occurrences are an assembly-tab panel (`UI.cpp:112-143`). No enum member, no
tab-bar button, no stash/activate plumbing — a fraction of the drawings/A2
touch list.

## 2. Application: regenerate against overrides (the core mechanic — C0 gate)

> **C0 gate: PASSED (2026-07-14).** Shipped `core/Configuration.h` + the
> `Document` integration (override overlay in `EvaluateParameters`,
> `EffectiveSuppressed` in the regen gate + folded into the prefix-cache key,
> active-config accessors) + `tests/test_configuration.cpp`, all green in the
> 378-test suite. Proven: (1) a config overrides a parameter and restores on
> switch-back with the base expression intact (no stored-state mutation);
> (2) an override to `width` propagates to `depth = width*2` (overrides
> replace the base *expression* for the regen, so dependents follow);
> (3) config suppression composes with the authored `suppressed` flag;
> (4) a param-override switch re-executes only the affected cache suffix, and
> a suppression-only switch still invalidates via the folded effective-
> suppressed bit. No design changes — proceed to C1.


The one design-critical decision, and the C0 spike. The document already:

1. evaluates the parameter table into `m_symbols` at the top of every regen
   (`Document.cpp:68`, `EvaluateParameters` at `307-383`);
2. refreshes every expression-driven **feature field** against `m_symbols`
   *before* hashing (`Document.cpp:88-99`);
3. chain-hashes each feature's **serialized, evaluated** JSON as the prefix
   cache key (`Document.cpp:106-112`), so a changed value moves the cache
   divergence point back and re-executes exactly the affected suffix.

So a configuration needs only to **inject its overrides into that existing
flow**, with no new geometry path and no cache work:

- **Parameter overrides** → after the normal topo-evaluation, force
  `m_symbols[name] = eval(overrideExpr)` for each overridden name (evaluating
  the override expression against the same symbol table, so `depth = width*3`
  resolves). Because feature fields are then refreshed from `m_symbols`
  (step 2) and the cache keys on the result (step 3), the geometry updates and
  the cache invalidates **for free**. Implementation: `EvaluateParameters()`
  consults `m_configurations[active]` (or takes the active config's override
  list) as a final overlay pass — **no stored `Parameter` is mutated**, which
  matters because `Document` is move-only and cannot be cheaply copied/restored
  (survey: no `Clone`, undo is Command/JSON-based).
- **Feature suppression** → regen's `if (!m_features[i]->IsSuppressed())` gate
  (`Document.cpp:133`) becomes `if (!EffectiveSuppressed(i))`, where
  `EffectiveSuppressed = feature.m_suppressed || activeConfig.suppresses(id)`.
  The feature's own flag is untouched; the config layer composes on top. This
  also keys the cache correctly (the effective-suppressed state is part of what
  regen produces), but note the current hash is over `FeatureToJson` which
  encodes only `m_suppressed` — **the cache key must additionally fold in the
  active config id (or the effective-suppressed bit)** so switching configs
  invalidates. This is the one real cache subtlety and a C0 assertion.

**C0 — the spike gate (headless, one focused day):** a `test_configuration.cpp`
that (a) builds a part whose extrude distance is `depth` and defines a config
overriding `depth`; applies it; asserts the regenerated volume matches and the
prefix cache re-executed only the affected suffix; (b) defines a config that
suppresses a feature; asserts the body count drops and composes with an
already-authored-suppressed feature; (c) switches active config back and forth
and asserts geometry + cache stay correct (the cache-key-includes-active-config
assertion). **No further configurations work until C0 passes** — it settles
whether the override-overlay + suppression-compose + cache-key story holds
across feature types. Everything downstream (serialization, RPC, UI) is
mechanical once it does.

## 3. Serialization — v31, additive

A configuration persists like a study (`Serialization.cpp:1057-1116` shape,
`1306-1410` dir layout): tagged JSON, only-non-empty fields, **absent key ⇒
empty ⇒ a pre-v31 document re-saves byte-identically** (the v27/v29
discipline).

```jsonc
{
  "format": "designer-document", "version": 31,
  "parameters": [ { "name": "depth", "expression": "3" } ],
  "features": [ /* ... */ ],
  "active_configuration": 2,                 // omitted when 0 (base)
  "configurations": [
    { "id": 1, "name": "Default" },
    { "id": 2, "name": "Tall",
      "parameter_overrides": [ { "name": "depth", "expression": "12" } ],
      "suppressed_features": [ 7 ] }
  ]
}
```

- **Directory (.dsgn)**: file-per-config under `configurations/<id>-<slug>.json`
  with an ordered id list in `index.json`, plus `active_configuration` in the
  index envelope — the `studies/` precedent (`Serialization.cpp:1306-1323`) so
  each config is a conflict-free git unit.
- **Bump to v31**, one migration + one frozen `v30-*` golden that must still
  load and round-trip identically + a v31 golden exercising a config, per the
  migration policy (`docs/versioning/format.md:58-72`, `test_golden.cpp`).
  `Configuration`-to/from-JSON helpers mirror `StudyToJson`/`StudyFromJson`.
- References are by **feature id / parameter name**, never index (guarantee 1);
  a config's suppressed-features list and override map survive feature
  reordering.

## 4. Undo — Command-based

Configuration edits are undoable Commands with the before/after-JSON
idempotent shape (`Command.cpp:131-184`, the Study command trio):
`AddConfigurationCommand` (reuses its id on redo via `RestoreConfiguration`),
`RemoveConfigurationCommand`, `ModifyConfigurationCommand(before, after)`, and
`SetActiveConfigurationCommand` (stores the old + new active id, regenerates).
The GUI edit helper mirrors `ModifyStudy` (`StudyPanel.cpp:26-37`): snapshot
`ConfigurationToJson`, mutate a copy, execute the modify command.

## 5. UI — a Configurations section in the Model dock

Part-tab panel (`ImGui::Begin("Model")`, the StudyPanel shell), no new
DocKind:

- An **active-configuration selector** at the top (a combo: "Base" + each
  config), the `activeExplodedView`/`activeStudyId` transient-selection idiom
  — except the active config is *persisted* on the document, so switching it is
  a `SetActiveConfigurationCommand` that regenerates and resyncs the viewport.
- A **configuration list** with add / select / rename / delete — the
  `StudyPanel.cpp:48-66` list+add+select shape verbatim.
- The **selected config's editor**: a parameter-override table (each global
  parameter with an optional override expression + its resolved value) and a
  feature-suppression list (checkboxes over the feature tree, or a compact
  id list). Editing routes through the modify command.
- The feature tree dims features suppressed by the active config, reusing the
  existing suppressed-render styling (`FeatureTreePanel.cpp:54-79`).

## 6. Surface — RPCs + MCP

The Study RPC block is the template (`RemoteServer.cpp:671-860`, MCP at
`designer_mcp.py:828-929`); part-config methods stay **out** of `kNoPartDoc`
(they need a live part doc):

- `add_configuration {name?}` / `remove_configuration {id}` /
  `list_configurations` (each config + its overrides/suppression, and which is
  active)
- `set_configuration {id, parameter_overrides?, suppressed_features?, name?}`
  — the modify path (mirrors `set_study`), incremental override/suppression
  edits
- `set_active_configuration {id}` — regenerate under that config; returns the
  new body/volume summary so a headless caller sees the effect

The LLM parity chain is the acceptance test: `add_configuration Tall` →
`set_configuration Tall depth=12` → `set_active_configuration Tall` →
`export bracket-tall.step`, fully headless. Batch that over a list and you have
"export the whole family," the capability no mainstream CAD exposes.

## 7. Assembly configurations (C4 — committed, sequenced last)

In scope; built on the finished part machine so it lands last:

- `AssemblyDocument` gains its own `m_configurations` + active id (the same
  `Configuration` type, reused). An assembly config's overrides are **occurrence
  suppression** (a set of occurrence ids to hide/exclude — the long-promised
  occurrence suppression, a4-design.md:107) plus **per-occurrence part-config
  selection** (an occurrence names which config of its part to show).
- `Occurrence` gains an optional `configName`/`configId` selector
  (`Occurrence.h:18-31` has no override today); resolving an occurrence's bodies
  regenerates its referenced part **at that config** rather than the part's own
  active config. This needs a "regenerate a part at config X, read its bodies,
  don't disturb its active config" path — a pure read using the override-overlay
  from §2 with an explicit config argument (another reason §2 evaluates against
  an argument, not a mutated document).
- Same doc-level-object + Command + serialization + panel patterns as the part
  side; the assembly panel gains a Configurations sub-list (the mates/exploded
  sub-list idiom, `OccurrencePanel.cpp`).

## Decisions for sign-off

1. **Scope: part configurations then assembly configurations, both committed.**
   Part side = named override sets (parameter-value overrides as expressions +
   feature suppression), a persisted active configuration (`id 0` = base),
   serialization v31, a Model-dock panel, the RPC/MCP surface. **Assembly
   configurations** (occurrence suppression + per-occurrence part-config
   selection) are **C4, in scope** — sequenced after the part machine, not
   deferred. Configured materials/properties, design tables, and implicit
   active-config editing remain deferred.
2. **A configuration is a doc-level named object**, the FEM Study pattern
   verbatim (`Document` storage + accessors, Command trio, tagged
   serialization) — **not a new `DocKind`**, edited by a panel section within
   the part tab.
3. **Application = regenerate against an override symbol table + composed
   suppression, never mutating stored state** (the `Document` is move-only).
   Parameter overrides overlay `m_symbols` after topo-eval; effective
   suppression = authored `OR` config; the prefix cache invalidates for free
   **once the cache key folds in the active config id**. **C0 spike-gated** on
   exactly this — override-regen correctness, suppression composition, and
   config-switch cache invalidation across feature types.
4. **Serialization v31, additive** — `configurations` file-per-config dir +
   `active_configuration` in the index envelope, absent ⇒ empty ⇒ older files
   byte-identical; one migration + frozen v30 golden + a v31 golden;
   references by feature id / parameter name.
5. **Overrides are expressions** (`{name, expression}`, reusing the parameter
   shape), not literals — a config can re-derive a value from other parameters.
6. **v1 edits are explicit**: `set_variable` edits the base parameter table;
   config overrides are edited only through the configuration surface. Implicit
   "an edit while a config is active targets that config" (SolidWorks
   configured dimensions) is a deliberate future enhancement, not v1.
7. **Refactor-first** (the roadmap's standing instruction): configurations add
   ~5 RPC handlers, a doc-level object, and AppActions helpers straight into
   `RemoteServer.cpp` (108 handlers, ~2120 LOC) and `AppActions.cpp` (~3170).
   Per the roadmap policy, the **first step of C1 is carving the study/config
   doc-level-object RPCs out of the `HandleSemantic` if-chain into a module**
   (and the matching AppActions helpers into their own TU), guarded by the
   existing smokes, backfilling tests opportunistically.

## Phasing

0. **C0 — the override-regen spike (gate):** `core/Configuration.h` + the
   minimal `Document` integration (override overlay in `EvaluateParameters`,
   `EffectiveSuppressed` in regen, active-config in the cache key) +
   `test_configuration.cpp` proving override-regen, suppression composition,
   and config-switch cache invalidation. **No further work until this passes.**
1. **C1 — model + persistence:** the `Configuration` type + full `Document`
   accessor set + Command trio + serialization v31 (+ golden + round-trip),
   with the `RemoteServer`/`AppActions` doc-level-object split as its opening
   step (decision 7).
2. **C2 — surface:** `add/remove/list/set_configuration` +
   `set_active_configuration` RPCs + MCP mirrors; `configuration_smoke.py`
   drives the family-export LLM chain headless.
3. **C3 — GUI:** the Configurations panel section (active selector, list,
   override table, suppression toggles) + dimmed suppressed features.
4. **C4 — assembly configurations:** occurrence suppression + per-occurrence
   part-config selection; the `AssemblyDocument` config object + panel.

Order rationale: C0 settles the only real unknown for a day; C1 makes configs
persist before there's a surface to edit them; the RPC/GUI split (C2/C3)
mirrors the FEM ordering; C4 is a self-contained phase that reuses the whole
part-side machine. After v1: configured materials/properties and design-table
import are the natural C5/C6, informed by what real families need.
