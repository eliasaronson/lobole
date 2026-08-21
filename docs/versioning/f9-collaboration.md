# F9 — A small team sharing a project — design

> **SHIPPED 2026-08-21** — C1–C5. Audience decided (Elias): **a small team who
> already understand git**. That is not a hedge — it is what made this a week
> rather than a quarter, and every decision below leans on it. The sections
> are the design as written, with "What shipped" at the end recording where
> building it changed the plan.

## Why this is small

[F7](f7-merge-and-conflicts.md) already built the expensive half without
meaning to. `pull` is `fetch` + `merge`, and merge is done: a teammate's
incoming work lands in the Conflicts panel reading
`Extrude "Boss" in part Stand — distance 12 vs 18`, the feature order
union-merges instead of picking a side, duplicate ids are found even though
git reports none, and nothing concludes until the project loads *and
regenerates*. None of that changes for two people or for five. It is the same
code path with a different source of commits.

[F8](f8-external-changes.md) covers the other half of the same problem: two
people, or two of your own instances, writing the same directory. The stamp
already refuses a save onto a project that moved.

So what is left is plumbing, one algorithm, and one rule to reverse.

## C1 — Who made this change (~half a day)

Every commit is authored `Designer <designer@local>`. That identity is
injected inline (`GitVault.cpp:119`) so commits work on CI and on a fresh
machine with no git config — a good reason that produced a bad side effect:
it is passed on **every** commit, so it overrides the identity a person
already configured. With one user that is invisible. With a team it means the
History panel, `git log`, and every blame answer the same wrong thing about
everyone.

- Use the repository's own `user.name`/`user.email` when git has them
  (`git config user.email` exits non-zero when it does not).
- Otherwise two new Preferences fields. The settings store is one table
  driving the field list, the JSON round trip and the window rows, so this is
  one row each rather than three edits that drift.
- Otherwise the current inline fallback, unchanged — `headless_smoke`, ctest
  and CI all commit without any git config and must keep doing so.

Old commits keep the old identity. Nothing to migrate; history is honest
about when it started being honest.

## C2 — Remotes (~2 days, and the threading is most of it)

`GitVault` gains `Remotes`/`AddRemote`/`RemoveRemote`, `Fetch`, `Push`,
`Upstream`, and `AheadBehind` (`rev-list --left-right --count @{u}...HEAD`,
which is what a "2 out, 3 in" indicator needs). `Pull` is not a new
mechanism: fetch, then the `Merge` that already exists.

**Authentication is deliberately not our problem.** The audience has git
working. Run with `GIT_TERMINAL_PROMPT=0` and no askpass, so a credential
prompt *fails* instead of hanging, and report the failure with the remote's
name and git's own message. A team that can `git push` in a terminal can push
here; one that cannot has a terminal panel inside the app to fix it in. The
alternative — SSH agents, passphrase prompts, host-key verification, token
storage — is a week of misery for an audience that has already paid it.

**The real work is that these are network calls and `RunGit` blocks.** Every
GitVault call today is a fast local operation on a small directory; `fetch`
and `push` are not, and a frozen frame while a remote is slow is not
acceptable. The split that makes this easy:

- **`fetch` and `push` go to a worker thread.** They touch `.git/` only —
  never the working tree, never the Document — so the cross-thread surface is
  a status enum and a message. This is exactly the shape `TopoJob` already
  uses (worker + atomics + one join on the main thread, over a snapshot with
  no Document access); follow it rather than inventing a second pattern.
- **`merge` stays on the main thread.** It is local, fast, and it changes the
  working tree under an open document — which is precisely the thing that
  must stay ordered with the UI.

So "Get" is: fetch (background) → merge (main thread, may raise the Conflicts
panel). "Send" is: push (background), and a rejected push says *get first*
rather than silently pulling, because a merge nobody asked for is a surprise.

**And how does the second person get the project at all?** Clone belongs
here or the story starts with "use the terminal": a URL and a folder,
`git clone` on the worker thread, then the existing open path. Half a day,
inside C2's estimate.

**F8 interaction, checked:** `fetch` writes only `.git/`, which the stamp
excludes, so it raises no false "changed on disk". `pull` changes the working
tree, but it goes through merge, and every merge path already ends in
`OpenProject`, which re-stamps.

## C3 — Duplicate ids, attributed by side (~1 day)

`RenumberDuplicate` refuses when anything still references the contested id,
because a `sketch_id: 15` cannot say which of two features it meant. For one
person with two machines that refusal is rare. For a team it would become
routine manual work — and this is the pressure that would otherwise reopen
the id-format decision.

