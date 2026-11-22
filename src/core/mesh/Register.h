// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Laying one surface onto another, rigidly
// (docs/interchange/README.md#laying-a-scan-onto-a-body): point-to-plane ICP
// from several starts on a thinned sample of the moving surface, each step
// solved about the correspondences' centroid with the pose kept in doubles,
// pairs past three times the median distance left out of the step, and the
// winner the pose with the lowest CAPPED MEAN DISTANCE OVER EVERY SAMPLE --
// never over the pairs a step kept, which lets a pose slide off the part
// and look better. A sample is matched only to a surface that faces its
// way (mesh/Facing.h), so a plate is not laid on its back. Never scales.
// Pure data, no OpenCASCADE.

#include "core/mesh/Facing.h"
#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace mesh {

// x -> rotation · x + translation.
struct Rigid {
  glm::dmat3 rotation{1.0};
  glm::dvec3 translation{0.0};
  [[nodiscard]] glm::dvec3 operator()(const glm::dvec3& p) const {
    return rotation * p + translation;
  }
  // `next` after this one.
  [[nodiscard]] Rigid Then(const Rigid& next) const {
    return {next.rotation * rotation,
            next.rotation * translation + next.translation};
  }
  [[nodiscard]] Rigid Inverse() const {
    const glm::dmat3 rt = glm::transpose(rotation);
    return {rt, -(rt * translation)};
  }
};

// The angle of a rotation (rad, 0 to pi).
double TurnAngle(const glm::dmat3& rotation);

// A point on a surface and the unit normal there: its triangle's own, or on
// a sliver (whose own normal points anywhere) its corners' vertex normals
// blended.
struct SurfaceSample {
  glm::dvec3 point{0.0}, normal{0.0};
};

// `count` points spread over the triangles of `m` by area (a triangle twice
// the size draws twice the points), each at a deterministic place from
// `seed`; only the triangles `keep` admits, when given. Degenerate triangles
// draw none.
std::vector<SurfaceSample>
SampleSurface(const TriMesh& m, size_t count, uint64_t seed,
              const std::function<bool(uint32_t)>& keep = nullptr);

// The 24 attitudes that lay the principal axes of `from` (its covariance and
// centroid) on those of `to`, each with the centroid on the centroid: every
// proper signed permutation of one frame onto the other, since a partial
// scan ranks its axes differently from the whole.
std::vector<Rigid> PrincipalStarts(const glm::dvec3& fromCentroid,
                                   const glm::dmat3& fromCovariance,
                                   const glm::dvec3& toCentroid,
                                   const glm::dmat3& toCovariance);

// The starts a best fit searches from, for the moving surface `from` (its
// samples, spread by area) onto `to`: the 24 principal attitudes, and each
// also turned 45° about each of the target's principal axes -- a partial
// scan ranks its axes differently from the whole, and where two of its
// spreads are alike its frame can stand at any angle between them -- each
// placed with its centroid on the target's and with each of the 8 corners
// of its box, in the target's frame, on the target's same corner: a scan
// of part of an object still reaches some of its extremes.
std::vector<Rigid> SearchStarts(const std::vector<SurfaceSample>& from,
                                const TriMesh& to);

// The rigid motion taking the three points `from` onto `to` (least squares,
// Kabsch), or nothing when either three are in a line.
std::optional<Rigid> FromPairs(const std::array<glm::dvec3, 3>& from,
                               const std::array<glm::dvec3, 3>& to);

struct RegisterOptions {
  // Where each start lays the moving surface; at least one.
  std::vector<Rigid> starts;
  // A sample farther than this from every surface facing its way counts
  // this far in the score, and is no pair in a step.
  double capMm = 1.0;
  // A sample's normal and the triangle it is matched to must agree within
  // this (cos): 0.3 is 72°, past a noisy scan's normal and short of a
  // wall's far side.
  double minFacingCos = 0.3;
  size_t maxPasses = 60;
  // A start is done when a step moves no sample farther than this.
  double settleMm = 1e-6;
  // Set from another thread to stop between passes.
  const std::atomic<bool>* cancel = nullptr;
  // Called between passes with the share done (0-1).
  std::function<void(double)> progress;
};

struct StartOutcome {
  Rigid pose;
  double scoreMm = 0.0; // the capped mean distance over every sample
  size_t passes = 0;
  bool settled = false;
};

struct RegisterResult {
  bool cancelled = false;
  size_t winner = 0;                // index into starts
  std::vector<StartOutcome> starts; // each, in the order given
  [[nodiscard]] const StartOutcome& Best() const { return starts[winner]; }
};

// Runs every start on `samples` (where the moving surface stands now)
// onto `target`. The starts run in parallel, one pass of each at a time.
RegisterResult Register(const std::vector<SurfaceSample>& samples,
                        const FacingSurface& target,
                        const RegisterOptions& options);

// The search the best fit runs
// (docs/interchange/README.md#laying-a-scan-onto-a-body): every start screened
// by a few passes on a small sample, the best `keep` of them run until they
// settle on `samples`, the winner among those refined on `fine`. Cheap enough
// to give a partial scan many starts, whose principal frame says little about
// the whole's.
struct SearchOptions {
  std::vector<Rigid> starts;
  double capMm = 1.0;
  double minFacingCos = 0.3;
  double settleMm = 1e-6;
  size_t screenSamples = 150, screenPasses = 3, keep = 12;
  // Two screened starts closer than this (and 10°) are one.
  double distinctMm = 0.0;
  size_t passes = 50, finePasses = 60;
  const std::atomic<bool>* cancel = nullptr;
  std::function<void(double)> progress;
};
struct SearchResult {
  bool cancelled = false;
  // The kept starts as they settled on `samples` (the winner first), each
  // with its index in `starts`.
  std::vector<StartOutcome> kept;
  std::vector<size_t> keptFrom;
  Rigid pose; // the winner refined on `fine`
};
// `samples` and `fine` where the moving surface stands now; the first
// `screenSamples` of `samples` screen. The screen and the run match onto
// `rough` (a stand-in for the target, Surfels) when given, the refinement
// onto `target`.
SearchResult Search(const std::vector<SurfaceSample>& samples,
                    const std::vector<SurfaceSample>& fine,
                    const FacingSurface& target, const SearchOptions& options,
                    const FacingSurface* rough = nullptr);

// A stand-in for a dense surface while a pose is still far off: `count`
// samples of `m` spread by area, each a small triangle in its tangent plane
// facing the way the surface does, about as wide as the samples are apart.
// A search over it costs what `count` triangles cost, whatever `m` has.
TriMesh Surfels(const TriMesh& m, size_t count, uint64_t seed);

// How far each sample, laid by `pose`, is from the nearest surface facing
// its way (`capMm` where there is none within it), over every sample; the
// mean over those that have one, and the share that have none.
struct Distances {
  double meanMm = 0.0, p95Mm = 0.0, maxMm = 0.0;
  double within = 0.0;    // the share within `withinMm`
  double unmatched = 0.0; // the share with no surface within the cap
};
Distances MeasureDistances(const std::vector<SurfaceSample>& samples,
                           const Rigid& pose, const FacingSurface& target,
                           double minFacingCos, double capMm, double withinMm);

} // namespace mesh
