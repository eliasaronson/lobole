// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Organic meshes with known answers, generated in-test: what a sculpt
// download or a phone scan is, built straight as triangles
// (docs/interchange/README.md#mesh-bodies). No file is committed: the
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
#include <functional>
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

// A scanned turned part, for fitting primitives to: a flat base, a
// cylinder of radius 12 and height 40, a cone narrowing to radius 4 over
// 12 mm and a flat top, its axis tilted off Z and moved off the origin.
// Every vertex is moved along the surface normal by a deterministic noise
// of at most `noise` mm, and a smooth lump `lump` mm high stands on the
// cylinder -- the stray matter a least-squares fit is dragged by. Closed.
struct Post {
  Organic organic;
  glm::dvec3 base{0.0};                // the axis point on the bottom face
  glm::dvec3 axis{0.0, 0.0, 1.0};      // unit, bottom to top
  double radius = 12.0, height = 40.0; // the cylinder
  double coneHeight = 12.0, topRadius = 4.0;
  double noise = 0.0;
  [[nodiscard]] glm::dvec3 Apex() const;  // where the cone's sides meet
  [[nodiscard]] double HalfAngle() const; // the cone's, rad
  // Points on each surface, clear of its edges and of the lump.
  glm::dvec3 onCylinder{0.0}, onCone{0.0}, onTop{0.0}, onBottom{0.0};
  glm::dvec3 onLump{0.0}; // the lump's crest
  double lumpSigma = 0.0; // its width (mm, a Gaussian's sigma)
};
Post ScannedPost(size_t triangles, double noise = 0.05, double lump = 1.5);

// A scanned block with every edge rounded, for a fit that must stop at the
// edge: half sizes `half`, each edge a round of `round`, every vertex moved
// along the true normal by a deterministic noise of at most `noise`, turned
// off the axes and moved off the origin. Closed, a grid of about `spacing`.
struct Block {
  Organic organic;
  glm::dmat3 turn{1.0};
  glm::dvec3 centre{0.0};
  glm::dvec3 half{15.0, 12.0, 5.0};
  double round = 0.5, noise = 0.0;
  [[nodiscard]] glm::dvec3 ToWorld(const glm::dvec3& local) const;
  // The outward unit normal of the face on `axis` (0-2) at `sign` (+-1).
  [[nodiscard]] glm::dvec3 FaceNormal(int axis, int sign) const;
  glm::dvec3 nearEdge{0.0}; // on the +Z face, 2 mm in from its +Y edge
};
Block ScannedBlock(double spacing = 0.25, double noise = 0.05,
                   double round = 0.5);

// A scanned cup, for steering a fit by its side: an outer wall of radius
// 15, an inner one of 13.8, 30 tall with a 3 mm floor, the rim a flat
// between them; tilted off Z and moved off the origin, every vertex moved
// along the normal by a noise of at most `noise`. Closed.
struct Cup {
  Organic organic;
  glm::dvec3 base{0.0}, axis{0.0, 0.0, 1.0};
  double outerRadius = 15.0, innerRadius = 13.8, height = 30.0, floor = 3.0;
  double noise = 0.0;
  glm::dvec3 onRim{0.0}; // half way across the rim
};
Cup ScannedCup(size_t triangles, double noise = 0.05);

// A scanned ring: a torus of core radius `major` and tube `minor`, a grid
// of about `spacing`, its axis tilted off Z and its centre moved off the
// origin, every vertex moved along the true normal by a noise of at most
// `noise`. Closed.
struct Ring {
  Organic organic;
  glm::dvec3 centre{0.0}, axis{0.0, 0.0, 1.0}; // axis: largest component +
  double major = 0.0, minor = 0.0, noise = 0.0;
  // The surface at `turnDeg` round the axis and `tubeDeg` round the tube (0
  // the outer equator, 90 the side the axis points to).
  [[nodiscard]] glm::dvec3 At(double turnDeg, double tubeDeg) const;
};
Ring ScannedRing(double major, double minor, double spacing, double noise);

