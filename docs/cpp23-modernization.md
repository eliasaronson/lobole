# C++23 modernization + spdlog-everywhere

Status: **the scheduled work is COMPLETE** — see the sequencing table at the
bottom, which is the authoritative row-by-row record. Part 1 (spdlog
everywhere) `12583aa`; `to_underlying` `25f8b71`; `std::expected` slice A
(Expression) `27e13fc` and slice B (serialization) both landed. Slice C
(FEM/femgpu) was **deliberately skipped** as poor risk/reward, and Part 3
(ranges views, deducing `this`, `std::print`) is **policy-only** — adopt in
new code where a measured case exists, do not sweep. `CMAKE_CXX_STANDARD 23`
is set (GCC 15.2, near-complete libstdc++ C++23 support). No `kVersion` bump,
no file-format change — pure internal hygiene.

> Note (2026-07-23): file paths below predate the `src/` reorg — the renderers
> are now under `src/render/`, CADObject under `src/cad/`, etc. (see
> designer-src-layout). `Application.cpp`/`Viewport.cpp` were dead code and were
> deleted. The femgpu `printf`s the survey cited were already gone. Part 1 as
> shipped migrated ~100 sites across RayTracing/Shadow/Mesh/SSAO/Outline
> renderers + CADObject + main.cpp. DEVIATION from the done-check: main.cpp's
> CLI argument-validation errors stay on `std::cerr` alongside the `--help` text
> — both are CLI contract, not runtime diagnostics, so they must not be
> timestamp-prefixed or silenceable by `--log-level`.

Scope: three adoptions worth scheduling as deliberate work, plus two we adopt
opportunistically and one we explicitly withdraw. Nothing here changes behavior
users can see; the payoff is fewer error-handling footguns, less boilerplate,
and one logging path instead of two. Everything is incremental — no big-bang
sweep, no branch that has to land atomically.

The guiding rule: **adopt where the codebase already has a textbook case for
it; do not go looking for places to use a feature.**

---

## Where we actually are

Measured on the current tree (`src/`), not estimated:

- **Logging is already spdlog — via macros — for most of the code.** `Logger.h`
  wraps spdlog behind `LOG_TRACE/DEBUG/INFO/WARN/ERROR/CRITICAL`, lazily
  initialized (`GetLogger()` self-inits so core/tests/scripts can log without
  app startup), color stdout sink, pattern `[%H:%M:%S] [%l] %v`, level driven by
  `main.cpp`'s `--log-level`. The `LOG_*` macros are used across ~45 files. A
  raw `grep spdlog::` finds only 3 files precisely *because* everyone goes
  through the macros. **The infrastructure exists and is the dominant path.**
- **But 127 `std::cout`/`std::cerr` lines + 11 `printf` bypass it**, concentrated
  in Vulkan/renderer bring-up and femgpu: `main.cpp`, `Application.cpp`,
  `MeshRenderer.cpp`, `OutlineRenderer.cpp`, `RayTracingRenderer.cpp`,
  `ShadowRenderer.cpp`, `SSAORenderer.cpp`, `Viewport.cpp`, `CADObject.cpp`,
  `core/fem/FemTopo.cpp`, `femgpu/TopoCgSolver.cpp`, `femgpu/TopoGpuSolver.cpp`.
  These are the stragglers — early renderer code written before the logger was
  wired in. This is the "spdlog everywhere" gap, and it is small and mechanical.
- **The remote/MCP wire protocol is a TCP socket** (`RemoteServer.cpp`,
  `AF_INET`/`bind`/`accept`), **not stdout.** So routing all diagnostics to
  stdout via spdlog cannot corrupt any protocol stream. The *only* genuine
  non-log stdout is `main.cpp`'s `--help`/usage text.
- **Error handling is the `bool f(..., std::string* error)` out-param idiom:**
  ~25 function declarations (`Save`, `OpenProject`, `RunStudy`, `RunStudyAndCache`,
  the femgpu `ComputeContext`/`TopoCgSolver`/`TopoMultigrid` stack,
  `EvaluateStudyExprs`, …) plus their call sites. `std::expected` is unused.
