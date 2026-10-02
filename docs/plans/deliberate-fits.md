# Deliberate fits: an overlap the tolerances allow is not a clash

## Problem

An assembly's contact checks (`interference`, the exact sweep,
`contact_report`, exact motion studies; `Measure::ContactBetween`) cannot
tell a designed interference from a mistake. A dowel pin drawn 0.02 mm over
its bore, a printed bore carrying its allowance against a steel pin, a
heat-set insert, a bearing modelled at its seat's interference: each reads
as a clash, so a correct assembly cannot be shown clean. The one exception
is a retaining ring in its groove, relieved by a `RingGrip` stamped on the
geometry (docs/parts/README.md#a-ring-in-its-groove).

The information that makes an overlap deliberate is already in the model.
Faces carry ISO 286 size tolerances and bearing rings their ISO 492 bands
(docs/tolerances/README.md), and `check_fit`/`check_fits` already classify
each round mate as clearance, transition or interference from them. The
contact checks just never ask.

## Fix

**The fit declares the overlap.** A pair of round faces, coaxial, one
inside the other, both carrying a size band (a tolerance, a bearing ring's
ISO 492 band, or a printed part's allowance read at its printed size), whose
fit permits interference: the contact check relieves the overlap the bands
allow.

- **What is relieved:** the annulus between the two cylinders, up to the
  fit's maximum interference radially, over the length the two faces
  engage. Nothing else.
- **Beyond the band is still a clash.** So is any overlap with no fit
  behind it, and a pair that is not coaxial within a stated tolerance.
- **The report says so:** `press_fit` pairs list the interference, the fit
  ("Ø47 K7 / ISO 492 normal") and the relieved volume, separately from
  clashes. That mirrors how `ring_grip_pairs` is reported.
- **One path.** The `RingGrip` relief stays (a ring has no tolerance to
  read), but both go through one "designed overlap" step in
  `ContactBetween`, so every contact check honours both. A mesh-mode sweep
  treats a fit the way it treats threads and grips today, and says so.

**No new flag, and no new saved form.** The tolerances are already stored.
A fit does not need a mate either: two coaxial toleranced faces of placed
parts are found geometrically, as the ring's grip is. Where `check_fits`
already pairs them through a mate, use that pairing.

## Files

- `src/core/Measure.*` (`ContactBetween`, the designed-overlap step);
- the fit classification in `src/core/FitStandards.*` and `src/ui/` `check_fits`;
- the contact RPCs' replies in `src/remote/`;
- docs: `docs/tolerances/README.md`, `docs/inspect/README.md`.

## Verification

- A dowel pin modelled at the top of an interference fit in its bore:
  relieved and reported as a press fit.
- The same pin 0.01 mm larger than the band allows: a clash, naming the
  excess.
- An untoleranced overlap: a clash.
- A bearing modelled at its K7 seat's interference: a press fit in every
  contact check (named pair, sweep, contact report, exact motion).
- The pan/tilt head's exact sweep stays clean.
