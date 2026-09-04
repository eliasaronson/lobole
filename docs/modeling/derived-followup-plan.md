# After the gripper: the follow-up plan (2026-09-03)

Everything [derived-gripper-dogfood.md](derived-gripper-dogfood.md) left
open, plus the two load findings measured after it (a part tab
re-tessellates all its bodies on every switch; the project itself opens in
1.3 s). Ordered by what it cost in the dogfood. Each group is one commit
with the suites green; the measurements that justify each item are in the
record.

## P — performance (hours in the dogfood)

| # | what | fix | measure after |
|---|---|---|---|
| P1 | a part tab rebuilds every body's mesh on every sync (2.2 s per switch to a 200-body part, 2–5 s per edit); the in-context ghost sync tessellates every neighbour per regeneration | a tessellation cache keyed by shape identity (TShape + location), shared by the part tab and the ghost sync; unchanged bodies reuse their mesh, edges and picking data — upload only | switch to Gripper body, edit in the jaw with it as a neighbour |
| P2 | `interference {}` and `contact_report` over 253 occurrences never returned and blocked `ping` | a bounding-box prefilter (with `near_tol`) before any boolean, and a per-pair budget so the sweep is bounded and says what it skipped | both RPCs on the full arm |
| P3 | `list_elements` without `body` on 17 k faces: 172 s | say so: the doc names `body`; a part over a face budget refuses without it and names the count | — |

## I — import (silent breakage, lost names, the 197-body camera)

| # | what | fix |
|---|---|---|
| I1 | `import_step as_assembly` left 12 of 97 parts broken and said nothing | the response carries `failed: [{part, error}]` and `regen.ok` |
| I2 | product names lost for some nodes ("SOLID 6" for `Wrist_Roll_Follower_SO101`) | prefer the PRODUCT name when the shape label is a generic "SOLID n" |
| I3 | Derived's `bodies` filter is by `created_by`; an Import's 197 bodies are one | a `body_names` filter (format bump, golden, history row, MCP/RPC docs) |

## R — RPC contracts (each cost one detour)

| # | what | fix |
|---|---|---|
| R1 | `add_occurrence {part: <id>}` → `type must be string` | accept a slot id as well as a name |
| R2 | `add_drawing_view` documents `drawing`, refuses it | accept `drawing` (name or id) and switch to that tab |
| R3 | `add_feature Sketch` refuses `external`; `describe` never says `project_geometry` | accept `external` in the feature JSON; the Sketch row names both forms |
| R4 | a cylinder face's `normal` is the normal at the centroid | `axis` (and `axis_point`) on cylindrical, conical and toroidal faces |
| R5 | `delete_feature` takes `index`, every other verb an id | accept `id` too |
| R6 | autosave re-saved an unchanged 200-body part every minute | skip a document whose version has not moved since its last autosave |

## Not in this plan

- Inspection sweeps as jobs on the A3 registry (so `ping` answers during
  one). P2's prefilter first; if the bounded sweep is still minutes, that
  is the next design.
- A sketch with crossing loops (the flange disc and web) — a legitimate
  refusal.
- The vendor STEP's own naming (`ATMEL_ATSHA204A_8UDFN` for the lens).
