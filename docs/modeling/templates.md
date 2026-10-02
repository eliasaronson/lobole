# Feature templates

A composite that recurs -- a bearing seat, a boss with its bore and relief,
a bolted flange -- is saved ONCE as a template and placed as one feature, a
`Template` instance, wherever it is wanted. Feature types are a compiled
table (`kFeatureTypes`), and an interpreter inside regeneration is ruled out
([../roadmap.md](../roadmap.md#6-the-agent-interface): a sandbox, a pinned
runtime and a determinism gate of its own), so a template is DATA: a saved
list of features with named, typed parameters, whose fields are expressions
over them. No code runs. Expanding one is substituting values into JSON, and
regeneration stays exactly as deterministic as it was.

Code: `src/core/Template.{h,cpp}` (namespace `tmpl`: the template, its
values, expanding and saving one), `src/core/features/TemplateFeature.{h,cpp}`
(the instance), `src/ui/TemplateActions.{h,cpp}` (the project-level verbs
both surfaces use), `src/ui/TemplatePanel.cpp` (Save as template),
`src/remote/RemoteTemplates.cpp` (RPCs).

## The saved form

A template (`tmpl::Template`) is `{name, revision, comment?, parameters,
features, bindings}`:

- **`features`** are the features it was saved from, as `FeatureToJson`
  writes them, with three rewrites. Their ids are LOCAL, 1..N in order, and
  so are the tags in the face and edge names they name among themselves
  (`topo::RewriteIds`, [toponaming.md](toponaming.md#naming-grammar)); every
  expression reads only the template's own parameters; and where a feature
  stood on something outside the selection there is a placeholder its
  binding fills.
- **`parameters`** (`tmpl::Param`) each have a `name` (an identifier), a
  `kind` -- `length` (mm), `angle` (degrees), `count` (a whole number >= 1),
  `string` (a designation, a thread, an end condition), `reference` (what it
  stands on) -- the `example` it was saved with, an optional `comment`, and
  for a string optional `choices`.
- **`bindings`** say where a string or reference parameter lands: a JSON
  pointer into a feature (`/end_mode`, `/edges/2`), or `attach` -- the
  sketch stands on the reference, as `face_ref`, `plane_ref` or a world
  plane, whichever the value is. Numbers need no binding: they are read by
  the features' `_expr` fields.

`tmpl::Validate` refuses, naming it, a template whose name is empty, whose
parameters are not identifiers or repeat, whose examples are of the wrong
type, whose local ids are not 1..N, whose expressions read anything but its
number parameters, or whose bindings name nothing; a file holding one does
not load (it is never read as a different template).

## Parameters

An instance gives EVERY parameter a value. Nothing is filled in from the
example: `add_feature` without one is refused naming each that is missing,
its kind and what it was saved as, so an agent can copy it
(`tmpl::ValuesFromRequest`):

- a length, angle or count is a number, or an expression string over the
  part's variables (`"D": "pan_brg_D"`), written `D` + `D_expr` like every
  expression field -- which is what lets `check_literals` and
  `extract_variables` read an instance's numbers as they read any feature's;
- a string is text, one of `choices` when the template lists them;
- a reference is in the `on` form a primitive takes
  ([README.md](README.md#primitives)): `"Top"`, `"Front"`, `"Right"`,
  `{feature_id, element}` (a face), `{feature_id}` (a DatumPlane, a Sketch's
  plane or a Frame). The one a sketch stands on is named `on` and given at
  the top level of the request.

Refused, naming what is accepted: an unknown parameter (naming the
template's), a missing one, a value of the wrong type, a count that is not
whole, a string outside its choices, an unknown variable in an expression, a
reference that is not in the `on` form, a face that is not there, a curved
face for a sketch to stand on. A value the features themselves refuse -- a
designation the table lacks, a diameter that makes no circle -- shows as the
instance failing to build, and `add_feature` then takes the instance back
out and refuses with its error: a bad parameter is refused, not committed.

## Expansion

`TemplateFeature::Execute` builds the template's features one at a time
(`tmpl::BuildFeature`): its feature JSON with the local ids mapped to the
instance's ids, the string and reference values put where they are bound,
`FeatureFromJson`, its expressions evaluated against the parameters, and it
executes against the same bodies, in a context whose `RegenContext::inner`
lists the template's own features before it, so a reference inside the
template finds them first and the document's after. The marks a face carries
follow it after each (`FollowModifiedFaces`), as they do after any feature.
A feature that fails puts every body back as it was before the instance and
is the instance's error, naming it: the instance adds nothing, as any failed
feature does.

**The chain hash covers it.** An instance's JSON carries its definition and
its evaluated values, and its expressions read only its parameters, so the
regen cache keys an instance on exactly what it builds from.

**A lost reference is an error, never a fall-back.** Before it builds, the
instance checks every reference parameter: a face that no longer resolves,
or a plane feature that is gone or suppressed, is its error ("the face it
stands on ... is gone; pick what it stands on again"), and it builds
nothing. A sketch inside it standing on a lost face fails the same way.

## Names

An instance's expanded features have document ids of their OWN, minted once
from the document's counter when it is placed and stored on it (`ids`, local
-> document id; `Feature::VisitOwnedIds`). The faces they make are named as
any feature's are -- `<id>/g1`, `<id>/cap1`, `<face>;M<id>` -- under those
ids, so:

- two instances never collide: their ids differ;
- a later feature references an instance's face by that name and it
  survives regeneration, an upstream edit, and an update to a new revision
  (an update keeps the id of every template feature it keeps, by its local
  id: a feature keeps its id while it keeps its place in the template);
- unlink hands the tree the same features with the same ids, so every name
  and every reference to one survives it.

The document keeps its id counter past the ids an instance owns
(`Document::InsertFeature`), a feature naming one depends on the instance
(`Document::FirstConsumerIndex`), `Document::OwnerIndex` finds the instance
that owns one, and a body one of them created takes the instance's
material when it has one.

**A face of the body it stands on.** A template that cuts a bore into the
face it stands on leaves that bore on a body it did not create: a reference
to it (a relief sketched on the seat's floor) is saved with `feature_id` 0
and found, when the instance expands, as the body that carries the name --
which, the name being the instance's own, is one body.

## Revisions

The library holds each template's latest revision; `save_template` with
`replace` (or an import over a name the library has) makes the next one. An
instance CARRIES the definition it expands, like a Derived carries its
snapshot ([derived.md](derived.md)), so a part regenerates from its own file
alone, with or without the library, and nothing changes because a template
did:

- `get_state` rows say `template_changed`: true when the library's template
  of that name is not the one the instance expands, false when it is, null
  when the library has none by that name (unknown, never "unchanged");
  `check_geometry` lists `templates_changed`; the tree tints the row amber;
  `save_template` answers which instances it just marked.
- `update_template_instances` (the tree's **Update from template**) moves an
  instance to the library's revision as one undo step: every parameter the
  new revision still declares keeps its value, `parameters` gives one it
  added (refused without), and every feature it kept keeps its id.

## The library

`AppState::projectTemplates`, beside the project variables and tables:
saved in `project.json` under `templates` (additive), and in a lone part's
file envelope (format v161), which an open takes back into the project. A
malformed template refuses the open. Saving, importing or deleting one is a
project-scope edit on the project variables' undo history
(`ProjectVarSnapshot::templates`). Deleting a template leaves its instances
building from their own definitions.

`export_templates` writes `{"format": "designer-templates", "version": 1,
"templates": [...]}`; `import_templates` reads one, refusing a name the
library has unless `replace`.

## Saving one

`tmpl::FromFeatures` (`save_template`, Save as template) takes features of
the active part, in the document's order:

- every variable their expressions read becomes a parameter of its name (or
  `rename`'s), its example what it is now; a literal the caller chooses
  becomes one too (`literals`: `{feature_id, path, param}` with the
  `check_literals` field path, `entities/0/cx`, `distance`), and a string
  field by JSON pointer (`strings`). Two fields share a parameter only when
  they hold the same value. `tmpl::Suggest` (the dry run, the window) offers
  the literal candidates with `extract_variables`' names;
- what a sketch stood on outside the selection becomes the reference
  parameter `on` (a second distinct one `on2`), a world plane exactly where
  it is `Top`/`Front`/`Right`; any other face or edge outside it `ref`;
- refused, naming it: a feature that is suppressed or in error, an
  instance, a Derived (its snapshot is assets of this part), an Import (a
  file), a Frame (a mate anchors to it by name, and nothing outside an
  instance sees inside it), a feature outside the selection named by id
  (select it too), a face made partly inside and partly outside the
  selection, a field that is not a literal or must not be driven, an
  expression reading a table (`lookup`: give the value a variable), a name
  the library has (without `replace`).

**It is checked by expanding it.** With its own examples and its local ids
mapped back to the source's, the template must give back exactly the
features it was saved from (their JSON, expressions aside), and with its
ids mapped to ids no feature has, every reference must come back as the
originals' did (`VisitFeatureRefs`): a reference the save failed to carry
shows up and refuses the save rather than placing wrong.

## Unlink

`unlink_template` (the tree's **Unlink template**) replaces an instance
with its expanded features as plain features, one undo step: the same ids
(so the same face names), the same geometry, each expression with the
template's parameters replaced by the instance's values
(`tmpl::SubstituteParams`, `(pan_brg_D) / 2`), so the plain features follow
the document's variables the instance followed. An instance that has not
built is refused.

## In the GUI

- **Save as template** -- the tree's context menu on the selected features
  (`###feat_save_template`) opens a window: the name, what the features
  stand on, the variables they read (each a parameter), and every literal
  they carry as a checkbox with a parameter name; Save
  (`###tpl_save`) reports the revision and how many instances it marked, or
  the refusal in red.
- **Insert template** (`###tb_template`, `part.template`) arms the plane
  pick a primitive uses (`SketchPurpose::Template`): a planar face (by its
  name), a base plane or a datum, in the viewport or the tree. With an empty
  library the pick's window says so and every pick refuses. A pick opens a
  preview of the last template inserted (the library's first otherwise)
  standing on it, with the values it was saved with -- the template's own
  data, not a literal of the tool's -- and the window edits the template
  and every parameter: a number as a drag or an expression, a string from
  its choices. Accept commits it as one undo step.
- An instance's row: amber with a tooltip when its template changed;
  **Update from template** and **Unlink template** in its context menu; its
  editor shows the revision, the status, its parameters and Unlink.

## RPCs

`add_feature {type: "Template", template, parameters, on, name?,
material?}` returns `{index, id, ids, features[]{id, type, name}, regen,
digest}`. `save_template`, `list_templates`, `delete_template`,
`update_template_instances`, `unlink_template`, `export_templates`,
`import_templates`: their rows in `describe`. `get_state` rows add
`template_changed` and `expanded[]{id, type, name}`.

## Tests

`tests/test_templates.cpp` (the saved form, two instances with their own
names following an upstream edit and a later feature on one's face, a
changed template changing nothing until updated and the update keeping the
names, unlink, the refusals, a save and reload in both file forms, a lost
reference, a template standing on its own floor and taking a string, what a
save refuses); `tests/templates_smoke.py` (the RPC surface end to end,
headless); GUI `templates/*` and `defaults/template`; golden
`v161-feature-template`. The pan/tilt head's bearing seats are instances of
one template (`generators/pantilt/common.py`, `bearing_seat`).

## Not built

- Templates inside templates: an instance is refused by a save; unlink it
  first.
- A face made partly inside and partly outside the selection (a chamfer
  round the mouth of the bore on the face it stands on) as a reference.
- A conditional feature (a relief only some instances want): a template is
  one fixed list; a string parameter can switch an end condition, not add
  or remove a feature.
- Live update: the changed flag and the update verb are the mechanism.
