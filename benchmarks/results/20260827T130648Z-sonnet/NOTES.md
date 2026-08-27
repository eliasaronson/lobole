# One cell in this sweep is mine, not the arm's

`t1-mount-plate / script-first / r1` records 0/5 checks, 0 tool calls, $0.00 and
3.4 s. That is not a modelling failure. The agent never started:

    Error: Can't access working directory .../runs/t1-mount-plate__script-first__r1/cwd

While this sweep was running, I ran `find benchmarks/results -type d -name cwd
-empty -delete` in the course of deciding what to commit — and deleted the
directory this run was about to launch in. Unnecessary as well as destructive:
git does not track empty directories, so there was nothing to clean up.

Fixed in two places rather than papered over:

- `run.py` creates the agent's cwd immediately before launching it, not minutes
  earlier, so an external tidy-up cannot land in that window.
- A cell where the agent never started is now marked `harness_error` and
  **excluded from every total** instead of scoring zero. Counting a failed
  `claude` invocation as a failed part is substituting a plausible number for a
  missing one — the exact thing this repo refuses to do everywhere else, and
  here it would have slandered whichever arm the accident happened to land in.

The re-run of that single cell is in the sibling sweep directory. This one is
left exactly as it was recorded: the rule is that results are never
overwritten, and that has to hold when the embarrassing result is my own.
