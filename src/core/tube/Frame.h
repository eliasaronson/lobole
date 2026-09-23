#pragma once

#include "core/tube/Check.h" // Bend, Violation, CheckResult

#include <glm/glm.hpp>

#include <string>
#include <vector>

// Is this frame a TRUSS? (docs/tube/README.md#checks)
//
// Every other check in this build is per-PROCESS — check_sheet_metal,
// check_tube, check_fasteners, check_fits — and none of them is structural.
// So a Formula Student chassis was built with no diagonal at all across the
// 570 mm between its bulkhead support and its front hoop, and nothing said
// so; finding it took a Python script written by hand.
//
// Three questions, all answered on the member GRAPH rather than on geometry:
// is every member welded to something at both ends, does any joint land on a
// neighbour's bend, and is any bay a quadrilateral with nothing across it. A
// four-bar loop carries its load by BENDING its welds, which is the one
// thing a space frame exists not to do.
//
// What it cannot answer is stiffness. That is a beam model's question, and
// this build has only tet10 solids — so `notChecked` says so rather than
// letting an empty `violations` read as "this frame is stiff".
namespace tube {

// One member as the frame sees it: where its metal runs, how thick it is,
// and the corners it is bent round.
struct FrameMember {
  std::string name;
  std::vector<glm::dvec3> centreline; // sampled, in order along the member
  double outerDiameter = 0.0;
  std::vector<Bend> bends;
  std::vector<glm::dvec3> corners; // one per bend, same order
};

// Checks the frame. `members` may be a single part's members or an
// assembly's — this cares only about where the metal is.
[[nodiscard]] CheckResult CheckFrame(const std::vector<FrameMember>& members);

} // namespace tube
