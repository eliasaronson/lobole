# Standard parts — plan (F1)

> Status: **COMPLETE — phases 1-5 SHIPPED 2026-09-01.** The tables and their
> refusals; the `Fastener` feature (v69) placed as an occurrence; the
> derivation — pick a hole's rim and the size and length follow; seven
> standards covering the whole joint (v70); and the polish: the BOM tells
> bought from made, `check_fasteners` reports a stack that changed under a
> screw, and a drawing callout names the part.
>
> Written 2026-09-01 from a survey of the open
> libraries, both of which were cloned to `~/Documents/CAD/parts/` and read
> rather than judged from their websites. The survey, the source comparison and
> the interface argument live in
> [../capability-review-2026-08-31.md](../capability-review-2026-08-31.md)
> under "F1 in detail"; this doc is the build.
>
> It became the roadmap's **seventeenth workstream**, v1-complete;
> roadmap.md is authoritative for status.
>
> **One thing it did not build: a way to place a fastener by CLICKING.**
> `fit_fastener` is RPC-only, which inverts the U10 argument the derivation
> was for. Planned in [gui-plan.md](gui-plan.md), about a day, and it opens
> with a correctness fix that should ship on its own — the derivation
> resolves a picked rim against the FIRST occurrence of the part, so on a
> joint made of two occurrences of one plate it places at the wrong end.

## Why

Not convenience. The review's original justification — that dogfooding rounds
"hand-modelled their screws" — was checked and was wrong: the C-clamp's screw
is a *designed component*, the dock's fasteners exist only in a bought-parts
note, and the speaker's cross-dowels are deliberately absent because they are
driven by hand. The real pattern is that **holes are modelled and fasteners are
omitted**, in every assembly we have built.

Which makes this a correctness gap, not a tedium gap:

> Every assembly we have built is missing its fasteners, so the BOM, the mass
> properties, and every contact and interference check are blind to them.

`contact_report` cannot tell you a bolt does not reach. `bom` cannot list what
to buy. `mass_properties` is light by however much the hardware weighs. Those
are the checks this project is unusually good at, and they are all reporting
confidently about an incomplete model.

## The data

Vendored, not depended on. Two sources, each for what it is best at — see the
review for the measurements behind this split.

| from | what | licence |
|---|---|---|
| **FastenersWB** | fastener dimensions, one CSV per standard | GPL-2.0-or-later |
| **FastenersWB** | `iso4757def.csv` (Phillips), `iso10664def.csv` (Torx) | GPL-2.0-or-later |
| **bd_warehouse** | `clearance_hole_sizes.csv`, `tap_hole_sizes.csv`, `drill_sizes.csv` | Apache-2.0 |

The hole tables are bd_warehouse's alone: FastenersWB has only an inch tap-drill
chart and no metric clearance table at all. They belong to
[../modeling/hole-standards.md](../modeling/hole-standards.md), which is a
separate and much cheaper plan — **do not fold the two together**, or the
298-row lookup waits behind the geometry work.

Obligations that travel with the files: a provenance header in each vendored
CSV, entries in `THIRD_PARTY.md`, and the Apache NOTICE. Both licences are
compatible with AGPL-3.0 (FastenersWB is GPL-2.0-**or-later**; v2-only would
not have been — checked, not assumed).

**Vendor selectively.** One standard per file is why FastenersWB is the right
quarry: ship the four types phase 1 needs, add a file when someone needs the
fifth. A vendored table is a fixed scope by definition, which is why an unknown
size is REFUSED naming what is stocked rather than interpolated — a screw
interpolated to a size nobody makes is the plausible-and-wrong answer this
codebase refuses everywhere else.

### Where the data lives, and how it is found

`assets/parts/`, following the pattern the HDRI and the fonts already use:
CMake copies it to `${CMAKE_BINARY_DIR}/assets/parts` for a development build
and installs it to `share/designer/parts`, and `RuntimePaths::DataDir()`
resolves either (with `DESIGNER_DATA_DIR` overriding both). A
`RuntimePaths::PartsDir()` beside `EnvironmentHdri()` is the one place the
subdirectory name is written, for the reason that comment already gives: the
HDRI filename had been spelled out at six sites before it was centralised.