- **`std::optional` is already idiomatic** — 27 headers, return types like
  `std::optional<ElementRef>`, `std::optional<DistanceResult>`. Good; that's the
  value-or-nothing half. The error-*with-message* half is the out-param.
- **Ranges: zero usage.** 271 indexed `for (size_t i = 0; …)` / `for (int i…)`
  loops. Matrices are already `Eigen::Matrix<…>` (`FemSolverInternal.h`), so
  **mdspan has nothing to fix** — Eigen owns that.
- **374 `static_cast<int>`-family casts**, of which ~37 are clearly enum→int
  (`static_cast<int>(settings.mode)`, `curveType`, `deviceType`, …). The rest
  are legitimate numeric narrowing, out of scope.
- **~20 paired const/non-const accessors** (deducing-`this` candidates).
- **`std::stacktrace` already in use** (`CrashHandler`). Nothing to do.

Two premises going in were wrong and are corrected here: we are *not* "using
spdlog everywhere" (127 stragglers remain), and we are *not* facing a 278-site
`std::expected` sweep — the real signature count is ~25.

---

## Part 1 — spdlog everywhere

**Decision: the 127 cout/cerr + 11 printf sites become `LOG_*` calls. No new
`std::print` for logging.** This supersedes the earlier Tier-2 suggestion to
route those sites through `std::print` (see Part 3).

The work is a straight substitution, and the target already exists:

```cpp
// before (MeshRenderer.cpp, main.cpp Vulkan setup, femgpu progress, …)
std::cout << "Ray Tracing: Added ray query extension" << std::endl;
std::cerr << "Failed to create buffer: " << err << std::endl;

// after
LOG_INFO("Ray Tracing: added ray query extension");
LOG_ERROR("Failed to create buffer: {}", err);
```

Rules for the migration:

1. **Level, not stream.** `cerr` → `LOG_ERROR`/`LOG_WARN` by intent (an actual
   failure vs. a caveat); `cout` → `LOG_INFO`, or `LOG_DEBUG`/`LOG_TRACE` for
   the chatty per-frame / per-iteration renderer + femgpu spew that should be
   silent at default level. Bring-up logs that fire once per launch stay INFO;
   anything in a draw or solve loop drops to DEBUG/TRACE so `--log-level`
   actually quiets them.
2. **`<<` chains become format strings.** `a << x << b` → `LOG_INFO("{}{}", x, …)`.
   This is the only non-mechanical part — each site's args must be reshaped by
   hand. spdlog uses fmt, so `{}` / `{:.3f}` / `{:#x}` all work; no `std::endl`.
3. **The one exception that stays on stdout:** `main.cpp`'s `--help`/usage text
   is program output a user piped for, not a diagnostic. Leave it as `std::cout`
   (or move it to `std::print` — Part 3). Do **not** send it through the logger:
   it must not carry a `[HH:MM:SS] [info]` prefix and must not be silenced by
   `--log-level`.
4. **Drop the includes.** Once a TU has no direct stream use, remove
   `<iostream>`/`<cstdio>` and add `#include "Logger.h"`.

Optional polish (do only if it falls out naturally): add a rotating-file sink to
`Logger::Initialize` so headless/CI runs leave a log artifact. Not required for
"everywhere"; noted so it isn't forgotten.

Guardrail: after migration, `grep -rn 'std::cout\|std::cerr\|printf(' src` should
return only the `main.cpp` usage text. That grep becomes the done-check.

---

## Part 2 — Tier 1 (scheduled)

### 2a. `std::expected<T, E>` for the error-out-param functions

The `bool f(..., std::string* error)` idiom has two footguns `std::expected`
closes: the error channel is discardable (callers can ignore the `bool`), and on
the failure path a caller can read a stale/garbage value through a
non-error out-param. `std::expected` makes the error non-discardable and unifies
"the value" and "why not" into one return.

```cpp
// before
bool Save(const Document& doc, const std::string& path, std::string* error);
if (!Save(doc, p, &err)) { LOG_ERROR("save: {}", err); }

// after
std::expected<void, std::string> Save(const Document& doc, const std::string& path);
if (auto r = Save(doc, p); !r) LOG_ERROR("save: {}", r.error());
```

