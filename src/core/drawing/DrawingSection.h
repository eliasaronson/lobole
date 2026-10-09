// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Section views (docs/drawings/README.md#section-views): a body cut by a plane,
// what is left of it behind the plane, and the hatching on the faces the cut
// made. GUI-free core code, like DrawingProjection.
//
// The cut is a boolean, so the result's edges and faces are not the source's:
// a dimension on a section view still names the SOURCE's elements, so the
// cut carries, for every edge and face of the result, which source edge or
// face it is (an edge the cut shortened is still that edge), and for an edge
// the plane made, the source face it was cut out of -- the wall of a bore
// seen in section is a line no source edge made, and it is named by the
// bore's face, the way a silhouette is.

#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace drawing {

struct SectionCut {
  // What is left on the far side of the plane, in the body's own frame. Null
  // when the plane leaves nothing of the body (it is all in front).
  TopoDS_Shape shape;
  // Per edge of `shape` (TopExp::MapShapes order, 0-based): the source edge
  // it is, or -1 for an edge the plane made.
  std::vector<int> edgeOrigin;
  // Per edge of `shape`: for an edge the plane made, the source face it was
  // cut out of; -1 otherwise.
  std::vector<int> edgeFromFace;
  // Per face of `shape`: the source face it is, or -1 for a face lying on
  // the plane (what the cut exposed, which is what gets hatched).
  std::vector<int> faceOrigin;
  // The exposed faces' boundary loops, in the body's frame: what a section
  // view hatches. Outer and inner loops alike; hatching is even-odd.
  std::vector<std::vector<glm::dvec3>> cutLoops;
};

// Cuts `shape` (in its own frame) by the plane through `point` with normal
// `normal`, keeping the half the normal points into: a section view looks
// along `normal`, so what is nearer the reader than the plane is taken away.
// The source shape is not modified (the boolean runs non-destructive).
// Returns nullopt and sets *error when the boolean fails.
std::optional<SectionCut> CutBody(const TopoDS_Shape& shape,
                                  const glm::dvec3& point,
                                  const glm::dvec3& normal,
                                  std::string* error = nullptr);

// Hatch lines over the region the closed `loops` bound (even-odd: a loop
// inside another is a hole), at `angleRad` to the X axis and `spacing`
// apart, in the loops' own units. Each line is a segment.
std::vector<std::pair<glm::dvec2, glm::dvec2>>
HatchRegion(const std::vector<std::vector<glm::dvec2>>& loops, double spacing,
            double angleRad);

} // namespace drawing