The files keep their upstream names (`iso4762def.csv`, `iso888length.csv`) so
a diff against the source is possible, with a provenance header prepended.

**Missing data is a refusal, not an empty result.** A build that cannot find
its tables says so, naming the path it looked in. A fastener that silently
comes back as nothing is the failure this codebase spends most of its
conventions preventing.

### The shape of the vendored tables

FastenersWB's files are CSV with a quoted table name on line 1 and the header
on line 2:

    "ISO4762def"
    "Dia","P","b","dk_max","da","ds_min","e","lf","k","r","s_mean","t","v","dw","w"
    "M1.6",0.35,15.0,3.0,2.0,1.46,1.73,0.34,1.6,0.1,1.56,0.7,0.16,2.72,0.55

84 sizes for ISO 4762 alone. The **key is the `Dia` column** — `"M6"` — with
pitch as its own column `P`, which is better than folding the pitch into the
key: a fine-pitch variant is another row, not a different naming convention.

Lengths come from `iso888length.csv`, the nominal length series (63 entries,
2 mm upward) — that is what a derived grip snaps to. Which lengths a given
type is actually *made* in is a refinement, and bd_warehouse's
`nominal_screw_lengths.csv` has it per type when that matters.

### Material, because mass is one of the reasons to do this

A generated fastener carries a material — steel by default, since that is what
an unmarked screw is — and settable per placement, because "the BOM and mass
are blind to the hardware" is one of the two arguments for building this at
all. A fastener with no material would leave `mass_properties` exactly as
wrong as it is today, in a more convincing way.

## The model

**A fastener is a PART, placed as an OCCURRENCE.** Not a body inside your part,
and not for tidiness:

- **CAM would machine it.** Toolpath generation runs over the active part's
  bodies, so a screw body in a part tab gets sectioned, stocked around and cut.
- **Mass and BOM would count it as your part** rather than as a bought item.
- **`check_geometry` would flag it**, correctly — a loose screw body is a
  disconnected component of a multi-body part.

So: one generated part document per distinct `(standard, size, length, drive)`,
reused by every occurrence with that spec, named as its BOM designation
("ISO 4762 M6x20"). Quantity then falls out of `bom`, which already aggregates
by part; mass falls out of the material; and the faces get stable element names
because it is an ordinary regenerated document.

Adding one on a **part tab is refused**, naming why and what to do instead.

A new feature type carries the spec:

    {"Fastener", <version>,
     "standard: iso4762|iso4014|iso4032|iso7089|…, size: \"M6-1\", "
     "length (+_expr), drive?: hex|ph|torx|slot|square, "
     "thread?: cosmetic|modelled"}

with the usual consequences: `kFeatureTypes` gains a row (gated both
directions), the format bumps, a golden lands at that version, and
docs/versioning/format.md's history table gets its row.

### Generated parts must not flood the tab bar

Twenty occurrences across five sizes and four lengths is twenty part documents.
The tab-folder machinery already exists and STEP import already uses it (a
folder named after the assembly), so generated fasteners go in a "Standard
parts" folder. Named here because the failure mode — a tab bar nobody can find
their own part in — would be discovered late and blamed on something else.

## The interface: derived, not a form

The tempting shape is a dialog with thread, length, head and drive. Three of
those four should not be asked for, because
[../ui/u10-defaults-and-refusals.md](../ui/u10-defaults-and-refusals.md)
already forbids it: *a default carrying a unit must be derived from what it
acts on, never a literal*. A typed screw length is right for the stack the
person had in mind, wrong for every other, and — the part that matters — **a
wrong-length screw looks correct in the viewport**.

| | |
|---|---|
| thread size | **derived** from the picked hole |
| length | **derived** from the stack it passes through |
| head type | chosen — no geometry implies it |
| drive | chosen — likewise, though the standard usually fixes it |

Primary entry point: **pick the hole's rim in an assembly** (a circular edge —
see below for why the rim rather than the bore), choose a head, done. CAM's
verb model applied to fasteners. The toolbar path with no pick has
nothing to derive from and therefore **refuses by name** — "pick the hole this
fastener goes through" — which U10 requires and which is the honest answer for
a screw with no hole.

