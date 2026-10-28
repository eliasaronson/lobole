// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
// A follower driven by the SHAPE of its cam
// (docs/assemblies/relations.md#contact): the `contact` relation's geometry.
// Both bodies are cut across the cam's axis in the middle of the width they
// share; at each cam angle the follower is carried along its own path from the
// clear side until it first touches. That 2D solve fills a table over one turn,
// once per geometry, and the assembly solve reads the table as it reads a
// cam's.

#include "core/AssemblySolve.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

class TopoDS_Shape;

namespace contact {

// A section's boundary as a polyline whose chords stay within `deflection`
// of the true curves, with each vertex's radius of curvature and its centre
// (radius 0 and centre = the point where the curve is straight).
struct Outline {
  std::vector<glm::dvec2> pts;
  std::vector<std::pair<int, int>> segs;
  std::vector<double> radius; // per vertex; 0 = straight
  std::vector<glm::dvec2> centre;
  std::vector<glm::dvec2> tangent; // unit, along the edge's parameter
  glm::dvec2 lo{0.0}, hi{0.0};
  double Size() const { return glm::length(hi - lo); }
};

// The section of `shape` (already in the plane frame) by the plane z = `z`.
// Empty `pts` with `*why` set when the plane misses it.
Outline SectionOutline(const TopoDS_Shape& shape, double z, double deflection,
                       std::string* why);

// How the follower moves with its DOF value q. Slide: by q along `dir`
// (unit). Turn: by `sense * q` radians about `pivot`.
struct Path {
  bool turn = false;
  glm::dvec2 dir{1.0, 0.0};
  glm::dvec2 pivot{0.0};
  double sense = 1.0;
};

struct Touch {
  bool found = false;
  double travel = 0.0;    // q at first contact
  glm::dvec2 point{0.0};  // contact point, plane frame
  glm::dvec2 normal{0.0}; // unit, from the cam toward the follower
  glm::dvec2 motion{0.0}; // unit: where the follower's point moves with +q
  double camRadius = 0.0; // signed: + convex, - concave, 0 straight
  double followerRadius = 0.0;
  bool bridge = false; // a second point touches within the tolerance
  glm::dvec2 point2{0.0};
};

// The follower's first contact with the cam turned to `camAngle` (radians,
// about the plane's +Z), approached with q moving in the `toward` sense
// (+1/-1) from clear. A turning follower is looked for within half a turn
// either side of q = `travelRef`.
Touch FirstTouch(const Outline& cam, double camAngle, const Outline& follower,
                 const Path& path, int toward, double travelRef, double tol);

// The relation's law over one turn of the driver: q at driver angle a
// (radians). Nodes every few degrees, bisected until a cubic through them
// reproduces an exact solve at every interval's midpoint within `tolerance`;
// an interval that keeps failing down to a microradian is a DROP, a step the
// interpolation does not smooth.
struct Table {
  struct Node {
    double a = 0.0, q = 0.0;
    bool drop = false; // the interval to the next node is a drop
  };
  std::vector<Node> nodes; // a in [0, 2pi), rising
  struct DropAt {
    double a = 0.0, from = 0.0, to = 0.0;
  };
  std::vector<DropAt> drops;
  std::vector<std::pair<double, double>> bridges; // driver ranges, radians
  double tolerance = 0.0; // q's unit (mm, or radians for a turn)
  double bound = 0.0;     // worst midpoint error found
  std::string error;
};
// `camSense` turns the cam by camSense * a. `toward` as in FirstTouch.
Table BuildTable(const Outline& cam, double camSense, const Outline& follower,
                 const Path& path, int toward, double tolerance);
// q at driver angle `a` and dq/da.
double TableValue(const Table& t, double a, double* slope = nullptr);

// Everything one relation needs, derived from the assembly and cached on
// the geometry (the bodies' GeometryIds, the frames between them, `toward`).
struct Model {
  uint64_t cam = 0, follower = 0; // occurrences
  bool turn = false;              // the follower's DOF is a rotation
  // The plane frame: mate a's connector on its BASE side, Z the cam's axis,
  // in that occurrence's coordinates; and the base occurrence.
  uint64_t base = 0;
  glm::dmat4 planeInBase{1.0};
  double camSense = 1.0;
  Path path;
  Outline camOutline, followerOutline;
  double z = 0.0;         // the section's height in the plane frame
  double tolerance = 0.0; // mm: 1e-5 of the smaller outline
  int toward = 1;
  Table table;
};

// The model for relation `r` (kind Contact): the outlines, the path and the
// table, from the cache when the geometry is unchanged. Null with `*why`
// set when it cannot be built.
std::shared_ptr<const Model> ModelFor(const AssemblyDocument& asmDoc,
                                      const Relation& r,
                                      const assembly::PartResolver& partById,
                                      std::string* why);

// Why `r` cannot be a contact relation in `asmDoc` (empty when it can): its
// mates, their degrees of freedom, the occurrences each moves, the bodies,
// the planar case, and the bases rigid to each other. Cheap: no geometry
// beyond the bodies' boxes.
std::string ContactProblem(const AssemblyDocument& asmDoc, const Relation& r,
                           const assembly::PartResolver& partById);

// `toward` derived from gravity: the sign of the generalised gravity force
// on b's DOF at the current pose, from the follower's mass centre (and that
// of every occurrence rigid with it). 0 with `*why` when gravity is too
// weak along the DOF to choose (a horizontal slide).
int TowardFromGravity(const AssemblyDocument& asmDoc, const Relation& r,
                      const assembly::PartResolver& partById, std::string* why);

// The occurrences rigid with `occ` (fastened mates, attachments, and every
// fixed occurrence together), `occ` included.
std::vector<uint64_t> RigidGroup(const AssemblyDocument& asmDoc, uint64_t occ);

struct CacheStats {
  size_t builds = 0, hits = 0;
};
CacheStats Stats();

} // namespace contact