It pays for itself most where errors *compose*. `core/Expression.cpp` already
has a fail-loud parse/eval path threading an error struct by hand; `.and_then()`
/ `.transform()` turns parse→eval→bind into a chain that short-circuits on the
first failure without an `if` after every step.

**This is NOT a sweep.** ~25 signatures + their call sites. Convert in
value-delivering slices, each independently landable and testable:

- **Slice A — `core/Expression.cpp` eval/parse path.** The best fit (composition
  is real here). This is also the reference implementation other slices copy.
- **Slice B — serialization: `Save` / `OpenProject` / `Load`
  (`core/io/Serialization.cpp`, `ui/AppActions`).** High-traffic, few sites,
  clear win.
- **Slice C — the FEM/femgpu run stack** (`RunStudy`, `RunStudyAndCache`,
  `EvaluateStudyExprs`, `ComputeContext`/`TopoCgSolver`/`TopoMultigrid`
  `std::string*` params). **SKIPPED (2026-07-23) — poor risk/reward.** These are
  all `bool f(…, StudyRuntime& rt, std::vector<double>& u, float* ec,
  std::string* error)`: they return success/fail and write results into
  *out-refs*, not the return value. So `expected`'s real safety win (can't read a
  value that isn't there) doesn't apply — a minimal `expected<void, std::string>`
  would leave the actual footgun (reading a half-written `rt`/`u`/`ec` after a
  failed solve) fully in place; closing it means folding results into the
  expected, a large rewrite of the solver stack's incremental-write pattern. No
  composition to gain (imperative mesh→assemble→solve). Not faster (zero; the
  path is minutes of compute). The only realizable gain is a non-discardable
  error — cheaper via `[[nodiscard]] bool` if ever wanted, plus fixing the one
  deliberate swallow at `RemoteSimulation.cpp` (`EvaluateStudyExprs(…, nullptr)`).
  Highest-risk slice (femgpu solver internals, slow FEM/topopt tests) for the
  thinnest benefit, so it was dropped rather than done.

Error type: start with `std::expected<T, std::string>` to match today's string
messages verbatim — no call-site rewrites beyond `.error()`. Only promote to a
richer error type (code + message + source location) if a slice actually needs
to branch on error *kind*; nothing does today, so don't build it speculatively.

Rule for the future: **new** fallible functions return `std::expected` from the
start. The out-param idiom is legacy from here on.

### 2b. Ranges views — `enumerate`, `zip`, `ranges::to`

C++23 adds exactly the three that fit this codebase's 271 indexed loops:

```cpp
for (auto [i, occ] : std::views::enumerate(occurrences)) { … }        // C++23
for (auto [v, n] : std::views::zip(verts, normals)) { … }             // C++23
auto ids = elems | std::views::transform(&E::id) | std::ranges::to<std::vector>();
```

Value is correctness + readability — `zip` kills parallel-array index-mismatch
bugs; `enumerate` removes the manual counter. **No conversion project.** Adopt in
new loops; rewrite an existing loop only when already editing it for another
reason. See the performance note below for the one place *not* to use them.

### 2c. `std::to_underlying`

~37 clear enum→int casts. `std::to_underlying(settings.mode)` self-documents and
can't silently pick the wrong integer width the way `static_cast<int>` can.
Mechanical, but each of the 37 needs an eyeball to confirm it's an enum (the
other ~337 integer casts are real numeric narrowing — leave them). Batch it as
one ~1–2h pass, or fold into files already being touched.

---

## Part 3 — Tier 2 (opportunistic / withdrawn)

