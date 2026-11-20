// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A solved study as a VTK unstructured grid (.vtu), for an outside
// post-processor (docs/simulation/README.md#vtu-export): the tet10 mesh as
// VTK_QUADRATIC_TETRA with what the result holds at its nodes.

#include "core/fem/FemSolver.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace fem {

// Our tet10 slot for each VTK_QUADRATIC_TETRA node, for a tet whose corner
// volume is positive and for one whose is negative (Netgen's). VTK wants
// (p1-p0) x (p2-p0) . (p3-p0) > 0, and its midside nodes on edges
// (0,1)(1,2)(2,0)(0,3)(1,3)(2,3) -- our order -- so a positive tet goes
// out as it is and a negative one with corners 1 and 2 exchanged, its
// midside nodes following their edges.
inline constexpr std::array<int, 10> kVtkTet10Positive = {0, 1, 2, 3, 4,
                                                          5, 6, 7, 8, 9};
inline constexpr std::array<int, 10> kVtkTet10Negative = {0, 2, 1, 3, 6,
                                                          5, 4, 7, 9, 8};

struct VtuSummary {
  std::size_t points = 0;
  std::size_t cells = 0;
  std::vector<std::string> pointData; // the arrays written, in order
  std::vector<std::string> cellData;
  std::size_t bytes = 0;
};

// Why `rt` cannot be exported, or empty: a result cut to its surface
// (KeepWhatIsRead) has no elements to write.
[[nodiscard]] std::string VtuRefusal(const StudyRuntime& rt);

// The whole document as text (ASCII arrays: readable by anything, and by
// the test's parser). Every body is one piece of the same grid, its nodes
// offset; `caseIndex` picks a load case's fields (-1 = the result's own).
// Point data: displacement (mm), von_mises (MPa), stress (MPa, the
// symmetric tensor as XX YY ZZ XY YZ XZ), plastic_strain, temperature (C),
// heat_flux (W/m^2), mode_<k> -- each when the result holds it. Cell data:
// body, domain, and a topology result's density.
std::string VtuText(const StudyRuntime& rt, int caseIndex, VtuSummary* summary);

} // namespace fem
