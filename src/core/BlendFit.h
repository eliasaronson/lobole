// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The size that fits (docs/modeling/README.md#the-size-that-fits): what a
// Fillet or Chamfer that does not build at the size asked says instead of
// "too large". Bounds from the measures at the picked edges (Measure.h:
// DihedralAngleAt, WidthAcrossEdgeAt, ConcaveRadiusAt), a search of a fixed
// number of builds that the least bound seeds, and the sentence.

#include "core/Feature.h" // Body, FitHint

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class EdgeBlendOp; // core/features/FilletFeature.h

namespace fit {

// What cannot fit, for the cost of a look: nothing above `size` does, and
// something below it may still fail. `why` is the geometric fact ("face
// 'F' is 15 mm wide between the two picked edges, so the rounds meet at
// 7.5 mm"), `element` the name of the face or edge it is about.
struct Bound {
  double size = 0.0; // mm
  std::string why;
  std::string element;
};

// The least of the bounds over every picked edge (edge indices in the body's
// element-map space), or none.
std::optional<Bound> BlendBound(const Body& body, const std::vector<int>& edges,
                                EdgeBlendOp op);

// `v` rounded down to three significant digits: every candidate is one, so
// the size reported is a size that was built. 7.4925 -> 7.49.
double RoundDown3(double v);

// The steps after the first candidate: a fixed count, so the answer does not
// depend on how busy the machine is.
inline constexpr int kSearchSteps = 8;

// What the search established. `fits` is the largest candidate that built
// (none: nothing did), `fails` the least size known to fail (the asked size,
// or less). `bound` is set when the bound's own candidate built, so the
// bound is why that is the limit.
struct Answer {
  double asked = 0.0;
  std::optional<double> fits;
  double fails = 0.0;
  bool stopped = false;
  std::optional<Bound> bound;
};

// The largest size that builds, given that `asked` does not. The first
// candidate sits just under `bound` when it is below the asked size (where
// the bound is the true limit this is the only build); then kSearchSteps
// steps between the largest that built (or zero) and the least that failed.
// `builds` builds one candidate; `stop` is asked before each, and a stopped
// search reports the bracket it has.
Answer LargestThatBuilds(double asked, const std::optional<Bound>& bound,
                         const std::function<bool(double)>& builds,
                         const std::function<bool()>& stop);

// Candidates built by every search in this process: what the test of the
// memo below counts.
std::uint64_t CandidatesBuilt();

// The answer kept with a feature's executed state (copied with it by
// CopyDerivedState), keyed on the input body, the edges and the operation:
// a feature that stays broken is regenerated again and again, and a size
// dragged further past the limit searches once.
struct Memo {
  CADShape body;
  std::vector<int> edges;
  int op = -1;
  Answer answer;
  // The answer for `asked`, when this one holds for it: the same input and
  // a size at or above the one known to fail.
  [[nodiscard]] std::optional<Answer> For(const CADShape& input,
                                          const std::vector<int>& edgeIndices,
                                          EdgeBlendOp blendOp,
                                          double asked) const;
};

// The feature's error: "fillet radius 10 mm does not fit: the largest
// radius that fits is 7.49 mm", the places of the faulty contours appended.
// `cause` is what the build at the asked size said, kept where nothing fits.
std::string Sentence(EdgeBlendOp op, const Answer& a, const std::string& places,
                     const std::string& cause);

// The FitHint for the answer: param "radius" for both operations (the
// feature's field), basis "built".
FitHint HintOf(const Answer& a);

// A size in mm as the sentences print it: 7.49, 10, 0.3.
std::string Mm(double v);

} // namespace fit
