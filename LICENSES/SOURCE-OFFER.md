# Corresponding source and LGPL replacement notice

Designer 0.1.0 and the locally modified PlaneGCS and libarea libraries are
available in source form at:

<https://github.com/eliasaronson/Designer/tree/v0.1.0>

The release source archive generated from that tag is the exact corresponding
source for Designer, `libplanegcs` and `libdesigner_adaptive`, including build
scripts and all local PlaneGCS and libarea modifications. Until the `v0.1.0`
tag is published, the corresponding source is the commit identified by the
release artifact's build metadata.

Designer dynamically links the following LGPL-family libraries:

- **PlaneGCS** — LGPL-2.1-or-later, locally modified, source under
  `deps/planegcs` in the Designer repository.
- **libarea Adaptive** (`libdesigner_adaptive.so`) — Kresimir Tusek's
  adaptive clearing, LGPL-2.1-or-later, with the Clipper 1 it uses internally
  (BSL-1.0) built into the same library. Taken from FreeCAD commit
  `b2da06bfe0521773f302c8be977b31dc27f60203` (`src/Mod/CAM/libarea/`) and
  locally modified: source under `deps/libarea` in the Designer repository,
  with every change as a dated patch in `deps/libarea/patches/`.
- **Netgen 6.2.2605** — LGPL-2.1-or-later, unmodified upstream source:
  <https://github.com/NGSolve/netgen/tree/v6.2.2605>.
- **OpenCASCADE Technology 7.9.3** — LGPL-2.1 with the OCCT exception,
  unmodified upstream source:
  <https://github.com/Open-Cascade-SAS/OCCT/tree/V7_9_3>.

The shipped libraries remain separate shared objects. Users may replace them
with compatible modified builds. For PlaneGCS, configure Designer against the
replacement library or replace `libplanegcs.so` while preserving its ABI; the
same holds for `libdesigner_adaptive.so`, whose only caller is
`src/core/cam/CamAdaptive.cpp`. No cryptographic signature or other mechanism
prevents replacement.

For at least three years after a Designer binary release, a copy of the exact
corresponding LGPL source will also be provided on request for no more than the
reasonable physical cost of distribution. Requests may be filed at:

<https://github.com/eliasaronson/Designer/issues>
