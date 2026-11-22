// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Meshes with known answers, generated in-test. The mechanical ones (here)
// are CAD parts built with OpenCASCADE and written as STL through the app's
// own writer (CADKernel::SaveSTL) at a chosen tessellation, so every hole's
// radius and axis is known exactly
// (docs/interchange/README.md#recognising-a-faceted-mesh). The organic ones
// -- sculpts and scans -- are in mesh_organic.h.

#include "cad/CADKernel.h"
#include "core/mesh/TriMesh.h"
#include "mesh_organic.h" // the organic fixtures: sculpts and scans

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fixtures {

struct Round {
  glm::dvec3 point;     // on the axis (a cone: its apex; a sphere, a torus:
                        // centre)
  glm::dvec3 axis;      // unit (a cone: into the cone)
  double radius = 0;    // cylinder, sphere; a torus's core
  double halfAngle = 0; // cone (rad)
  double minor = 0;     // a torus's tube
};

struct Flat {
  int sides = 0;
  double circumradius = 0;
  glm::dvec3 centreXY{0.0}; // x and y of its axis (vertical)
};

struct Part {
  std::string name;
  CADShape shape;
  double volume = 0.0;
  std::vector<Round> cylinders;
  std::vector<Round> cones;
  std::vector<Round> spheres;
  std::vector<Round> tori;
  std::vector<Flat> polygons; // vertical prisms of so many sides
};

// A 120 x 80 x 6 plate with 24 vertical through holes, 5 to 7.5 mm.
Part Plate();
// An 80 x 40 x 8 block: a counterbored hole, a countersunk hole (a 90°
// cone), a boss with a bore, and hexagon, octagon and decagon pockets.
Part Bracket();
// An L of two plates, its inside corner filleted (a cylinder tangent to both
// plates) and the foot's two far upright edges rounded.
Part FilletedAngle();
// A block with one hole tipped 30° from vertical, and a hemispherical dome.
Part ObliqueAndDome();
// A 60 x 40 x 4 plate whose four upright corners are rounded (radius 7):
// each corner a quarter cylinder tangent to the two walls either side.
Part RoundedPlate();

// A 50 x 36 x 14 block, its four upright edges rounded (radius 6) and then
// its top edges (radius 2), so that at each corner the two rounds meet in a
// quarter of a torus (core radius 4, tube 2) about the upright round's
// axis; and a boss of radius 5 on its top, its foot rounded 1.5 -- a whole
// ring of a concave torus. Turned off the axes and moved off the origin, as
// every answer it lists is.
Part RoundedCorners();

// Writes `part` as binary STL tessellated at `deflection` mm and
// `angularRad`, into a fresh copy so an earlier tessellation does not
// carry over, and returns the path.
std::filesystem::path WriteStl(const Part& part, double deflection,
                               double angularRad, const std::string& tag);

// The same, read back with mesh::ReadStl (REQUIREd to succeed).
mesh::TriMesh MeshOf(const Part& part, double deflection, double angularRad);

} // namespace fixtures
