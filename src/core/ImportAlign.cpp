// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ImportAlign.h"

#include "core/Angles.h"
#include "core/Document.h"
#include "core/MeshOperand.h"
#include "core/mesh/Align.h"
#include "core/mesh/MeshBvh.h"
#include "core/mesh/Register.h"
#include "core/mesh/Segment.h" // ScanTolerance

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
  case Mode::BestFit:
    return "best_fit";
  }
  return "floor";
}

std::optional<Mode> ModeFromString(const std::string& s) {
  for (Mode m : {Mode::Floor, Mode::ThreePoints, Mode::Principal, Mode::Scale,
                 Mode::BestFit})
    if (s == ToString(m))
      return m;
  return std::nullopt;
}

std::string AcceptedModes() {
  return "floor, three_points, principal, scale, best_fit";
}

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
  case Mode::BestFit:
    return 0;
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
        "no points", "two points_mm and the true distance_mm between them",
        "no points (its picked pairs are pairs_mm)"};
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
  const auto finite = [](const glm::dvec3& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  };
  if (r.mode != Mode::BestFit && (!r.pairsMm.empty() || r.region))
    throw std::runtime_error(std::string(ToString(r.mode)) +
                             " takes no pairs_mm or region: they are "
                             "best_fit's");
  if (!r.pairsMm.empty() && r.pairsMm.size() != 3)
    throw std::runtime_error(
        "pairs_mm is three pairs [[on the import, on the body], ...]; got " +
        std::to_string(r.pairsMm.size()));
  for (const auto& pair : r.pairsMm)
    if (!finite(pair[0]) || !finite(pair[1]))
      throw std::runtime_error("a point is three finite numbers (mm)");
  if (r.region &&
      (!finite(r.region->pointMm) || !std::isfinite(r.region->radiusMm) ||
       !(r.region->radiusMm > 0.0)))
    throw std::runtime_error("region: point_mm is three finite numbers and "
                             "radius_mm a number of mm greater than 0");
}

