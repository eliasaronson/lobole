#pragma once

#include "core/drawing/DrawingDimension.h"
#include "core/drawing/DrawingDocument.h"
#include "core/drawing/DrawingRender.h"

#include <map>
#include <string>
#include <vector>

// Sheet-layout validation (drawings round 2): the checks a drafter does by
// eye — everything inside the border, views not colliding with each other or
// the title block, dimensions on the paper, nothing dangling or stale.
// GUI-free and derived-only, so the check_drawing RPC, export_drawing's
// issue echo, and (later) a viewport badge all agree. Rects come from the
// shared SheetChrome, so the validator can never disagree with what the
// renderers draw.

namespace drawing {

struct DrawingIssue {
  // "view_outside_border" | "view_overlap" | "view_overlaps_title_block" |
  // "dim_outside_border" | "dangling_dim" | "stale_view" | "unprojected_view"
  std::string kind;
  uint64_t viewId = 0;      // 0 when not view-scoped
  uint64_t otherViewId = 0; // the second view of an overlap pair
  uint64_t dimId = 0;       // 0 when not dim-scoped
  std::string detail;       // one human sentence with mm numbers
};

// `renders`: the per-view projection cache (views absent from it are
// reported "unprojected_view"). `dims`: every dim resolved against its view.
// `staleViews`: viewId -> true when the source moved since projection.
std::vector<DrawingIssue>
ValidateDrawing(const DrawingDocument& doc,
                const std::map<uint64_t, SheetView>& renders,
                const std::vector<ResolvedDim>& dims,
                const std::map<uint64_t, bool>& staleViews);

} // namespace drawing
