# Typed contracts beyond the inspection pilot

## Problem

Two RPCs — `geometry_snapshot` and `validate_snapshot` — have
machine-readable request and result schemas (`api/inspection-contracts.json`,
[docs/scripting/rpc.md](../scripting/rpc.md#typed-inspection-contracts)).
Every other RPC is described by a `MethodDoc` row: parameter keys and a
`returns` string, but no types, no enums per field, no required/optional
split, and no result schema. So:

- A wrong enum, a missing field, a bool or NaN where a dimension belongs, or
  a conflicting `radius`/`diameter` pair is caught only by the handler, often
  after work has started, and a raw client gets no structured `diagnostic`.
- The SDK checks keyword names, not types; a type checker sees `Any`.
- Snapshots cover part-space edges only: no faces, no world space, no
  pagination, and nothing ties a snapshot's revision to a later write, so a
  modelling call made with a stale snapshot's references resolves against
  whatever the document is now.

## Fix

1. **Extend the contract file domain by domain**, starting with what
   generators use most: sketch and extrude inputs, feature variants (a tagged
   union on `type`), `measure`, `fit_fastener`, `interference`. Inventory each
   handler's actual required/optional keys, result shape and units first, and
   change behaviour only where the inventory finds a contradiction. The
   generator keeps rejecting schema keywords it cannot enforce; cross-field
   rules stay named semantic validators on the server.
2. **Structured errors everywhere a contract exists**: stable `code`, field
   `path`, expected and observed, distinguishing invalid input, stale
   reference, conflict, kernel failure and incomplete check.
3. **Generated SDK stubs** for every contracted method (`TypedDict` records,
   `Literal` enums, overloads for mutually exclusive forms), with positive and
   negative type-check fixtures. Uncontracted methods stay callable
   dynamically and are never advertised as statically checked.
4. **Snapshots**: faces; world space for an occurrence (invalidated by
   placement, which part space is not); pagination that cannot mix revisions
   across pages; and an optional `expected_revision` on writes that take
   element references, refusing when the part changed since the snapshot.
5. Keep `rpc_params_gate` as the vocabulary gate while migrating; the
   contract replaces a row's `params` only once the gate checks the two
   agree.

## Files

`api/inspection-contracts.json` (or a sibling per domain),
`tools/generate_inspection_contracts.py`, `src/remote/InspectionContracts.h`,
the domain handlers in `src/remote/`, `tools/designer_sdk.py`,
`tools/designer_snapshots.py`, `tests/inspection_contracts_smoke.py`,
`tests/inspection_typing_gate.py`.

## Verification

- For each contracted method: a wrong enum, a missing field, a bool or NaN
  dimension and a malformed nested value fail before any mutation or kernel
  work, over the raw RPC as well as the SDK, with the same diagnostic.
- Valid handler responses validate against their result schemas (a smoke
  that calls each contracted method and checks the answer).
- Type-check fixtures: invalid calls fail for the intended reason.
- A write carrying a stale `expected_revision` is refused and changes
  nothing; matching N hole rims takes a bounded number of snapshot requests
  and zero RPCs in the matching loop.
- The MCP benchmark is re-run before claiming the change helps agents
  ([docs/scripting/benchmark.md](../scripting/benchmark.md)).