It composes with the hole plan in both directions: a hole that recorded
`fit: {thread: "M6", kind: normal}` states its size outright, and a plain Ø6.6
hole is looked up in the same table BACKWARDS ("that is an M6 normal
clearance") or refused when it matches nothing.

Scripted twin, deriving identically:

    add_fastener {target: {feature_id, element},   # the rim, a circular edge
                  standard: "iso4762", drive: "hex",
                  far_end?: {feature_id, element}}  # optional override

### The pick is an EDGE, not a face

A cylindrical face has an axis but no direction, so "which end does the head go
on" is not answerable from it. A **circular edge** answers both: it is the rim
the head sits against, and `Measure` already reports `kind: "circle"` with a
`radius` and an `axis` for one. Same gesture as picking an edge to fillet.

So: **pick the rim you want the head against.** One pick, unambiguous
direction, no camera inference and no flip button to get wrong.

### Deriving the stack, and the four cases that break the naive version

From that edge, cast along the axis, inward, through the flattened assembly.
What comes back decides everything — and the interesting part is that the
awkward cases are all *findings* rather than obstacles.

**A gap between two parts.** The ray passes through air. Two meanings, and
nothing in the geometry distinguishes them: a real gap (an unmodelled gasket,
parts drawn apart before clamping) or a **defect** — the floating mount plate
that cost a dogfooding round, 35 mm apart and invisible at render distance
because confirmation bias filled it in.

So do not guess. **Span it, and decompose the answer in the summary:**

    M6x45 — grip 41.5: Plate 12.0, gap 3.0, Bracket 26.5

If the gap is a mistake, the fastener length is the thing that makes it
visible. And `contact_report` already reports unintended gaps independently, so
the two agree rather than one silently absorbing the other.

**A blind hole.** The ray terminates inside a part. Then length is the parts
above plus **thread engagement** into the last one, and engagement is a rule
rather than a table — roughly 1x diameter into steel, 1.5-2x into aluminium or
plastic. Per-body materials already exist, so that is derived, not typed.

A check falls out of it: a blind hole that is a **clearance** hole rather than
a tapped one has nothing for a machine screw to bite. With
[../modeling/hole-standards.md](../modeling/hole-standards.md) the hole says
which outright; without it the diameter tells you, from the same vendored
tables (M6 tap drill 5.0 against clearances of 6.4/6.6/7.0 — not a close call).
Refuse, naming the two ways out: tap it, or put a nut on the far side.

**The holes do not line up.** The ray hits a part's *material* instead of its
hole. That is a real defect, and refusing with "the hole in Bracket is 2.3 mm
off this axis" is worth far more than quietly generating a bolt through solid
metal.

**A hole that goes nowhere.** The ray exits without entering anything: a
surface feature, not a joint. Refuse and say so.

### Two picks, but only when you want them

Selecting both the start and the end rim is two picks for something derivable,
and it does not solve the hard half — you still need what lies between them for
the grip and for the clash check. So: **one pick derives, a second optional
pick overrides the far end.** The same shape as CAM, where a picked wall and a
targetless "cut out the whole part" are both legitimate.

### Length snaps to a stocked size

The derived grip is 22.7 mm; screws are made in 20, 25, 30.
`nominal_screw_lengths.csv` carries the list per type (DIN 931: 12, 16, 20, 25,
… 300), so the derived number is **snapped to the shortest stocked length that
still satisfies the rule** — full grip plus protrusion for a through bolt, grip
plus engagement for a blind one. If no stocked length fits, refuse naming the
range rather than inventing one: a screw at a length nobody makes is the
plausible-and-wrong answer, and it looks perfect in the viewport.

### What v1 does not place

The **nut and washers** — *until phase 4, which does*. The plan expected the
far-side nut to be a second pick on the far rim; it is not, and the reason is
the one argument for deriving at all: **the hardware changes the length**. Two
1.6 mm washers and a 5.2 mm nut on a 38 mm stack move an M6 from 40 to 50 — two
nominal lengths, not a rounding, and not something anyone adds in their head
reliably. A screw sized for the plates and *then* fitted with washers is too
short and looks exactly right in the viewport.

So one pick still derives, and `{nut, washer_under_head, washer_under_nut}`
say what the joint is made of — the one thing the geometry genuinely cannot
answer, since plenty of joints have neither and adding them because they would
fit is the substitution this document is about. Every fastener is built with
its bearing face on z = 0 and its body below, which is what lets all four be
placed by the same two questions: where is the face, and which way is away
from the material.

### The cross-document catch

Deriving length from the plate stack is a dependency on other documents'
geometry, which regeneration cannot express (**A2**). Resolve it the way CAM
resolves its work origin: **derive once at placement, store the value**, and
make a stale one *reported* rather than silently wrong — the assembly offers
"re-fit fasteners" the way A4 propagates part changes. A fastener whose stack
changed under it is a fact worth surfacing, not a live binding worth widening
regen's input surface for.

## Phases

1. **The table layer, no geometry.** — **SHIPPED.** `assets/parts/` (six
   vendored files), `core/parts/FastenerTable`, `RuntimePaths::PartsDir()`,
   `tests/test_parts.cpp`.

   Three things the data turned out to be, none of which this plan predicted:

   - **A file holds SEVERAL tables**, separated by blank lines.
     `iso4762def.csv` is three — the dimensions, the nominal length series for
     ISO 4762, and the length RANGE each size is made in. So "is an M6x200 a
     thing" is answerable from the vendored data, and `SnapLength` consults
     the per-size range rather than offering an M6 at a length only an M20 is
     made in. The plan expected to need the generic ISO 888 series for this.
   - **The format is not uniform.** Some files name each table on a line of
     its own, some go straight to the header. Both are read rather than one
     being normalised away, because the files are kept byte-identical to
     upstream so a re-vendor can diff.
   - **ISO 4762 stocks 20 sizes, not 84.** The larger number was the whole
     file, three tables conflated.

   And two bugs the tests caught, both worth recording because neither was
   about the parser being wrong in the abstract:

   - **A trailing empty field was dropped.** `"M6",6.0,` splits to two fields,
     not three, because `getline` hits EOF rather than yielding a final empty
     one — so a row whose last column is blank looked like a row with a column
     missing and was refused. A blank last column is exactly the shape a
     standard that does not define its final dimension at some size takes.
   - **A refusal named the wrong sizes.** "M7 is not stocked" truncated an
     84-entry list from both ends, dropping M6 and M8 — precisely the two
     sizes that answer the question. It now leads with the neighbours: *"the
     nearest stocked are M6 and M8; all sizes are …"*.

   One expectation of mine was also wrong rather than the data: `s_mean` is
   the MIDDLE of the socket's tolerance band, so an M6's 5 mm hex reads 5.06.
2. **One fastener, end to end.** — **SHIPPED.** `FastenerFeature` (format
   v69, golden at that version), `ui::AddFastener`, the `add_fastener` RPC,
   and the claims measured rather than asserted: nine occurrences of two specs
   make **two** part documents, `bom` reports quantity 8 for the repeated one
   without knowing anything about fasteners, and a part tab refuses.

   Three things the build corrected:

   - **Mass needs the material SLUG, not its name.** `MassProps` resolves
     through `MaterialLibrary::FromSlug`, so a body tagged "Steel" reads as
     unknown density and the BOM says "mass unknown". Found by reading the BOM
     rather than the field — which is the whole reason mass is one of the two
     arguments for this feature.
   - **`RuntimePaths::DataDir()` only looked beside the executable**, so
     `build/tests/designer_tests` could not find `build/assets`. A golden
     carrying a Fastener regenerated to zero bodies in the test binary and to a
     screw in the app. It now also looks one level up.
   - **The generated mass is an UPPER BOUND.** A cosmetic thread is a full
     -diameter shank, so a steel M6x20 comes out at 7.6 g against a
     catalogue's ~6 g. The right error to have, and worth stating before
     someone compares against a supplier and files a bug.

   The volume is gated analytically against the table's own numbers — head
   cylinder + shank cylinder − hexagonal socket = 970.21 mm³ for an M6x20 —
   rather than against a remembered mass, which is a figure this plan had
   wrong ("about 4.5 g").
3. **Derive from a hole.** — **SHIPPED.** `parts::TraverseAxis`,
   `ui::FitFastenerToHole`, the `fit_fastener` RPC (with `dry_run`), and the
   four cases each gated in `test_actions.cpp`.

   **The design was wrong and the geometry corrected it.** A ray straight down
   the hole's axis hits NOTHING: the hole is a void, and every plate the screw
   passes through has one on that line by construction. The material a screw
   spans is *beside* the hole. So the probe is a bundle of four rays offset
   just outside the bore wall — four rather than one because a hole near an
   edge has material on some sides and not others.

   **And then that was not enough either.** The off-axis bundle measures PLATE
   THICKNESS, which is a different question from what STOPS the screw: it
   reported a blind hole 8 mm into a 20 mm plate as a 20 mm through joint, and
   a plate whose hole was 20 mm off the axis as perfectly fine. Both need the
   on-axis ray the bundle deliberately does not cast. So there are two probes,
   and the classification falls out of comparing them:

   | on-axis | means |
   |---|---|
   | nothing | a through joint; the screw spans the stack |
   | blocked inside the LAST part, past its face | a blind hole; length snaps DOWN so it cannot bottom out |
   | blocked AT a part's face | that part has no hole on this axis — a misalignment, refused |
   | blocked in an EARLIER part | its hole does not go through, refused |

   Distinguishing the last two matters more than it looks: told "the hole is
   0 mm deep" a person goes looking for a drilling depth, when what is wrong is
   that two holes do not line up.

   Measured on a real stack: `M6x45 — grip 41: Top 12, gap 3, Bottom 26`. The
   size is read backwards out of the clearance table (Ø6.6 is an M6 normal),
   41 mm of grip snaps up to the 45 that ISO 4762 is made in, and the gap is
   named rather than silently absorbed.
4. **Breadth.** — **SHIPPED.** Seven standards (four screw heads, two drives,
   a nut and a washer), `core/parts/FastenerProfile`, and the joint rather
   than the screw: `fit_fastener` now takes `{nut, washer_under_head,
   washer_under_nut}` and the LENGTH accounts for them.

   **The plan's interface table was wrong about the drive, and this is the
   correction.** It said the head and the drive are *chosen*. They are not:
   you do not order an ISO 4762 with a torx drive, you order an ISO 14579.
   The standard fixes the head, the recess, and whether the nominal length is
   measured over the head or under it — a fastener is a catalogue item, not a
   configuration of one. So the drive defaults to the standard's own and a
   drive that disagrees is refused *naming the standard that has it*, which is
   the correction the caller actually needs. That also collapsed the form: of
   the four fields the plan expected, one (`standard`) is left.

   | standard | | length is |
   |---|---|---|
   | iso4762 | socket head cap screw, hex | under the head |
   | iso14579 | hexalobular (torx) socket head cap | under the head |
   | iso4017 | hex head screw, no recess at all | under the head |
   | iso7380 | button head, hex socket | under the head |
   | iso10642 | countersunk socket head | **overall** |
   | iso4032 | hex nut | — |
   | iso7089 | plain washer | — |

   Four things the data and the geometry corrected:

   - **A dimension table can hold a NAME.** `iso14579def.csv` says which
     driver each size takes ("M6","T30",2.3) between two real dimensions, and
     the phase-1 parser refused the whole file over it — which would have
     meant no torx at all. A non-numeric field is now text and is ABSENT from
     the numbers, so nothing reads a driver size as a millimetre. That is a
     deliberate loosening of "a file that half loads is worse than one that
     refuses", and the guarantee it keeps is the one that mattered.
   - **A parenthesised size is a size.** ISO 4032 stocks "(M3.5)"; the
     parentheses mark a size to avoid designing around, not a different part.
     Asking for M3.5 was refused with "the nearest stocked are M3 and M4",
     which is a lie about a row in the table.
   - **The button head's socket opens THROUGH the crown.** The recess is
     wider than the dome near the top, so the apex is not there to measure and
     the head is 0.36 mm shorter than the standard's `k`. That is what a
     button head cap screw looks like from above; the test asserts the derived
     crossing rather than `k`, and it is the one head whose volume is not
     area x depth.
   - **A tangent boolean came back 0.36 mm short.** Clipping the dome's sphere
     with a box whose floor sat exactly on the apex produced a measurably
     flatter dome and no error. Clipping below it costs nothing.

   And one asymmetry worth stating before someone files it as a bug: a
   cosmetic thread ADDS material to a screw (a full-diameter shank) and
   REMOVES it from a nut (a full-diameter bore), so the generated screw is a
   little heavy and the generated nut a little light. Same simplification,
   opposite signs, seen from outside and inside the thread.

   **What is refused rather than approximated:** a countersunk screw fitted to
   a plain bore's rim (its countersink is not modelled, so the head would end
   up inside solid material — it waits on
   [../modeling/hole-standards.md](../modeling/hole-standards.md)), a nut on a
   blind hole, a washer standard that is not a washer, and Phillips, whose
   vendored table is still unused because a cruciform drawn as a hexagon is
   the substitution this whole document is about.
