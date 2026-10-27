# Dogfood: editing geometry someone else made

Most people who print do not design from nothing: they download a part and
adapt it to what they have. Every dogfood round so far built its parts from
scratch, so import, direct editing of history-less geometry, and a sketch
shared with an imported part have only been exercised by their own tests.
This round adapts real downloaded parts the way a user would, through the
same surface an MCP user has, and reports what is missing for printed-part
work (the first goal, before showing off).

## Sources

Open-licensed parts from GitHub, fetched into their own empty directory under
the untracked `scratch/imported-edits/downloads/`, and treated as untrusted
data:

- **Voron Design** (`VoronDesign/Voron-2`, GPL-3.0): printed parts published
  as both STEP and STL, so the same part can be adapted from each.
- **SO-ARM100** (`TheRobotStudio/SO-ARM100`, Apache-2.0): the SO-101 arm's
  STEP files. Its wrist bracket is known to eat 4.7 mm of the PG-52 gripper's
  room.

The round's driver scripts live in `scratch/imported-edits/`, beside its
output, and are not committed.

## The four adaptations

1. **Mesh only.** A Voron fan or duct mount imported as STL: change its
   mounting-hole size and pattern for a different fan. Tests mesh-to-solid
   and direct edits on faceted geometry — what most downloads are.
2. **B-rep without history.** The SO-101 wrist bracket from STEP, reworked to
   give back the 4.7 mm: move and offset faces, delete a fillet, thicken a
   wall, cut a pocket.
3. **A mating part.** An adapter plate that shares one sketch with an imported
   part's hole pattern; edit the import and check the plate follows.
4. **Out to the printer.** Export STL and 3MF, `check_print`, and compare each
   result against its download: volume, bounding box, and the dimensions the
   edit was for, measured with real numbers. Nothing is printed; the round
   ends with sliceable files to print later.

## How defects are handled

A crash, hang or data loss is fixed on the spot, with a regression test built
from its report (CLAUDE.md, "Crash debugging"). A small bug in the code the
round is in is fixed with a test. Anything else is an entry in
`docs/known-bugs.md`, with its repro steps; a missing capability goes to the
area README's "does not do yet" or `docs/roadmap.md`. Fixes are made in a
worktree and merged one branch at a time.

## Verification

Each adaptation's numbers (before/after volume, bbox, the target dimensions)
and its screenshots, looked at. The suites pass on every fix that lands. The
round's report lists every finding with where it went.