- **`std::print` — withdrawn for logging, narrowed to CLI output.** Part 1's
  spdlog-everywhere decision covers the diagnostic sites, so `std::print` is no
  longer proposed for them. Its only remaining niche is genuine
  program-output-to-stdout: `main.cpp`'s usage/help text, where `std::println("{}",
  …)` is a fine type-safe upgrade over the `cout << … << std::endl` chain. Low
  priority, single site, do it or don't.
- **Deducing `this` (explicit object parameter).** ~20 const/non-const accessor
  pairs collapse to one templated getter. Cosmetic; apply only in headers where
  the duplication is actively annoying. ~1h if done at all.

Explicitly **not** adopting now: `std::mdspan` (Eigen owns matrices),
`std::flat_map`/`flat_set` (libstdc++ support still landing in GCC 15, our maps
aren't hot enough to justify the risk — revisit later), `import std;`/modules
(experimental + fragile under CMake at this project size), `[[assume]]` (only
behind a profile of a specific hot loop, never speculative).

---

## Performance note (ranges) — read before using views in a hot loop

At `-O2`/`-O3`, `enumerate`/`zip` are zero-overhead: they inline to the same
index arithmetic, and `ranges::to<vector>` allocates exactly like a manual
build. **But our default `CMAKE_BUILD_TYPE` is Debug**, and in an unoptimized
build the view wrappers are *not* inlined — a view pipeline can run several times
slower than the hand loop. That is irrelevant for UI/setup/I/O, but the
**FEM/femgpu/topopt inner solver kernels** (tet assembly, CG/multigrid inner
loops) run thousands of iterations under the debug build we develop against.

Rule: views freely in cold code; keep explicit loops in the solver kernels
(`core/fem/*`, `femgpu/*`) unless that TU is compiled `-O2`. This is a
develop-loop-speed concern, not a shipped-performance one.

---

## Sequencing & effort

Ordered so each step is independently landable and low-risk first:

| # | Task | Scope | Effort | Risk |
|---|------|-------|--------|------|
| 1 | ~~spdlog everywhere (Part 1)~~ **DONE 12583aa** | ~100 cout/cerr + 1 printf | done | — |
| 2 | ~~`to_underlying` (2c)~~ **DONE 25f8b71** | 19 enum casts (our enums only) | done | — |
| 3 | ~~`std::expected` slice A — Expression~~ **DONE 27e13fc** | Compile/Evaluate/EvaluateText + ~13 callers | done | — |
| 4 | ~~`std::expected` slice B — serialization~~ **DONE** | 6 Save/Load fns + ~15 prod callers + ~86 test sites (via tests/TestIO.h adapter) | done | test surface was ~86 sites, not "few" — used a test adapter |
| 5 | ~~`std::expected` slice C — FEM/femgpu~~ **SKIPPED 2026-07-23** | ~15 fns + call sites | dropped | poor risk/reward — see below |
| — | ranges views (2b) | going forward | 0 scheduled | — adopt in new code |
| — | deducing `this` / `std::print` CLI (Part 3) | ~20 + 1 | ~1 h | cosmetic, optional |

Landed: items 1–4 (logging, `to_underlying`, expected slices A + B). Item 5
(FEM/femgpu) was **skipped** as poor risk/reward. The remaining rows are
policy-only. This effectively completes the scheduled modernization.

---

## Verification

No feature behavior changes, so this leans on existing tests plus targeted checks:

- **Build clean** at C++23 with warnings-as-usual after each slice.
- **Part 1:** the `grep -rn 'std::cout\|std::cerr\|printf('` done-check returns
  only the `main.cpp` usage text; launch once at `--log-level=debug` and confirm
  the migrated renderer/femgpu lines appear with proper levels, and at default
  level confirm the per-frame/per-iteration ones are silent.
- **`std::expected` slices:** the existing unit + headless smoke tests for each
  subsystem must stay green (expression eval tests, save/open round-trip golden
  files, FEM study tests). The failure paths matter most — add/confirm a test
  that exercises an *error* return per converted function, since that's the path
  the old idiom made easy to get wrong.
- **`to_underlying` / ranges / deducing `this`:** covered by "it still compiles
  and existing tests pass" — none change behavior.

## Non-goals

- No `kVersion` bump, no serialized-format change, no golden-file regen.
- No public/RPC/MCP API surface change (error *messages* stay identical; only the
  in-process C++ signatures change).
- Not a "modernize the whole codebase" mandate — the six items above are the
  whole list. New features are adopted where a measured case exists, not hunted.
