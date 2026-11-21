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

// What a bound is about: the face's width from the edge, the width between
// two picked edges, a face's curvature toward the round, a disc's rim.
enum class BoundKind { Width, Between, Curvature, Rim };

// What cannot fit, for the cost of a look: nothing above `size` does, and
// something below it may still fail. `why` is the geometric fact ("face
// 'F' is 15 mm wide between the two picked edges, so the rounds meet at
// 7.5 mm"), `element` the name of the face or edge it is about.
struct Bound {
  double size = 0.0; // mm
  std::string why;
  std::string element;
  BoundKind kind = BoundKind::Width;
};

// Every bound over every picked edge (edge indices in the body's
// element-map space), and the least of them, or none.
std::vector<Bound> BlendBounds(const Body& body, const std::vector<int>& edges,
                               EdgeBlendOp op);
std::optional<Bound> Least(const std::vector<Bound>& bounds);
std::optional<Bound> BlendBound(const Body& body, const std::vector<int>& edges,
                                EdgeBlendOp op);

// Whether a size past a bound of this kind is refused before the build, not
// only searched under: the kinds the kernel-case corpus and a sweep of the
// action matrix's bodies (the hidden "[.fit_sweep]" test) found OpenCASCADE
// builds nothing past (docs/modeling/README.md#bounds-that-refuse).
bool Refuses(BoundKind kind);

// How far past a bound a size is refused: a thousandth, so a size at the
// bound (two rounds meeting exactly, a round as wide as its face) is still
// the builder's to answer.
inline constexpr double kRefuseMargin = 1e-3;

// The least bound of a refusing kind that `asked` is past, or none.
std::optional<Bound> RefusingBound(const std::vector<Bound>& bounds,
                                   double asked);

// The rim bounds alone, for the cost of a look at the picked edges' faces:
// what Execute asks before every build, where BlendBounds' measures of
// every face cost more than a fitting blend (100 rims of a plate: 0.35 s of
// CPU against a 0.2 s build). The same bounds BlendBounds offers as Rim.
std::vector<Bound> RimBounds(const Body& body, const std::vector<int>& edges,
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

// Thrown by a search's `builds` when its build was stopped part way (a
// Shell's deadline ran out inside the offset): that candidate neither
// built nor failed.
struct Stopped {};

// How a search runs: `steps` after the first candidate, and whether the
// seed is a bound (nothing above it fits, so where its candidate builds it
// is the answer) or only a first guess, after which the search goes on
// between it and the least that failed (a Shell's half its thinnest wall).
struct SearchOptions {
  int steps = kSearchSteps;
  bool seedIsBound = true;
};

// The largest size that builds, given that `asked` does not. The first
// candidate sits just under `bound` when it is below the asked size (where
// the bound is a true limit, `seedIsBound`, this is the only build); then
// `steps` steps between the largest that built (or zero) and the least that
// failed. `Answer::bound` is set when the seed's candidate is the answer.
// `builds` builds one candidate; `stop` is asked before each, and a stopped
// search reports the bracket it has.
Answer LargestThatBuilds(double asked, const std::optional<Bound>& bound,
                         const std::function<bool(double)>& builds,
                         const std::function<bool()>& stop,
                         SearchOptions options = {});

// Candidates built by every search in this process: what the test of the
// memo below counts.
std::uint64_t CandidatesBuilt();

// The answer kept with a feature's executed state (copied with it by
// CopyDerivedState), keyed on the input body, the elements (a blend's edges,
// a Shell's open faces) and the operation (an EdgeBlendOp, or kShellOp): a
// feature that stays broken is regenerated again and again, and a size
// dragged further past the limit searches once.
inline constexpr int kShellOp = 100;
// A Shell's search: an offset costs seconds where a blend costs
// milliseconds, so four steps after its first guess.
inline constexpr int kShellSearchSteps = 4;
struct Memo {
  CADShape body;
  std::vector<int> edges;
  int op = -1;
  Answer answer;
  // The answer for `asked`, when this one holds for it: the same input and
  // a size at or above the one known to fail.
  [[nodiscard]] std::optional<Answer> For(const CADShape& input,
                                          const std::vector<int>& elements,
                                          int operation, double asked) const;
};

// The feature's error: "fillet radius 10 mm does not fit: the largest
// radius that fits is 7.49 mm", the places of the faulty contours appended.
// `cause` is what the build at the asked size said, kept where nothing fits.
std::string Sentence(EdgeBlendOp op, const Answer& a, const std::string& places,
                     const std::string& cause);

// The FitHint for the answer: param "radius" for both operations (the
// feature's field), basis "built".
FitHint HintOf(const Answer& a);

// A size past a refusing bound, which is not built: the search runs from
// the bound with no build at the asked size. "fillet radius 10 mm does not
// fit: face 'F' is 15 mm wide between the two picked edges, so the rounds
// meet at 7.5 mm; the largest radius that fits is 7.49 mm", and the hint of
// basis "bound".
std::string BoundSentence(EdgeBlendOp op, const Answer& a, const Bound& past,
                          const std::string& places);
FitHint BoundHint(const Answer& a);

// A size in mm as the sentences print it: 7.49, 10, 0.3.
std::string Mm(double v);

} // namespace fit
