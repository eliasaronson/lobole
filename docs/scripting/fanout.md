# Fan-out: N agents, N parts, one project

> **Status: proved 2026-09-04** by `tests/fanout_smoke.py` (agent-scale plan
> G5). Nothing here is new machinery — worktrees, scratch ports, the instance
> registry, the F8 disk stamp and the F9 git verbs all existed. What was
> missing was anyone having run them together, and a written recipe.

The question this answers: **can several agents build several parts of one
project at once, and does the result merge?** The answer is yes for
file-disjoint work, which is what a project of many parts is, and the
mechanics are three commands.

## The recipe

One worktree, one instance, one part each:

```sh
git worktree add ../designer-wt1 -b wt1 && (cd ../designer-wt1 && tools/worktree-build.sh)
# worktree-build.sh prints a scratch port for that worktree — never 8799,
# never one another instance announced (tools/scratch_port.py).
../designer-wt1/build/designer --headless --remote <port1> &
```

Then point an agent at it. The MCP server pins an instance with
`DESIGNER_PORT`, so a subagent per instance is one environment variable:

```sh
DESIGNER_PORT=<port1> claude -p "build the bracket in parts/bracket" ...
```

Integrate **serially**: merge one branch into main, run the suites, then the
next (CLAUDE.md). `branch_merge` refuses a merge that would produce duplicate
ids, and `conflict_list` / `conflict_resolve` / `merge_finish` are the F7
surface for the rest.

## What actually makes it safe

- **The server takes eight clients, not one.** They queue into a single
  main-thread loop, so execution is serialized while connections are not. A
  screenshot or a UI action blocks the queue for its duration; a modelling
  call does not.
- **A scratch port is claimed, not guessed.** `tools/scratch_port.py` asks
  the kernel for a free one AND checks the announced instances in
  `~/.local/state/designer/instances/`, excluding the human's 8799.
- **Two instances on ONE project directory is the case to avoid**, and the
  F8 disk stamp is what makes it loud instead of silent: the second save is
  REFUSED because the stamp moved under it (`save_project` needs
  `force: true` to overwrite, and `project_changed_on_disk` is the poll).
  That refusal is the whole reason a fan-out uses a worktree per agent
  rather than a directory shared by all of them.
- **Parts are file-disjoint by construction.** The `.dsgn` directory format
  is one file per part, per mate, per occurrence — two agents editing two
  parts touch two disjoint sets of files, and git merges them without
  reading either.

## What it is NOT for

Overlapping edits. Two agents on the same part, or on the same assembly's
mates, produce a conflict that someone has to resolve by hand, and the
parallelism has bought nothing but the cost of merging. The rule from
CLAUDE.md stands: worth it only for genuinely file-disjoint work.

## The gate

`tests/fanout_smoke.py` (headless, in `ctest -LE gui`): three instances
build three parts in one project, the parts merge into one document, and a
FOURTH instance opening the same directory as one already editing it has its
save refused by name rather than clobbering the first.
