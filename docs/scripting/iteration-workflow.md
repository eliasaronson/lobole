# Iterating generated CAD

Use `preview` while shaping parts. Run `local` after changing an interface, then
`full` before accepting an assembly. Collision checking is explicit; it does not
run automatically after every feature or generator attempt.

```python
from designer_validation import validate

report = validate(app, stage='preview')
report = validate(app, stage='local', assembly=rig)
report = validate(app, stage='full', assembly=rig)
print(report.current(app))  # rechecks the session revision
```

Preview checks existing B-Reps, feature errors, nonempty parts, stale derived
parts and unresolved occurrence references. Local adds the exact standing sweep.
For now this conservatively checks the **whole visible assembly**, including
rigid pairs, rather than guessing the affected neighbor closure. Full also runs
every defined motion study on that assembly; those studies must explicitly use
exact collision checks. No study means incomplete, not accepted. Motion checks
cover the defined samples, not the continuous trajectory. Hidden/suppressed
occurrences remain outside the existing sweep's scope.

Every report distinguishes geometry, standing and motion checks. `status=passed`
is relative to the requested stage; only full validation can set `accepted`.
`not_checked`, `incomplete`, `failed` and `stale` are distinct. Persisted report
files are historical evidence; call `current(app)` before using a live report.
Session revision tokens are deliberately invalid after reopening a project.
Empty construction-only parts currently require a separate workflow: the strict
preview gate expects geometry in every project part.

The motion panel offers **Preview motion** (kinematics only) and **Validate
motion** (exact sampled collisions). These are motion controls, not substitutes
for the standing assembly check. Older results are labeled stale after a model
or study edit. The SDK stages compose existing native checks; a unified GUI
panel for all three stages remains follow-up work.

## Update one owned component

```python
from designer_components import claim, plan

part = build_housing(app)
claim(part, namespace='my-generator', key='housing', fingerprint=inputs_hash)

with plan(app, namespace='my-generator', key='housing',
          fingerprint=new_inputs_hash, builder=build_housing) as change:
    print(change.report)        # target still unchanged
    housing = change.apply()   # one undoable change; keeps the part ID
```

Declare the fingerprint from every input and dependency. The SDK skips the
builder completely when it matches. Ownership and the last generated recipe
hash live in the part document (format v94), so reuse survives save/reopen.
Existing unowned parts need an explicit `claim`; ownership is never inferred
from a matching name. Human recipe edits refuse automatic replacement.

Changed components build in a disposable staging part. Stable feature IDs are
matched by type, name and ordinal among duplicates; preserve these keys when
editing builders. The application validates geometry and dependent assembly
mates before applying. Both source and target revision guards are rechecked on
apply. The target keeps its document/part identity, occurrence references and
feature IDs. Applying and undoing reuse the already-built geometry; mounting datums still
resolve against it. `geometry_reused` verifies shape identity against staging,
while `regen.executed` includes datum resolution.

This first version supports one part per builder. It refuses feature retirement,
renames that retire existing IDs, assets, configurations, FEM studies and rolled
back feature trees. Those need explicit reference migration or richer snapshot
support. Context manager exit removes staging geometry, including after failure.
The builder proxy is an ergonomic adapter, not a sandbox for untrusted code.

## OM615

```sh
python3 generators/om615/om615.py --port 8801 --out /tmp/engine --stage preview
python3 generators/om615/om615.py --port 8801 --out /tmp/engine-review --stage full --no-export
python3 generators/om615/om615.py --port 8801 --update-component intake --stage local --out /tmp/intake-review
python3 generators/om615/om615.py --port 8801 --validate-only --stage full --out /tmp/full-review
```

Use your own application instance. `--project PATH` optionally opens a saved
project before an update or validation. Without it, these two operations retain
the live project and its collision cache. Fresh generation defaults to preview;
`--build-only` and `--no-study` remain legacy aliases for preview and local.
Draft outputs use `om615-unchecked.dsgn`; skipped motion cannot produce an
accepted filename. `--component` still creates an isolated part.

Nine OM615 component builders carry ownership. Shared specification changes
require a full rebuild because they also affect the remaining mechanism parts.
A selected component update does not automatically update other builders.
