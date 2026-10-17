// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/StlExport.h"

#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/PrintAllowance.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

constexpr double kUnprintedDeflectionMm = 0.1;

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

} // namespace

std::expected<StlExportResult, std::string>
ExportPartStl(const Document& part, const std::string& path, int bodyIndex,
              std::optional<double> deflectionMm) {
  if (deflectionMm && !(std::isfinite(*deflectionMm) && *deflectionMm > 0.0))
    return std::unexpected("stl_deflection_mm must be a positive chord "
                           "deviation in mm, not " +
                           Mm(*deflectionMm));
  // Derived reference bodies are other parts' geometry: never exported.
  std::vector<const Body*> bodies;
  for (const Body& b : part.GetBodies())
    if (!b.reference)
      bodies.push_back(&b);
  if (bodies.empty())
    return std::unexpected(
        part.GetBodies().empty()
            ? "no bodies to export"
            : "only reference bodies (derived ghosts); nothing to export");
  if (bodyIndex >= static_cast<int>(bodies.size()))
    return std::unexpected("body index out of range");

  StlExportResult r;
  // What the part's print asks for, when it is printed.
  double printed = 0.0;
  if (const auto& proc = part.PrintProcess()) {
    printed = printfit::StlDeflectionMm(*proc);
    if (const printfit::Table* t = printfit::Find(*proc))
      r.smallestGapSideMm = printfit::SmallestGapSideMm(*t);
  }
  if (deflectionMm) {
    r.deflectionMm = *deflectionMm;
    r.deflectionFrom = "asked";
  } else if (printed > 0.0) {
    r.deflectionMm = printed;
    const auto& proc = *part.PrintProcess();
    r.deflectionFrom =
        proc.profile && proc.profile->stlDeflectionMm > 0.0 ? "profile" : "gap";
  } else {
    r.deflectionMm = kUnprintedDeflectionMm;
    r.deflectionFrom = "default";
  }
  if (r.smallestGapSideMm > 0.0)
    r.chordShare = r.deflectionMm / r.smallestGapSideMm;
  // Exported as asked -- the caller may know its slicer -- but SAID: a
  // chord may sit its whole deviation inside a round surface, so each side
  // of a gap can close by that much.
  if (deflectionMm && printed > 0.0 && *deflectionMm > printed + 1e-12) {
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * r.chordShare);
    r.warning = "stl_deflection_mm " + Mm(*deflectionMm) +
                " mm is coarser than this printed part's print asks (" +
                Mm(printed) + " mm): the chords can take up to " +
                Mm(*deflectionMm) + " mm, " + pct + " of the smallest gap (" +
                Mm(r.smallestGapSideMm) + " mm a side)";
  }

  CADShape shape;
  if (bodyIndex >= 0) {
    shape = bodies[static_cast<size_t>(bodyIndex)]->shape;
    r.bodies = 1;
  } else {
    // STL is single-shape: several bodies are fused, as every CAD does.
    shape = bodies[0]->shape;
    for (size_t i = 1; i < bodies.size(); i++)
      shape = shape.Union(bodies[i]->shape);
    r.bodies = bodies.size();
  }
  if (!CADKernel::Instance().SaveSTL(path, shape, r.deflectionMm,
                                     kStlAngularDeflectionRad))
    return std::unexpected("could not write " + path);
  // Watertightness of the SOURCE B-Rep (not the mesh): a non-watertight
  // input still exports -- the report says whether to trust the STL.
  r.shells = Measure::AnalyzeWatertight(shape);
  return r;
}

std::string StlExportSummary(const StlExportResult& r) {
  std::string s = "chords " + Mm(r.deflectionMm) + " mm";
  if (r.smallestGapSideMm > 0.0) {
    char pct[16];
    std::snprintf(pct, sizeof(pct), "%.0f%%", 100.0 * r.chordShare);
    s += std::string(", ") + pct + " of the " + Mm(r.smallestGapSideMm) +
         " mm gap a side";
  }
  return s;
}

} // namespace ui
