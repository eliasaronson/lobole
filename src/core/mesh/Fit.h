// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Least-squares fits of the four surfaces a machined part is mostly made of
// (docs/interchange/README.md#recognising-a-faceted-mesh). Each takes the
// points (and, for an axis, the facet normals) of one region and returns the
// surface with the residual of every point: the RMS and the largest.
//
// No Eigen: designer_core builds without it (HAVE_EIGEN is optional), and
// the solves here are 3x3 to 6x6. No RANSAC either: a CAD export's vertices
// are ON its surfaces, with no outliers. A scan's are not, and the scan
// round adds a robust fit behind the same result.

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

  // The distance of `p` from the surface, signed: positive outside a
  // cylinder, cone or sphere, and on the normal's side of a plane.
  [[nodiscard]] double Distance(const glm::dvec3& p) const;
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
