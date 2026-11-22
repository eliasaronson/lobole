// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Least-squares fits of the surfaces a machined part is mostly made of
// (docs/interchange/README.md#recognising-a-faceted-mesh): a plane, a
// cylinder, a cone, a sphere, and the torus a round turned about an axis
// is (a fillet round a boss, two fillets meeting at a corner, a ring). Each
// takes the points (and, for an axis, the facet normals) of one region and
// returns the surface with the residual of every point: the RMS and the
// largest.
//
// No Eigen: designer_core builds without it (HAVE_EIGEN is optional), and
// the solves here are 3x3 to 7x7. A CAD export's vertices are ON its
// surfaces, with no outliers, so least squares is enough for recognition. A
// scan's are not: FitRobust (RANSAC, then least squares over the points it
// keeps) gives the same result for those
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh).

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mesh {

enum class Kind : uint8_t { Plane, Cylinder, Cone, Sphere, Torus };

// Of two kinds fitted from one point, the simpler is taken when it covers at
// least this share of the other's area (fit_primitive's auto,
// meshfit::kAutoShare): a cone fits a cylinder's triangles as well as the
// cylinder does, with a half angle of nearly nothing.
inline constexpr double kSimplerShare = 0.95;

// "plane", "cylinder", "cone", "sphere", "torus"
const char* KindName(Kind k);

struct FitResult {
  bool ok = false;
  Kind kind = Kind::Plane;
  // Plane: a point on it and its unit normal. Cylinder: a point on the axis
  // and the axis. Cone: the apex and the axis, pointing from the apex into
  // the cone. Sphere: the centre (axis unused). Torus: the centre and the
  // axis its core circle turns about.
  glm::dvec3 origin{0.0};
  glm::dvec3 axis{0.0, 0.0, 1.0};
  double radius = 0.0;    // cylinder and sphere; a torus's core circle (mm)
  double halfAngle = 0.0; // cone: between the axis and a generator (rad)
  double minor = 0.0;     // torus: the tube's radius about its core (mm)
  double rms = 0.0;       // of the points' distances to the surface
  double max = 0.0;
  // The points the rms and max are over: every point given to a least
  // squares fit, the inliers of a robust one.
  size_t inliers = 0;

  // The distance of `p` from the surface, signed: positive outside a
  // cylinder, cone, sphere or a torus's tube, and on the normal's side of a
  // plane.
  [[nodiscard]] double Distance(const glm::dvec3& p) const;
  // The nearest point of the surface to `p` (a cone's: on the nappe `p` is
  // on, or the apex).
  [[nodiscard]] glm::dvec3 Project(const glm::dvec3& p) const;
  // The unit normal at `p` on the side Distance counts positive (the
  // gradient of Distance); zero on a cylinder's or cone's axis or a torus's
  // core circle.
  [[nodiscard]] glm::dvec3 Normal(const glm::dvec3& p) const;
};

// What a fit from a picked point may be steered by: fit_primitive's hints
// (docs/modeling/README.md#hints), all optional. A hypothesis outside one is
// not drawn (FitRobust's `admit`), and a fit that ends outside one is refused
// saying which (Miss).
struct FitHints {
  // A curved kind's triangles face away from its axis or centre (outside: a
  // boss, a ball, a ring) or towards it (inside: a bore, a socket, a
  // concave round); a torus's from its core circle.
  std::optional<bool> outside;
  std::optional<double> radiusMin, radiusMax; // mm, a cylinder's or sphere's
  // A unit direction the plane's normal or the cylinder's, cone's or
  // torus's axis keeps within `axisToleranceRad` of, either sense.
  std::optional<glm::dvec3> axis;
  double axisToleranceRad = 0.0;
  // A world box the region's triangles stay inside (every corner).
  std::optional<glm::dvec3> withinLo, withinHi;

  [[nodiscard]] bool Any() const;
  // The first hint `kind` cannot take ("side", "radius_mm", "axis"), or
  // nullptr when it takes them all.
  [[nodiscard]] const char* NotFor(Kind kind) const;
  // Why `f` is outside a hint, naming it, or "" when it meets every one.
  // `at` is a point on the surface and `facing` the unit normal its
  // triangles face there (what the side is judged by). `within` is not
  // judged here: it bounds the region, not the fit.
  [[nodiscard]] std::string Miss(const FitResult& f, const glm::dvec3& at,
                                 const glm::dvec3& facing) const;
  // "side inside, radius_mm [4, 6]": what a refusal names.
  [[nodiscard]] std::string Describe() const;
  // Whether `p` is inside the `within` box (true when there is none).
  [[nodiscard]] bool Inside(const glm::dvec3& p) const;
};

// The plane through `points` (at least three, not collinear).
FitResult FitPlane(const std::vector<glm::dvec3>& points);

// The cylinder through `points`, its axis started from `axisGuess` (the
// Gauss map's, AxisFromNormals) and refined with the rest.
//
// `giveUpAbove`, when positive, returns a failed fit without refining when
// the starting estimate already misses some point by more than that: what
// keeps trying a wrong hypothesis on a large region cheap.
FitResult FitCylinder(const std::vector<glm::dvec3>& points,
                      const glm::dvec3& axisGuess, double giveUpAbove = 0.0);

// The cone through `points`, started from the facet planes (`normals[i]`
// through `onPlane[i]`), which all pass through its apex, and the axis guess.
FitResult FitCone(const std::vector<glm::dvec3>& points,
                  const std::vector<glm::dvec3>& normals,
                  const std::vector<glm::dvec3>& onPlane,
                  const glm::dvec3& axisGuess, double giveUpAbove = 0.0);

// The sphere through `points` (at least five, not coplanar).
FitResult FitSphere(const std::vector<glm::dvec3>& points,
                    double giveUpAbove = 0.0);

// The torus through `points` (at least eight), refined by least squares on
// its seven numbers (centre, axis, the two radii) from `start`, a torus.
FitResult FitTorus(const std::vector<glm::dvec3>& points,
                   const FitResult& start, double giveUpAbove = 0.0);

// A torus to start FitTorus from, read from oriented samples: every normal
// of a torus meets its axis (it lies in the plane through the axis), so the
// axis is the line the normal lines -- `normals[i]` through `onLine[i]`,
// unit -- come nearest to meeting, by least squares; the core circle and
// the tube are then the circle `points` make in the half-plane through the
// axis (their distance from it against their height along it). Not ok when
// the normals do not fix one line: a sphere's all meet at its centre, a
// cone's of one ring at one point of its axis.
FitResult TorusFromNormals(const std::vector<glm::dvec3>& points,
                           const std::vector<glm::dvec3>& normals,
                           const std::vector<glm::dvec3>& onLine);
// The torus about the line through `axisPoint` along unit `axis` that
// `points` lie on: the circle they make in the half-plane through it.
FitResult TorusAbout(const std::vector<glm::dvec3>& points,
                     const glm::dvec3& axisPoint, const glm::dvec3& axis);

// The surface of `kind` through most of `points`, robust to outliers: a scan
// has stray points (a lump, a hand in the frame, the table) that drag a
// least-squares fit through them. RANSAC over minimal samples of oriented
// points -- each point with its unit surface normal, `normals[i]` -- makes
// hypotheses (a plane from three points, a sphere or a cylinder from two, a
// cone from three tangent planes; a torus as below), each scored as MSAC
// does: a point costs
// its squared distance, one past `toleranceMm` the tolerance's square, so
// the tighter of two that keep as many points wins. The best is refitted
// by least squares over the points it keeps while the cost falls. rms, max and
// `inliers` are over those points. A plane takes no normals (empty): it is
// turned to face the way the normals do when they are given.
//
// A torus is not drawn from a minimal sample: its hypotheses are one per
// tube radius, on a log scale either sign (a convex tube and a concave
// round) -- the oriented samples moved along their normals by it lie on the
// core circle when it is right, and a plane and a circle through the moved
// points give the axis, the centre and the core's radius, the tube's then
// the median distance of the points from that circle -- and the axis the
// normal lines meet (TorusFromNormals); each scored as the others are.
//
// Deterministic: the samples come from a generator seeded by the points
// themselves, so the same points give the same surface every time -- a
// datum refitted on regeneration does not move.
//
// With `through`, only a surface passing within the tolerance of that point
// is taken: a region grown from a picked triangle must be the surface THAT
// triangle is on, not a larger one beside it (a box's next face). With
// `admit`, a hypothesis it refuses is not drawn (a hint's); the least
// squares that refines the best is not held to it, so a fit the surface
// does not allow comes back as the surface has it, for the caller to judge.
using Admit = std::function<bool(const FitResult&)>;
FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& normals, double toleranceMm,
                    const glm::dvec3* through = nullptr,
                    const Admit& admit = {});
