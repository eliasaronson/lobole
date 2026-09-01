# Standard parts — plan (F1)

> Status: **planned, not built.** Written 2026-09-01 from a survey of the open
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

Primary entry point: **pick a cylindrical face in an assembly**, choose a head,
done. CAM's verb model applied to fasteners. The toolbar path with no pick has
nothing to derive from and therefore **refuses by name** — "pick the hole this
fastener goes through" — which U10 requires and which is the honest answer for
a screw with no hole.

It composes with the hole plan in both directions: a hole that recorded
`fit: {thread: "M6", kind: normal}` states its size outright, and a plain Ø6.6
hole is looked up in the same table BACKWARDS ("that is an M6 normal
clearance") or refused when it matches nothing.

Scripted twin, deriving identically:

    add_fastener {target: {feature_id, element}, standard: "iso4762",
                  drive: "hex"}

### The cross-document catch

Deriving length from the plate stack is a dependency on other documents'
geometry, which regeneration cannot express (**A2**). Resolve it the way CAM
resolves its work origin: **derive once at placement, store the value**, and
make a stale one *reported* rather than silently wrong — the assembly offers
"re-fit fasteners" the way A4 propagates part changes. A fastener whose stack
changed under it is a fact worth surfacing, not a live binding worth widening
regen's input surface for.

## Phases

1. **The table layer, no geometry.** Vendored CSVs, a reader, and the
   refusals: an unknown standard, size or drive is refused naming what is
   stocked. Entirely headless-testable, and the phase that pins the data before
   any geometry depends on it.
2. **One fastener, end to end.** `Fastener` feature → a generated part
   document → an occurrence, for the socket head cap screw (ISO 4762, hex
   drive). RPC only, no GUI. This is where the architectural claims get tested:
   part-per-spec, folder placement, BOM aggregation, refusal on a part tab.
3. **Derive from a hole.** The pick, the size lookup (forward and backward),
   the stack traversal for length, and the staleness report. GUI verb + the
   `add_fastener` RPC.
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
