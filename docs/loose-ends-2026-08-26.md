# Agent-friction round — plan (2026-08-26)

> Status: **PLANNED.** Source: one end-to-end dogfooding day over the new
> `--offscreen` + MCP-autostart path — a Visaton W 170 S speaker taken from
> datasheet to parts, assembly, modal FEM, five manufacturing drawings and
> verified router G-code, entirely windowless. Every item below REPRODUCED
> in that session (the 08-25 round's lesson: three of its seven items were
> already fixed when planned — plan against what you watched happen, not
> against summaries). Defect details live in [known-bugs.md](known-bugs.md);
> this doc is the fix sequencing.
>
> Already fixed same-day, not in this plan: MCP `describe(method=…)` arg
> collision (22aa8bd), FEATURE_FORMATS documenting `add_feature`'s `id` and
> Pattern's body-only sources, tool-change M0 (verified already on by
> default and present in posted Mach3 output).

## Wave 1 — refusals, addressing, honesty — **SHIPPED 2026-08-26**

> bc4bbe9 (R1–R3), 65b447a (R4, R5, R16), and the R7/R8/R9 commit; R6
> retracted below. One meta-find from the round: rpc_gate_smoke's handler
> extractor keys on `if (m == "...") {`, so an inner braced dispatch on the
> method name SHADOWS the handler's body in the scan — the gate caught its
> own blind spot when cam_toolpath's return grew braces (fixed braceless,
> commented at the site).

- **R1 json-shape refusals in drawings.** `create_drawing {sheet: "A4"}`
  and a scalar `add_drawing_dim offset` die as raw `json.exception` —
  wrap the shape reads so each refusal names the accepted form
  ("sheet is an object: {size: a4|a3|…, landscape?, title_block?}").
  Audit RemoteDrawings for sibling `.at()` landmines. Gate: error-path
  asserts in `drawing_smoke.py`.
- **R2 `distance` line↔circle anchors on the silhouette.** A 144-centred
  hole dims as "170". Circle↔circle already measures centre-centre and
  `distance_x/_y` measure centres — make line↔circle do the same. Behaviour
  change: pin the new value in `drawing_smoke`.
- **R3 offscreen drawing-tab screenshot is silently blank.** Sheets are
  ImGui-drawn; the offscreen capture is scene-only by design — so refuse:
  "drawing sheets render in the GUI layer — export_drawing writes
  SVG/PDF, or use a windowed instance". Gate: `offscreen_smoke`.
- **R4 duplicate tab names accepted silently.** `add_part`/`add_assembly`/
  `create_drawing`/`rename_part` refuse a name that already exists (the
  merge machinery's duplicate-id conviction, one level up). Gate:
  `headless_smoke`.
- **R5 one part resolver.** `set_active_part`/`delete_part`/`rename_part`
  accept `{name}` OR `{index}` through one shared resolver (ambiguous name
  → refusal listing indexes); `add_occurrence` unchanged. Retires the
  name→index helper every dogfood script has grown. Gate: `headless_smoke`
  + MCP prose.
- **R6 — RETRACTED (2026-08-26, during the fix round).** `list_studies` has
  existed all along, covered by `fem_smoke` and `topo_smoke`; the dogfood
  session's discovery grep filtered method names on the substring "study",
  which misses the plural "studies", and the absence was then "confirmed"
  by brute-forcing ids instead of re-checking `describe`. The 08-25 round's
  lesson, reproduced in the planning direction: verify each premise against
  the code before planning a fix for it.
- **R7 drill ergonomics.** `type: drill` with `targets` derives top from
  the stock and bottom from the hole depth instead of refusing "drilling
  picks cylindrical faces, not a verb" (which fires even WHEN the caller
  passed cylindrical faces — the derive path drops drill targets). Gate:
  CAM smoke.
- **R8 client timeouts.** `designer_remote.request(..., timeout=)` per
  call; study/mesh guidance in describe docs names the async+poll path.
- **R9 top_z honesty.** `cam_toolpath` warns when an op's explicit `top_z`
  sits below the stock top ("rapids start inside stock — stock top is Z20")
  — the session lost a round to exactly this and the verifier's "line 11"
  message named neither the op nor the height. Gate: CAM smoke.
