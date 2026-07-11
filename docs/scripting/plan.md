# Scripting & the LLM interface — plan

Goals 3 + 5 of the [roadmap](../roadmap.md): expose the command layer as an API
so humans and LLMs are genuinely equivalent clients, and every scripted session
is a replayable test case.

## Current state

Shipped early: `--remote <port>` serves JSON-RPC — semantic feature/document
commands (create sketch, add entities/constraints, extrude, query) **plus**
human-parity UI actions through the ImGui test engine **plus** live
screenshots. `tools/designer_remote.py` is the client. This is already the
hard part; what remains is packaging and reach.

**Standing rule** (already the architecture — enforce in review): every new
capability lands in the command layer first, GUI second. A feature an LLM
can't drive is half-shipped.

## What the LLM-CAD ecosystem teaches

Surveyed the [text-to-cad CAD skill](https://github.com/earthtojake/text-to-cad)
(a Claude skill for CAD generation). It picks **build123d** (Python, OCC-based,
code-as-source-of-truth) — *not* OpenSCAD — and its prescriptions map almost
1:1 onto what we've built:

- **Source of truth is code/text, never exported geometry** — ours is the
  document format + command stream. ✓ by architecture.
- **Mandatory geometric verification** (`scripts/inspect`: measurements,
  positions, stable selector refs like `#o1.2.f1`) — our equivalent is query
  RPCs + stable feature/entity ids; the gap is richer queries (measure, mass
  properties, bounding boxes → [inspect/plan.md](../inspect/plan.md) I2 should
  expose `mass_properties`/`measure` over RPC).
- **Mandatory visual verification** ("ALWAYS snapshot after every update") —
  our screenshot RPC. ✓. Add camera/framing control (`look_at`, `fit`,
  standard views) so an agent can compose a useful snapshot.
- **Parameters-first modeling** (named parameters, design intent in names) —
  exactly [variables/plan.md](../variables/plan.md); expressions over RPC are
  a prerequisite for good LLM ergonomics.

**On OpenSCAD support**: not worth it as an interface. Its advantages (LLMs
know the language from a huge corpus; text-first) are outweighed by the
mismatch: OpenSCAD is CSG-over-meshes — no B-Rep, no sketches/constraints, no
fillets-on-edges, no STEP — and it doesn't map onto our feature/constraint
document, so "supporting" it means writing a CSG evaluator that produces dumb
geometry outside the parametric history. The ecosystem's own answer (build123d)
confirms the direction: a **Python API over a real B-Rep kernel** beats a
familiar-but-weak language. LLM familiarity is instead earned with a good MCP
surface + docs + the inspect/snapshot loop. (A one-way OpenSCAD *importer* —
evaluate CSG into a body — could be a community feature someday; not planned.)

## Milestones

### S1 — MCP server — SHIPPED — [design: s1-mcp.md](s1-mcp.md), [usage: mcp.md](mcp.md)

Shipped 2026-07-10: `tools/designer_mcp.py` (28 tools, inline screenshot/
render images, `modeling-workflow` prompt), the `view` standard-views RPC,
and the `mcp_smoke` bracket test. Driving sketches over RPC also surfaced and
fixed two core bugs: expression-only constraints solved with value 0, and
profile wires silently dropping arc edges on solver float noise.
`tools/designer_mcp.py`: an MCP server (official Python SDK, stdio transport)
wrapping the JSON-RPC client — tools mirroring the RPC surface
(`create_sketch`, `add_line`, `add_constraint`, `extrude`, `query_document`,
`set_variable`, `screenshot`, …) with good descriptions + a usage prompt
(the text-to-cad skill is the template: brief → build → inspect → snapshot →
iterate). Smoke test: an agent models a dimensioned bracket end-to-end.

### S2 — Headless mode — SHIPPED — [design: s2-headless.md](s2-headless.md)

Shipped 2026-07-10: `--headless --remote <port>` runs the document engine +
remote server with no window/GPU/display; GUI-dependent methods degrade with
specific errors. Landed with inspection I2 (`mass_properties` with a material
density table) plus `list_elements` (stable face/edge names → agents drive
Fillet/Chamfer), `tests/headless_smoke.py`, and the first CI
(`.github/workflows/ci.yml`: build + `ctest -LE gui` on ubuntu-24.04).
`--headless`: run `designer_core` + `CADKernel` with no window/GPU — execute a
command script or serve RPC. Geometry ops need no Vulkan already (unit tests
prove it); the work is a startup path that skips renderer/ImGui init and a
degraded-capability answer for `screenshot` (error or offscreen render later).
Payoff: geometry tests + scripted regeneration on CI with no virtual display.

### S3 — Python package — SHIPPED (v1, 2026-07-11)
Client-first, not embedded: a `designer` Python package wrapping the RPC in a
pythonic API (`doc.sketch(plane="XY").line(...).rect(...); doc.extrude(sk,
depth="width/2")`), usable from any REPL/notebook. Same interface as the LLM —
which is the point (goal 5). An *embedded* interpreter (pybind11) only if
in-process latency or GUI-blocking interactivity ever demands it; it's a build
and API-stability cost we don't need to pay to get 100% of the capability.

### S4 — Recorded sessions as tests — SHIPPED (v1, 2026-07-11)
Record the command stream of a session to a file; replay headless and assert
the resulting document/geometry. Every reported bug becomes a replayable
regression test; ties into [versioning/plan.md](../versioning/plan.md)
checkpoints (a checkpoint is a point in exactly this stream).

## Recommended order

S1 (cheap, immediately useful for AI-assisted development of Designer itself)
→ S2 (unlocks CI) → S3 → S4. Do after variables V1 so the API speaks
expressions from the start.
