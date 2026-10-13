# Configurations

A configuration is a named variant of a document. You can switch to it in
place, and it is stored with the document.

- **On a part**, a configuration overrides variable values and changes which
  features are suppressed. Examples: a bracket in six sizes, or with and
  without a boss.
- **On an assembly**, a configuration suppresses occurrences, mates and
  relations, and can hold mate degrees of freedom at fixed values. An
  example is a watch with its crown pulled out.

Export, drawings and simulation all read the regenerated bodies, so they
reflect the active configuration -- unless something names another: an
assembly placement, a job part, a Derived feature or a drawing view that
asks for one reads the part regenerated in that one
(`ui::ConfiguredPartDocument`), whatever is active.

## Model

A `Configuration` is defined in `src/core/Configuration.h`. It is a sibling
of the parameter table, stored on the document (`Document::m_configurations`
and `AssemblyDocument`). It is **not** a Feature and has no place in the
feature list.

| field | part | assembly |
|---|---|---|
| `id`, `name` | yes | yes |
| `parameterOverrides` (name → expression) | yes | unused |
| `suppressedFeatures` | feature ids | **occurrence** ids |
| `unsuppressedFeatures` | features turned back on; this has the last word | — |
| `suppressedMates`, `suppressedRelations` | — | yes |
| `mateOverrides` (`{mate, rotation?` rad, `offset?` mm`}`) | — | yes |

Ids come from the document's shared id pool, so they never collide with
feature ids.

The document stores which configuration is active. Id `0` is the **base**:
the model as authored, with no overrides. The base always exists.

## How a part configuration is applied

A configuration **never mutates stored state**. Instead, regeneration runs
against an overlay:

- **Variable overrides.** `EvaluateParameters` (`src/core/Document.cpp`)
  replaces the overridden variable's *expression* for this regeneration
  only. Dependent variables are re-derived from it, and the base
  expression stays untouched. An override may itself be an expression.
  - An override of a **project** variable is injected as a local variable
    that shadows it in this part only.
- **Suppression.** `Document::EffectiveSuppressed(i)` decides whether
  feature `i` is suppressed. It is suppressed when:
  - it is not listed in `unsuppressedFeatures`, and
  - either its authored flag is set or the configuration suppresses it.
- **Regen cache.** A variable override reaches the cache key through the
  evaluated field values. Effective suppression is folded into the chain
  hash explicitly (`"|cfg-suppressed"`). Together these mean that switching
  configurations re-executes only the affected suffix of the feature list,
  and switching back to one evaluated before executes nothing.

`set_variable` always edits the **base** expression. A configuration's
overrides are edited only through the configuration surface, so it is
never ambiguous whether an edit changed the base or the variant.

## How an assembly configuration is applied

The overlay is applied inside `assembly::Solve`
(`src/core/AssemblySolve.cpp`):

- **Suppressed items sit out.** Suppressed occurrences are left out.
  Suppressed mates and relations are skipped, and so is any mate or
  relation touching a suppressed occurrence (`MateSuppressed`,
  `RelationSuppressed` in `src/core/Occurrence.cpp`).
- **Held degrees of freedom.** A held DOF replaces the mate's authored value
  for the whole solve: placement, relations, and the loop pass. The
  authored value is restored afterwards, so leaving the configuration puts
  every mate back.
- **Editing and driving a held DOF is refused.** Changing the value
  underneath the hold would do nothing, so these refuse instead:
  - `set_mate`, which names the configuration holding the DOF;
  - the gizmo drag (`src/ui/GizmoInput.cpp`);
  - a motion study (`src/core/MotionStudy.cpp`). A motion study also
    refuses to drive a mate the active configuration suppresses.
- **Activating a configuration re-solves** the assembly.
- **`list_mates` reports the authored values.** It gives each mate's
  authored `rotation`/`offset`, plus `held` and `suppressed` when the
  active configuration applies.