5. **Polish.** — **SHIPPED.** Format v70 (golden + history row): the BOM knows
   a bought part from a made one, `check_fasteners` closes the staleness gate,
   and `fastener_callout` puts the designation on a drawing.

   - **Bought vs made, derived.** A `BomRow` carries `standard` and
     `designation` when the part's whole feature list is one Fastener — which
     stays right when the tab is renamed, where a stored flag would not. They
     go in the CSV, where a purchaser reads them, and in the row tooltip.
     There is deliberately **no sixth column**: the panel already clips at
     five in a 210 px dock (three parts all reading "Gang" is what drove the
     adaptive layout), and a fastener's part name IS its designation, so the
     column would repeat column one while narrowing every other cell.
   - **The staleness gate, with nothing stored.** The plan expected to
     "derive once at placement, store the value". It does not need to: a
     placed fastener's position and orientation ARE its rim and its axis, so
     `check_fasteners` re-traverses from scratch and compares. That also
     checks a fastener nobody derived, which a stored record could not, and it
     finds the washers and nut the same way the fit did — they are occurrences
     on the same axis.
   - **`stack.back()` is not the last thing on the axis.** `TraverseAxis`
     orders by where each solid is ENTERED, which stops matching where it is
     LEFT as soon as anything overlaps: a plate moved down past a nut is
     entered after the washer and left after the nut. Reading the end of the
     ordered list said a stale screw still reached — the exact case the check
     exists for. Both the check and the fit now take the maximum exit.
   - **A placed fastener does not know its hole.** It knows its size, so the
     probe is offset past the LOOSEST clearance that size is drilled to. A
     ray at the thread radius runs down the inside of the bore and finds no
     material at all, which is how this was found; a hole wider than "Loose"
     makes the check cry wolf, which is the right way round.
   - **`length` was required by the loader**, so a nut and a washer were
     unreachable through `add_feature` while the feature itself built them
     perfectly well. Whether a length means anything is the standard's
     business; `Execute` still refuses a screw without one.
   - **The drawing callout is a leader, not a balloon.** `fastener_callout`
     names the occurrence's own designation — the same string the BOM carries,
     so a drawing cannot say M6x50 while the parts list says M6x45. Anchored
     on whatever edge is picked rather than on a circle, because a screw is a
     circle from above and two lines on the section anybody actually draws.
     Balloons and a parts-list block on the sheet are the other half of
     assembly drafting and are NOT this: they are a workstream, not polish.

