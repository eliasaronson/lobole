// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Laying balloons out round a view (docs/drawings/README.md#balloon-layout):
// a ring of slots around the view, clear of everything else on the sheet,
// matched to the parts in the order they go round the view, then untangled
// so that no two leaders cross. Pure geometry in sheet mm: the drawing
// actions gather the anchors (points on each part's visible edges, as refs
// that resolve to them) and turn the answer into balloon notes.

namespace drawing {

struct BalloonItem {
  double radius = 4.0; // the balloon's circle, sheet mm
  // Where the leader may end, each a point on the part as drawn. The
  // layout picks one per item; `cost` is added to its leader's length (an
  // end shared with other edges is a worse place to point than the middle
  // of one).
  struct Anchor {
    glm::dvec2 at{0.0};
    double cost = 0.0;
  };
  std::vector<Anchor> anchors;
};

struct BalloonPlacement {
  bool placed = false;
  size_t anchor = 0;      // index into the item's anchors
  glm::dvec2 centre{0.0}; // the balloon's
};

struct BalloonLayoutInput {
  glm::dvec2 viewMin{0.0}, viewMax{0.0}; // the view's box
  glm::dvec2 borderMin{0.0}, borderMax{0.0};
  // Boxes a balloon may not touch: other views, the title block, tables,
  // labels, balloons already placed.
  std::vector<std::pair<glm::dvec2, glm::dvec2>> avoid;
  std::vector<BalloonItem> items;
};

struct BalloonLayout {
  std::vector<BalloonPlacement> placements; // one per item, in input order
  int crossings = 0; // leader pairs that still cross (0 when it succeeded)
  std::string error; // why an item could not be placed, when one was not
};

[[nodiscard]] BalloonLayout LayoutBalloons(const BalloonLayoutInput& in);

// Whether segments ab and cd cross (touching at an end does not count).
[[nodiscard]] bool SegmentsCross(const glm::dvec2& a, const glm::dvec2& b,
                                 const glm::dvec2& c, const glm::dvec2& d);

// A balloon's leader as drawn: from the circle's edge toward the anchor.
[[nodiscard]] std::pair<glm::dvec2, glm::dvec2>
BalloonLeader(const glm::dvec2& centre, double radius,
              const glm::dvec2& anchor);

} // namespace drawing
