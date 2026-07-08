# Designer docs

Planning and design docs, grouped by area. The [roadmap](roadmap.md) is the
top-level vision + phase index.

## Areas

- **[roadmap.md](roadmap.md)** — vision (checkpoints, variables, scripting,
  git, LLM-parity), the headless-core architecture, and the phase list.
- **[sketching/](sketching/)** — the 2D constraint sketcher. **Complete** (A–N
  + follow-ups). [plan.md](sketching/plan.md) is the master; `phase-*.md` are
  the per-phase implementation plans.
- **[modeling/](modeling/plan.md)** — 3D modeling (datum planes, revolve,
  booleans, 3D mirror/pattern, fillets), and where topological naming gates the
  edge/face-referencing features. **Next frontier.**
- **[rendering/](rendering/plan.md)** — visual fidelity: real normals, AA, PBR
  materials, SSAO, tone-mapping, IBL, then raytracing (offline render, then
  hybrid RT lighting).

Project-level files (`README.md`, `THIRD_PARTY.md`) stay at the repo root.
