# Reader fuzzing, round 2

## The problem

The committed reader-fuzz sweep runs with one fixed base seed, and every case
it generates passes. A second seed finds what the first did not. Seed 1 on
the part family alone fails eight cases, none of them in code that landed
with the first round
(docs/versioning/README.md#reader-fuzzing):

- **A file that eats the machine.** `part/v01-plain-box.json:grow_array:0`
  grows the v1 box (whose features carry no ids) to about 110 000 identical
  Box features. The reader accepts it and the rebuild runs past 8 GB; one
  attempt reached 23 GB before it was killed. In the app, opening such a file
  freezes the machine or gets the app killed by the OS.
- **Seven loads that keep going without a word**, each a value read wrong or
  dropped:
  - `v149-hole-point:liar_number:1` (`thread_depth`)
  - `v161-feature-template:replace_line:2` (a template's `parameters/id`)
  - `v137-text:json_liar:0`: its own save does not load (an angle in
    radians)
  - `v42-curve3d:replace_word:5` (`axis`)
  - `v179-printed-press:replace_line:5` (`print/name`)
  - `v179-printed-press:liar_number:1` (the print rows' `to_mm`)
  - `v51-mirror-and-center-ref:liar_number:3` (a circle's centre and radius)

The other families have not been run with any seed but the committed one.

## The shape of the fix

### A feature-count limit per part

A part document holding more than **50 000 features** is refused on load,
naming the file, the count and the limit: "a part holds at most 50 000
features; this one lists 110 412". The limit sits beside `kMaxJsonDepth` in
`src/core/io/ReadContext.h`, as `kMaxPartFeatures`, and is checked against
the `features` list's length before a single feature is built, in both part
loaders (the single file and the directory's index).

It is a sanity bound, not a default: the largest real part here (the om615
head) holds 389 features, and a part near the limit would already take far
too long to rebuild to be usable. It counts one part's features only. An
assembly's occurrences, which a large real assembly can number in the tens of
thousands, are not limited.

So that the app never writes what it cannot open, `add_feature` (and a
`batch` or GUI action that adds) refuses past the same limit, naming it. If
the limit is ever hit by real work, it is raised in one place.

### The seven silent loads

Each is reproduced as a committed case (`tools/reader-fuzz.py case`) before it
is fixed, so it fails first. The fix is in the reader that took the value: it
refuses what the writer never writes (a value of the wrong kind or out of its
range, a renamed key), naming the file and the item. `v137-text` is a writer
bug as well as a reader one: the angle it saves must be the one it reads.

### More seeds, every family

`tools/reader-fuzz.py run --seed S` runs every family for seeds 1 to 3. Every
failure becomes a case and a fix, as above, in this plan. A failure whose fix
needs a decision (a new limit, a behaviour change a user would see) is
written up instead and left out.

The committed sweep keeps its one base seed: a seed that moves would make
the suite's result depend on the day.

## Files

- `src/core/io/ReadContext.h`: `kMaxPartFeatures`.
- `src/core/io/Serialization.cpp`: the count check in both part loaders.
- `src/remote/RemoteModeling.cpp`, the GUI add path (`src/ui/PreviewFeature.cpp`):
  `add_feature` refuses past the limit.
- The readers each case points at (`src/core/io/SerializeFeatures.cpp`,
  the drawing, job and print readers), and the text writer for `v137`.
- `tests/reader_cases/<name>/`: one case per fix.
- `docs/versioning/README.md#what-a-load-refuses`: the limit.

## Verification

- The 110 000-feature case is refused in milliseconds with the count named,
  and its memory stays under 100 MB (`/usr/bin/time -v` in the case's test).
- `add_feature` on a part at the limit is refused with the document
  unchanged (`refused_calls_smoke`'s rule).
- Each of the seven cases fails before its fix and passes after.
- `tools/reader-fuzz.py run --seed S` for S = 1, 2, 3 on every family: zero
  failures, or each remaining one written up with its reason.
- `tools/ctest -L reader_fuzz` (including the every-case tests),
  `tools/ctest -L golden`, the headless suite.