It does not have to. **After a merge we know which side each file came from**,
and the machinery to ask already exists (`MergeBase`, `ListTree`):

- `addedByOurs = ListTree(HEAD) − ListTree(base)`, and likewise for theirs.
- A duplicate id 15 is file A (added by ours) and file B (added by theirs).
  Renumber B, and rewrite references to 15 **only in files added by theirs**.
  Their extrude meant their sketch; ours meant ours. Both stay correct.
- Refuse, exactly as today, when attribution is genuinely unavailable: a
  reference inside a file that both sides modified, an id that existed in the
  base, or a project that arrived by rsync or zip with no merge to attribute
  against.

This keeps ids sequential and filenames readable — the reason they were kept
— while removing the case that would have made that choice expensive. It is
strictly more capable than today's behaviour, and it fails the same way when
it cannot know.

## C4 — Built geometry stops travelling (~half a day)

`m_commitGeometry` defaults to `true` (`Document.h:431`): the built bodies are
committed beside the recipe so opening is a load, not a rebuild (259 s → 0.04 s
on a 148-feature part — worth every byte for one person).

For a team it is the opposite trade. It is megabytes of binary pushed on every
save, it conflicts on every concurrent edit, and the resolution is always
"discard it and rebuild" because the chain hash decides at load anyway. A
project that gains a remote should stop committing it — the per-project
setting and the `.gitignore` half of `WriteRepositoryHints` already exist, so
this is a default plus one commit that removes `geometry/` from tracking.

Two wrinkles worth naming: the flag is per-*document* while this decision is
per-*project*, so it has to be set across every document in the project; and
history already carrying the blobs is fine — it costs clone size once, not a
conflict per save.

## C5 — Named versions across clones (~half a day)

Two problems, both small, both currently silent.

- **Tags are not pushed.** A named version is a `cp/<slug>` tag, so today it
  stays on the machine that made it. Push it with the commit.
- **`Tag` uses `-f`** (`GitVault.cpp:183`), so two people naming a version the
  same thing clobber each other — locally without a word, and on push as a
  rejected or force-updated ref. The fix falls out of a decision F6 already
  made: **the ref is only an address, the name is the commit subject**. So on
  collision, disambiguate the *address* (append a short hash) and leave the
  person's prose name alone. Two people can both name a version "before the
  hinge rework" and both names survive, pointing at their own commits.

## The rule this reverses — approved 2026-08-21

F6 was explicit: the History panel *speaks CAD, not git*. It says version,
name this version, restore; never HEAD, ref, detached, or hash. "A user who
wants git has a terminal, and one who does not should still get 'go back to
how it was on Tuesday'."

Sharing cannot fully keep that, and pretending otherwise would be worse. A
remote is a URL a teammate sends you. A rejected push is a real thing with a
real cause. If the app invents words for those, then when git fails — and it
will, on auth, on a rejected push, on a bad URL — the message will be in
git's vocabulary and the UI will have taught the wrong one.

**The sharing surface names git operations; everything else keeps F6's rule.**
"Remote", "Push", "Pull"/"Fetch" appear in the sharing section and in its
errors; the version list above it still says version, name, restore, and never
grows a hash column. Elias approved this, adding that it "might be more honest
as well" — which is the argument. The branch controls moved with it, since
they are git operations with git failure modes: "Branches", "New branch",
"Checkout", "Merge", where they had been "Lines of work", "Start a separate
line", "Switch to", "Bring in".

## What does not change

- **The conflict machinery.** Incoming work from a teammate is the same five
  classes, the same panel, the same integrity pass.
- **Linear restore.** Restoring writes a new commit on top rather than moving
  HEAD backwards, which is exactly what a shared branch needs — no rewritten
  history for anyone else to reconcile. Worth one warning in the UI when the
  restore reaches past a commit that is not yours, since the *effect* on a
  shared branch is to undo someone else's work even though the mechanism is
  safe.
- **No rebase, no force-push, no history rewriting.** GitVault has never had
  them and should not gain them here.

## Phases

- **C1 identity (~0.5 d)** — first, and independent. A history where everyone
  is `designer@local` gets less useful with every commit.
- **C2 remotes + clone (~2 d)** — the worker-thread split is the substance.
- **C3 attributed renumber (~1 d)**
- **C4 geometry stops travelling (~0.5 d)**
- **C5 tags across clones (~0.5 d)**

**≈ 4.5–5 days.** C1 and C4 are independently useful and could ship first.

## Decisions (settled 2026-08-21)

1. **The sharing surface says "Push" and "Pull".** Approved by Elias, as a
   stated exception to F6's rule; the branch controls moved to git's words
   with it.
