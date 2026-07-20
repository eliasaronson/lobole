# Caster-round backlog — execution plan

Status: DRAFT — awaiting sign-off. Anchor decision made by Elias 2026-07-20:
**project-wide variables** (shared symbol table, not explicit per-variable
linking). Source: [backlog-dogfooding-2026-07-20-caster.md](backlog-dogfooding-2026-07-20-caster.md).

Sequenced by value. Phase V (variables) is the anchor and lands first —
it's the highest-value fix and its fail-loud half makes every later
dogfooding round trustworthy. Effort in sessions.

---

## Phase V — Project-wide variables + fail-loud expressions (~1.5–2 sessions)

The finding: `SymbolTable m_symbols` and `std::vector<Parameter> m_parameters`
are `Document` members, so each part has an isolated variable set — an
assembly's `bearing_bore` can't drive axle + wheel + fork together. Plus the
compounding correctness bug: an expression referencing an unknown variable
**silently falls back to its literal twin** with no feature error.

### Design

**Ownership.** Core has no "Project" object (AppState owns the `PartSlot`
list), so introduce a project-level table without coupling `Document` to
AppState:
- `Document` keeps its own `m_parameters`/`m_symbols` as the **part-local**
  scope and gains a non-owning `const SymbolTable* m_projectSymbols = nullptr`.
- Expression evaluation (the `Regenerate` pre-hash refresh + all `EvaluateText`
  calls) resolves **local-first, then project** — a part-local variable
  shadows a project one of the same name.
- AppState owns the project parameter table + its evaluated `SymbolTable`, and
  points every slot's `Document::m_projectSymbols` at it (set on load, on part
  add, on activate). Headless/core tests inject the pointer directly, so the
  core stays self-contained and testable.

**set_variable scope.** Gains `scope: "project" | "part"`, **default
"project"** (the intent — define `bore` once). Part scope writes the active
Document's local table as today. `list_variables` returns each variable's
scope; `rename_variable`/`delete_variable` operate within the given scope and
their reference-tracking spans **all parts** for a project variable (walk every
slot's features, not just the active one).

**Regenerate-all.** A project-variable edit must regenerate **every part** that
references it (today set_variable regens only the active doc). Because
evaluated values are baked into the feature JSON the regen-cache hashes, a
changed project value naturally invalidates dependent features across parts —
but the regen must actually be triggered per slot. AppState drives a
project-wide regen pass on project-variable edits.

**Undo.** A project-variable edit spans tabs, so it can't be a single
`Document` command on the active stack. Add a project-level undo entry
(snapshot the project table + re-regen all) — either its own small stack or an
AppState-level command; the active-tab stacks stay part-scoped. (Implementation
risk — the fiddliest part; scope it first.)

**Fail-loud (the correctness half).** An expression field whose variable
doesn't resolve in either scope sets a **feature error** ("unknown variable
'bore'") instead of silently using the literal twin. Applies to feature exprs
AND sketch-entity exprs (Decision 2A fields) AND constraint exprs. This alone
fixes finding #3's silent 1 mm hole. Keep the last-good literal as the
*rendered* value (regen continues past the error, per the existing UX) but the
error is visible.

**Serialization (format v38 → v39).** Project parameters serialize into
`project.json` (the multi-doc index envelope — where `active`/`documents`
already live). A single-part flat `.dsgn` has no project.json: its variables
stay in the part file and ARE the project scope (one part = the whole project),
promoted to project scope in memory on load. Migration: an old multi-part
project's per-part `parameters` load as **part-local** (exact behavior
preserved — nothing silently changes scope); new `set_variable` puts them at
project scope. Golden `tests/golden/v39-project-vars.json` + a migration case
proving an old per-part-vars project still loads and regenerates identically.

**GUI.** The Variables panel gains a scope column/section (Project vs. this
Part); `set_variable`'s panel path defaults to project. Minimal — the panel
already lists name/expr/value/comment.

