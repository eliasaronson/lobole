// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/ImportCheck.h"

#include "cad/CADKernel.h"
#include "core/Measure.h"

namespace interchange {

std::string CheckImportedSolid(const CADShape& shape,
                               std::optional<double> volume) {
  if (!shape.IsValid())
    return "came in with no geometry";
  const int faces = shape.FaceCount();
  if (faces > static_cast<int>(kImportCheckFaceLimit))
    return "was not checked: " + std::to_string(faces) + " faces, past the " +
           std::to_string(kImportCheckFaceLimit) + " the import checks";
  const Measure::GeometryReport report = Measure::CheckGeometry(shape);
  std::string out;
  if (report.freeEdges > 0) {
    out = "came in open: " + std::to_string(report.freeEdges) + " free edge" +
          (report.freeEdges == 1 ? "" : "s");
  } else if (!report.valid) {
    std::string why;
    for (const std::string& r : report.reasons)
      why += (why.empty() ? "" : ", ") + r;
    out = "came in invalid: " + (why.empty() ? std::string("BRepCheck") : why);
  }
  const double v = volume ? *volume : shape.Volume();
  if (v < 0.0)
    out += std::string(out.empty() ? "came in" : ", and") +
           " inside out (its volume is negative)";
  return out;
}

} // namespace interchange