2. **Committed geometry is disabled automatically** when a remote is added,
   with a commit that says so. It is a decision about derived data, not a
   preference.
3. **Clone is in-app.** It is the entire first-run experience for the second
   person, and without it the story starts with "use the terminal".

## What shipped, and what building it changed

C1–C5, 2026-08-21, over four commits. Five things the build changed or found.

**Two shell-quoting traps, both the same shape.** `--pretty=format:%an|%ae`
made the shell read a pipe; F7 had already been bitten by `%(refname:short)`,
whose parens are a shell syntax error. **Quote every git format string** —
they go through a shell, and the failure is silent (an empty answer), not an
error.

**Testing "git has no identity" needs the machine's own hidden.** The
resolution order deliberately honours a global config, so on any developer
machine there IS one, and the fallback case is unreachable without
`GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_SYSTEM=/dev/null`. Worth knowing
before writing a test that asserts the wrong thing and looks like a bug in
the code.

**Lightweight tags never leave the machine.** `push --follow-tags` carries
annotated tags and silently ignores lightweight ones, so C5's "named versions
travel" needed the tags to become annotated — which also records who named the
version, the same question C1 answers for commits. And once an address can be
disambiguated, `LogEntry` has to carry the ref it FOUND: recomputing it from
the name cannot find `cp/slug-<hash>`, so a second version with a shared name
could not be restored by name.

**A joinable thread destructing calls `std::terminate`.** Quitting the app
mid-fetch would have taken it down rather than closed it. `SyncJob` joins in
its destructor, the same guard `TopoJob` already had — and unlike the
optimizer there is nothing to cancel, since a fetch touches only `.git/`.

**`Field` is initialised positionally.** The free-text flag the two identity
settings needed had to go LAST in the struct: added anywhere else it silently
re-binds every row after it (the font row's `100.0f` displayScale would have
become a `bool`).

### Not done, and why

- **Locking / check-out, presence, review, a hosted service, real-time.**
  Deferred as designed below.
- **The GUI sharing controls have no automated test.** The engine, the RPCs
  and the whole two-person round trip are covered headlessly
  (`two_collaborators.py`); the panel's buttons are thin wrappers over
  RPC-tested calls, and a GUI test would need a remote and a worker thread
  inside the ImGui test harness. Stated rather than implied.

## Verification

The whole thing tests **locally, with no network**: `git init --bare` in a
temp directory, clone it twice, and drive both clones through the RPC. That is
a real two-collaborator scenario end to end.

- Unit: a repo with `user.email` configured produces commits authored by it; a
  repo with none still commits (the CI path).
- Unit: `AheadBehind` after each side commits; a rejected push reports *why*.
- Unit: the attributed renumber — Alice's extrude keeps pointing at Alice's
  sketch after Bob's same-id feature is renumbered; the ambiguous case still
  refuses.
- Integration (two clones, one bare remote): Alice adds a feature and pushes;
  Bob pulls and has it; both edit the same feature; Bob's push is rejected;
  Bob pulls, resolves in the Conflicts panel, pushes; **Alice pulls and the
  project loads and regenerates**. That last clause is the bar, as it was for
  F7.
- Both name a version the same thing, and both names survive.

## Deferred, with reasons

- **Locking / check-out.** Real PDM systems make you claim a part before
  editing it, because two people editing one feature has no correct automatic
  answer. It would sidestep the toponaming risk below rather than detect it.
  A `locks/` convention in the repo is ~2–3 days, and it is a different
  product decision — worth its own design if merging turns out to hurt.
- **Presence, review, approval, a hosted service.** A different product, not
  an increment: a server, accounts, permissions. None of the above is the long
  pole for that.
- **Real-time co-editing.** Not with a git-backed format.

## The risk that is not a line item

**Toponaming divergence.** Everything F7 and F8 do fails loudly — unresolved
markers refuse, duplicate ids refuse, a merge that will not regenerate
refuses. But face names are derived from the modeling history. If two people
edit the same feature differently, downstream references (fillets, mates,
drawing dimensions) can bind to *different but valid* faces on each side.
`Verify()` proves the merged project loads and rebuilds; it does not prove it
means what either person intended. That is the one path here to a silently
wrong result rather than a refusal, and more collaborators make it more
likely.

The mitigation is already designed elsewhere: `compare_geometry` (V4 in
[../inspect/agent-validation-plan.md](../inspect/agent-validation-plan.md),
awaiting sign-off) is a boolean diff of two solids. Run across a merge — "is
the result still what either side had?" — it turns the silent case into a
visible one. It is not part of C1–C5, and it is the thing I would add next.
