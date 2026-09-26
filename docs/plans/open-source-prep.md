# Ready to publish

## The problem

A readiness audit before open-sourcing found the tree mostly clean (no
secrets, a 30 MB pack, every link resolving) but with loose ends that a
public repo should not carry:

- **The Onshape importer** is impractical (the API's rate limit). It also
  carries the only reason for an Onshape-terms document, and its fixture is
  a real API dump with the owner's Onshape id and name.
- **The licence inventory is incomplete:** libarea (LGPL + BSL Clipper 1),
  Clipper2, JetBrains Mono, the fastener tables (GPL-2.0+), bd_warehouse
  (Apache-2.0) and libvterm.
- **TRADEMARKS.md is inaccurate.** It claims product names appear in three
  places only, and in no source comment.
- **Personal paths** (`/home/elias/…`, `~/Documents/…`) appear in tracked
  files. Stray tracked files: a 37 KB ELF `opencascade_test`, `build_out.log`,
  tracked `scratch/*.py`. `.claude/`, `.direnv/` and `.envrc` are not ignored.
- **The flake misses the clipper2 submodule**, so `nix build` and the
  container build without CAM.
- **The README** promises a Python REPL that does not exist, omits clipper2,
  and quotes counts that go stale.
- **No SECURITY.md** (the RPC server is unauthenticated), no code of
  conduct, no issue templates.
- **`tools/git-sessions.sh`** exists only to read commit trailers that are
  being removed.
- **`benchmarks/results/`** carries agent transcripts. The summaries stay;
  the transcripts leave the tree.

The FS chassis's source data is a separate decision, and is not part of this.

## The shape of the fix

One commit per item above, and the history rewrite afterwards. The rewrite
removes the `Co-Authored-By` and `Claude-Session` trailers, maps the author
to the personal address, and removes the Onshape fixture from history.

## Verification

- The headless suite and the `[docs]` gate.
- `nix build` builds with CAM.
- `benchmarks/runners/reference.py`.
