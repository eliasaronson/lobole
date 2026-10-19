// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>

#include <atomic>
#include <expected>
#include <string>
#include <vector>

struct Body;

namespace Measure {

// Can a part reach where it is mated (docs/inspect/README.md#insertion)?
// Insertion() slides the moving set along an axis from clear of the fixed
// set to its seat -- where it is placed now -- and at every step asks
// whether the two interfere: they overlap (the clash check's exact,
// non-destructive boolean), or come nearer than `allowanceMm`, how much the
// faces close up as made (a printed bore prints under its model). The
// travel spent interfering is compared with the press at the seat: the
// axial length over which the two interfere where the part ends up. A
// press of its own length is pushed exactly that far; anything longer is
// something forced through on the way -- a round pivot through a D bore,
// a shaft through a bore too small for it -- and fails.
//
// Rotation about the axis is free while sliding, so a failing slide is
// tried again at `rotations` orientations about the axis and the one that
// clears best is reported (a D bore passes a D shaft only aligned). An
// orientation either side is symmetric under is the seated one again and is
// skipped; one that does not seat (interferes at the seat more than the
// placed part does) is reported, never chosen.

enum class InsertionEntry {
  Positive, // the part comes from the axis direction's side
  Negative, // from the other side
  Either,   // both are tried; the better is reported
};

struct InsertionTry {
  bool positive = true;        // the side it came from
  double rotationDeg = 0.0;    // about the axis, from where it is seated
  double travelMm = 0.0;       // seat to clear
  double interferingMm = 0.0;  // travel spent interfering
  double pressMm = 0.0;        // the seat's own press length
  double excessMm = 0.0;       // interfering beyond the press, >= 0
  double fromMm = 0.0;         // where the interference begins, from the seat
  double worstDepthMm = 0.0;   // the most the faces must give (incl. allowance)
  double worstVolumeMm3 = 0.0; // the largest overlap in the model
  bool pass = false;
  bool complete = true; // false: abandoned once it could not beat the best
  // false: turned so, the part interferes at its seat more than it does
  // where it is placed -- a keyed seat -- so it cannot end up there.
  bool seats = true;
};

struct InsertionReport {
  glm::dvec3 axisPoint{0.0}, axisDir{0.0, 0.0, 1.0};
  double allowanceMm = 0.0;
  double stepMm = 0.0;
  double leadInMm = 0.0;
  int samples = 0;
  InsertionTry best;               // the passing try, else the least excess
  std::vector<InsertionTry> tries; // in the order they ran
  bool pass = false;
};

struct InsertionOptions {
  InsertionEntry entry = InsertionEntry::Either;
  double allowanceMm = 0.0; // 0: the model as modelled
  int samples = 64;         // steps from seat to clear
  int rotations = 12;       // orientations tried when the seated one fails
  double leadInMm = -1.0;   // < 0: two steps, the sampling's own resolution
};

// A and B are placed by their matrices; the axis is in that same (world)
// frame. An error when either side is empty, the axis has no direction, or
// the kernel fails; `cancel` is polled between steps and `fraction` (when
// given) advances through the work.
std::expected<InsertionReport, std::string>
Insertion(const std::vector<Body>& moving, const glm::dmat4& xfMoving,
          const std::vector<Body>& fixed, const glm::dmat4& xfFixed,
          const glm::dvec3& axisPoint, const glm::dvec3& axisDir,
          const InsertionOptions& opt,
          const std::atomic<bool>* cancel = nullptr,
          std::atomic<double>* fraction = nullptr);

} // namespace Measure
