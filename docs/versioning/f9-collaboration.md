# F9 — A small team sharing a project — design

> **Status: not built.** Planned 2026-08-21. Audience decided (Elias): **a
> small team who already understand git**. That is not a hedge — it is what
> makes this a week rather than a quarter, and it is the assumption every
> decision below leans on.

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

## The rule this reverses — for sign-off

F6 was explicit: the History panel *speaks CAD, not git*. It says version,
name this version, restore; never HEAD, ref, detached, or hash. "A user who
wants git has a terminal, and one who does not should still get 'go back to
how it was on Tuesday'."

Sharing cannot fully keep that, and pretending otherwise would be worse. A
remote is a URL a teammate sends you. A rejected push is a real thing with a
real cause. If the app invents words for those, then when git fails — and it
will, on auth, on a rejected push, on a bad URL — the message will be in
git's vocabulary and the UI will have taught the wrong one.

**Proposal: the sharing surface names git operations; everything else keeps
F6's rule.** Concretely: "Remote", "Push", "Pull"/"Fetch" appear in the
sharing section and in its errors; the version list above it still says
version, name, restore, and never grows a hash column. This is a deliberate
exception with a stated reason, not a drift — hence sign-off rather than a
decision made in passing.

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

## Open decisions (for Elias)

1. **Does the sharing surface say "Push" and "Pull"?** Recommended above, as a
   stated exception to F6's rule. The alternative is inventing words for
   operations whose failures speak git.
2. **Auto-disable committed geometry when a remote is added, or ask?**
   Automatic with a line saying so is my recommendation — it is a decision
   about derived data, not a preference — but it does silently change what a
   project pushes.
3. **Clone in-app, or tell people to clone in the terminal?** In-app is half a
   day and is the entire first-run experience for the second person.

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
