# Standard parts — plan (F1)

> Status: **phases 1 and 2 SHIPPED 2026-09-01** — the tables and their
> refusals, then the `Fastener` feature (format v69), the generated
> part-per-spec and the `add_fastener` RPC. Phases 3-5 planned. Written 2026-09-01 from a survey of the open
> libraries, both of which were cloned to `~/Documents/CAD/parts/` and read
> rather than judged from their websites. The survey, the source comparison and
> the interface argument live in
> [../capability-review-2026-08-31.md](../capability-review-2026-08-31.md)
> under "F1 in detail"; this doc is the build.
>
> The roadmap lists sixteen workstreams and calls them all v1-complete.
> Whether this becomes a seventeenth or stays a follow-on is not decided here —
> roadmap.md is authoritative for status.

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

The **nut and washers**. A through bolt needs them, and "complete the joint"
(screw + washer + nut, all derived from the same ray) is the obvious follow-on
— but it multiplies the placement rules before the single-fastener ones have
been used in anger. v1 places one fastener per pick; the far-side nut is a
second pick on the far rim.

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
3. **Derive from a hole.** The edge pick, the size lookup (forward and
   backward), the axis ray and its stack decomposition, length snapping to a
   stocked size, and the staleness report. GUI verb + the `add_fastener` RPC.
   The four awkward cases above — gap, blind, misaligned, nowhere — are each a
   refusal or a reported decomposition, and each wants its own test: they are
   the reason this phase is bigger than it looks.
4. **Breadth.** Nuts and washers, then the remaining head types and drives, one
   vendored file at a time. Each is a profile and a table row — the survey
   measured a socket head cap screw's entire head at seven lines.
5. **Polish.** BOM designation column, the drawing callout, and whatever the
   first real assembly asks for.

Phases 1 and 2 are the ones that decide whether this is right. If part-per-spec
turns out to be wrong, it is cheap to discover with one fastener type and
expensive to discover with twenty.

## Gates

- **The table is the oracle.** A generated ISO 4762 M6x20 is checked against
  the standard's own numbers — head diameter, head height, socket size, thread
  length — because the vendored row IS the specification. This is the check
  that matters; the geometry is a means to it.
- **Mass, in real numbers.** An M6x20 steel socket cap screw is about 4.5 g.
  A test that asserts it catches a unit error, a wrong density and a
  half-scale profile at once.
- **Refusals**: unknown size, unknown standard, unknown drive, and a fastener
  on a part tab — each naming what is accepted (`rpc_gate_smoke`,
  `strict_inputs_smoke`).
- **BOM aggregation**: eight identical screws are one line with quantity 8.
- **Staleness**: a fastener whose stack has changed reports it rather than
  silently keeping its old length.
- Format bump + golden + the format.md history row, per policy.

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