### Sub-decisions (recommended defaults — flag if you disagree)
- **VD1 part-local override layer**: YES (keeps back-compat; lets a part shadow
  a project var). Alternative: project-only, migrate all old vars to project —
  simpler but can collide names across parts on load.
- **VD2 set_variable default = project**: YES (the intent). Part scope stays
  reachable via `scope:"part"`.

### Tests
Unit: project var drives features in two different parts through regen incl.
cache invalidation across parts; part-local shadows project; unknown var →
feature error (not silent literal); rename/delete a project var updates refs in
all parts; migration round-trip. Smoke: `set_variable` (project) ripples a bore
into two parts' geometry; save/reload preserves scope. Golden v39.

---

## Phase G1 — Geometry correctness (~1 session)

- **#2 Loft twist on mismatched section vertex counts**: square→circle pinches
  into an hourglass; matched circles loft exactly. Fix the section start-vertex
  correspondence / anti-twist pairing in the loft builder (core/features/
  LoftFeature). Unit test: square→circle loft volume within tolerance of the
  frustum, no waist below either cap area.
- **#3 Hole silent 1 mm default**: mostly resolved by Phase V's fail-loud
  (an unresolved `diameter_expr` now errors instead of drilling 1 mm). Verify;
  add a test that an unresolved hole diameter errors rather than defaulting.
- **#4 Extrude requires literal `distance` with `distance_expr`**: make the
  deserializer default `distance` when only `distance_expr` is present (mirror
  Revolve's `angle` default). Smoke: extrude driven purely by `distance_expr`.

## Phase A1 — Assembly usability (~1–1.5 sessions)

- **#6 Mate posing** (the assembly-motion path — DOF works, posing is hard):
  add an **axial offset DOF to revolute** (slide along the mate Z, like
  slider's `offset`); an **auto-center / lateral-align** option on face mates
  so a wheel doesn't land on a fork leg; and **fix the inverted `offset`
  sign**. Design note: this is a mate-model change — check whether offset-sign
  is a solve convention or a display bug first (cheap win if display). Tests:
  revolute with axial offset places the part at the offset; sign matches world
  direction.
- **#5 Assembly mass_properties**: aggregate occurrence masses/CoM in world
  space (transform each part's inertia by OccurrenceMatrix, as assembly
  interference already transforms bodies). Reply gains real `total`. Smoke:
  two-occurrence assembly total mass = sum, CoM correct.

## Phase E1 — Ergonomics batch (~0.5–1 session, cheap papercuts)

Could be pulled EARLIER to smooth the next dogfood round — all small:
- **#7 auto_explode** auto-creates a view (or errors helpfully) instead of raw
  "key 'view' not found".
- **#8 list_elements face normals** = [0,0,0]; populate from the face's surface
  normal at its centroid (Measure already resolves the shape).
- **#9 `part=<int>`** collides with the visible slot index; accept index too
  (or error on unknown id) and document.
- **#10 rename_part** RPC (mirror rename_occurrence) so the default "Part 1"
  isn't permanent.
- **#11 raw RPC screenshot(view=…)** ignores view — align the raw handler with
  the MCP wrapper (reorient then capture) or document that view is client-side.
- Cosmetic: `set_material` updates mass but doesn't recolor the shaded body in
  the viewport (RPC path skips the renderer material resync — same class as an
  earlier fixed issue; quick).

---

## Order & sign-off

Recommended: **V → G1 → A1 → E1**. Value-first: shared variables + fail-loud
first (unblocks real multi-part parametrics and makes findings trustworthy),
then wrong-geometry, then assembly friction, then papercuts. E1 can jump ahead
if we want the next dogfood round smoother sooner — say the word.

Decisions needed to start V: confirm VD1 (part-local override — recommend yes)
and VD2 (set_variable defaults to project — recommend yes). Everything else is
implementation detail I'll decide in place and note.