Phases 1 and 2 are the ones that decide whether this is right. If part-per-spec
turns out to be wrong, it is cheap to discover with one fastener type and
expensive to discover with twenty.

## Gates

- **The table is the oracle.** A generated ISO 4762 M6x20 is checked against
  the standard's own numbers — head diameter, head height, socket size, thread
  length — because the vendored row IS the specification. This is the check
  that matters; the geometry is a means to it.
- **Mass, in real numbers** — and the figure this line first carried, "about
  4.5 g", was WRONG, which is why the gate is analytic instead. A generated
  steel M6x20 is 7.6 g: a cosmetic thread is a full-diameter shank, so the
  model is an upper bound on a catalogue's ~6 g. The test checks the volume
  against head + shank − socket computed from the table's own numbers, so it
  still catches a unit error, a wrong density and a half-scale profile —
  without pinning a remembered mass that was never right.
- **Refusals**: unknown size, unknown standard, unknown drive, and a fastener
  on a part tab — each naming what is accepted (`rpc_gate_smoke`,
  `strict_inputs_smoke`).
- **BOM aggregation**: eight identical screws are one line with quantity 8.
- **Staleness**: a fastener whose stack has changed reports it rather than
  silently keeping its old length — `check_fasteners`, gated in
  `test_actions.cpp` by moving a plate out from under a placed joint.
