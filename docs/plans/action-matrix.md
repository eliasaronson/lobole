# Action matrix

## The problem

Every modeling test builds the part it was written for. The fillet tests
fillet the edges someone chose, the reference parts pick the edges their
recipe names, and the random designs (`tools/fuzz.py`) pick one to three
edges of a body they just made. Nothing asks the question a user asks on
every part they open: *does fillet, chamfer, shell, hole, sketch-on-face,
extrude-from-face, draft, pattern or mirror work HERE, on this face or this
edge?* Defects live in the pairs nobody chose:

- a fillet on every edge of `14-box-round-all` fails 48 of 48 (each edge
  meets a blend tangentially, so OpenCASCADE has "no suitable edges"), as
  a broken feature rather than a refusal;
- on `25-modelled-thread`, 41 of 172 edges fillet, and one of the failures
  is an `OSD_SIGSEGV` that OpenCASCADE caught inside the blend builder and
  that came back as an ordinary error message;
- shell fails on all 53 planar faces of the hinge box's `box` part at 0.5 %
  of its size.

None of this is pinned anywhere, so a change that turns a working fillet
into a broken one on some part's edge 37 lands silently.

## The shape of the fix

Every ordinary action on every face, edge and body of a fixed corpus of
real designs, sent over the RPC the way an agent or the GUI sends it, with
each outcome recorded in a committed baseline. The baseline is a ratchet: a
run fails when something that passed now does not, and a newly passing entry
is listed so that `--bless` can tighten the baseline.

1. **The corpus**, listed in `tests/action_matrix/corpus.json` (one row per
   design: kind, source, the parts it contributes). Everything is built from
   what is committed, never from `scratch/`:
   - `reference/<case>`: every case in `tests/reference_parts/`, built with
     `reference_parts.build` (picks by point, so names are what is tested).
     A case marked `known_failure` is skipped whole.
   - `fuzz/<seed>`: the clean seeds of `fuzz_seeds_smoke` (`SEEDS` under
     `fuzz_design.GENERATOR_VERSION`), built by `fuzz_design.run_design`;
     the corpus records the version, and a new generator version re-blesses.
   - `golden/<file>`: the part documents in `tests/golden/v*.json` (not
     `.expect.json`, not `assembly/`, `job/`, `drawing/`) that open with a
     clean regen and at least one body. The ones that do not are listed in
     the baseline's `skipped` table with the reason, so a golden that stops
     opening shows up in the diff.
   - `generator/<name>`: generators that finish in seconds from nothing:
     `gear.py`, `gear_train.py`, `slider_crank.py`, `toolmakers_clamp.py`
     and `hinge_box/build.py`, each into a private `--out` directory. Only
     part tabs that own their geometry count: a Derived copy, a print plate
     or a negative control repeats another tab's bodies (the hinge box has
     eight tabs and two of them own geometry).

   The big generators (`-C generators`, minutes each) are not in it.
2. **Entities.** For each body, `list_elements` gives its faces and edges,
   each addressed as `{feature_id: <the body's created_by>, element: name}`.
   It must be the body's `created_by`, not the element's `made_by`: a Hole's
   edge sent with `made_by` builds a broken feature, "the body the edges
   belong to no longer exists". Unnamed faces (`unnamed_faces`) and unnamed
   edges (the `query_geometry` count minus the named ones) cannot be
   referenced: they are counted per design and reported, not tried.
   Vertices are not elements here (`list_elements` and `query_geometry`
   name faces and edges only), so there is no vertex row. A body is capped
   at 400 faces and 1200 edges; the gear (195, 579) is the largest.
