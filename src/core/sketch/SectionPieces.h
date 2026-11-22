// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The pieces fit of a section (docs/sketching/README.md#the-pieces-fit): a
// chain of a cut as the lines and arcs it is made of, found from the
// chain's own scatter rather than a tolerance -- corners first, runs merged
// best first while one line or arc explains them, the breaks slid to where
// neighbouring pieces meet, each piece's kind settled by an F test -- and
// each piece's residual sigma and the uncertainty of its own parameters.

#include "core/features/SketchFeature.h"

#include <glm/glm.hpp>

#include <vector>

namespace sketch {

// Every threshold of the pieces fit. A sigma here is the chain's own noise
// (MeasureNoise); `s` is the detail's scale, kDetailRange^(1 - 2 detail):
// 1 at the default detail 0.5, 2 at 0 (fewer, longer pieces), 1/2 at 1.
struct PieceThresholds {
  // The noise: each point's miss from a quadratic through it and its
  // kNoiseHalfWindow neighbours each side, the median of those as a sigma.
  static constexpr int kNoiseHalfWindow = 7;
  // Corners first: Douglas-Peucker at this many sigmas.
  static constexpr double kSplitSigmas = 3.0;
  // "Explains", one: the residuals' rms within sigma (1 + kSigmaSlack s).
  static constexpr double kSigmaSlack = 0.6;
  // Two: no run of kStretchPoints consecutive residuals misses on average
  // by more than s sigma_w (kStretchSlack + sqrt(2 ln windows)), sigma_w a
  // window mean's own sigma (the chain's correlation counted), so the
  // allowance grows only as the largest of that many chance means does.
  static constexpr int kStretchPoints = 6;
  static constexpr double kStretchSlack = 1.5;
  // Three: the heading of the line through the kHeadingPoints points at
  // each end agrees with the piece's within kHeadingSigmas s of that
  // line's own angle sigma.
  static constexpr int kHeadingPoints = 8;
  static constexpr double kHeadingSigmas = 4.0;
  // Walls wander, rounds do not: a line's rms and stretch allowances are
  // this many times an arc's (its heading's is the same).
  static constexpr double kLineWander = 3.0;
  // ...reached over this many points: over fewer, a line is allowed
  // proportionally less (a wall is long; a short line wanders nowhere).
  static constexpr double kWanderPoints = 30.0;
  // A piece of fewer points, and no longer than kLeftoverSigmas sigmas, is
  // given to a neighbour (an arc needs this many points, too).
  static constexpr int kMinPiecePoints = 5;
  static constexpr double kLeftoverSigmas = 30.0;
  // A step this many times the chain's median step (an exact cut's
  // straight edge, its two ends) is filled in at the median before the fit,
  // so it weighs as its length does.
  static constexpr double kEvenSteps = 4.0;
  // Passes of sliding the breaks to where the pieces meet.
  static constexpr int kSlidePasses = 3;
  // Two intersections of neighbouring pieces closer than this share of the
  // shorter piece are one tangent meeting: the break is between them.
  static constexpr double kTangentShare = 0.5;
  // A line is kept over an arc (and an arc turned into a line) unless the
  // arc is better by an F test at this level.
  static constexpr double kArcAlpha = 1.0e-2;
  // An arc of a radius past this many times its span is a line.
  static constexpr double kMaxRadiusSpan = 1.0e3;
  static constexpr double kDetailRange = 2.0;
  // An exact cut (a B-Rep's) is fitted as if it scattered this share of
  // the section's tolerance.
  static constexpr double kNoiseFloorShare = 0.1;
  // The residuals' lag-one correlation is taken to at most this.
  static constexpr double kMaxCorrelation = 0.95;
};

// A chain's noise: a robust sigma of its points' scatter (mm, at least the
// floor) and the lag-one correlation of neighbouring residuals.
struct ChainNoise {
  double sigma = 0.0;
  double correlation = 0.0;
};
ChainNoise MeasureNoise(const CutChain& chain, double floorSigma);

struct PiecesFit {
  std::vector<Entity> entities; // lines and arcs (a circle for a round
                                // chain), construction false, ids 0
  std::vector<PieceFit> pieces; // one per entity
  // Per entity, the `which` of its point at the piece's start and at its
  // end, in the chain's order (a line 0 and 1; an arc 1 and 2, or 2 and 1
  // when the chain runs clockwise round it; a circle 0 and 0).
  std::vector<std::pair<uint8_t, uint8_t>> ends;
  // The largest distance from a point of the chain to its piece (mm).
  double deviation = 0.0;
  ChainNoise noise;
};

// Chain `chainIndex` as pieces. `tolerance` gives only the noise floor
// (kNoiseFloorShare of it); `detail` is clamped to [0, 1].
PiecesFit FitPieces(const CutChain& chain, int chainIndex, double tolerance,
                    double detail);

// The chain with every step past kEvenSteps times its median step filled
// in at the median, as the fit sees it.
CutChain EvenedChain(const CutChain& chain);

// How far points stand from a drawn line or arc (section_fit): the rms of
// their distances, their signed mean, the worst short-stretch mean miss and
// the middle of that stretch, and whether the piece's own three tests
// (above, at the line's or arc's allowance) still pass.
struct SpanMiss {
  size_t points = 0;
  double sigmaMm = 0.0;
  double meanMm = 0.0;
  double stretchMm = 0.0;
  glm::dvec2 stretchAt{0.0};
  bool explained = false;
};
SpanMiss MeasureMiss(const Entity& entity, const std::vector<glm::dvec2>& pts,
                     const ChainNoise& noise, double detail);

} // namespace sketch
