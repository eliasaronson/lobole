# Typed inspection and measurement snapshots

The inspection pilot adds two methods: `geometry_snapshot` and
`validate_snapshot`. Other RPCs retain their current dynamic discovery and
validation. The broader rollout is in [the SDK iteration plan](../inspect/sdk-iteration-plan.md).

```python
from designer_sdk import connect

app = connect(port=8855)
part = app.part("cylinder head")
snapshot = part.body().snapshot(circular=True)
for edge in snapshot.edges:
    print(edge.center, edge.radius, edge.ref)  # zero RPCs
snapshot.validate()                         # one explicit freshness check
new_snapshot = snapshot.refresh()            # one new measurement request
```

Snapshots are frozen records. Coordinates are in **part space**, lengths in mm.
Centers of circles/arcs are their centers, not points on their rims; other
curves use the existing measurement anchor convention. `ref` returns a fresh
reference dictionary, so changing it does not change the record. Unrequested
measurements and inapplicable radii are `None` on SDK records and absent on the
wire. Existing `Edge.center` keeps its live-query behavior.

A snapshot requires a stable `part_id` and explicit body index. It never switches
tabs and does not accept a slot index as a substitute for a part ID. A process
nonce, document instance ID and document version identify its revision. Reopening,
replacing or regenerating the part invalidates it. This is conservative: even a
regeneration that reuses cached geometry can invalidate it. Moving an occurrence
does not change the part-local coordinates. World-space snapshots are not yet
supported.

Validation is a point-in-time check, **not an atomic mutation guard**. Snapshot
references can still be passed to existing modeling operations, which resolve
against the then-current document. Guarded writes and semantic re-resolution are
later work; do not use a prior `validate()` call as a lock. `refresh()` uses the
same body index in the current part; inspect the new results if body ordering
changed.

The request bounds edge analysis before doing it: `max_elements` defaults to
2,000, capped at 10,000 edges scanned per body, including edges excluded by the
`circular` filter. Larger bodies are refused with `budget_exceeded`; this pilot
does not paginate or silently return partial measurements. Faces, fields beyond the four published edge measurements, and occurrence-space
measurements remain outside this pilot.

## Static and runtime checks

```python
# app.inspection is a generated, statically typed facade.
result = app.inspection.geometry_snapshot(
    part_id=part.id, body=0, circular=True, fields=["center", "radius"])
app.inspection.validate_snapshot(revision=result["revision"])

# Still available: discovered dynamic RPCs, or an explicit raw method name.
result = app.geometry_snapshot(part_id=part.id, body=0)
result = app.call("geometry_snapshot", part_id=part.id, body=0)
```

The facade catches missing arguments, misspelled fields, incorrect types and
unknown methods during type checking. Booleans-as-integers, non-finite numbers,
ranges, nested keys and stale revisions need runtime validation. Client preflight
runs before network writes; the server independently validates raw requests.
JSON Schema integer semantics accept integral JSON numbers such as `0.0`.

`api/inspection-contracts.json` is the source for the embedded server schemas
and generated Python request/result records and facade. Run
`python3 tools/generate_inspection_contracts.py` after editing it. The generated
files are checked in; CI checks they match. `describe` publishes the schemas and
an `inspection_contract` version, SHA-256 hash and dialect independently of the
project file version. The typed facade checks version and hash before sending a
request, so it cannot silently cast another server's result into local types.
An incompatible server raises `unsupported_contract`; update the SDK/server
pair or use explicit dynamic/raw calls with the live schema. No document format
changed.

Supported schema keywords are `type`, `properties`, `required`,
`additionalProperties: false`, `enum`, `minimum`, `maximum`, `minLength`,
`items`, `minItems`, `maxItems`, `uniqueItems` and `default`. The generator
rejects unsupported keywords rather than publishing unenforced constraints.
This subset covers wire structure; it does not establish engineering tolerances,
dimensional consistency or manufacturability.

Server errors remain readable `RuntimeError` subclasses (`RemoteError`) and carry
an optional `.diagnostic`. Client preflight raises `ContractError` with the same
shape: `operation`, `code`, `path`, `expected`, `observed`. Existing RPC errors
may still have no diagnostic. Batch failures preserve nested diagnostics,
including when the batch rolls back. No message-text parsing is required for
these new endpoints.

Run the generated-file check, static positive/negative fixtures and real-server
integration tests with:

```sh
nix-shell shell.nix --run "cd build && ctest -R 'inspection_' --output-on-failure"
```
