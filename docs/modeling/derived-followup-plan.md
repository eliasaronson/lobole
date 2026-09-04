# After the gripper: the follow-up plan (2026-09-03, done 2026-09-04)

> **Status: done.** P1 `ab273297`, P2 `82294840`, R+I `1e0ff834`, I3 (v85)
> below. Two items turned out not to be defects and were dropped rather
> than "fixed": see the last section.

Everything [derived-gripper-dogfood.md](derived-gripper-dogfood.md) left
open, plus the two load findings measured after it (a part tab
re-tessellates all its bodies on every switch; the project itself opens in
1.3 s). Ordered by what it cost in the dogfood. Each group is one commit
with the suites green; the measurements that justify each item are in the
record.

## P — performance (hours in the dogfood)

| # | what | fix | measure after |
|---|---|---|---|
| P1 ✅ | a part tab rebuilds every body's mesh on every sync (2.2 s per switch to a 200-body part); the in-context ghost sync tessellates every neighbour per regeneration | one cache keyed by shape identity, shared by the part tab, the ghosts, the assembly sync and the ray tracer; entries are shared_ptr-owned so trimming never drops geometry something is drawing | **return to a 202-body part 2.6 s → 0.60 s** (sync 2355 → 84 ms); assembly first sync 2850 → 1036 ms. A FIRST switch still pays 1.8 s of tessellation — persisting meshes beside the baked B-Reps is the next step |
| P2 ✅ | `interference {}` and `contact_report` over 253 occurrences never returned and blocked `ping` | box prefilter (interference had none), closest-first order, `max_pairs` 4000 AND `deadline_s` 20 s, with `pairs_checked`/`box_cleared`/`unchecked`/`elapsed_s` and a truncation line always in the response; interference reports clashes only, as its doc already promised | **arm: 23 s and 123 s, from never returning** (28,203 of 31,878 pairs box-cleared). The gripper's own 8 occurrences complete untruncated in 0.6 s / 4.6 s. A single boolean between two imported servos measured 100 s, so the deadline bounds starts, not one call — hence `elapsed_s` |
| P3 ✅ | `list_elements` without `body` on 17 k faces: 172 s | bounded by `max_faces` (4000) counted over the faces it would DESCRIBE — each is an OCC surface analysis; the refusal names `body` and the count | refuses instead of running for 3 minutes |

## I — import (silent breakage, lost names, the 197-body camera)

| # | what | fix |
|---|---|---|
| I1 ✅ | `import_step as_assembly` left 12 of 97 parts broken and said nothing | the response carries `ok`, `parts` and `failed: [{part, feature, error}]` — verified on the arm: `ok=false`, 12 named |
| I2 ◐ | product names lost for some nodes ("SOLID 6" for `Wrist_Roll_Follower_SO101`, `=>[0:1:1:28]` for occurrences) | the `=>[entry]` half is fixed: that is an XCAF pointer marker, not a name, and it was going straight into occurrence names. The "SOLID" half is XCAF's own label name — reaching past it means reading the STEP product structure directly, which is its own change |
| I3 ✅ | Derived's `bodies` filter is by `created_by`; an Import's 197 bodies are one | `body_names` (v85, golden `v85-derived-by-name`, history row); both filters narrow together and a filter selecting nothing says so. Verified on a 131-body adapter STEP |

## R — RPC contracts (each cost one detour)

| # | what | fix |
|---|---|---|
| R1 ✅ | `add_occurrence {part: <id>}` → `type must be string` | accepts a slot id as well as a name |
| R2 ✅ | `add_drawing_view` documents `drawing`, refuses it | accepts it, switching to that tab |
| R3 ✅ | `add_feature Sketch` refuses `external` | it refuses an EMPTY one: the strict check round-trips the built feature and an empty list serializes back to nothing. A key the type's documented dialect names is now accepted when empty, and the Sketch dialect describes `external` and `face_ref` |
| R4 ✗ | a cylinder face's `normal` is the normal at the centroid | **not a defect.** `list_elements` already reports `axis` on cylindrical/conical/toroidal faces; I had read the wrong field. Record corrected |
| R5 ✅ | `delete_feature` takes `index`, every other verb an id | accepts `id` too |
| R6 ✗ | autosave re-saved an unchanged 200-body part every minute | **not a defect.** The loop already skips a document whose version has not moved (`Autosaver::NeedsSave`); what I saw was documents that had genuinely changed |

## Checked and dropped

R4 and R6 above: both were mine to get wrong, and the fix for a
non-defect is a corrected record, not code.

## Not in this plan

- Inspection sweeps as jobs on the A3 registry (so `ping` answers during
  one), and a cheaper contact check between two large imports. P2 made the
  sweeps bounded and honest; the numbers it now reports (22 pairs checked
  of 881, one boolean at 100 s) are the argument for both.
- Persisting tessellations beside the baked B-Reps, so the FIRST switch to
  an imported part is not 1.8 s of meshing (P1 covers every switch after).
- Reading STEP product names past XCAF's generic labels (I2's other half).
- A sketch with crossing loops (the flange disc and web) — a legitimate
  refusal.
- The vendor STEP's own naming (`ATMEL_ATSHA204A_8UDFN` for the lens).
