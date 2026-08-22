# Handoff — versioning F7, F8, F9 (2026-08-21)

Written for whoever picks this up next. Short version: **F7, F8 and F9 are all
built, tested and committed.** Nothing is half-finished in the working tree.
This file says what exists, what deliberately does not, and where the traps
are; the design docs carry the reasoning.

| | Design | State | Commits |
|---|---|---|---|
| **F7** merge + conflict handling | [f7-merge-and-conflicts.md](f7-merge-and-conflicts.md) | **shipped** — phases A, B, C | `e116416`, docs `95e0697` |
| **F8** project changed on disk | [f8-external-changes.md](f8-external-changes.md) | **shipped** — D1, D2, D3 | `a75babb`, docs `fda3a56` |
| **F9** a small team sharing a project | [f9-collaboration.md](f9-collaboration.md) | **shipped** — C1–C5 | `c7e50ff`, `81aae75`, `c2d09dd`, plan `f0fd0f3` |
| two bug fixes found underneath | — | shipped, isolated on purpose | `fc01bcb`, `7b3b9ce` |

Each design doc ends with a **"What shipped, and what building it changed"**
section. Read that before the body — several decisions in the body were
overtaken by what the build found, and the closing section is the one that is
true.

## What exists now, in one paragraph each

**F7.** `GitVault` does branch, switch, merge, three-stage reads, resolve,
abort and commit-merge — local branches only. `core/io/ProjectMerge` turns a
conflicted path into a sentence about a feature, and handles the five classes
the directory format actually produces; they are *not* the same pick, and the
doc is explicit about why. `ui/ConflictsPanel` shows them and exists only when
there is something to resolve. `ProjectMerge::Verify` is the bar: a merge
cannot be concluded until every document **loads and regenerates**.

**F8.** `core/io/ProjectStamp` is a `{fileCount, maxMtime, totalSize}` quick
check of the project directory, skipping `.git/` and `geometry/`. It is
recorded wherever memory and disk are known to agree, and checked in
`ui::SaveProject` — the one place every save funnels through. A moved project
refuses and offers Reload / Save anyway / Cancel.

**F9.** Remotes, clone, push, pull, per-person commit identity, duplicate ids
attributed by merge side, built geometry that stops travelling once a project
is shared, and named versions that survive two people choosing one name.

## Deliberately not done

- **Git remotes handle no authentication.** Every network call runs with
  `GIT_TERMINAL_PROMPT=0` and empty askpass so unconfigured credentials fail
  fast with git's own message. The audience is a team that already has git
  working. This is a decision, not an omission.
- **No rebase, force-push, or history rewriting.** GitVault has never had them.
- **Locking / check-out, presence, review, a hosted service, real-time
  co-editing** — see F9's Deferred section.
- **The F9 GUI sharing controls have no automated test.** The engine, the RPCs
  and the whole two-person round trip are covered headlessly; the panel
  buttons are thin wrappers over RPC-tested calls.
- **The F8 menu-bar notice has no automated assertion.** A disabled `MenuItem`
  in the main menu bar does not resolve through the ImGui test engine's item
  paths (three forms tried). Verified by screenshot; the test asserts the flag
  that drives it.

## The risk I got wrong, and what it actually was

An earlier version of this file named **toponaming divergence** as the one
path to a silently wrong merge, and recommended `compare_geometry` (V4 of the
agent-validation plan) as the mitigation. Elias pushed on whether those were
the same thing. They were not, and chasing it changed the answer twice.

**`compare_geometry` is the wrong tool, structurally.** It is
`{a, b | golden_path}` → IoU, dice, volume ratio: *does this solid match that
known-correct one*. A merge has no second operand. The merged result is
**supposed** to differ from both parents — that is what merging is — so a
boolean diff fires on every successful merge, and there is no golden because
nobody has ever seen the correct merged result. V4 is still right for its own
job (goldens, "did my edit do what I claimed", regression against stored
BREPs); it was never a merge safety net.

**The real mechanism was narrower and fixable.** A face's stable name is
literally `<featureTag>g<entityId>` (`SketchProfile.cpp:1130`). So a name can
only mislead if one of those two ids means different things on the two sides:

- **Feature ids** — the class F9 C3 already handles.
- **Sketch entity ids** — a per-sketch `nextEntityId++` counter, the same
  shape one level down, and *invisible* to C3 (which excludes `entity_id`
  deliberately, since a sketch-local id is not a document feature id).
  Reproduced: a clean git merge leaving two entities both `id: 9`, one a line
  and one an arc, making `4/g9` ambiguous.

