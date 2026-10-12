# A format bump takes its number when it lands

## The problem

A format change bumps `kVersion`, freezes a golden at the new number and adds
a row to [versioning/format.md](../versioning/format.md)'s history. Branches
built in parallel each take "the next number", so two of them that both change
the format collide: the `kVersion` line, the comment block above it, the
history row, the golden's file name and its `"version"` field, and every
`(v173)`-style comment the change scattered through the code all have to be
renumbered by hand during the cherry-pick. Nearly every merge of the last two
rounds needed it.

Two things make it conflict, and both have to go:

1. **The number is chosen on the branch.** Whatever a branch writes is a guess
   at what main will be when it lands.
2. **Every bump edits the same lines.** The `kVersion` literal, the comment
   history prepended above it, and the row appended at the end of the table
   are the same few lines on every branch, so even a placeholder would
   conflict textually.

## The shape of the fix

**A branch never writes a version number; it writes `vNEXT`.** The merge
numbers it.

- `src/core/io/SerializationInternal.h` splits the constant:
  `kVersionLanded` (main's number, edited only by the merge),
  `kVersionPending` (how many bumps this branch carries, 0 on main), and
  `kVersion = kVersionLanded + kVersionPending`. A branch raises
  `kVersionPending`; the two lines are separated by an unchanged line, so a
  landing that moves `kVersionLanded` and resets `kVersionPending` never
  conflicts with the next branch's `0 -> 1`. `kVersionNEXT`,
  `kVersionNEXT2`, `kVersionNEXT3` (`kVersionLanded + k`) are what a loader
  migration compares against (`if (version < kVersionNEXT)`).
- The per-version comment block above `kVersion` goes: every entry in it
  (v57 on) is already a row of format.md's table, usually a longer one. The
  table is the one place the history is written.
- format.md gains a **Pending changes** section, a table that is empty on
  main. A branch adds its row there with first cell `vNEXT` (`vNEXT2` for a
  second bump on the same branch). It sits a paragraph away from the history
  table, and the landing returns it to its empty state, so a second branch's
  row applies cleanly after the first one landed.
- Everywhere else the branch writes `vNEXT`: the golden's file name
  (`tests/golden/vNEXT-foo.json` and its `.expect.json`, in any of the four
  golden directories), the test code that names it, code comments, the docs.
  The golden's `"version"` field holds the branch's real `kVersion`, so the
  suite passes on the branch exactly as it will on main.

**`tools/format-version.py`** does the two jobs:

- `land --base <commit> --label <branch>`: when `kVersionPending` is above
  zero, number the k-th bump `kVersionLanded + k`; replace `vNEXTk` and
  `kVersionNEXTk` on the lines the branch added since `<base>` (only those,
  so a document that describes the rule keeps its placeholders); `git mv`
  every added path carrying the token and set a renamed golden's top-level
  `"version"`; move the pending rows into the history table, numbered; set
  `kVersionLanded += kVersionPending`, `kVersionPending = 0`; commit
  `chore(format): number <branch>'s format change vN`.
- `check [--landed]`: the header's arithmetic holds; the history table's
  highest row is `kVersionLanded`; the pending rows are exactly `vNEXT` ..
  `vNEXT{P}`; every golden named `v<N>-…` carries `"version": N` and every
  `vNEXTk-…` one `kVersionLanded + k`; no `vNEXTk`/`kVersionNEXTk` with
  `k > P` anywhere in the tracked tree (the files that describe the rule are
  exempt). `--landed` adds: `kVersionPending` is 0, so no placeholder can
  remain.

**`tools/merge-branch.sh`** records main's commit before the cherry-pick,
refuses a branch that edits `kVersionLanded` itself (it guessed a number),
and after the pick — or after `--no-pick`, once a conflict was resolved by
hand — runs `land` and then `check --landed` under `build/`'s lock, before
the build and the suites.

**Gates.**

- ctest `format_version_gate` runs `check` (a branch with pending bumps
  passes; an inconsistent one fails).
- CI runs `check --landed` on pushes to main, so a branch landed by a plain
  cherry-pick, without the numbering, goes red.
- `test_golden.cpp`'s history gate counts pending rows: the highest numbered
  row plus the pending rows equals `kVersion`. The golden gate is unchanged.

## Files

- `src/core/io/SerializationInternal.h` — the split constant, the comment
  block removed.
- `tools/format-version.py` — new.
- `tools/merge-branch.sh` — base record, refusal, `land` + `check --landed`.
- `tests/test_golden.cpp` — history gate counts pending rows.
- `tests/CMakeLists.txt` — `format_version_gate`.
- `.github/workflows/ci.yml` — `check --landed` on main.
- `docs/versioning/format.md` — migration policy rewritten, Pending section.
- `docs/versioning/README.md`, `CLAUDE.md` — the rule, briefly.

## Verification

- The build and the headless suite pass with `kVersionPending = 0`.
- Dry run in a temporary clone (never main): a stand-in main branch, two
  branches cut from it that each add a `vNEXT` golden, pending row, comment
  and `kVersionPending = 1` (one of them two bumps); land both in sequence
  with `merge-branch.sh`. They get consecutive numbers, neither cherry-pick
  conflicts, and `check --landed` plus the `[golden]` gates pass.
