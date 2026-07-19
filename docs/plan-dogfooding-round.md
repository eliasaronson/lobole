# Dogfooding-round execution plan

Status: APPROVED 2026-07-19 (Elias). Decisions: dimension tool = full
traditional inference (Decision 1A); parametric sketch geometry = `*_expr`
fields on sketch entities, format bump (Decision 2A — supersedes the
constraint-helper option; 4.6 below updated). GATE: implementation starts
only after the in-flight STEP import/export work lands and the tree is
green.

Scope: everything in [backlog-dogfooding-2026-07-19.md](backlog-dogfooding-2026-07-19.md),
sequenced into six phases by risk and value. Effort in sessions (a session ≈
one focused implementation block with suites green at the end).

---

## Phase 0 — C++23 toolchain bump (~0.5 session, mechanical)

Flip `CMAKE_CXX_STANDARD` 20 → 23; link `stdc++exp` (GCC's stacktrace lib);
replace the glibc `backtrace_symbols_fd` body of `CrashHandler.cpp` with
`std::stacktrace` (symbolized function names, no addr2line step — keep the
signal-safety shape: capture in the handler, print via preallocated buffer or
accept the malloc risk post-crash as today's backtrace_symbols_fd already
does). Sanity-run a deliberate `raise(SIGSEGV)` behind a hidden `--crash-test`
flag once, manually. Gate: full headless + GUI suites; the only real risk is
third-party headers (OCC 7.9 / ImGui / spdlog) under the new standard, which
the build immediately reveals. One `chore:` commit. Follow-ups it unlocks
(NOT in this phase): `std::expected` in the RPC/feature-error plumbing,
`std::print`, `std::mdspan` for FEM.

Precondition: a green tree (the in-flight STEP-import work must land or
stash first).

## Phase 1 — Crashers & pattern correctness (~1 session)

1. **Circular-pattern segfault**: headless param sweep already exonerated the
   core; reproduce the GUI flow (Create > Pattern on a body, switch circular,
   drag params) via a scripted `ui_action` sequence against a disposable
   dev-watch instance, now yielding a backtrace on crash. Fix root cause —
   prime suspects are the render-sync path (per-frame version bumps
   interleaving with OutlineRenderer mesh adds) or a stale feature pointer in
   `DrawPatternParams`. Regression GUI test with the exact flow.
2. **Linear sketch-pattern preview**: verify apply vs preview; fix whichever
   half is broken (circular previews fine, so the delta is small). GUI test.

## Phase 2 — S3 rendering tuning (~1–1.5 sessions, screenshot-driven)

The S3 pass the shadows/SSAO design already scheduled, now with concrete
repros (fidget-spinner part + assembly, screenshots in the session record):

1. **SSAO halo fix**: absolute/capped sample radius (stop scaling with scene
   diagonal), tighter range-check falloff at depth discontinuities, higher
   bilateral depth-sharpness. Acceptance: assembly top view shows NO arcs
   tracing neighboring silhouettes; crevices between lobes/hub visibly
   darken on plastic_white.
2. **Dark-dielectric wash**: bound the IBL specular term for rough dielectrics
   (horizon/specular-AO or prefilter energy audit) so plastic_black reads as
   dark glossy plastic, clearly distinct from the unassigned palette.
3. **RPC/MCP toggles + quality settings**: `set_render_options` RPC (shadows
   on/off, AO on/off/half/full, shadow map 1024/2048) + MCP tool; View menu
   gains the quality submenu. Lets agents A/B renders without menu-driving.
4. Record final constants + before/after screenshots in the design doc.

Needs dev-watch (bias/strength judgment calls are visual); I drive it via
MCP screenshots, flagging anything that needs your eyes.

## Phase 3 — Dimension & sketch UX cluster (~2–3 sessions, the big one)

Order inside the phase:

1. **Selection precision first** (everything else sits on it): pick priority
   point > line-body when both are under the cursor (with same-spot click
   cycling like the U1 model-space convention), larger line hit tolerance.
2. **Dimension tool**: a real armed tool (toolbar + D key). Click targets
   accumulate; the tool infers the kind — line = length, two parallel lines =
   perpendicular distance, point+line = perpendicular distance, two points =
   distance, circle/arc = radius/diameter. Esc clears, click-away commits.
   (Kills the two-lines-gives-diagonal surprise — that mapping becomes
   point-pair only when explicitly picking endpoints.)
3. **Expression input in the inline dimension editor**: reuse the variables
   panel's expression parsing so "22/2 + 0.15" and variable refs evaluate;
   store as `valueExpr` exactly like RPC-added constraints do.
4. **Dimension-on-create**: while a draw tool is live, typing digits opens a
   small value field; Enter applies it as a driving dimension on the entity
   just drawn (line length / circle radius / rect width,height in sequence).
5. **Label drag constrained to the dimension axis** (perpendicular offset
   only), so the indicator stays aligned with what it measures.
6. **Center rectangle** (center + corner, Ctrl toggles mode or its own
   toolbar entry; decomposes to 4 lines + symmetric constraints).

Each lands with GUI tests; 1–3 first since they compound.

## Phase 4 — MCP/interface round (~1 session)

1. `list_elements` enrichment: per-edge kind/length/radius, per-face
   kind/area; optional `part` name/id on list_elements + measure
   (kills the set_active_part round-trips).
2. **Assembly interference**: extend the I4 check to occurrence pairs
   (transform part bodies by DisplayMatrix, same as assembly measure).
3. **Mate error messages**: name the failing connector and suggest near-miss
   element names ("6/g1;M6 not found; 6/g1;M6;M7 exists — modified by
   feature 7").
4. `export_stl` watertightness report (shells + free-edge count in reply).
5. Workflow-prompt additions (cad-skill adoptions): FDM minimums, research
   real dims first, phased delivery + parameter-table closing.
6. **Expression-driven sketch entities (Decision 2A — committed, not
   stretch)**: `*_expr` on entity fields (line endpoints, circle/arc
   center+radius, ellipse radii), evaluated during regeneration against the
   symbol table exactly like feature exprs (must be hash-visible via the
   VisitExprs pattern so the regen cache invalidates). Format bump (v37 at
   time of writing) + golden corpus entry per policy; GUI shows expr-driven
   values read-only in the sketch editor v1; RPC/MCP accept the fields on
   add_feature/modify_feature.

## Phase 5 — Headless preview (stretch, ~1 session)

Offscreen render fallback so screenshot works without a GUI instance
(llvmpipe/lavapipe swiftshader-style device, or a minimal STL rasterizer like
cad-skill's). Unblocks agent/CI visual verification; sized last because
dev-watch covers the interactive case today.

---

## Suggested approval shape

Phases 0–2 are low-controversy (chore + bug fixes + already-signed-off S3).
All phases approved 2026-07-19. Remaining in-flight choices are
implementation-level (center-rectangle activation, headless-preview backend)
and will be decided in place.
