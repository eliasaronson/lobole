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

## 1. Two people adding the same part to an assembly lose one of them — silently

The one worth acting on.

Alice and Bob both add an occurrence of `Pin` to assembly `Frame`. Both mint
id 4 from the assembly's `nextId++`, both write `occurrences/4-pin.json`, and
**the two files are byte-identical** — an occurrence of the same part at the
default transform has nothing to tell them apart. Git sees one file added
identically on both sides, merges it without a word, and the assembly ends up
with **one** Pin where two were placed.

```
pull=up_to_date  conflicts=0  needs_decision=0
occurrences after merge: [(1,'P1'), (2,'P1')]     # 3 were placed
```

Nothing can detect this after the fact. The duplicate-id scanner needs two
files; here there is one. `Verify()` passes — the project loads and
regenerates perfectly. The information that two were intended was destroyed
at the git level, and no checker downstream can recover it.

**Why it is worse than the feature case F9 C3 handles.** Two people adding
*identical features* is rare — features carry distinct names and parameters.
Two people adding *the same part to an assembly* is the normal case ("we both
added a bolt"), and identical content is guaranteed by construction.

**Content-based disambiguation cannot fix it.** Both sides start from the same
state, so any name the app derives ("P1 2") is derived identically on both.
Only a **non-deterministic id** separates them — the [F9
C6](f9-collaboration.md) fix, applied to occurrence ids.

**The tension, and why it is Elias's call.** C6's deciding argument was that a
sketch entity id is never a filename, where a feature id is. An occurrence id
*is* a filename (`4-pin.json`), so the readability argument that kept feature
ids sequential applies here too. But the failure profile is completely
different: for features a collision is usually *detected* (different type
suffixes → two files), while for occurrences it is usually *silent* (identical
content → one file). Same argument, opposite risk.

## 2. Adding a remote can silently keep committing geometry

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

## 4. Project-scope variables are gated on the active document

`set_variable`/`list_variables` with `scope: project` refuse on a drawing tab
with *"the active document is a drawing, not a part; switch to a part tab"*.
A project-scope variable is not a property of the tab you happen to be on.

Exactly the class fixed in `9048d58` for the F9 sharing RPCs, which were
gated the same way and failed on an assembly tab.

## 5. A missing required field comes back as a raw JSON exception

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