// The same with the hypotheses drawn from oriented samples apart from the
// points scored and fitted: a mesh region's triangle centres and their unit
// normals beside its vertices. A facet's normal is its surface's where a
// vertex's, at a sharp edge, averages two surfaces' -- a CAD cylinder's
// vertices are all on its rims.
//
// `also`, when given, is scored with the hypotheses drawn: a region regrown
// under a fit keeps it when nothing drawn does better. A torus needs it on a
// fine scan, where a facet's normal is too noisy for the hypotheses read
// from normals to land near it once the region is too wide for the
// osculating fan.
FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& facetCentres,
                    const std::vector<glm::dvec3>& facetNormals,
                    double toleranceMm, const glm::dvec3* through = nullptr,
                    const Admit& admit = {}, const FitResult* also = nullptr);

// The least-squares fit of `kind` over `points`, started from `start` (its
// axis, and a plane's sense): what FitRobust refines its best hypothesis
// with. A cone starts from the tangent planes `facetNormals` through
// `facetCentres` (at least three); the other kinds take them empty.
FitResult FitLeastSquares(Kind kind, const FitResult& start,
                          const std::vector<glm::dvec3>& points,
                          const std::vector<glm::dvec3>& facetCentres,
                          const std::vector<glm::dvec3>& facetNormals);

// The Gauss map of a cylinder or a cone: unit facet `normals`, each weighted
// (by area), lie on a circle of the unit sphere -- a great circle for a
// cylinder, a smaller one for a cone. The axis is that circle's plane's
// normal; `offset` is the plane's distance from the centre (0 for a
// cylinder, sin of the half angle for a cone). With `throughCentre` the
// plane is held through the centre (a cylinder). `spread` is the RMS of the
// normals' distances from the plane.
glm::dvec3 AxisFromNormals(const std::vector<glm::dvec3>& normals,
                           const std::vector<double>& weights,
                           bool throughCentre, double& offset, double& spread);

// The eigenvalues (ascending) and unit eigenvectors of a symmetric 3x3
// matrix, by Jacobi rotations.
void SymmetricEigen3(const glm::dmat3& m, glm::dvec3& values,
                     glm::dmat3& vectors);

} // namespace mesh
