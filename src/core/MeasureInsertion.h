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
// set to its seat -- where it is placed now -- and asks where along the way
// the two interfere: overlap, or come nearer than `allowanceMm`, how much
// the faces close up as made (a printed bore prints under its model).
// It reads that by SECTIONS (core/MeasureSections.h): each side cut into
// bands along the axis over which its cross-section does not change; two
// bands interfere or not whatever the slide (one 2D polygon question), and
// the slides at which their spans overlap are an interval from their ends.
// So the travel under interference, the press and the deepest overlap come
// out exact for prismatic matter, and to the slice thickness where a face
// is not along the axis (a chamfer, a fillet, a helical flank). One exact
// boolean, at the worst offset of the answer, confirms it.
// The travel spent interfering is compared with the press at the seat: the
// axial length over which the two interfere where the part ends up. A
// press of its own length is pushed exactly that far; anything longer is
// something forced through on the way -- a round pivot through a D bore,
// a shaft through a bore too small for it -- and fails. So does travel
// spent interfering deeper than the seat itself does: forced harder than
// the press it ends in, however short the stretch.
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
  // travel spent interfering deeper than the seat does (by more than half
  // the allowance): something forced harder than the press it ends in
  double deeperMm = 0.0;
  double worstDepthMm = 0.0;   // the most the faces must give (incl. allowance)
  double worstVolumeMm3 = 0.0; // the largest overlap in the model
  double worstAtMm = -1.0;     // the offset the worst depth was read at
  bool pass = false;
  bool complete = true; // false: abandoned once it could not beat the best
  // false: turned so, the part interferes at its seat more than it does
  // where it is placed -- a keyed seat -- so it cannot end up there.
  bool seats = true;
};

// The one exact boolean: the best try's worst offset (the seat when
// nothing interferes on the way), where the sections' reading is set
// against the clash check's own boolean: overlap in the model or not, and,
// where the sections read only a gap under the allowance, the exact least
// distance. A pass the boolean does not agree with is not a pass.
struct InsertionConfirm {
  bool ran = false;
  double atMm = 0.0;            // from the seat, along the try's entry
  bool expectedOverlap = false; // the sections: matter in matter there
  bool expectedGap = false;     // the sections: only nearer than allowed
  bool overlaps = false;        // the boolean: matter in matter there
  double volumeMm3 = 0.0;       // the boolean's overlap
  double gapMm = -1.0;          // the exact least distance, when asked
  bool agrees = false;
};

struct InsertionReport {
  glm::dvec3 axisPoint{0.0}, axisDir{0.0, 0.0, 1.0};
  double allowanceMm = 0.0;
  double stepMm = 0.0;  // the resolution: travel over samples
  double sliceMm = 0.0; // the slice thickness where a section varies
  double leadInMm = 0.0;
  int samples = 0;
  int bandsMoving = 0, bandsFixed = 0; // sections each side was cut into
  int pairsRead = 0;                   // band pairs the 2D test ran on
  // Where the time went: cutting the sections, the 2D tests, the boolean.
  double sectionsSeconds = 0.0, pairsSeconds = 0.0, confirmSeconds = 0.0;
  InsertionTry best;               // the passing try, else the least excess
  std::vector<InsertionTry> tries; // in the order they ran
  InsertionConfirm confirm;
  bool pass = false;
};

struct InsertionOptions {
  InsertionEntry entry = InsertionEntry::Either;
  double allowanceMm = 0.0; // 0: the model as modelled
  int samples = 64;         // the travel's resolution (slices, lead-in)
  int rotations = 12;       // orientations tried when the seated one fails
  double leadInMm = -1.0;   // < 0: one step, the sampling's own resolution
  bool confirm = true;      // run the one exact boolean
};

// A and B are placed by their matrices; the axis is in that same (world)
// frame. An error when either side is empty, the axis has no direction,
// both sides carry modelled threads (a screw goes on by turning), or the
// kernel fails; `cancel` is polled between steps and `fraction` (when
// given) advances through the work.
std::expected<InsertionReport, std::string>
Insertion(const std::vector<Body>& moving, const glm::dmat4& xfMoving,
          const std::vector<Body>& fixed, const glm::dmat4& xfFixed,
          const glm::dvec3& axisPoint, const glm::dvec3& axisDir,
          const InsertionOptions& opt,
          const std::atomic<bool>* cancel = nullptr,
          std::atomic<double>* fraction = nullptr);

} // namespace Measure
