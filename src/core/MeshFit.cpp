// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MeshFit.h"

#include "core/Angles.h"
#include "core/MeshOperand.h"
#include "core/mesh/MeshBvh.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

namespace meshfit {

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

std::string Point(const glm::dvec3& p) {
  return "[" + Mm(p.x) + ", " + Mm(p.y) + ", " + Mm(p.z) + "]";
}

nlohmann::json Json(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

// The order auto prefers a kind in: by how many numbers it takes.
int Simplicity(mesh::Kind k) {
  switch (k) {
  case mesh::Kind::Plane:
    return 0;
  case mesh::Kind::Sphere:
    return 1;
  case mesh::Kind::Cylinder:
    return 2;
  case mesh::Kind::Cone:
    return 3;
  }
  return 4;
}

} // namespace

std::string AcceptedKinds() { return "plane, cylinder, cone, sphere, auto"; }

bool KindFromString(const std::string& s, std::optional<mesh::Kind>& kind) {
  if (s == "auto") {
    kind.reset();
    return true;
  }
  for (mesh::Kind k : {mesh::Kind::Plane, mesh::Kind::Cylinder,
                       mesh::Kind::Cone, mesh::Kind::Sphere})
    if (s == mesh::KindName(k)) {
      kind = k;
      return true;
    }
  return false;
}

void CheckRequest(const Request& r) {
  const glm::dvec3& p = r.pointMm;
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    throw std::runtime_error("point_mm is three finite numbers (mm)");
  if (r.toleranceMm &&
      (!std::isfinite(*r.toleranceMm) || !(*r.toleranceMm > 0.0)))
    throw std::runtime_error("tolerance_mm is a number of mm greater than 0 "
                             "(leave it out to take the scan's own noise)");
}

std::vector<const Body*> BodiesOf(const std::vector<Body>& bodies,
                                  uint64_t featureId) {
  std::vector<const Body*> out;
  for (const Body& b : bodies)
    if (b.createdBy == featureId && !b.reference)
      out.push_back(&b);
  return out;
}

glm::dvec3 CanonicalAxis(const mesh::FitResult& f) {
  glm::dvec3 a = f.axis;
  if (f.kind == mesh::Kind::Cone)
    return a;
  int big = 0;
  for (int i = 1; i < 3; i++)
    if (std::abs(a[i]) > std::abs(a[big]))
      big = i;
  return a[big] < 0.0 ? -a : a;
}

bool Fit(const std::vector<const Body*>& bodies, const Request& r, Result& out,
         std::string& error) {
  CheckRequest(r);
  // The body and the triangle the point is on.
  std::shared_ptr<const mesh::TriMesh> best;
  const Body* bestBody = nullptr;
  mesh::Bvh::Closest hit;
  hit.distance = std::numeric_limits<double>::max();
  glm::dvec3 lo(std::numeric_limits<double>::max()), hi(-lo);
  for (const Body* b : bodies) {
    double used = 0.0;
    auto m = meshop::Triangles(*b, 0.0, used);
    if (m == nullptr || m->triangles.empty())
      continue;
    glm::dvec3 l, h;
    m->Bounds(l, h);
    lo = glm::min(lo, l);
    hi = glm::max(hi, h);
    const mesh::Bvh::Closest c = mesh::Bvh::For(m)->ClosestPoint(r.pointMm);
    if (c.found && c.distance < hit.distance) {
      hit = c;
      best = m;
      bestBody = b;
    }
  }
  if (best == nullptr) {
    error = "there are no triangles to fit: the feature made no body with "
            "a surface";
    return false;
  }
  const double diag = glm::length(hi - lo);
  // A picked point is ON the surface; one this far off is a mistake in the
  // coordinates, not a seed.
  if (!hit.found || hit.distance > diag / 50.0) {
    error = "point_mm " + Point(r.pointMm) + " is " +
            (hit.found ? Mm(hit.distance) : std::string("far")) +
            " mm from the nearest surface (the body is " + Mm(diag) +
            " mm across): pick a point on the surface to fit";
    return false;
  }
  const mesh::TriMesh& m = *best;
  out = Result{};
  out.body = bestBody->name;
  out.seedDistanceMm = hit.distance;
  out.toleranceMm = r.toleranceMm.value_or(mesh::ScanTolerance(m));

  std::vector<mesh::Kind> kinds;
  if (r.kind)
    kinds.push_back(*r.kind);
  else
    kinds = {mesh::Kind::Plane, mesh::Kind::Sphere, mesh::Kind::Cylinder,
             mesh::Kind::Cone};
  std::vector<mesh::Region> grown;
  for (mesh::Kind k : kinds) {
    std::string why;
    mesh::Region g = mesh::GrowFrom(m, hit.triangle, k, out.toleranceMm, &why);
    Candidate c;
    c.kind = k;
    c.ok = g.fit.ok;
    c.areaMm2 = g.area;
    c.rmsMm = g.fit.rms;
    c.why = why;
    out.tried.push_back(c);
    grown.push_back(std::move(g));
  }
  if (r.kind) {
    if (!grown[0].fit.ok) {
      error = out.tried[0].why + " (at point_mm " + Point(r.pointMm) + " on '" +
              out.body + "', tolerance_mm " + Mm(out.toleranceMm) + ")";
      return false;
    }
    out.region = std::move(grown[0]);
    out.tried.clear();
    return true;
  }
  double most = 0.0;
  for (const mesh::Region& g : grown)
    if (g.fit.ok)
      most = std::max(most, g.area);
  int pick = -1;
  for (size_t i = 0; i < grown.size(); i++) {
    if (!grown[i].fit.ok || grown[i].area < kAutoShare * most)
      continue;
    if (pick < 0 || Simplicity(grown[i].fit.kind) <
                        Simplicity(grown[size_t(pick)].fit.kind))
      pick = static_cast<int>(i);
  }
  if (pick < 0) {
    error = "no plane, sphere, cylinder or cone fits at point_mm " +
            Point(r.pointMm) + " on '" + out.body + "' to " +
            Mm(out.toleranceMm) + " mm";
    return false;
  }
  out.region = std::move(grown[size_t(pick)]);
  out.chosen = true;
  return true;
}

nlohmann::json ResultJson(const Result& r) {
  const mesh::FitResult& f = r.region.fit;
  nlohmann::json j;
  j["kind"] = mesh::KindName(f.kind);
  switch (f.kind) {
  case mesh::Kind::Plane:
    j["origin_mm"] = Json(f.origin);
    j["normal"] = Json(f.axis);
    break;
  case mesh::Kind::Cylinder:
    j["origin_mm"] = Json(f.origin);
    j["axis"] = Json(CanonicalAxis(f));
    j["radius_mm"] = f.radius;
    break;
  case mesh::Kind::Cone:
    j["apex_mm"] = Json(f.origin);
    j["axis"] = Json(f.axis);
    j["half_angle_deg"] = angles::ToDegrees(f.halfAngle);
    break;
  case mesh::Kind::Sphere:
    j["centre_mm"] = Json(f.origin);
    j["radius_mm"] = f.radius;
    break;
  }
  j["rms_mm"] = f.rms;
  j["max_mm"] = f.max;
  j["tolerance_mm"] = r.toleranceMm;
  j["triangles"] = r.region.triangles.size();
  j["area_mm2"] = r.region.area;
  j["body"] = r.body;
  j["seed_distance_mm"] = r.seedDistanceMm;
  if (r.chosen) {
    nlohmann::json tried = nlohmann::json::array();
    for (const Candidate& c : r.tried) {
      nlohmann::json t{{"kind", mesh::KindName(c.kind)}, {"fitted", c.ok}};
      if (c.ok) {
        t["area_mm2"] = c.areaMm2;
        t["rms_mm"] = c.rmsMm;
      } else {
        t["why"] = c.why;
      }
      tried.push_back(std::move(t));
    }
    j["tried"] = std::move(tried);
  }
  return j;
}

bool Refit(const RegenContext& ctx, const FitRef& ref, Result& out,
           std::string& error) {
  const std::vector<const Body*> bodies =
      BodiesOf(ctx.bodies, ref.bodyFeatureId);
  if (bodies.empty()) {
    const Feature* f = ctx.FindById(ref.bodyFeatureId);
    error = f == nullptr
                ? "the body it fits (feature " +
                      std::to_string(ref.bodyFeatureId) +
                      ") is gone or not before this datum"
                : "the body it fits, '" + f->GetName() + "' (feature " +
                      std::to_string(ref.bodyFeatureId) + "), made no body";
    return false;
  }
  Request r;
  r.pointMm = ref.pointMm;
  r.kind = ref.kind;
  r.toleranceMm = ref.toleranceMm;
  try {
    return Fit(bodies, r, out, error);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

} // namespace meshfit