- Format bump + golden + the format.md history row, per policy. Four of them
  in the end: v69 the feature, v70 the seven standards and the callout, v71 a
  hole recording what it is for, v72 an occurrence riding on another. The
  corpus holds no drawing documents, so the `fastener_callout` dim kind is
  gated by `test_drawing.cpp` instead of by a golden — said here rather than
  left as a gap someone has to rediscover.

## Follow-ups

> All three SHIPPED 2026-09-01; see [f1-loose-ends.md](f1-loose-ends.md),
> which also answers the sequencing question they raise: **the assembly
> solver (A1) does not gate any of it.** A fastener is a zero-DOF leaf, the
> current solver already places those correctly, and the design below bypasses
> the solver entirely.

### A placed fastener is positioned, not mated

Checked 2026-09-01, and true by construction: `AddFastener` builds an
`Occurrence` with a position and an orientation and nothing else, `fixed`
defaults to false, and the solver says what that means outright — *"unmated
occurrences have no world entry and are untouched"*.

So a fastener will not drift; it also will not follow. Move a mated plate and
the screw stays behind in space, and `check_fasteners` reports it only when
somebody asks.

**The A2 argument does not rescue this one, and that is the point.** Deriving
a LENGTH across documents genuinely cannot be a live binding, so reporting
staleness is the right answer there. POSITION within a single assembly is
exactly what mates are for, so "we report it instead" is a weaker answer here
than it was there.

