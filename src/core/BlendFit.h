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
// two picked edges, a face's curvature toward the round, a circle's axis
// (a face of revolution leaning toward it from the rim ends there), two
// blends meeting across a planar face, an edge bending in a planar face.
enum class BoundKind { Width, Between, Curvature, Rim, Meet, Bend };

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
// bound (a round as wide as its face) is still the builder's to answer. Two
// blends that meet are given a hundredth: the builder runs the second a
// little onto the first, and built 1.002 times the bound where a round meets
// a concave rim's (the sweep: none at 1.01).
inline constexpr double kRefuseMargin = 1e-3;
inline constexpr double kMeetRefuseMargin = 1e-2;
// An edge's bend is given a tenth: just past it the fold in the blend's edge
// is smaller than the builder's tolerance, and it built rounds of 1.01 times
// the bend on most of the corpus's spline and text pads (none at 1.1).
inline constexpr double kBendRefuseMargin = 1e-1;
double RefuseMargin(BoundKind kind);

// The least bound of a refusing kind that `asked` is past, or none.
std::optional<Bound> RefusingBound(const std::vector<Bound>& bounds,
                                   double asked);

// The refusing bounds, for the cost of a look at analytic geometry: what
// Execute asks before every build, where BlendBounds' measures of every face
// cost more than a fitting blend (100 rims of a plate: 0.35 s of CPU
// against a 0.2 s build). BlendBounds offers the same.
//
// Rim: a closed circular edge whose two faces each have a straight meridian
// about its axis (a plane square to it, a coaxial cylinder or cone). A face
// that leaves the rim leaning toward the axis (its meridian direction u
// radially negative) reaches the axis R / -u_radial from it, and a blend's
// setback on it cannot pass that: a chamfer's distance, a round's r /
// tan(phi/2), phi the angle between the two faces' directions from the rim.
// A shaft's end gives R; a 120 degree drill point's floor 2R; a bore's top
// rim (material outside) nothing. A disc to its own rim beside a face of
// another kind keeps the setback across the disc.
std::vector<Bound> RimBounds(const Body& body, const std::vector<int>& edges,
                             EdgeBlendOp op);

// Meet: the least over two edges of the blend (the builder's contours: the
// picked edges and their tangent chains) on one planar face, each a line or a
// circle whose other face has a straight section (a plane beside a line, a
// coaxial cylinder or cone beside a circle), of g / (k1 + k2), g the length of
// a segment square to both at points inside both that lies in the face, k a
// blend's setback per mm (r tan((pi - psi) / 2) of a round, psi the angle
// between the faces' directions into themselves; a chamfer's distance).
// Past it the two blends' edges on the face cross. Only under `below` mm
// (the pre-build look asks under the size asked): the segments in the face
// are checked from the least up.
std::optional<Bound> MeetBound(const Body& body, const std::vector<int>& edges,
                               EdgeBlendOp op, double below);

// Bend: an edge of the blend (the builder's contours) that is neither a
// line nor a circle, on a planar face whose other face is a wall extruded
// square to that plane (a pad's side), so a blend's setback on the plane is
// its size all along it. The blend's edge on the plane is the edge's
// parallel at that setback, which folds back on itself (a swallowtail)
// where the edge bends toward the face with a radius under it: the least
// such radius of curvature along the edge. A circle's is the Rim's or no
// bound at all (a bore's rim bends away from its face).
std::vector<Bound> BendBounds(const Body& body, const std::vector<int>& edges,
                              EdgeBlendOp op);

// RimBounds, MeetBound and BendBounds together: what Execute asks before it
// builds.
std::vector<Bound> RefusingBounds(const Body& body,
                                  const std::vector<int>& edges, EdgeBlendOp op,
                                  double below);

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

// How much a blend can move the volume
// (docs/modeling/README.md#a-blend-moves-the-volume-it-can): on a convex
// edge a round or a chamfer only
// removes, on a concave one only adds, and at most the edge's length times
// the blend's section in the wedge of its faces -- r^2 (cot(phi/2) - (pi -
// phi)/2) for a round, d^2 sin(phi)/2 for a chamfer, phi the faces' angle
// across the material at a convex edge and across the air at a concave one
// -- times 1 + setback x curvature where the edge curves. Per edge of the
// blend's contours (the picked edges and their tangent chains), measured
// once at stations along it: the reach at a size is the edge's length times
// the largest section at any station, summed.
struct EdgeReach {
  double length = 0.0;    // mm
  double curvature = 0.0; // 1/mm, the edge's most
  // phi at the stations where the edge is convex, and where it is concave.
  std::vector<double> convex, concave;
};
// None where an edge's angle cannot be measured: then nothing is held.
std::optional<std::vector<EdgeReach>>
ReachOf(const Body& body, const std::vector<int>& contourEdges);
// The most it can remove and add at `size` along the edges, mm^3, and past
// their ends either way: a ball of the edge's largest setback at each end
// (a round meeting a slanted end face, or running out over a neighbour).
struct VolumeReach {
  double remove = 0.0;
  double add = 0.0;
  double ends = 0.0;
};
VolumeReach ReachAt(const std::vector<EdgeReach>& reach, EdgeBlendOp op,
                    double size);
// How far past its reach a blend's volume change may be before the result
// is refused: the corpus's most is in docs/modeling/README.md (the sweep,
// "[.fit_sweep]"), well under it. Plus an absolute floor, so a blend whose
// reach is a speck is not held to the volume's own noise.
inline constexpr double kReachMargin = 2.0;
inline constexpr double kReachFloor = 1e-6; // of the body's volume
// Why `after` (the blend's result's volume) is not a blend of `size` on a
// body of volume `before`, or empty: a change of the wrong sign, or past
// kReachMargin times the reach, by more than `slack` mm^3 (what the two
// volumes' integration may be off: the caller asks it only of a result this
// refuses, since the uncertainty costs the area's integration too).
std::string VolumeObjection(EdgeBlendOp op, double size, double before,
                            double after, const VolumeReach& reach,
                            double slack = 0.0);
// VolumeObjection of `built` on `input`, the two volumes' uncertainty
// (CADShape::ComputeMassProps) taken as slack, four times over, where it
// would refuse without.
std::string VolumeObjection(EdgeBlendOp op, double size, const CADShape& input,
                            const CADShape& built, const VolumeReach& reach);

} // namespace fit
