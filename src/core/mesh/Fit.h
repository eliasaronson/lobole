// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Least-squares fits of the four surfaces a machined part is mostly made of
// (docs/interchange/README.md#recognising-a-faceted-mesh). Each takes the
// points (and, for an axis, the facet normals) of one region and returns the
// surface with the residual of every point: the RMS and the largest.
//
// No Eigen: designer_core builds without it (HAVE_EIGEN is optional), and
// the solves here are 3x3 to 6x6. A CAD export's vertices are ON its
// surfaces, with no outliers, so least squares is enough for recognition. A
// scan's are not: FitRobust (RANSAC, then least squares over the points it
// keeps) gives the same result for those
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh).

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace mesh {

enum class Kind : uint8_t { Plane, Cylinder, Cone, Sphere };

const char* KindName(Kind k); // "plane", "cylinder", "cone", "sphere"

struct FitResult {
  bool ok = false;
  Kind kind = Kind::Plane;
  // Plane: a point on it and its unit normal. Cylinder: a point on the axis
  // and the axis. Cone: the apex and the axis, pointing from the apex into
  // the cone. Sphere: the centre (axis unused).
  glm::dvec3 origin{0.0};
  glm::dvec3 axis{0.0, 0.0, 1.0};
  double radius = 0.0;    // cylinder and sphere (mm)
  double halfAngle = 0.0; // cone: between the axis and a generator (rad)
  double rms = 0.0;       // of the points' distances to the surface
  double max = 0.0;
  // The points the rms and max are over: every point given to a least
  // squares fit, the inliers of a robust one.
  size_t inliers = 0;

  // The distance of `p` from the surface, signed: positive outside a
  // cylinder, cone or sphere, and on the normal's side of a plane.
  [[nodiscard]] double Distance(const glm::dvec3& p) const;
  // The nearest point of the surface to `p` (a cone's: on the nappe `p` is
  // on, or the apex).
  [[nodiscard]] glm::dvec3 Project(const glm::dvec3& p) const;
  // The unit normal at `p` on the side Distance counts positive (the
  // gradient of Distance); zero on a cylinder's or cone's axis.
  [[nodiscard]] glm::dvec3 Normal(const glm::dvec3& p) const;
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

// The surface of `kind` through most of `points`, robust to outliers: a scan
// has stray points (a lump, a hand in the frame, the table) that drag a
// least-squares fit through them. RANSAC over minimal samples of oriented
// points -- each point with its unit surface normal, `normals[i]` -- makes
// hypotheses (a plane from three points, a sphere or a cylinder from two, a
// cone from three tangent planes), each scored as MSAC does: a point costs
// its squared distance, one past `toleranceMm` the tolerance's square, so
// the tighter of two that keep as many points wins. The best is refitted
// by least squares over the points it keeps while the cost falls. rms, max and
// `inliers` are over those points. A plane takes no normals (empty): it is
// turned to face the way the normals do when they are given.
//
// Deterministic: the samples come from a generator seeded by the points
// themselves, so the same points give the same surface every time -- a
// datum refitted on regeneration does not move.
//
// With `through`, only a surface passing within the tolerance of that point
// is taken: a region grown from a picked triangle must be the surface THAT
// triangle is on, not a larger one beside it (a box's next face).
FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& normals, double toleranceMm,
                    const glm::dvec3* through = nullptr);
// The same with the hypotheses drawn from oriented samples apart from the
// points scored and fitted: a mesh region's triangle centres and their unit
// normals beside its vertices. A facet's normal is its surface's where a
// vertex's, at a sharp edge, averages two surfaces' -- a CAD cylinder's
// vertices are all on its rims.
FitResult FitRobust(Kind kind, const std::vector<glm::dvec3>& points,
                    const std::vector<glm::dvec3>& facetCentres,
                    const std::vector<glm::dvec3>& facetNormals,
                    double toleranceMm, const glm::dvec3* through = nullptr);

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