Three things make it a design question rather than a missing line:

- **What does each piece mate to?** The screw is rigid relative to the part
  its head bears on; the nut bears on the FAR part. Mate everything to the
  entry part and the far side is wrong once the plates move relative to each
  other; mate each to what it touches and a separating joint leaves the screw
  and the nut drifting apart — honest, and it looks broken.
- **Mates MOVE things.** The solver places by direct frame alignment, so a
  fastened mate would relocate the fastener to align frames, possibly
  fighting the transform the fit computed. The generated parts' faces do
  carry element names, so connectors are possible; agreeing with the
  derivation rather than overriding it is the work.
- **Volume.** Up to four mates per joint, and there is no rigid-group or
  parent concept on `Occurrence` to lean on (checked). Fifty bolts is two
  hundred rows in the Mates list.

**The dogfood round answered it, and the answer was not "a mate".** Using it
surfaced the sharper version: a placed fastener is not merely uncarried, it is
freely DRAGGABLE — `GizmoOccurrence` hands a gizmo to any selected non-fixed
occurrence — so you can pull a screw out of its hole and nothing objects. And
a mate is the wrong instrument for a thing with zero degrees of freedom. See
[f1-loose-ends.md](f1-loose-ends.md) for the attachment design that replaces
this entry.

### A pick slot for a non-feature tool must be registered twice

See [../known-bugs.md](../known-bugs.md). Latent rather than broken — all four
sentinels are correct today — and the fix is one registry gated by
`test_conventions.cpp`.

## Out of scope

- **Modelled helical threads.** Cosmetic by default — which is what Designer
  already has, and what bd_warehouse itself defaults to (`simple=True`, because
  the real thread is expensive). The roadmap's modelled-thread stretch flag is
  a separate question this does not force.
- **Pozidriv.** Neither source has it. Worth knowing before promising it.
- **Bearings, extrusions, motors, o-rings.** `NopSCADlib` has many, but GPL and
  OpenSCAD-native makes it a data-mining exercise rather than a dependency.
  Fasteners are the right first scope: they are what every assembly we have
  built is missing.
- **A parts browser.** The pick-a-hole verb is the interface; a searchable
  catalogue is what you build when the verb is not enough, not before.