- **R16 refuse `.json` for multi-document saves** (known-bugs entry): the
  container form writes under any name, but the git vault keys on `.dsgn`
  — the W170S project saved as a directory *named* `w170s.json`, historyless,
  messages dropped. Refuse naming `.dsgn`; keep `.json` = single-document
  interchange, per format.md. Gate: `headless_smoke` + a format.md line.

## Wave 2 — agent-capability unlocks — **SHIPPED 2026-08-26**

- **R10 CAM picks for round openings — DONE.** Verified on the real W170S
  baffle: drill + both openings + outline all derived from picks, verify
  ok, no onion-skin twin. Gated by "a round opening is cut by picking its
  wall" in test_cam. Original finding: `FindFace` resolves every pick via
  `FacePlane`, so cylindrical walls fail before `IsWallFace` can classify
  them; "cut the opening" is impossible for the most common opening there
  is. Fix: a curved-wall path — for a cylindrical face with axis ∥ Z,
  build the WallContour from the face's own section circle (the machinery
  downstream is already shape-agnostic); keep the flat-face refusal for
  cones/frees. Retires the onion-skin-twin workaround for plain round
  holes. Gate: a CAM test that wall-picks Ø73 + Ø148 on the W170S baffle
  geometry and passes `cam_verify ok`.
- **R11 `execute_script` MCP tool — DONE** (gated by mcp_smoke: a
  scripted loop of boxes lands in the model through real MCP stdio).
  Original plan: Run a short python snippet in a
  subprocess with `designer_remote` importable and DESIGNER_PORT pinned to
  the attached instance; timeout + output caps; returns stdout/stderr.
  This is the loop-and-computed-args path for MCP-only agents (claude.ai,
  Desktop) — today it exists only via a local shell. Same trust domain as
  the `rpc` tool; the MCP host's approval gates it. Gate: `mcp_smoke`
  builds a small part through it.

## Wave 3 — drawing quality (1–2 sessions)

- **R12 dimension auto-placement** (defect entry in known-bugs): ladder
  placement outside the nearest view edge, skipping view bboxes, the title
  block, and labels already placed; `check_drawing` gains an overlap
  validator so this stays fixed. Five sheets today needed 2–4 manual
  offset passes each; the gate is the point, not the heuristics.
- **R13 notes + centre marks.** `add_drawing_note` (text, anchor, optional
  leader to a picked ref) for the "6× Ø4.4 on Ø175.8" class no dimension
  kind covers; automatic centre marks on view circles. One format bump
  covers both: kVersion + golden + format.md row (both gated).

## Wave 4 — modeling/CAM depth (larger, in this order)

- **R14 pattern of a cut — SHIPPED** (067ba26). The cut Extrude caches the
  prism it swept; the pattern replicates that cutter and subtracts each
  copy. Instance faces carry the source's names with a `#<k>` suffix, so
  the patterned holes are as referenceable as the first. The GUI pick is
  the CUT'S TREE ROW (a cut has no body to click); FEATURE_FORMATS' "a cut
  cannot be patterned" note is gone.
- **R15 CAM tool types — SHIPPED.** `cam::ToolType` (flat/ball/vbit/drill)
  + `tip_angle`, refused by name rather than defaulted; one `TipHeight`
  function is the tool's shape, and the VERIFIER simulates it. First
  consumer is the `edge_round` operation — "Break an edge" follows a
  wall's top edge, chamfering with a V-bit and rounding with a ball, and
  refuses a flat mill. A break the MODEL does not have gouges by
  construction, so the verdict names that cause instead of reading as
  scrap. Format v66 (with R13's notes), golden `v66-cam-tool-shape`.

## Deliberately not in this round

- Project name + public repo (gates PyPI, Cachix, Flathub — decision, not
  code) — [packaging/plan.md](packaging/plan.md) deliverable 4.
- The oracle-graded agent benchmark (seed it with THIS session's tasks).
- The W170S project's own follow-ups (driver rebate + re-posted G-code,
  terminal cup, damping, configurations for Visaton's 20 l/50 l variants)
  — scratch project work, on request.
- MSAA setting + subassembly-hide rebuild (parked in known-bugs from the
  GPU round).
