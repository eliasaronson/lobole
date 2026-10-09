// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Split for printing (docs/topopt/README.md#split-for-printing): where to cut
// a body so that every piece fits a printer's bed. Pure geometry over one
// shape; the verb that turns the plan into parts is split_for_printing
// (src/remote/RemoteModeling.cpp).
//
// Cuts are axis-aligned planes in part coordinates. Each piece is the body
// inside one cell of the grid the cuts make; a piece fits when its bounding
// box fits the bed in one of the six axis-aligned orientations (a piece is
// never rotated off its axes, so a long diagonal member that would fit turned
// is reported as not fitting).

#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace printsplit {

struct Cut {
  int axis = 0;    // 0 = x, 1 = y, 2 = z
  double at = 0.0; // mm, part coordinates
};

struct Piece {
  // The grid cell, reaching past the body's box where no cut bounds it.
  glm::dvec3 cellMin{0.0}, cellMax{0.0};
  glm::dvec3 min{0.0}, max{0.0}; // the piece's own bounds
  double volume = 0.0;           // mm^3
  // Solids in the piece: a cell's solids are pieces of their own when a box
  // can take each alone, and one piece when their boxes interlock.
  int solids = 0;
  bool fits = false;
  // How it goes on the bed: which of the piece's axes lies along the bed's
  // x, y and z (z = up). Meaningful when `fits`.
  std::array<int, 3> orientation{0, 1, 2};
  glm::dvec3 Size() const { return max - min; }
};

struct Plan {
  std::vector<Cut> cuts; // the ones given, or the ones chosen
  std::vector<Piece> pieces;
  bool automatic = false;
  bool fitsWhole = false; // the body fits as it is: no cut was needed
};

// Whether a box of `size` fits `bed` in some axis-aligned orientation; the
// one found (the piece's axis along bed x, y, z) in `orientation`. Prefers
// the orientation whose height is least.
bool FitsBed(const glm::dvec3& size, const glm::dvec3& bed,
             std::array<int, 3>* orientation = nullptr);

// The pieces `cuts` make of `shape`, each checked against `bed`. Cuts that
// miss the body are refused (naming the cut and the body's extent along that
// axis); so is an empty shape. Empty cells are dropped.
std::optional<Plan> PlanCuts(const TopoDS_Shape& shape, const glm::dvec3& bed,
                             std::vector<Cut> cuts, std::string* error);

// The fewest equal slabs along the body's longest axis whose pieces all fit
// `bed` (at most `maxPieces`). A body that fits whole answers fitsWhole with
// one piece. When no count up to the limit works, nullopt and *error naming
// the piece that still does not fit.
std::optional<Plan> PlanAutomatic(const TopoDS_Shape& shape,
                                  const glm::dvec3& bed, std::string* error,
                                  int maxPieces = 12);

} // namespace printsplit