3. **Two fields `list_elements` needs.** An edge row gains `smooth` (the
   two faces meet G1, from `BRep_Tool::Continuity`): blending a tangent
   edge has nothing to blend, so the matrix skips it as `n/a` instead of
   recording 48 meaningless failures. A face row gains `inside_point` (a
   point on the face, found by classifying UV samples, because the centroid
   of an annulus or an L lies off the face) and `inset_mm` (that point's
   distance to the face's boundary). Both go in the `describe` row and the
   SDK's `Face`/`Edge`.
4. **The actions and their defaults.** Each default is derived from the
   entity it acts on (CLAUDE.md "Defaults"), in one table in
   `tools/action_matrix.py`. Here `size` is the body's largest bbox extent,
   `least` its smallest, `a` a face's √area and `r0` its `inset_mm`. An
   action whose default would come out at zero or would not apply is `n/a`
   and is not recorded. Some GUI tools start from a literal: Fillet and
   Chamfer at 0.5 and Shell at 0.25 (`CreateActions.cpp`). The matrix does
   not copy those numbers.

   | entity | action | feature and default |
   |---|---|---|
   | edge (not `smooth`) | `edge.fillet`, `edge.chamfer` | the edge alone, 10 % of the shortest of its length, each adjacent face's `a`, and its radius if circular |
   | circular edge | `edge.frame` | Frame `from_element` |
   | planar face | `face.sketch` | Sketch with `face_ref`, a circle at `inside_point` of radius ½ `r0`; it passes when the sketch's plane after regen is the face's plane |
   | | `face.extrude_add`, `face.extrude_cut` | that sketch, Extrude `add` / blind `cut`, 5 % of `size` |
   | | `face.hole` | a point sketch at `inside_point`, Hole `simple`, `through`, diameter `r0` |
   | | `face.shell` | Shell opening that face, 5 % of `least` |
   | | `face.draft` | Draft of the adjacent faces square to it, 2°, neutral plane the world plane square to the face (a face whose normal is not a world axis is `n/a`) |
   | | `face.frame` | Frame `from_element` |
   | cylindrical face | `curved.thread` | Thread with the ISO coarse designation whose major diameter is within 5 % of the face's (none is `n/a`), cosmetic |
   | | `curved.frame` | Frame `from_element` |
   | body | `body.pattern_linear` | Pattern of the body's newest feature, 2 along X, spacing 1.5 × its X extent (the GUI's own rule) |
   | | `body.pattern_circular`, `body.mirror` | 3 at 120° about Z, and mirror about YZ |
   | | `body.split` | Split at the mid-plane square to its largest extent (CreateSplit's default) |
5. **What an outcome is.** Each action is one `add_feature` or one `batch`
   (a sketch and its feature), followed by `undo`:
   - **pass**: the regen is ok with no warnings (a cut that removed nothing
     is a warning), `check_geometry` is valid, and `fuzz_design.check_step`
     holds (finite positive volumes, and the volume moves the declared way:
     a cut, Hole or Shell never adds, an add never removes);
   - **refuse**: the RPC answered with a structured error whose message is
     `fuzz_design.measured()` (it states a condition), and the part's
     `revision` is unchanged ("Refuse, don't substitute"; a refused call
     changes nothing);
   - **fail**: accepted, and the regen leaves the feature broken, warns, or
     a check after it fails. A broken feature is never a refusal;
   - **finding**: a crash or hang (60 s per action, killed by pid), an
     `internal` error, `nonfinite` numbers, a message naming a signal
     OpenCASCADE caught (`OSD_SIGSEGV`, `OSD_SIGBUS`, …), a failure message
     that states nothing, or an `undo` that does not give back the digest
     from before. A crash or hang restarts the instance and reopens the
     design, as `fuzz.py` does, and the run goes on.
6. **The ratchet.** `tests/action_matrix/baseline.json` holds, for each
   design and entity, its actions on one line, sorted: `edge.fillet` for a
   pass, `!edge.fillet` for a fail, `-edge.fillet` for a refusal. A run
   fails on:
   - a pass that is now a fail or a refusal;
   - a refusal that is now a fail (a visible reason became a broken
     feature);
   - any finding that is not in the baseline's `known` table, which lists
     each one with its `docs/known-bugs.md` anchor and its reproduction in
     `tests/action_matrix/open/`. A `known` entry that no longer reproduces
     also fails the run, saying to remove it and its bug entry, as
     `reference_parts`' `known_failure` does.

   Improvements (a fail or a refusal that now passes, a fail that is now a
   refusal) and entities or designs the baseline does not have are listed
   but not judged. Entities a later feature split into pieces get names by
   position (`piece.ordinal`, or role `positional`), and those names can
   swap between runs. They are keyed by family (the name without its
   ordinal) and judged by the number of passes in the family, so a swap is
   not a regression.
7. **Determinism.** The same build must give the same answer. A sample of
   one action in eight (by the hash below) is built in two fresh instances.
   The two runs must agree on the list of entity names (the open bug
   [a blend face's edge names depend on what the process did
   before](../known-bugs.md#a-blend-faces-edge-names-depend-on-what-the-process-did-before)
   is exactly this), and on each action's outcome and result digest (each
   body's volume and bbox to 1e-9 relative). Within one instance, the same
   action is also run twice (add, undo, add), which catches state an undo
   leaves behind.
8. **One tool and three tests.**

       tools/action_matrix.py run [--sample] [--shard K/N] [--design PAT]
                                  [--actions a,b] [--jobs N] [--out DIR]
                                  [--determinism] [--bless]
       tools/action_matrix.py bless --from DIR    # merge shard outputs

   Each worker owns one headless instance (`smoke_app` isolation, a
   private `XDG_STATE_HOME`); `--jobs` comes from free memory. A run writes
   one JSONL per design (entity, action, outcome, message, the call sent,
   seconds) and a `report.md` (rates by action and design, messages by
   `fuzz_design.gist`) under untracked `scratch/action_matrix/`. `--bless` rewrites
   the baseline and refuses a partial run (`--sample`, `--design`,
   `--actions`, or one shard without `--from`). The baseline is sorted, one
   line per entity, so a review of the diff sees exactly which entity
   changed. Sampling is by a stable hash of (design, action, entity family):
   one in 32 for `--sample`.

## Cost

Measured on a headless instance (`build/lobole --headless`): the 43
reference cases that build have 480 faces and 1062 edges and build in 4.5
s together, 3.2 s of it the modelled thread. The gear has 195 faces and 579
edges and builds in 0.25 s. The hinge box takes 2.6 s and its two owning
tabs have 106 faces and 245 edges. Per action, including the undo: a fillet
takes 5–9 ms on the small plates, 21–29 ms on the thin extrude and the
sheet-metal fold, and 87 ms on the thread; an extrude from a face 13–17 ms,
a through hole 17–23 ms, and a shell 210–300 ms.

The whole corpus comes to about 3500 edges and 1100 planar faces, about
14 000 actions: some 15 minutes in one instance. The sample is the
reference cases, eight of the fuzz seeds and the gear, at one in 32. That is
about 220 actions, under 20 s with the builds and the instance start,
which keeps it in the inner loop.

## Registration (tests/CMakeLists.txt, beside hostile_params)

- `action_matrix_sample`: `--sample`, unlabelled, so it runs in the
  headless suite and in `test-fast.sh`. TIMEOUT 120.
- `action_matrix_full_${k}` for `k` in 0..7: `--shard k/8`, with designs
  sharded by a stable hash of their name as `hostile_params_full_` shards
  methods. LABELS `slow;action_matrix`, TIMEOUT 900, so
  `tools/ctest -L action_matrix -j4` runs the sweep and `test-fast.sh`
  never does.
- `action_matrix_determinism`: `--determinism`, LABELS
  `slow;action_matrix`, TIMEOUT 600.
- `action_matrix_selftest`: the classifier is fed a fake source (a pass, a
  refusal that changed the revision, a broken feature, a caught
  `OSD_SIGSEGV`, an undo that did not restore, a swapped pair of ordinal
  pieces) and must judge each one right, as `fuzz_design`'s self-test does.
  Unlabelled, TIMEOUT 60.

## Files

- `tools/action_matrix.py` (new): the corpus loader, the action table, the
  classifier, the ratchet, `--bless` and `--from`. It imports
  `tools/fuzz_design.py` (`check_step`, `measured`, `gist`) and
  `tests/reference_parts.py` (`load_case`, `build`) rather than copying
  them.
- `tests/action_matrix_smoke.py` (new): the ctest entry point for the four
  tests above.
- `tests/action_matrix/corpus.json`, `tests/action_matrix/baseline.json`,
  `tests/action_matrix/open/` (new).
- `src/remote/RemoteInspection.cpp`: `list_elements` edge `smooth`, face
  `inside_point` and `inset_mm`, and its `MethodDoc` row (`inset_mm` is
  in mm, which `rpc_units_gate.py` checks); `python/lobole/sdk.py` (`Face`,
  `Edge`), `python/lobole/rpc.py` and `rpc-surface.json` remade.
- `tests/CMakeLists.txt`: the registrations above.
- `docs/modeling/README.md`: an "Action matrix" section beside "Random
  designs" (the corpus, the actions and their defaults, the outcome classes,
  the ratchet, how to bless); `docs/scripting/rpc.md` if `list_elements`
  is described there; the test table in `CLAUDE.md` gains the
  `-L action_matrix` row.

## Verified by

- `action_matrix_selftest` judging every planted case right.
- The new `list_elements` fields: a C++ case for an annulus and an L-shaped
  face (`inside_point` on the face, `inset_mm` its true distance to the
  boundary), and for a box rounded on all edges (every blend edge `smooth`,
  every edge between two planes not).
- The first full run, blessed into the baseline and read in review. Every
  finding in it is either fixed with a regression test or filed in
  `docs/known-bugs.md` with its reproduction in `tests/action_matrix/open/`
  and its `known` entry. The thread's caught `OSD_SIGSEGV` is the first.
- The ratchet seen to bite: a planted regression (Fillet refusing every
  circular edge) fails `action_matrix_sample` naming the entities, and
  reverting it passes again.
- `action_matrix_determinism` passing on a quiet machine and under
  `tools/with-cpu-load.sh`; `test-fast.sh`'s time grown by no more than the
  sample's own seconds; the headless suite.
