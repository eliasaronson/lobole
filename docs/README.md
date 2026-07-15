# Designer docs

Planning and design docs, grouped by area. The [roadmap](roadmap.md) is the
top-level vision + workstream index and holds the recommended sequencing.

## Areas

- **[roadmap.md](roadmap.md)** — vision (checkpoints, variables, scripting,
  git, LLM-parity), the headless-core architecture, current state, the
  workstream table, and sequencing.
- **[sketching/](sketching/plan.md)** — the 2D constraint sketcher. **Complete**
  (A–N + follow-ups; B-splines deferred). `phase-*.md` are the per-phase
  implementation plans.
- **[modeling/](modeling/plan.md)** — 3D modeling: datum planes, revolve,
  booleans, 3D mirror/pattern, richer extrude, fillets; regeneration-failure
  UX. **Next frontier.** [toponaming.md](modeling/toponaming.md) is the
  persistent face/edge reference design study that gates the
  edge/face-referencing features.
- **[variables/](variables/plan.md)** — parameters + expressions on every
  numeric input, evaluated during regeneration. **Do early** — new modeling
  features should be expression-driven from birth.
- **[scripting/](scripting/plan.md)** — the LLM/automation interface: JSON-RPC
  remote (shipped) → MCP server, headless mode, Python package, recorded
  sessions as tests. Includes the OpenSCAD/build123d ecosystem survey.
- **[versioning/](versioning/plan.md)** — format spec + golden files, autosave,
  diffable on-disk layout, checkpoints/rollback bar, git integration.
- **[ui/](ui/plan.md)** — unified selection, sketch-region picking for
  extrude, the feature-dialog pattern, DPI/layout correctness, visual polish,
  command palette.
- **[inspect/](inspect/plan.md)** — measure tool, mass properties (volume/
  CoG/mass from material density), section views.
- **[interchange/](interchange/plan.md)** — export: STEP/STL (kernel writers
  exist, unexposed), BREP, glTF via an XCAF bridge; import widening (IGES,
  mesh bodies) later. 3MF and DXF consciously deferred.
- **[rendering/](rendering/plan.md)** — visual fidelity: real normals, AA, PBR
  materials, SSAO, tone-mapping, IBL, then raytracing (offline render, then
  hybrid RT lighting).
- **[assemblies/](assemblies/plan.md)** — multi-part projects, occurrences +
  instancing, mate connectors; the prerequisites gate (incremental regen,
  instancing, toponaming) and the memory/performance strategy.
- **[simulation/](simulation/plan.md)** — FEM structural analysis: Netgen tet
  meshing, in-house linear-static solver on Eigen, face BCs via toponaming
  refs, colormapped results. **Shipped** (FEM0–FEM4; single-part linear static).
  Modal / assembly-contact / thermal deferred.
- **[drawings/](drawings/plan.md)** — 2D drafting: a third document kind with
  HLR-projected views, driven dimensions via toponaming refs, title block,
  SVG/PDF export. **Shipped** (D0–D4). See [guide.md](drawings/guide.md) for
  the workflow; `plan.md` is the design.
- **[configurations/](configurations/plan.md)** — parametric part/assembly
  families: a doc-level named object (the FEM Study pattern) that overrides
  parameters + suppresses features, applied by regenerating against an override
  symbol table. **Design for sign-off** (the roadmap's next workstream; the
  deferred-from-A4 design).

Project-level files (`README.md`, `THIRD_PARTY.md`) stay at the repo root.
