// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What traced geometry nearly is
// (docs/sketching/README.md#relations-the-scan-supports): the relations and
// round sizes the lines and arcs traced over a section come within their own
// uncertainty of, each with the evidence for it, measured on the current cut
// and never on the drawing; and accepting them as ordinary constraints and
// dimensions, nearest first, each one kept only where the solver meets it
// without moving a piece off its points.

#include "core/Units.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SectionPieces.h"

#include <string>
#include <vector>

namespace sketch {

// The round figures a size is offered at, in the document's own length
// unit (one row per unit, StepsFor): a length or distance below `smallBelow`
// to `smallStep`, above it to `largeStep`; a radius to `radiusStep`; an
// angle to kAngleStepDeg whatever the unit.
struct SizeSteps {
  double smallBelow = 10.0;
  double smallStep = 0.5;
  double largeStep = 1.0;
  double radiusStep = 0.5;
};
SizeSteps StepsFor(units::Length unit);
inline constexpr double kAngleStepDeg = 5.0;

enum class CandidateKind {
  Horizontal,
  Vertical,
  Parallel,
  Perpendicular,
  Collinear,
  OnAxis, // collinear with the sketch's X or Y axis
  EqualLength,
  EqualRadius,
  Concentric,
  AtOrigin, // a round's centre on the sketch's origin
  Tangent,
  Symmetric, // two rounds' centres mirrored about an axis
  Length,
  Distance, // between two parallel lines, or two rounds' centres
  Radius,
  Diameter, // a whole circle's
  Angle,
};
const char* ToString(CandidateKind k);
inline bool IsSize(CandidateKind k) {
  return k == CandidateKind::Length || k == CandidateKind::Distance ||
         k == CandidateKind::Radius || k == CandidateKind::Diameter ||
         k == CandidateKind::Angle;
}

struct Candidate {
  CandidateKind kind = CandidateKind::Horizontal;
  // The traced entities it relates, then a datum id (kAxisX, kAxisY,
  // kOrigin) where it names one.
  std::vector<uint32_t> entities;
  // How far the fit is from the relation or the nominal (mm, or degrees
  // when `degrees`), that over the fit's own uncertainty of it.
  double off = 0.0;
  bool degrees = false;
  double sigma = 0.0;
  double sigmas = 0.0;
  // A size: what the fit measures and the round figure (mm or degrees).
  double measured = 0.0;
  double nominal = 0.0;
  // What accepting it adds.
  std::vector<Constraint> adds;
  // Its name for `only`: "<kind>:<id>,<id>".
  std::string key;
};

// Each traced entity refitted on the current cut: its points (as
// SectionFitReport assigns them), its fit on its own, and for an arc beside
// traced lines its fit held tangent to them, whose sigmas are the ones a
// small round has (FitTangentRound).
struct TracedRefit {
  uint32_t entity = 0;
  bool ok = false;
  SpanFit fit;
  RoundFit held;
  std::vector<glm::dvec2> pts;
  ChainNoise noise;
  double floorMm = 0.0;
};
std::vector<TracedRefit> RefitTraced(const SketchFeature& sk);

struct CandidateOptions {
  double maxSigmas = 3.0;
  units::Length unit = units::Length::Millimetre;
};

// Every candidate within `maxSigmas`, fewest sigmas first. Tangent, equal
// and a round angle only between neighbours: pieces that meet, that are one
// piece apart along their chain, or that face each other (the first traced
// piece a normal from one's middle reaches is the other). A relation the
// sketch already holds is not offered.
std::vector<Candidate> FindCandidates(const SketchFeature& sk,
                                      const CandidateOptions& o);

struct AcceptOptions {
  double maxSigmas = 3.0;
  // How far a traced piece may move off its points (mm, on the curve);
  // 0 = three times that piece's own sigma.
  double maxMoveMm = 0.0;
  // Only these keys (empty = every candidate).
  std::vector<std::string> only;
  units::Length unit = units::Length::Millimetre;
};

struct CandidateOutcome {
  Candidate candidate;
  std::string reason;  // a left one's: why it was taken back
  double moveMm = 0.0; // the largest move of a traced piece it caused
  uint32_t movedEntity = 0;
};

struct AcceptResult {
  std::vector<CandidateOutcome> accepted, left;
  std::vector<std::string> unknown; // `only` keys no candidate has
};

// Adds the candidates as constraints, fewest sigmas first, solving after
// each: one the solver cannot meet, or reads as redundant, is taken back;
// so is one that moves a traced piece further than the limit from where it
// stood. The sketch is left solved; the caller commits.
AcceptResult AcceptCandidates(SketchFeature& sk, const AcceptOptions& o);

} // namespace sketch
