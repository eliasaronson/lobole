// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// STL export of a part (docs/tolerances/README.md#exporting-a-printed-part):
// the one helper export_stl and File › Export both call, so a printed part's
// gaps survive the chords whichever way it is exported.

#include "core/Measure.h"

#include <cstddef>
#include <expected>
#include <optional>
#include <string>

class Document;

namespace ui {

// OCC's own angular deflection, now passed rather than assumed: a chord is
// fine enough when it meets BOTH this and the linear deviation, so the
// linear one is an upper bound on what a chord takes (printfit::ChordLossMm).
inline constexpr double kStlAngularDeflectionRad = 0.5;

struct StlExportResult {
  size_t bodies = 0;
  double deflectionMm = 0.0; // what the file was meshed at
  // Where that came from: "asked" (the caller's), "profile" (the part's
  // print profile states one), "material" (the material's own, TPU's),
  // "gap" (a tenth of the smallest gap a side), or "default" (an unprinted
  // part: 0.1 mm).
  std::string deflectionFrom;
  // A printed part: its smallest clearance a side, and the share of it the
  // chords can take (deflection / that). 0 when not printed.
  double smallestGapSideMm = 0.0;
  double chordShare = 0.0;
  // Non-empty when an explicit deflection is coarser than the part's print
  // asks: how much of the smallest gap the chords can take.
  std::string warning;
  Measure::ShellReport shells; // of the SOURCE B-Rep, not the mesh
  // Mesh bodies written as their own triangles, not re-meshed: the
  // deflection did not apply to them (docs/interchange/README.md#mesh-bodies).
  size_t meshBodies = 0;
};

// The non-reference bodies of `part` (only `bodyIndex`'s when >= 0), fused,
// as binary STL. With no `deflectionMm`, a printed part meshes at its
// print's printfit::StlDeflectionMm and an unprinted one at 0.1 mm; an
// explicit one is exported as asked, and warned about when coarser.
std::expected<StlExportResult, std::string>
ExportPartStl(const Document& part, const std::string& path, int bodyIndex,
              std::optional<double> deflectionMm);

// "chords 0.01 mm", plus the share of the smallest gap on a printed part:
// the status line's words for a result.
std::string StlExportSummary(const StlExportResult& r);

} // namespace ui
