# The regeneration track — failure UX, rollback bar, incremental regen

Three changes to the same organ (`Document::Regenerate`), designed together
so the surgery happens once. R1 is overdue by this plan's own gate (fillets
and up-to-face shipped first); R3 is the scaling limiter and assemblies
prerequisite #1.

## Grounded in code (Jul 2026)

- `Document::Regenerate()` (`Document.cpp:64`): clears bodies, bumps the
  version, evaluates expressions (`VisitExprs`), executes features in order,
  and **returns false at the first failure** — everything downstream of a
  broken fillet vanishes. Callers (commands, editors, RPC) mostly ignore the
  bool; nothing depends on stop-at-first semantics.
- Features already carry `m_error` (`SetError`/`GetError`), shown in the
  feature tree and, since S2, in `get_state`.
- `Body` snapshots are cheap: `TopoDS_Shape` is refcounted; `ElementMap` is
  two `std::map<int,std::string>`.
- Format `kVersion = 18`; the F1 policy binds every bump to a golden file +
  round-trip test.

## R1 — Failure UX (continue past failure + suppress) — format v19

- **Continue-past-failure**: a failed feature records its error and
  contributes nothing; regeneration proceeds. Downstream features see the
  bodies as of the failure — deterministic because execution order is the
  feature order and a failure has no side effects on `ctx.bodies` (features
  build first, mutate `ctx` last; audit the few that interleave). A feature
  whose *reference* is gone (missing sketch/body/element) fails on its own
  with its specific error. `Regenerate()` returns "no feature errored".
- **Suppress**: `Feature::suppressed` (serialized v19, default false; golden
  `v18-end-conditions.json` guards the bump). Suppressed features skip
  execution, clear errors, and render dimmed with "(suppressed)". A
  suppressed feature whose outputs others reference makes *those* fail with
  their normal missing-reference errors — same rule as deletion, no special
  case.
- **Surfacing**: feature tree already shows error icon + tooltip; add a
  context-menu Suppress/Unsuppress. RPC `suppress_feature {id, suppressed}`;
  `get_state` features gain `"suppressed": true` (error field exists).
- **Repair path** stays what it is today (edit params / re-pick refs via the
  editors); a guided reattach dialog is future UI work, not in this track.

## R2 — Rollback bar

- `Document::m_rollback` (index; -1 = end): `Regenerate` executes features
  `[0, rollback)`; the rest are skipped like suppressed ones but styled
  "grayed/after-marker" and keep their params untouched. Serialized v19
  (optional `"rollback"`) — SolidWorks persists the marker position; so do we.
- RPC `set_rollback {index}` (-1 = end); `get_state` gains `"rollback"`.
  Feature tree: context menu "Rollback to here" / "Roll to end"; features
  after the marker render dimmed with a divider line.
- Insert-at-marker is v2 (needs downstream-reference validation — plan F4
  note), not in this track.

## R3 — Incremental regeneration (prefix cache)

Chain-invalidation model, deliberately conservative:

- After expression evaluation, compute per feature `h_i = H(h_{i-1},
  FeatureToJson(f_i), suppressed_i)` — the serialized form is exactly the
  state that determines outputs (F1's contract), and chaining makes any
  upstream change invalidate every later feature. Editing feature k re-runs
  `k..N` only; editing the last fillet stops re-running the whole history.
  Precise dependency-graph invalidation (only true dependents) is v2.
- Cache: per index, `h_i` + a **snapshot of the body list after executing
  feature i** (shape handles + element maps + names — cheap copies).
  `Regenerate` finds the first index whose hash differs, restores snapshot
  `i-1`, and executes from there; snapshots after the divergence are
  replaced. Rollback/suppress states are hash inputs, so R1/R2 compose free.
- Feature-internal mutated state (solved sketch entities, cached datum
  planes) is safe under chaining: a skipped prefix implies identical inputs,
  so the retained internal state equals what re-execution would produce.
  (This invariant is WHY chain invalidation was chosen over a dependency
  graph for v1.)
- `Document::RegenStats {executed, cached}` for tests and `get_state`, so
  the cache is observable, not vibes. Undo/redo get the same speedup for
  free (they regenerate).
- Invalidation hammer: `ClearRegenCache()` on document load/new (first run
  executes everything) and available to tests.

## Order, tests, risks

R1 → R2 → R3, one commit each. Tests per phase: continue-past semantics
(break a mid-history feature, assert later independent features still
produce bodies), suppress round-trip + reference-into-suppressed failure,
rollback prefix volumes + round-trip, cache stats (edit last feature →
executed == 1; touch nothing → executed == 0; edit first → all), and the
full suite + golden corpus green throughout. Biggest risk is R3 correctness
vs. hidden feature side effects — mitigated by the chain invariant above,
the 268-test suite, and RegenStats making staleness visible.