// A scanned boss standing on a round plate, a concave round at its foot: a
// plate of radius 30 and 5 thick, a boss of radius 8 and 15 tall above it,
// the round of radius 3 tangent to both -- a torus about the axis of core
// radius 11 and tube 3, its triangles facing its core. Tilted off Z and
// moved off the origin, every vertex moved along the normal by a noise of
// at most `noise`, a grid of about `spacing` on the round. Closed.
struct Boss {
  Organic organic;
  glm::dvec3 base{0.0}, axis{0.0, 0.0, 1.0}; // axis: bottom to top
  double plateRadius = 30.0, plate = 5.0, bossRadius = 8.0, bossHeight = 15.0,
         round = 3.0, noise = 0.0;
  // The round's torus: its centre on the axis, its core's radius.
  [[nodiscard]] glm::dvec3 RoundCentre() const;
  [[nodiscard]] double RoundMajor() const { return bossRadius + round; }
  // Half way round the round's quarter, on the plate's top clear of the
  // round, on the boss's side clear of it.
  glm::dvec3 onRound{0.0}, onPlate{0.0}, onBossSide{0.0};
};
Boss ScannedBoss(double spacing = 0.5, double noise = 0.05);

// An open patch of a cylinder's side: `arcDeg` of its arc and `length`
// along it, a grid of about `spacing`, facing out of the cylinder or (a
// bore) into it, tilted off the axes; noise radial, at most `noise`.
struct Arc {
  Organic organic;
  glm::dvec3 base{0.0}, axis{0.0, 0.0, 1.0};
  double radius = 0.0, arcDeg = 0.0, noise = 0.0;
  glm::dvec3 onSurface{0.0}; // the middle of the patch
};
Arc ScannedArc(double radius, double arcDeg, double length, double spacing,
               double noise, bool bore = false);

// A scanned bore whose radius is `radiusAt(h)` at `h` mm along its axis,
// for telling one surface from two (docs/modeling/README.md#two-surfaces):
// a stepped bore, a taper. The whole turn, `length` long, open at both
// ends and facing its axis, a grid of about `spacing` (a step between two
// grid rows is joined by the row of triangles across it, as a scan joins
// what it cannot resolve), tilted off the axes and moved off the origin;
// every vertex moved radially by a noise of at most `noise`.
struct Bore {
  Organic organic;
  glm::dvec3 base{0.0}, axis{0.0, 0.0, 1.0}; // axis: largest component +
  double length = 0.0, noise = 0.0;
  std::function<double(double)> radiusAt;
  // The point at `h` along the axis and `turnDeg` round it, on the bore.
  [[nodiscard]] glm::dvec3 At(double h, double turnDeg) const;
};
Bore ScannedBore(std::function<double(double)> radiusAt, double length,
                 double spacing, double noise);

// A scanned sheet: a height field `heightAt(x, y)` over [0, width] x [0,
// depth] facing +z in its own frame, a grid of about `spacing`, turned off
// the axes and moved off the origin; every vertex moved along the sheet's
// normal by a noise of at most `noise`. Open: a stepped or crowned plate's
// top.
struct Sheet {
  Organic organic;
  glm::dmat3 turn{1.0};
  glm::dvec3 origin{0.0};
  double width = 0.0, depth = 0.0, noise = 0.0;
  std::function<double(double, double)> heightAt;
  [[nodiscard]] glm::dvec3 ToWorld(const glm::dvec3& local) const;
  [[nodiscard]] glm::dvec3 Normal() const; // the sheet's +z, world
  // The surface over (x, y), world.
  [[nodiscard]] glm::dvec3 At(double x, double y) const;
};
Sheet ScannedSheet(std::function<double(double, double)> heightAt, double width,
                   double depth, double spacing, double noise);

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
