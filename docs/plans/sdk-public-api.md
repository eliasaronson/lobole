# The Python SDK as a public API

## The problem

Making the repository public freezes the SDK's names: the package and its
modules, the client class, the exception classes, and every key a method
takes or returns. After that, a rename breaks other people's scripts. Today
those names are whatever was convenient inside one repository:

- **Not a package.** Ten top-level modules in `tools/` (`designer_remote`,
  `designer_sdk`, `designer_contracts`, `designer_rpc`, …). `pyproject.toml`
  ships three of them under the placeholder name `designer-mcp`. The other
  seven, the SDK included, ship nowhere. 65 scripts in `generators/`,
  `examples/` and `benchmarks/` reach them with `sys.path.insert`.
- **Three unrelated exception roots.** `RemoteError` (`designer_remote.py`),
  `ContractError` (`designer_contracts.py`) and `SdkError`
  (`designer_sdk.py`) all derive from `RuntimeError` and from nothing else in
  common. A script that wants to catch "refused" has to catch all three and
  then switch on `e.diagnostic["code"]`. That holds even though the server
  already sends a structured `code`
  ([structured errors](../scripting/rpc.md#structured-errors)). The client's
  preflight refusal and the server's refusal of the same call raise
  different classes.
- **The largest surface is untyped.** `add_feature` takes `feature:object`.
  Each feature type's fields exist only as prose in `FeatureTypes()`
  (`src/core/io/SerializeFeatures.cpp`), and generators build these dicts by
  hand more than anything else. Results are typed for the two inspection
  methods only. Every other stub in `designer_rpc.py` returns
  `dict[str, Any]`.
- **Result keys break the units rule.** `rpc_units_gate` checks parameter
  names only. So `mass_properties` returns `volume`, `surface_area` and
  `center_of_mass` with no unit beside `mass_g`. `add_mate` still accepts a
  unitless `rotation` (radians) beside `rotation_deg`.
- **Private helpers used as public.** Five bike generators import `_id`
  from `designer_sdk`. Four generators copy the same "find a part by name in
  `list_parts`" loop. `generators/_lib.py`'s `plane_at_*` duplicates the
  SDK's `Plane`.
- **No API version.** `describe` versions the document format and the
  inspection contract, but not the RPC surface.
- **The MCP tool schemas are hand-written** Python signatures, separate from
  the contracts the server validates against. They have drifted:
  `modify_feature` and `delete_feature` take only `index`, although the
  contract prefers `feature_id`, and `set_material` requires `material`
  where the contract makes it optional.

## The shape

Seven stages. Each one lands and is verified on its own. Stages 1-4 are the
ones that freeze names, so they come before the release. Stages 5-7 deepen
the typing behind names that are already settled.

### 1. One package, `lobole`

The modules move to `python/lobole/`, and each loses its `designer_` prefix:

| module | from |
|---|---|
| `lobole.client` | `designer_remote` (`Designer` → `Client`) |
| `lobole.sdk` | `designer_sdk` (`App`, the handles, `Plane`, `V`) |
| `lobole.contracts`, `lobole.rpc` | `designer_contracts`, generated `designer_rpc` |
| `lobole.inspection`, `.snapshots`, `.components`, `.validation`, `.replay` | the matching `designer_*` |
| `lobole.mcp` | `designer_mcp` |
| `lobole.errors` | new, stage 2 |

`lobole/__init__.py` re-exports `Client`, `App` and the error classes, and
the package carries `py.typed`. In `pyproject.toml` the distribution is
`lobole`, the console scripts are `lobole-mcp` and `lobole-remote`, and
`package-dir` points at `python/`.

In-repo callers stop editing `sys.path`. `shell.nix` exports `PYTHONPATH`
with `python/` on it, and every Python test in `tests/CMakeLists.txt` gets
the same through its `ENVIRONMENT` property. `test_conventions.cpp` then
refuses a new `sys.path.insert` that names `tools`. Scripts that are not
library code (`generate_rpc_contracts.py`, `headless-preview.sh`, the
gates) stay in `tools/`. The import rewrite is mechanical, about 200 files,
and it is one commit.

### 2. One error hierarchy

`lobole.errors`:

```
LoboleError(RuntimeError)
├── ConnectionLost             the socket dropped and the retry failed
├── Refused                    .code .path .param .expected .observed .accepted .call
│   ├── MissingArgument        code missing_argument
│   ├── UnknownArgument        code unknown_argument
│   ├── InvalidArgument        code invalid_argument
│   └── StaleRevision          code stale_revision
├── InternalError              code internal: the handler fell over
├── NonFiniteError             a NaN/inf in a request
└── SdkError                   misuse caught client-side (a handle from another document, …)
```

`Client.request` maps a response's `diagnostic.code` to its class.
An unknown code becomes plain `Refused`, so a server that is newer than the
client still raises something catchable. `lobole.contracts`' preflight
raises the class the server would raise. The inspection methods' own codes
(`stale_snapshot`, `budget_exceeded`) become `Refused` subclasses of their
own.

### 3. Public helpers for what generators already use

- `lobole.sdk.ref_of(handle)` is the public form of `_id`.
- `App.part(name)` looks a part up by name and refuses an ambiguous name, as
  the server does.
- `generators/_lib.py`'s `plane_at_*` give way to `Plane`.
- The five bike generators and the four lookup loops move onto these.

### 4. An API version, and units on result keys

`describe` gains `api_version`, an integer that is raised when a public
method or key is removed or renamed. Each row also gains `since`, the
version that added it. `lobole.rpc` is generated with the version it was
generated against, and `App` warns once when the app reports a different
one. This is the same pattern as the inspection contract's hash check.

Before the first release the version is 1 and nothing is deprecated. Every
rename below simply happens, because no one outside this repository calls
these methods yet:

- `add_mate` loses the unitless `rotation`.
- `rpc_units_gate` extends to result keys named in each row's `returns`
  column. The keys it finds (`volume` → `volume_mm3`, `surface_area` →
  `surface_area_mm2`, `center_of_mass` → `center_of_mass_mm`, and those
  in `get_state`'s `bodies[]`) are renamed, and every caller in the repo is
  updated in the same commit.

### 5. Result contracts

The `returns` column already reads almost like the grammar
(`bodies[]{body, name, volume_mm3, …}`). It becomes the grammar: the
request grammar plus `{key:type, …}` objects. `generate_rpc_contracts.py`
emits a result `TypedDict` for each method and types the stubs' returns.

Coverage starts with the methods already typed on the request side, and
then CAM. `rpc_contracts_smoke` validates each contracted method's real
response against its schema, so a result cannot drift from its row.

### 6. Typed feature specs

Each feature type's parameter column in `FeatureTypes()` becomes the
contract grammar. `add_feature`'s `feature:object` becomes a union tagged on
`type`, which the server validates per type, as it already does for a typed
row. The generator then emits `ExtrudeSpec`, `RevolveSpec`, … and a
`FeatureSpec` union keyed on `type: Literal[...]`. `mcp_formats_gate`'s
prose cross-check gives way to generating `FEATURE_FORMATS` from the same
table.

### 7. MCP schemas from the contracts, and mypy over the generators

The MCP tools' input schemas are built from `describe`'s
`request_schema`. The hand-written Python stays only as each tool's
description and for the few tools that shape a result (screenshots become
images). `rpc()` calls the app directly, so the wrapper fallback in
`designer_mcp.py`'s `rpc()` goes, and with it the drift in `modify_feature`,
`delete_feature` and `set_material`.

The hand-written half of `lobole.sdk` gets return annotations. A new gate
runs mypy over `generators/` against the package: the generators are the
largest real user of the API, so they are where wrong types show.

## Files it touches

| stage | files |
|---|---|
| 1 | `tools/designer_*.py` → `python/lobole/`, `pyproject.toml`, `shell.nix`, `tests/CMakeLists.txt`, every importer (~200), `tests/packaging_smoke.py`, `tests/test_conventions.cpp` |
| 2 | `python/lobole/{errors,client,contracts,sdk}.py`, `tests/rpc_contracts_smoke.py` |
| 3 | `python/lobole/sdk.py`, `generators/_lib.py`, `generators/{bike,chassis,counter,chess,grill}/` |
| 4 | `src/remote/RemoteDispatch.cpp` (`describe`), `src/remote/RemoteHelpers.h` (`MethodDoc.since`), `src/remote/Remote*.cpp` result keys, `tests/rpc_units_gate.py`, callers of the renamed keys |
| 5 | `src/remote/RpcContract.{h,cpp}`, `MethodDoc` `returns` columns, `tools/generate_rpc_contracts.py`, `python/lobole/rpc.py`, `tests/rpc_contracts_smoke.py` |
| 6 | `src/core/io/SerializeFeatures.cpp` (`FeatureTypes()`), `RemoteModeling.cpp`, the generator, `tests/mcp_formats_gate.py` |
| 7 | `python/lobole/mcp.py`, `python/lobole/sdk.py`, a new `tests/generators_typing_gate.py` |

Docs: `docs/scripting/{sdk,rpc,mcp}.md`, `docs/packaging/README.md#the-agent-surface`,
`CONTRIBUTING.md`, `README.md`'s scripting example, and the MCP setup line
(`claude mcp add lobole -- lobole-mcp`).

## How it is verified

- **1.** `packaging_smoke` builds a wheel (`pip wheel --no-deps`) into a
  temporary directory. With the source tree off `sys.path`, it imports every
  module from the wheel and runs `lobole-remote --help`. It has to fail when
  any module is left out of the package. The full headless suite, the GUI
  suite and `tools/ctest -L pygui` stay green after the import rewrite, and
  `tools/ctest -C generators -L generators -j2` rebuilds the big generators
  unchanged.
- **2.** `rpc_contracts_smoke` already checks that the preflight diagnostic
  equals the server's. It also asserts the same exception class, once per
  code. A test catches `lobole.Refused` around a refused call through each of
  `Client`, `App` and the MCP `rpc` tool.
- **3.** The rewritten generators produce the same volumes and the same
  exported STEP bounding boxes as before.
- **4.** `rpc_units_gate` fails before the renames and passes after.
  `describe` carries `api_version`, and `sdk_smoke` sees the warning against
  a stubbed mismatch.
- **5.** `rpc_contracts_smoke` fails a response that lacks a declared key or
  carries a wrong type. Seed one to see it fail.
- **6.** Each feature type's contract refuses a missing field, a wrong type
  and an enum miss through `add_feature`, with the diagnostic and no change
  to the document. `tools/fuzz.py run --count 200` stays clean.
- **7.** `mcp_smoke` drives each tool through its generated schema, including
  `rpc('set_material', {'body_name': …})` and `modify_feature` by
  `feature_id`. The generators typing gate fails on a seeded wrong key and
  passes on the tree.
