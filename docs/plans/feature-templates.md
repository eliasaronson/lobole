# Feature templates

## Problem

A composite that recurs, like a bearing boss with its bore, groove and
chamfers or a bolted flange, is rebuilt feature by feature every time,
by hand or by a generator. Feature types are a compiled table
(`kFeatureTypes`), so a user or an agent can drive every feature but
cannot define a new one. Generators cover logic outside the app, but their
output in the document is just the features they made, with no memory that
they belonged together. An interpreter inside regeneration is ruled out
(docs/roadmap.md#6-the-agent-interface): it brings a sandbox, a pinned
runtime and a new determinism gate.

## Fix

**A template is data:** a saved list of features with named, typed
parameters (lengths, angles, counts, a bearing designation, a face to stand
on), whose fields are expressions over those parameters. No code runs.
Regeneration stays exactly as deterministic as today.

**An instance is one feature in the tree.** A `Template` feature names its
template and gives values for its parameters, plus the reference it stands
on.
- On regeneration it expands into its features, renamed into the
  instance's own namespace, so two instances never collide.
- Faces it makes are named through the instance, so later features can
  reference them stably.
- A parameter that cannot be satisfied is refused naming what is accepted.
  A reference that is lost is an error, never a fallback.

**Where templates live:** in the project, as a library of templates any
part can use, and exported/imported as a file so they can move between
projects.

**When a template changes, instances follow it.** Each instance records the
template revision it expanded and reads "template changed" until
regenerated, so nothing changes silently. Unlinking an instance turns it
into plain features.

**Making one:** select features in a part and "Save as template", choosing
which literal values become parameters. The literals lint and
`extract_variables` already find the candidates.

**Interfaces:**
- **RPC:** `save_template`, `list_templates`, `add_feature {type:
  "Template", template, parameters, on}`, `unlink_template`.
- **GUI:** Save as template, and an insert-template tool session.
- **MCP:** text describing the above.
- **Saved form:** the template library, and the `Template` feature type.
  This is a format change.

## Files

- a new `src/core/features/TemplateFeature.*`;
- the template library in `src/core/` and its serialization;
- the RPCs;
- `src/ui/` (save and insert sessions);
- docs: a new `docs/modeling/templates.md`;
- `docs/versioning/format.md`.

## Verification

- A bearing-boss template saved from a part, inserted on two faces of
  another with different bearings: the right geometry, distinct face names,
  both regenerated after an upstream edit.
- Changing the template marks both instances, and regenerating updates
  them.
- Unlinking leaves plain features with the same geometry.
- A bad parameter is refused.
- A save, reload and regenerate gives identical geometry.
- The pan/tilt head's four bearing bosses become instances of one template.
