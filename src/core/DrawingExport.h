#pragma once

#include "core/DrawingDimension.h"
#include "core/DrawingDocument.h"
#include "core/DrawingRender.h"

#include <string>
#include <vector>

// Vector export of a drawing sheet (workstream 13, D2/D4). GUI-free core code:
// consumes the same DrawingDocument (sheet + title block) and projected
// SheetViews the viewport renders, so export is headless and CI-testable
// without a GPU. SVG lands in D2 to close the LLM loop; the PDF writer is D4.

namespace DrawingExport {

// Writes the sheet — border, title block, every view's visible (solid) and
// hidden (dashed) edges, and each resolved dimension (extension/dimension lines
// or a radial leader, arrows, value text) — as an SVG in millimetre user units.
// Returns false and fills *error on failure. `views` and `dims` are in sheet
// mm; unresolved (dangling) dims are skipped. `dims` may be empty.
bool SaveSVG(const DrawingDocument& doc,
             const std::vector<drawing::SheetView>& views,
             const std::vector<drawing::ResolvedDim>& dims,
             const std::string& path, std::string* error = nullptr);

// Writes the same sheet as a minimal single-page vector PDF (base-14 Helvetica,
// no font embedding; millimetres scaled to points). Enough to send to a machine
// shop; if it ever needs more, vendoring libharu is the escape hatch. Returns
// false and fills *error on failure.
bool SavePDF(const DrawingDocument& doc,
             const std::vector<drawing::SheetView>& views,
             const std::vector<drawing::ResolvedDim>& dims,
             const std::string& path, std::string* error = nullptr);

}  // namespace DrawingExport