**Fixed 2026-08-22.** `SketchFeature::MintEntityId` mints collision-resistant
ids instead of counting, and the loader refuses a sketch holding two entities
with one id — the same "refuse loudly" family as duplicate feature files and
unresolved conflict markers, so it catches a project that arrived by rsync or
a hand-edit too. Entity ids could be randomised where feature ids could not
because **a feature id is a filename and an entity id never is**: the
readability that kept feature ids sequential was simply not on the table.

### What genuinely remains

A merge can still combine two people's *intents* into a part neither of them
designed — the format merges text, and text is not intent. No checker
resolves that; it is what review is for. What is gone is the class where the
document rebuilds perfectly while a reference quietly points somewhere else.

## Also shipped (2026-08-22)

- **[F10](f10-autosave-the-project.md)** — autosave saved only the ACTIVE tab,
  and nothing at all on an assembly or drawing tab. One atomic file per dirty
  document now. It also uncovered that F9's whole sharing surface was
  part-gated and failed on an assembly tab (`9048d58`).
- **[F11](f11-seeing-what-changed.md)** — G3 (the conflicts scan stopped
  running on a timer), G1 (each version says what it changed), G2 (clicking a
  version shows what changed since it).
- **`2eda31d`** — the terminal panel could not be turned off and on again;
  `everStarted` latched and blocked every future start.

### One known-red test, not mine

`terminal/off_by_default_docks_right_on` fails after
`cam/tool_window_follows_the_viewport` in the same process — deterministically,
serially, no load. Diagnosed in a comment on the test: the window is created,
active and docked, but `Hidden`, so its body is never submitted. Four fixes
tried and rejected; the remaining suspect is which node it lands in after a
layout rebuild. Left red on purpose rather than papered over.

## Tests, and what each covers

| Where | Covers |
|---|---|
| `tests/test_gitvault.cpp` `[git]` | branch/switch/merge, three stages, abort, delete-vs-modify, the same-id blind spot, commit identity resolution, remotes/push/clone, rejected push, unreachable remote failing fast, two versions sharing a name, tags reaching a remote |
| `tests/test_projectmerge.cpp` `[merge]` | the union merge (additions, deletions, insertion order, every id-keyed collection), conflict description in CAD terms, disjoint fields never reaching the panel, duplicate-id detection, `TakeField`, the attributed renumber and its refusal, `Verify` |
| `tests/test_projectstamp.cpp` `[stamp]` | stamp stability, external edits, add/remove, `.git/` churn ignored, `geometry/` ignored at every depth, invalid never matching, single-file projects |
| `tests/test_command_modify.cpp` `[document]` | a modified feature keeps its id; no id-0 item is ever written |
| `tests/headless_smoke.py` | the F8 guard end to end, including `force` and Save-As |
| `tests/two_collaborators.py` | **the whole F9 story, no network** — bare repo, two clones, two headless instances, rejected push, resolve, and the project loading and regenerating on the other side |
| GUI `conflicts/*`, `disk/*` | the panels: resolving a merge, refusing a half-resolved finish, the moved-project modal, the banner |

Full suites at handoff: **735 headless** (`ctest -LE gui`), **168 GUI**
(`tools/gui-tests-xvfb.sh`).

## Traps that cost time here

- **Quote every git format string.** They go through a shell:
  `%(refname:short)` is a syntax error, `%an|%ae` becomes a pipe. The failure
  is silent — an empty answer, not an error.
- **No ImGui on a path the RPC layer can reach.** `ImGui::GetTime()` in a
  helper called from `FinishMerge` was a SIGSEGV in headless.
- **A poll cannot live in the panel that displays it** — the panel returns
  early when its tab is behind another.
- **`OpenProject` has two success paths**; a single-part `.dsgn` returns from
  an early branch that never reaches the container-format code.
- **A TreeNode pushes an id scope** — gui tests address through it.
- **A GUI test that leaves a dock tab selected changes the next test.** Model,
  History and Conflicts share one node. Re-select Model immediately after the
  last interaction that needs another tab, not at the end of the test — a test
  that fails partway never reaches cleanup written at the bottom.
- **No format change in any of this.** Git objects are not the document
  format; `kVersion` was not touched by F7, F8 or F9.
