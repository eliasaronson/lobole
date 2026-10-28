// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Organic meshes with known answers, generated in-test: what a sculpt
// download or a phone scan is, built straight as triangles
// (docs/plans/organic-meshes-and-scans.md). No file is committed: the
// Stanford models are research-only. Pure data -- no OpenCASCADE and no
// Catch2 -- so the manifold probe (tests/mesh_boolean_probe.cpp) and the
// fixture writer the bench calls build them too.
//
// Each asks for a triangle count and comes within a few percent of it. The
// answers are analytic or computed by construction, independently of the
// triangles, and each says how far its own triangles may stand from them.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace fixtures {

struct Organic {
  std::string name;
  mesh::TriMesh mesh; // indexed as generated, adjacency built
  // Known by construction; 0 where it is not known (an open mesh's volume).
  double volume = 0.0;
  double volumeTolerance = 0.0; // what the tessellation and noise allow
  double area = 0.0;
  double areaTolerance = 0.0;
  // A sphere's radius, or a torus's minor radius: every vertex lies within
  // `radiusTolerance` of that distance from the sphere's centre or the
  // torus's core circle.
  double radius = 0.0;
  double radiusTolerance = 0.0;
  glm::dvec3 lo{0.0}, hi{0.0}; // the true surface's bounding box
  size_t components = 1;
  size_t boundaryLoops = 0;
};

// A sphere of `radius` mm about the origin, each vertex moved radially by a
// deterministic noise of at most `noise` mm: a scan's surface. A
// cube-sphere grid, so 12 n² triangles.
Organic NoisySphere(size_t triangles, double radius = 40.0,
                    double noise = 0.05);
// A clean ring torus about +Z, major radius 30 and minor 10 mm: a smooth
// surface no primitive fits.
Organic Torus(size_t triangles);
// A metaball figure about 100 mm tall standing on z = 0: a round base,
// legs, body, arms and a head, blended smooth and contoured by marching
// tetrahedra. Closed; its volume comes from integrating the field's solid
// column by column, not from its triangles.
Organic Figurine(size_t triangles);
// The same figure with every triangle reaching below 3 mm cut away, as a
// phone scan of an object standing on a table arrives: open, one boundary
// loop round the cut.
Organic CutFigurine(size_t triangles);

// A triangle soup with what a bad file has, and how many of each.
struct DirtySoup {
  std::vector<glm::dvec3> soup; // three points a triangle
  double quantum = 0.0;         // the weld tolerance to read it at
  size_t vertices = 0;          // after the weld
  size_t triangles = 0;         // after the weld (none collapse)
  size_t nearDuplicates = 0;    // points moved off their vertex by < quantum/2
  size_t flipped = 0;           // triangles wound the wrong way, none adjacent
  size_t slivers = 0;           // triangles thinner than 1/100 of their edge
  size_t components = 2;        // a noisy sphere, and a small one far off
};
DirtySoup Dirty();

// The boundary loops of `m` (adjacency built), and the longest one's length
// in mm.
size_t BoundaryLoops(const mesh::TriMesh& m, double* longest = nullptr);

// Triangles thinner than `ratio` of their longest edge (height / edge).
size_t Slivers(const mesh::TriMesh& m, double ratio = 0.01);

// `m` as a binary STL (float32, as every scanning app writes it).
void WriteStlTo(const mesh::TriMesh& m, const std::filesystem::path& path);
// ...at a fresh temporary path naming `tag`, which it returns.
std::filesystem::path WriteStl(const mesh::TriMesh& m, const std::string& tag);

// A copy moved by `offset`.
mesh::TriMesh Translated(const mesh::TriMesh& m, const glm::dvec3& offset);

// By name, for the bench's fixture writer: "sphere", "torus", "figurine",
// "cut_figurine". False for any other name.
bool OrganicByName(const std::string& name, size_t triangles, Organic& out);

} // namespace fixtures
