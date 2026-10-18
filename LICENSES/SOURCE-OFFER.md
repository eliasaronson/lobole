# Corresponding source and LGPL replacement notice

Every Loböle release, with the locally modified PlaneGCS and libarea
libraries, is available in source form at its tag, `v<version>`:

<https://github.com/eliasaronson/lobole/tags>

The release source archive generated from that tag is the exact corresponding
source for Loböle, `libplanegcs` and `libdesigner_adaptive`, including build
scripts and all local PlaneGCS and libarea modifications. A build that is not
a release names its commit in its build metadata (`lobole --version`), and
that commit is its corresponding source.

Loböle dynamically links the following LGPL-family libraries:

- **PlaneGCS** — LGPL-2.1-or-later, locally modified, source under
  `deps/planegcs` in the Loböle repository.
- **libarea Adaptive** (`libdesigner_adaptive.so`) — Kresimir Tusek's
  adaptive clearing, LGPL-2.1-or-later, with the Clipper 1 it uses internally
  (BSL-1.0) built into the same library. Taken from FreeCAD commit
  `b2da06bfe0521773f302c8be977b31dc27f60203` (`src/Mod/CAM/libarea/`) and
  locally modified: source under `deps/libarea` in the Loböle repository,
  with every change as a dated patch in `deps/libarea/patches/`.
- **Netgen 6.2.2605** — LGPL-2.1-or-later, unmodified upstream source:
  <https://github.com/NGSolve/netgen/tree/v6.2.2605>.
- **OpenCASCADE Technology 7.9.3** — LGPL-2.1 with the OCCT exception,
  upstream source <https://github.com/Open-Cascade-SAS/OCCT/tree/V7_9_3>
  with the patches in `nix/patches/` applied (`nix/occt-overlay.nix` says
  what each one changes).

The shipped libraries remain separate shared objects. Users may replace them
with compatible modified builds. For PlaneGCS, configure Loböle against the
replacement library or replace `libplanegcs.so` while preserving its ABI; the
same holds for `libdesigner_adaptive.so`, whose only caller is
`src/core/cam/CamAdaptive.cpp`. No cryptographic signature or other mechanism
prevents replacement.

For at least three years after a Loböle binary release, a copy of the exact
corresponding LGPL source will also be provided on request for no more than the
reasonable physical cost of distribution. Requests may be filed at:

<https://github.com/eliasaronson/lobole/issues>
