# Collaboration dogfooding round — 2026-08-22

F7–F11 shipped in about a day and had never been *used*. Every conflict test
ran on a single part with one feature, and `two_collaborators.py` had zero
assemblies, drawings or extra parts. This round drove two headless instances
against a bare remote on a project that uses everything: three parts, an
assembly with occurrences and a mate, a **sub-assembly**, a drawing with two
views, tab folders, project variables, a section, a part configuration and a
CAM setup.

The reason to bother is on the roadmap already: `known-bugs.md` is empty
against ~800 tests, and one evening of *using* the app once surfaced six
defects. This round found five more in an afternoon, one of them a hole
nothing can detect.

## 1. Two people adding the same part to an assembly — noted, not a bug

Alice and Bob both add an occurrence of `Pin` to assembly `Frame`. Both mint
id 4 from the assembly's `nextId++`, both write `occurrences/4-pin.json`, and
the two files are **byte-identical** — an occurrence of the same part at the
same transform has nothing to tell them apart. Git merges them into one, and
the assembly ends with one Pin where two were placed.

I wrote this up first as silent data loss. **Elias's read is better and it
checks out:** if two people add the same part *in the same place*, one is the
right answer — that is a deduplication, not a loss. Anything else differs in
content, and then the machinery does its job. Measured both:

| | result |
|---|---|
| same part, **same place** | merged, 0 conflicts, one occurrence — correct |
| same part, **different place** | **conflicted, 1 needing a decision** |
| different parts, colliding ids | duplicate-id conflict, resolvable (§3) |

So the id collision only bites when the outcome is the one you wanted anyway.
No change.

**Kept as a note** because the reasoning is worth having written down: the
`nextId++` collision here is *masked* by identical content rather than
handled, so if occurrence files ever gain a field that differs between two
otherwise-identical adds (a creation timestamp, a per-session tag), this
becomes a conflict on every "we both added a bolt" — and the fix then is
[C6](f9-collaboration.md)'s, applied to occurrence ids. Note that an
occurrence id IS a filename, so C6's deciding argument does not carry over
unchanged.

**One real thing it exposed:** the different-place conflict reads

```
Occurrence "P1" in assembly Asm — transform {"orientation":[0.0,0.0,0.0,1.0],"position":[50.0,0…
```

Raw JSON, because `Describe()` falls back to `dump()` for a structured value.
"moved to (50, 0, 0)" is the sentence that belongs there. Small, and in
`ProjectMerge`.

## 2. Adding a remote can silently keep committing geometry — FIXED

Mine, and only visible when two features meet.

[F9 C4](f9-collaboration.md) says a project that gains a remote stops
committing its baked geometry. `AddProjectRemote` sets the flag on every part
and then saves — and that save goes through [F8](f8-external-changes.md)'s
disk-change guard. If anything wrote the project since the last save, the
guard refuses, `AddProjectRemote` only `LOG_WARN`s, and returns success.

Isolated cleanly:

```
clean   (nothing touched between save and remote_add): geometry pushed = 0, .gitignore written
touched (project.json changed between them)          : geometry pushed = 4, no .gitignore
```

The user gets a remote, no message anywhere in the UI, and pushes `.brep`
binaries to their team. It self-corrects on the next successful save (the
in-memory flags survive), so the damage is bounded — but the blobs are in
history by then, and "silently did not do the thing it said" is the part worth
fixing. Both features are correct alone; both test suites pass.

## 3. A duplicate id aborts `pull` instead of routing to the panel built for it

When the two sides add occurrences of *different* parts, the ids still collide
but the files differ, so the F7 machinery applies — and it works: the conflict
reads `Two occurrences claim id 2 in assembly Asm: "P2" and "P1"`,
`conflict_resolve take=auto` renumbers, `merge_finish` verifies, and the
project reopens.

But the *entry* is rough over the RPC. `pull` merges cleanly, then reopens,
and the loader refuses the duplicate — so `pull` raises a raw loader message
rather than reporting a conflict to resolve. The recovery path is fine
(`conflict_list` reads the directory, not the loaded document, so it still
answers), but a caller has to know to ask.

Worth noting the same shape exists for parts and the F7 tests never saw it:
they scanned after the merge without reopening.

## 4. Project-scope variables are gated on the active document — FIXED

`set_variable`/`list_variables` with `scope: project` refuse on a drawing tab
with *"the active document is a drawing, not a part; switch to a part tab"*.
A project-scope variable is not a property of the tab you happen to be on.

Exactly the class fixed in `9048d58` for the F9 sharing RPCs, which were
gated the same way and failed on an assembly tab.

## 5. A missing required field comes back as a raw JSON exception — FIXED

`add_feature` with a Cylinder missing `direction`:

```
[json.exception.out_of_range.403] key 'direction' not found
```

The same surface names *unknown* fields well ("unknown parameter 'drawing'
for 'add_drawing_view'. Accepted: base_view, name, orientation, ..."), and
`describe` documents the type correctly. A missing required field should be
the same quality of message, naming the feature type and pointing at
`describe`.

## What was checked and is fine

Worth recording, because two of these were flagged as risks in the design
docs and are not.

- **A conflict spanning two documents.** Both appear, grouped by document,
  each resolvable, and `merge_finish` verifies the whole project loads and
  regenerates. The "written as if a project were a single part" worry in
  [F7](f7-merge-and-conflicts.md) does not bite.
- **Everything survives a clone**: parts, assembly, sub-assembly (`ref_kind`
  intact), drawing views, variables, configurations, CAM setups, sections,
  and **tab folders** — which have no RPC and travel in `project.json`.
- **C4 on a clean sequence**: 0 geometry files pushed, `.gitignore` written.
- **The conflict machinery works on assemblies**, not just parts.

## Method

Two headless instances over a local bare repo, driven through the RPC —
round 1 in the CAM rounds' style. Scripts under the session scratchpad; each
finding above was reduced to an isolated A/B before being written down.


## What was fixed, 2026-08-22

- **§2** — sharing is one decision instead of a half-applied one. The disk
  state is checked *before* the remote is added, so the refusal says what to
  do ("reload or save it before sharing") rather than the geometry policy
  failing silently afterwards. If the save fails for any other reason the
  remote is removed again, because half-applied is the state nobody can
  reason about.
- **§4** — the four variable RPCs moved from the modeling module to the
  project one. Not a gate tweak: `HandleModelingRpc` takes a `Document&`, so
  it cannot be called at all without a part, and variables default to project
  scope. A part-scope variable still refuses, now saying *"a part-scope
  variable needs a part tab; scope \"project\" works from anywhere"*.
- **§5** — `add_feature`/`modify_feature` name the type and the field:
  *"Cylinder is missing the required field \"direction\" — describe
  feature_types[] lists the fields each type takes"*.

Left open: §1's transform rendering, and §3's `pull` ergonomics (the recovery
path works; the entry is a raw loader error rather than a reported conflict).