bool Align(const std::vector<const Body*>& bodies,
           const importplace::Placement& current, const Request& r, Result& out,
           std::string& error) {
  CheckRequest(r);
  if (r.mode == Mode::BestFit)
    throw std::logic_error("best_fit has a body to go onto: BestFit");
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
  case Mode::BestFit:
    break; // refused above
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

namespace {

// Several meshes as one, with only the triangles `keep` admits (by their
// centroid); the one mesh itself when there is one and it keeps all.
std::shared_ptr<const mesh::TriMesh>
Joined(const std::vector<std::shared_ptr<const mesh::TriMesh>>& meshes,
       const std::function<bool(const glm::dvec3&)>& keep) {
  if (meshes.size() == 1 && !keep)
    return meshes.front();
  auto out = std::make_shared<mesh::TriMesh>();
  for (const auto& m : meshes) {
    const uint32_t base = static_cast<uint32_t>(out->positions.size());
    out->positions.insert(out->positions.end(), m->positions.begin(),
                          m->positions.end());
    out->quantum = std::max(out->quantum, m->quantum);
    for (uint32_t t = 0; t < m->triangles.size(); t++) {
      if (keep &&
          !keep((m->Corner(t, 0) + m->Corner(t, 1) + m->Corner(t, 2)) / 3.0))
        continue;
      const auto& tri = m->triangles[t];
      out->triangles.push_back({tri[0] + base, tri[1] + base, tri[2] + base});
    }
  }
  return out;
}

// How many points the starts run on, and the winner is refined and
// measured on: thinned enough that 25 starts cost a few seconds on any
// scan, dense enough that the noise averages out of the pose.
constexpr size_t kStartSamples = 1500;
constexpr size_t kFineSamples = 12000;
constexpr size_t kSurfels = 20000; // the stand-in for a dense target
constexpr uint64_t kSeed = 0x5ca2b0d1ull;

} // namespace

BestFitInput GatherBestFit(const std::vector<const Body*>& scan,
                           const std::vector<const Body*>& onto) {
  BestFitInput in;
  for (const Body* b : scan) {
    double used = 0.0;
    if (auto m = meshop::Triangles(*b, 0.0, used);
        m != nullptr && !m->triangles.empty())
      in.scan.push_back(std::move(m));
  }
  for (const Body* b : onto) {
    double used = 0.0;
    if (auto m = meshop::Triangles(*b, 0.0, used);
        m != nullptr && !m->triangles.empty()) {
      in.onto.push_back(std::move(m));
      in.ontoChordMm = std::max(in.ontoChordMm, used);
      in.ontoName += (in.ontoName.empty() ? "" : ", ") + b->name;
    }
  }
  return in;
}

std::string CheckBestFit(const BestFitInput& in, const Request& r) {
  CheckRequest(r);
  if (in.scan.empty())
    return "the import has no triangles to lay onto anything: it made "
           "nothing (see its error)";
  if (in.onto.empty())
    return "'" + in.ontoName + "' has no surface to lay the import onto";
  if (r.region) {
    const auto any =
        [&](const std::vector<std::shared_ptr<const mesh::TriMesh>>& meshes) {
          for (const auto& m : meshes)
            for (uint32_t t = 0; t < m->triangles.size(); t++)
              if (glm::length(
                      (m->Corner(t, 0) + m->Corner(t, 1) + m->Corner(t, 2)) /
                          3.0 -
                      r.region->pointMm) <= r.region->radiusMm)
                return true;
          return false;
        };
    const std::string where = "radius_mm " + Mm(r.region->radiusMm) +
                              " of point_mm " + Point(r.region->pointMm);
    if (!any(in.scan))
      return "region: the import has no surface within " + where;
    if (!r.region->onScan && !any(in.onto))
      return "region: '" + in.ontoName + "' has no surface within " + where;
  }
  if (!r.pairsMm.empty()) {
    std::array<glm::dvec3, 3> from, to;
    for (int i = 0; i < 3; i++) {
      from[i] = r.pairsMm[i][0];
      to[i] = r.pairsMm[i][1];
    }
    if (!mesh::FromPairs(from, to)) {
      const auto line = [](const std::array<glm::dvec3, 3>& p) {
        return !mesh::FromPairs(
            p, {glm::dvec3(0, 0, 0), glm::dvec3(1, 0, 0), glm::dvec3(0, 1, 0)});
      };
      const std::array<glm::dvec3, 3>& bad = line(from) ? from : to;
      return std::string("pairs_mm: the three points ") +
             (line(from) ? "on the import" : "on the body") + " " +
             Point(bad[0]) + ", " + Point(bad[1]) + ", " + Point(bad[2]) +
             " are in a line, so they fix no turn about it: pick three "
             "spread over the part";
    }
  }
  return "";
}

bool BestFit(const BestFitInput& in, const importplace::Placement& current,
             const Request& r, const Progress& progress, Result& out,
             std::string& error) {
  if (error = CheckBestFit(in, r); !error.empty())
    return false;
  // A region is a patch of both: the scan's samples within the radius as
  // the scan stands now, and -- picked on the body -- the body's surface
  // within a quarter more, so a scan standing a little off still finds it.
  const auto within = [&](const glm::dvec3& c, double grow) {
    return glm::length(c - r.region->pointMm) <= grow * r.region->radiusMm;
  };
  const bool ontoRegion = r.region && !r.region->onScan;
  const std::shared_ptr<const mesh::TriMesh> scan = Joined(in.scan, nullptr);
  const std::shared_ptr<const mesh::TriMesh> target = Joined(
      in.onto, ontoRegion
                   ? std::function<bool(const glm::dvec3&)>(
                         [&](const glm::dvec3& c) { return within(c, 1.25); })
                   : nullptr);
  const auto keepScan =
      r.region ? std::function<bool(uint32_t)>([&](uint32_t t) {
        return within(
            (scan->Corner(t, 0) + scan->Corner(t, 1) + scan->Corner(t, 2)) /
                3.0,
            1.0);
      })
               : std::function<bool(uint32_t)>();
  const auto regionText = [&]() {
    return "radius_mm " + Mm(r.region->radiusMm) + " of point_mm " +
           Point(r.region->pointMm);
  };
  if (target->triangles.empty()) {
    error =
        "region: '" + in.ontoName + "' has no surface within " + regionText();
    return false;
  }
  std::vector<mesh::SurfaceSample> fine =
      mesh::SampleSurface(*scan, kFineSamples, kSeed, keepScan);
  if (fine.empty()) {
    error = r.region
                ? "region: the import has no surface within " + regionText()
                : std::string("the import's triangles have no area");
    return false;
  }
  const std::vector<mesh::SurfaceSample> coarse(
      fine.begin(), fine.begin() + std::min(kStartSamples, fine.size()));

  // The tolerance a sample is "within": the scan's noise, never finer than
  // the chord a B-Rep target was tessellated at (nor, below, a
  // ten-thousandth of the body).
  double withinMm = in.ontoChordMm;
  for (const auto& m : in.scan)
    withinMm = std::max(withinMm, mesh::ScanTolerance(*m));
  glm::dvec3 tlo, thi;
  target->Bounds(tlo, thi);
  const double targetDiag = glm::length(thi - tlo);
  // Never under a ten-thousandth of the body: an exact copy has the noise
  // of its file's precision, finer than any fit settles to, and "16% within
  // 8e-06 mm" says nothing about how it lies.
  withinMm = std::max(withinMm, 1e-4 * targetDiag);
  const double cap = std::max(targetDiag / 10.0, 10.0 * withinMm);

  // The starts.
  std::vector<mesh::Rigid> starts;
  std::string pairNote;
  if (!r.pairsMm.empty()) {
    std::array<glm::dvec3, 3> from, to;
    for (int i = 0; i < 3; i++) {
      from[i] = r.pairsMm[i][0];
      to[i] = r.pairsMm[i][1];
    }
    const std::optional<mesh::Rigid> start = mesh::FromPairs(from, to);
    if (!start) {
      error = CheckBestFit(in, r);
      return false;
    }
    // Rigid: three sides that differ say a scale is wrong, which a best fit
    // never fixes.
    double worst = 0.0;
    for (int i = 0; i < 3; i++) {
      const double a = glm::length(from[(i + 1) % 3] - from[i]),
                   b = glm::length(to[(i + 1) % 3] - to[i]);
      worst = std::max(worst, std::abs(a - b) / std::max(b, 1e-12));
    }
    if (worst > 0.05)
      pairNote = "the picked pairs' distances differ by up to " +
                 Mm(100.0 * worst) +
                 "% between the import and the body: a best fit is rigid, "
                 "so a wrong scale stays (align_import mode scale fixes it)";
    starts.push_back(*start);
  } else {
    starts.push_back(mesh::Rigid{}); // the placement as it stands
    // A patch has no shape of its own to find the part by: it is laid from
    // where it stands.
    if (!r.region)
      for (const mesh::Rigid& st : mesh::SearchStarts(coarse, *target))
        starts.push_back(st);
  }

  const mesh::FacingSurface surface(
      target == in.onto.front() ? mesh::Bvh::For(target)
                                : std::make_shared<const mesh::Bvh>(target));
  mesh::SearchOptions o;
  o.starts = starts;
  o.capMm = cap;
  o.settleMm = 1e-6 * targetDiag;
  o.cancel = progress.cancel;
  o.progress = progress.fraction;
  o.distinctMm = std::max(5.0 * withinMm, targetDiag / 20.0);
  // While the poses are far off, a stand-in of the target's surface: a
  // dense body costs what its triangles cost in every one of thousands of
  // searches, and a far-off pose does not need them.
  std::unique_ptr<mesh::FacingSurface> rough;
  if (target->triangles.size() > 4 * kSurfels)
    rough = std::make_unique<mesh::FacingSurface>(
        std::make_shared<const mesh::Bvh>(std::make_shared<const mesh::TriMesh>(
            mesh::Surfels(*target, kSurfels, kSeed ^ 0x7a))));
  const mesh::SearchResult found =
      mesh::Search(coarse, fine, surface, o, rough.get());
  if (found.cancelled) {
    out.cancelled = true;
    return true;
  }
  const mesh::Rigid pose = found.pose;
  const mesh::Distances d = mesh::MeasureDistances(
      fine, pose, surface, o.minFacingCos, cap, withinMm);

  double moved = 0.0;
  for (const glm::dvec3& v : scan->positions)
    moved = std::max(moved, glm::length(pose(v) - v));

  nlohmann::json& rep = out.report;
  rep = nlohmann::json::object();
  rep["onto"] = in.ontoName;
  rep["rotate_deg"] = Json(importplace::EulerDeg(pose.rotation));
  rep["translate_mm"] = Json(pose.translation);
  rep["moved_mm"] = moved;
  rep["mean_mm"] = d.meanMm;
  rep["p95_mm"] = d.p95Mm;
  rep["within_mm"] = withinMm;
  rep["within"] = d.within;
  rep["unmatched"] = d.unmatched;
  rep["samples"] = fine.size();
  rep["starts"] = starts.size();
  rep["winner"] = !r.pairsMm.empty()            ? "pairs"
                  : found.keptFrom.front() == 0 ? "placement"
                                                : "principal";
  if (in.ontoChordMm > 0.0)
    rep["chord_mm"] = in.ontoChordMm;
  // The best start that ended somewhere else: how close another attitude
  // comes, which a symmetric part makes as good as the winner.
  glm::dvec3 centre(0.0);
  for (const auto& s : coarse)
    centre += s.point;
  centre /= double(coarse.size());
  const mesh::StartOutcome& w = found.kept.front();
  // Another attitude, or the same one a tenth of the part away: a start that
  // ended a little short of the winner is the winner, not a rival.
  const double apart = std::max(5.0 * withinMm, targetDiag / 10.0);
  std::optional<size_t> second;
  for (size_t i = 1; i < found.kept.size(); i++) {
    const mesh::StartOutcome& s = found.kept[i];
    const double turn = angles::ToDegrees(
        mesh::TurnAngle(glm::transpose(w.pose.rotation) * s.pose.rotation));
    if (turn < 10.0 && glm::length(s.pose(centre) - w.pose(centre)) < apart)
      continue;
    second = i; // kept in order of score: the first is the best
    break;
  }
  if (second) {
    const mesh::StartOutcome& s = found.kept[*second];
    const double turn = angles::ToDegrees(
        mesh::TurnAngle(glm::transpose(w.pose.rotation) * s.pose.rotation));
    rep["runner_up"] = {
        {"score_mm", s.scoreMm},
        {"turn_deg", turn},
        {"move_mm", glm::length(s.pose(centre) - w.pose(centre))}};
    rep["score_mm"] = w.scoreMm;
    if (s.scoreMm < w.scoreMm + std::max(withinMm, 0.5 * w.scoreMm))
      rep["note"] =
          "another attitude, " + Mm(turn) +
          " deg from this one, lies nearly as close (a capped mean of " +
          Mm(s.scoreMm) + " mm against " + Mm(w.scoreMm) +
          "): the shape barely tells them apart; three pairs_mm say which";
  }
  if (!pairNote.empty())
    rep["note"] = rep.contains("note")
                      ? rep["note"].get<std::string>() + "; " + pairNote
                      : pairNote;
  // Rigid: the factor is 1, so the scale stays what it was.
  out.placement = current.Then(pose.translation, pose.rotation, 1.0);
  return true;
}

} // namespace importalign