A placement can use a configuration other than its document's active one:
`Occurrence::configurationId`, set with `add_occurrence {configuration}` or
`set_occurrence_configuration` (0 = the active one). For a **sub-assembly**
it is a suppression set; for a **part** (v135) it is the part regenerated in
that configuration, a cached copy (`ui::ConfiguredPartDocument`), so a pawn
and a rook can be two placements of one piece part. A configuration the part
lacks, or does not build in, is refused, naming the feature that fails
([assemblies](../assemblies/README.md#configurations-per-placement)).

## Undo

Part configurations use Commands (`src/core/Command.h`):
- `AddConfigurationCommand`
- `RemoveConfigurationCommand`: undo restores the configuration and the
  previously active id.
- `ModifyConfigurationCommand(before, after)`
- `SetActiveConfigurationCommand`

Assembly configuration edits are undone through the assembly snapshot
(`ui::SnapshotAssembly`).

## Serialization

Each configuration is written as tagged JSON, with only the non-empty
fields present:

```jsonc
{ "type": "Configuration", "id": 2, "name": "Tall",
  "parameter_overrides": [ { "name": "depth", "expression": "12" } ],
  "suppressed_features": [ 7 ],          // occurrence ids on an assembly
  "unsuppressed_features": [], "suppressed_mates": [],
  "suppressed_relations": [],
  "mate_overrides": [ { "mate": 5, "rotation": 1.57 } ] }  // rotation rad, offset mm
```

- **Directory projects** store one file per configuration,
  `configurations/<id>-<slug>.json`. The document index lists
  `"configurations"` (the ids, in order) and `"active_configuration"`,
  which is omitted when it is 0. Keeping each configuration in its own file
  lets two people edit different configurations without a merge conflict.
- **Single-file documents** keep the same objects inline under
  `"configurations"`.
- References are by feature, occurrence or mate **id**, and by variable
  **name**, never by list position. A configuration therefore survives
  reordering.

The code is in `src/core/io/SerializeConfigurations.cpp` and
`Serialization.cpp`. The golden files are `tests/golden/v31-configurations.json`
and its `.expect.json`.

## RPCs

The RPCs are in `src/remote/RemoteConfigurations.cpp`. They act on the
active tab, whether it holds a part or an assembly. On a drawing tab they
refuse with guidance -- except `list_configurations` with `part`, which reads
that part's from any tab.

| method | params | returns / notes |
|---|---|---|
| `add_configuration` | `name` | `{id, name}`; does not activate |
| `remove_configuration` | `id` | if it was active, the document drops to base |
| `list_configurations` | `part?` (id or name: that part's, from any tab) | `{configurations[], active}`; part entries carry `parameter_overrides, suppressed_features, unsuppressed_features`, assembly entries `suppressed_occurrences, suppressed_mates, suppressed_relations, mate_overrides` |
| `set_configuration` | `id, name?`, then the part keys (`parameter_overrides, suppressed_features, unsuppressed_features`) or the assembly keys (`suppressed_occurrences, suppressed_mates, suppressed_relations, mate_overrides`) | each list passed replaces the stored one. The other document kind's keys are **refused**, with the right key names in the error. Mate/relation ids must exist; a rotation hold needs a mate with a rotation DOF; `rotation` and `rotation_deg` together are refused. Editing the active configuration regenerates or re-solves. |
| `set_active_configuration` | `id` (0 = base) | part: `{active, bodies, volumes, digest, regen}`; assembly: `{active, visible_occurrences}` |

These RPCs are enough for an agent to export a whole part family headless:
`add_configuration`, then `set_configuration` with overrides, then
`set_active_configuration`, then export, repeated for each variant.
`tests/configuration_smoke.py` runs exactly that chain.

## GUI

The **Configurations** panel (`src/ui/ConfigurationsPanel.cpp`,
`DrawConfigurationsPanel`) is docked behind Model. It appears on part and
assembly tabs.

- **Part tab**
  - A list of "Base (as authored)" and each configuration; clicking an
    entry activates it.
  - "+ Add configuration", which adds one and activates it.
  - A name field and Delete.
  - An override table (Variable / Base / Override; a blank override means
    the base value). It lists the local variables and any project
    variables they do not shadow.
  - Checkboxes for suppressed features.
  - `unsuppressedFeatures` has no editor here. The sheet-metal flat-pattern
    flow sets it (`SheetMetalPanel.cpp`).
- **Assembly tab**
  - The same list, add and delete controls.
  - Checkboxes for suppressed occurrences and relations.
  - For each mate: a suppress checkbox, and "turn" / "slide" hold
    checkboxes, each with its value in degrees or millimetres. A new hold
    starts at the mate's current value.

## Tests

- `tests/test_configuration.cpp` checks:
  - an override applies during regeneration, drives a primitive through a
    variable, and propagates to dependent variables;
  - suppression composes with the authored flag;
  - save and load round-trip;
  - the configuration commands undo and redo;
  - switching configurations invalidates exactly the affected cache suffix.
- `tests/configuration_smoke.py` drives it headless:
  - adding configurations, overrides and suppression;
  - volumes changing on a switch;
  - undo of activation;
  - a STEP export per configuration;
  - save and reopen keeping the configurations and the active id.
- Assembly configuration GUI checks are in
  `src/app/guitests/GuiTestsAssembly.cpp`.

## Not built

- The feature tree does not dim a feature that only the active
  configuration suppresses. It shows the authored flag alone.
- Configurations cannot override the material, custom properties or part
  number.
- There are no design tables: a spreadsheet that defines many
  configurations at once.
- Editing a variable while a configuration is active cannot target that
  configuration. `set_variable` always edits the base.
- A placed part cannot use a configuration other than the part's active one
  (see above).
