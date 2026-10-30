// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ImportAlign.h"

#include "core/Document.h"
#include "core/MeshOperand.h"
#include "core/mesh/Align.h"
#include "core/mesh/MeshBvh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace importalign {

namespace {

constexpr glm::dvec3 kUp(0.0, 0.0, 1.0);

struct Gathered {
  std::vector<std::shared_ptr<const mesh::TriMesh>> meshes;
  std::vector<std::string> names;
  glm::dvec3 lo{0.0}, hi{0.0};
  double quantum = 0.0;
  [[nodiscard]] double Diagonal() const { return glm::length(hi - lo); }
};

// Every body as triangles: a mesh body's own, a B-Rep's tessellated at a
// deflection derived from its size (meshop::Triangles).
Gathered Gather(const std::vector<const Body*>& bodies) {
  Gathered g;
  bool any = false;
  for (const Body* b : bodies) {
    double used = 0.0;
    auto m = meshop::Triangles(*b, 0.0, used);
    if (m == nullptr || m->triangles.empty())
      continue;
    glm::dvec3 lo, hi;
    m->Bounds(lo, hi);
    g.lo = any ? glm::min(g.lo, lo) : lo;
    g.hi = any ? glm::max(g.hi, hi) : hi;
    any = true;
    g.quantum = std::max(g.quantum, m->quantum);
    g.meshes.push_back(std::move(m));
    g.names.push_back(b->name);
  }
  return g;
}

// The bounding box of every vertex turned by `turn`: its centre.
glm::dvec3 TurnedCentre(const Gathered& g, const glm::dmat3& turn) {
  glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-lo);
  for (const auto& m : g.meshes)
    for (const glm::dvec3& v : m->positions) {
      const glm::dvec3 p = turn * v;
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
  return 0.5 * (lo + hi);
}

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

std::string Point(const glm::dvec3& p) {
  return "[" + Mm(p.x) + ", " + Mm(p.y) + ", " + Mm(p.z) + "]";
}

nlohmann::json Json(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

// The turn and move that lay the plane through `origin` with normal `n` on
// z = 0, the bodies above it (on the side holding the surface's centroid;
// `facing`, the side the plane's own triangles face, goes down when the
// centroid is on the plane), centred over the origin in x and y.
void LayOnFloor(const Gathered& g, const glm::dvec3& origin, glm::dvec3 n,
                const glm::dvec3& facing, glm::dmat3& turn, glm::dvec3& move) {
  mesh::SurfaceMoments moments;
  for (const auto& m : g.meshes)
    moments.Add(*m);
  const double side = glm::dot(moments.Centroid() - origin, n);
  if (std::abs(side) > 1e-9 * g.Diagonal())
    n = side > 0.0 ? n : -n; // up: towards the bodies
  else
    n = glm::dot(facing, n) > 0.0 ? -n : n;
  turn = mesh::RotationBetween(n, kUp);
  const glm::dvec3 c = TurnedCentre(g, turn);
  move = {-c.x, -c.y, -(turn * origin).z};
}

} // namespace

nlohmann::json PlacementJson(const importplace::Placement& p) {
  return {{"translate_mm", Json(p.translateMm)},
          {"rotate_deg", Json(p.rotateDeg)}};
}

const char* ToString(Mode m) {
  switch (m) {
  case Mode::Floor:
    return "floor";
  case Mode::ThreePoints:
    return "three_points";
  case Mode::Principal:
    return "principal";
  case Mode::Scale:
    return "scale";
  }
  return "floor";
}

std::optional<Mode> ModeFromString(const std::string& s) {
  for (Mode m : {Mode::Floor, Mode::ThreePoints, Mode::Principal, Mode::Scale})
    if (s == ToString(m))
      return m;
  return std::nullopt;
}

std::string AcceptedModes() { return "floor, three_points, principal, scale"; }

size_t PointsFor(Mode m) {
  switch (m) {
  case Mode::Floor:
    return 1;
  case Mode::ThreePoints:
    return 3;
  case Mode::Principal:
    return 0;
  case Mode::Scale:
    return 2;
  }
  return 0;
}

std::vector<const Body*> ImportBodies(const Document& doc,
                                      const ImportFeature& import) {
  std::vector<const Body*> out;
  size_t index = 0;
  if (doc.FindFeatureById(import.GetId(), &index) != nullptr)
    if (const std::vector<Body>* after = doc.CachedBodiesAfter(index))
      for (const Body& b : *after)
        if (b.createdBy == import.GetId())
          out.push_back(&b);
  if (out.empty())
    for (const Body& b : doc.GetBodies())
      if (b.createdBy == import.GetId())
        out.push_back(&b);
  return out;
}

void CheckRequest(const Request& r) {
  const size_t want = PointsFor(r.mode);
  if (r.pointsMm.size() != want) {
    static const char* const kWhat[] = {
        "one point_mm on the floor", "three points_mm the plane passes through",
        "no points", "two points_mm and the true distance_mm between them"};
    throw std::runtime_error(std::string(ToString(r.mode)) + " takes " +
                             kWhat[static_cast<int>(r.mode)] + "; got " +
                             std::to_string(r.pointsMm.size()) + " point" +
                             (r.pointsMm.size() == 1 ? "" : "s"));
  }
  for (const glm::dvec3& p : r.pointsMm)
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
      throw std::runtime_error("a point is three finite numbers (mm)");
  if (r.mode == Mode::Scale &&
      (!std::isfinite(r.distanceMm) || !(r.distanceMm > 0.0)))
    throw std::runtime_error("scale: distance_mm is the true distance "
                             "between the two points, a number of mm "
                             "greater than 0");
  if (r.toleranceMm &&
      (!std::isfinite(*r.toleranceMm) || !(*r.toleranceMm > 0.0)))
    throw std::runtime_error("floor: tolerance_mm is a number of mm "
                             "greater than 0");
}

bool Align(const std::vector<const Body*>& bodies,
           const importplace::Placement& current, const Request& r, Result& out,
           std::string& error) {
  CheckRequest(r);
  const Gathered g = Gather(bodies);
  if (g.meshes.empty()) {
    error = "the import has no bodies to align: it made nothing (see its "
            "error)";
    return false;
  }
  const double diag = g.Diagonal();
  glm::dmat3 turn(1.0);
  glm::dvec3 move(0.0);
  double factor = 1.0;
  out.report = nlohmann::json::object();

  switch (r.mode) {
  case Mode::Floor: {
    const double tol =
        r.toleranceMm.value_or(std::max(diag / 500.0, 4.0 * g.quantum));
    // The body and the triangle the point is on.
    const glm::dvec3 p = r.pointsMm[0];
    size_t best = 0;
    mesh::Bvh::Closest hit;
    hit.distance = std::numeric_limits<double>::max();
    for (size_t i = 0; i < g.meshes.size(); i++) {
      const mesh::Bvh::Closest c = mesh::Bvh::For(g.meshes[i])->ClosestPoint(p);
      if (c.found && c.distance < hit.distance) {
        hit = c;
        best = i;
      }
    }
    // A picked point is ON the surface; one this far off is a mistake in
    // the coordinates, not a floor.
    if (!hit.found || hit.distance > diag / 50.0) {
      error = "point_mm " + Point(p) + " is " +
              (hit.found ? Mm(hit.distance) : std::string("far")) +
              " mm from the import's nearest surface (it is " + Mm(diag) +
              " mm across): pick a point on the floor of it";
      return false;
    }
    const mesh::TriMesh& m = *g.meshes[best];
    const mesh::PlaneRegion region = mesh::GrowPlane(m, hit.triangle, tol);
    if (!region.fit.ok) {
      error = "no plane fits at point_mm " + Point(p) +
              ": the triangle there is degenerate";
      return false;
    }
    glm::dvec3 facing = region.fit.axis;
    if (!region.FromLoop()) {
      facing = glm::dvec3(0.0);
      for (uint32_t t : region.triangles)
        facing += m.AreaNormal(t);
    }
    LayOnFloor(g, region.fit.origin, region.fit.axis, facing, turn, move);
    nlohmann::json& rep = out.report;
    rep["fitted"] = region.FromLoop() ? "boundary_loop" : "region";
    rep["body"] = g.names[best];
    if (region.FromLoop())
      rep["loop_vertices"] = region.loop.size();
    else {
      rep["triangles"] = region.triangles.size();
      rep["area_mm2"] = region.areaMm2;
    }
    rep["rms_mm"] = region.fit.rms;
    rep["max_mm"] = region.fit.max;
    rep["tolerance_mm"] = tol;
    rep["normal"] = Json(region.fit.axis);
    break;
  }
  case Mode::ThreePoints: {
    const glm::dvec3 a = r.pointsMm[0], b = r.pointsMm[1], c = r.pointsMm[2];
    const glm::dvec3 n = glm::cross(b - a, c - a);
    const double span =
        std::max({glm::length(b - a), glm::length(c - a), glm::length(c - b)});
    if (!(glm::length(n) > 1e-9 * span * span) || !(span > 0.0)) {
      error = "the three points_mm " + Point(a) + ", " + Point(b) + ", " +
              Point(c) +
              " are in a line, so no one plane passes through "
              "them: pick three spread over the floor";
      return false;
    }
    const glm::dvec3 unit = glm::normalize(n);
    LayOnFloor(g, (a + b + c) / 3.0, unit, unit, turn, move);
    out.report["normal"] = Json(unit);
    break;
  }
  case Mode::Principal: {
    mesh::SurfaceMoments moments;
    for (const auto& m : g.meshes)
      moments.Add(*m);
    if (!(moments.area > 0.0)) {
      error = "the import's triangles have no area to take axes from";
      return false;
    }
    glm::dvec3 values;
    glm::dmat3 vectors;
    mesh::SymmetricEigen3(moments.Covariance(), values, vectors);
    // The widest spread along X, the next along Y: a scan lies flat, its
    // longest way along X. Each axis keeps the sense nearest the world's.
    glm::dvec3 e1 = glm::normalize(vectors[2]), e2 = glm::normalize(vectors[1]);
    if (e1.x < 0.0)
      e1 = -e1;
    if (e2.y < 0.0)
      e2 = -e2;
    const glm::dvec3 e3 = glm::cross(e1, e2);
    turn = glm::transpose(glm::dmat3(e1, e2, e3));
    move = -TurnedCentre(g, turn);
    const glm::dvec3 spread(std::sqrt(std::max(0.0, values[2])),
                            std::sqrt(std::max(0.0, values[1])),
                            std::sqrt(std::max(0.0, values[0])));
    out.report["spread_mm"] = Json(spread);
    // Two spreads alike leave the axes between them to the noise.
    const char* const kAxis[] = {"X", "Y", "Z"};
    for (int i = 0; i < 2; i++)
      if (spread[i] > 0.0 && spread[i] - spread[i + 1] < 0.01 * spread[i])
        out.report["note"] =
            std::string("the spreads along ") + kAxis[i] + " and " +
            kAxis[i + 1] +
            " are within 1% of each other: which way those "
            "two axes point is not defined by the shape, and a re-scan can "
            "turn it";
    break;
  }
  case Mode::Scale: {
    const double measured = glm::length(r.pointsMm[1] - r.pointsMm[0]);
    if (!(measured > 1e-9 * std::max(diag, 1e-9))) {
      error = "the two points_mm are in one place, " + Point(r.pointsMm[0]) +
              ": pick the two ends of the distance you know";
      return false;
    }
    factor = r.distanceMm / measured;
    out.report["measured_mm"] = measured;
    out.report["factor"] = factor;
    break;
  }
  }
  out.placement = current.Then(move, turn, factor);
  return true;
}

} // namespace importalign
