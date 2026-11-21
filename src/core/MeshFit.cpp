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

const double kCreaseCos = std::cos(2.0 * M_PI / mesh::kMinSidesLowest);

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

std::string AcceptedClipSigma() { return "1, 2, 3 or \"all\""; }

bool ClipSigmaFromJson(const nlohmann::json& j, int& out) {
  if (j.is_string() && j.get<std::string>() == "all") {
    out = 0;
    return true;
  }
  if (!j.is_number())
    return false;
  const double v = j.get<double>();
  for (int k = 1; k <= 3; k++)
    if (v == double(k)) {
      out = k;
      return true;
    }
  return false;
}

nlohmann::json ClipSigmaJson(int clipSigma) {
  return clipSigma == 0 ? nlohmann::json("all") : nlohmann::json(clipSigma);
}

const std::vector<std::string>& HintKeys() {
  static const std::vector<std::string> keys{"side", "radius_mm", "axis",
                                             "axis_tolerance_deg", "within"};
  return keys;
}

namespace {

glm::dvec3 Vec3Of(const nlohmann::json& v, const std::string& what) {
  if (!v.is_array() || v.size() != 3 || !v[0].is_number() ||
      !v[1].is_number() || !v[2].is_number())
    throw std::runtime_error(what + " is [x, y, z], not " + v.dump());
  const glm::dvec3 p(v[0].get<double>(), v[1].get<double>(),
                     v[2].get<double>());
  if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
    throw std::runtime_error(what + " is three finite numbers");
  return p;
}

} // namespace

mesh::FitHints HintsFromJson(const nlohmann::json& j, const std::string& what) {
  mesh::FitHints h;
  const std::string at = what.empty() ? std::string() : what + ": ";
  if (j.contains("side")) {
    const nlohmann::json& s = j.at("side");
    const std::string v = s.is_string() ? s.get<std::string>() : s.dump();
    if (v == "inside")
      h.outside = false;
    else if (v == "outside")
      h.outside = true;
    else
      throw std::runtime_error(at + "side: unknown " + s.dump() +
                               ". Accepted: inside (a bore, a socket), "
                               "outside (a boss, a ball)");
  }
  if (j.contains("radius_mm")) {
    const nlohmann::json& r = j.at("radius_mm");
    if (!r.is_array() || r.size() != 2 || !r[0].is_number() ||
        !r[1].is_number() || !std::isfinite(r[0].get<double>()) ||
        !std::isfinite(r[1].get<double>()) || r[0].get<double>() < 0.0 ||
        !(r[1].get<double>() > 0.0) || r[0].get<double>() > r[1].get<double>())
      throw std::runtime_error(at +
                               "radius_mm is [min, max] in mm, 0 <= "
                               "min <= max and max > 0, not " +
                               r.dump());
    h.radiusMin = r[0].get<double>();
    h.radiusMax = r[1].get<double>();
  }
  if (j.contains("axis") != j.contains("axis_tolerance_deg"))
    throw std::runtime_error(
        at + "axis and axis_tolerance_deg go together: the direction, and "
             "how far in degrees the fitted axis (a plane's normal) may lean "
             "from it");
  if (j.contains("axis")) {
    const glm::dvec3 a = Vec3Of(j.at("axis"), at + "axis");
    if (!(glm::length(a) > 0.0))
      throw std::runtime_error(at + "axis is a direction, not [0, 0, 0]");
    h.axis = glm::normalize(a);
    const nlohmann::json& t = j.at("axis_tolerance_deg");
    if (!t.is_number() || !(t.get<double>() > 0.0) ||
        !(t.get<double>() <= 90.0))
      throw std::runtime_error(at +
                               "axis_tolerance_deg is a number of "
                               "degrees above 0 and at most 90, not " +
                               t.dump());
    h.axisToleranceRad = angles::ToRadians(t.get<double>());
  }
  if (j.contains("within")) {
    const nlohmann::json& w = j.at("within");
    if (!w.is_object() || !w.contains("min_mm") || !w.contains("max_mm"))
      throw std::runtime_error(at +
                               "within is {min_mm: [x, y, z], max_mm: "
                               "[x, y, z]}, a box, not " +
                               w.dump());
    for (const auto& [k, v] : w.items())
      if (k != "min_mm" && k != "max_mm")
        throw std::runtime_error(at + "within: unknown key '" + k +
                                 "'. Accepted: min_mm, max_mm");
    const glm::dvec3 lo = Vec3Of(w.at("min_mm"), at + "within.min_mm");
    const glm::dvec3 hi = Vec3Of(w.at("max_mm"), at + "within.max_mm");
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z))
      throw std::runtime_error(at + "within: min_mm is not below max_mm on "
                                    "every axis");
    h.withinLo = lo;
    h.withinHi = hi;
  }
  return h;
}

void HintsToJson(const mesh::FitHints& h, nlohmann::json& j) {
  if (h.outside)
    j["side"] = *h.outside ? "outside" : "inside";
  if (h.radiusMin || h.radiusMax)
    j["radius_mm"] = {h.radiusMin.value_or(0.0), h.radiusMax.value_or(0.0)};
  if (h.axis) {
    j["axis"] = {h.axis->x, h.axis->y, h.axis->z};
    j["axis_tolerance_deg"] = angles::ToDegrees(h.axisToleranceRad);
  }
  if (h.withinLo && h.withinHi) {
    j["within"] = {{"min_mm", {h.withinLo->x, h.withinLo->y, h.withinLo->z}},
                   {"max_mm", {h.withinHi->x, h.withinHi->y, h.withinHi->z}}};
  }
}

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
  if (r.clipSigma < 0 || r.clipSigma > 3)
    throw std::runtime_error("clip_sigma is " + AcceptedClipSigma());
  if (r.kind)
    if (const char* h = r.hints.NotFor(*r.kind))
      throw std::runtime_error(
          std::string(h) + " does not apply to a " + mesh::KindName(*r.kind) +
          (std::string(h) == "side"
               ? ": side is for a curved kind (cylinder, cone, sphere); a "
                 "plane faces the way its triangles do"
           : std::string(h) == "radius_mm"
               ? ": radius_mm is for a cylinder or a sphere"
               : ": axis is for a plane (its normal), a cylinder or a cone"));
  if (!r.hints.Inside(r.pointMm))
    throw std::runtime_error("point_mm " + Point(r.pointMm) +
                             " is outside within: the region grows only "
                             "inside the box, from the point");
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

  out.clipSigma = r.clipSigma;
  std::vector<mesh::Kind> kinds;
  if (r.kind)
    kinds.push_back(*r.kind);
  else
    for (mesh::Kind k : {mesh::Kind::Plane, mesh::Kind::Sphere,
                         mesh::Kind::Cylinder, mesh::Kind::Cone})
      if (r.hints.NotFor(k) == nullptr) // auto tries what the hints allow
        kinds.push_back(k);
  // The seeds: the triangle at the point, and with hints every triangle
  // within the pick's reach of it, nearest first -- near a rim the nearest
  // surface is often not the one the hints describe.
  std::vector<uint32_t> seeds{hit.triangle};
  if (r.hints.Any()) {
    const double reach = diag / 50.0;
    std::vector<std::pair<double, uint32_t>> near;
    std::vector<uint8_t> seen(m.triangles.size(), 0);
    std::vector<uint32_t> queue{hit.triangle};
    seen[hit.triangle] = 1;
    for (size_t i = 0; i < queue.size(); i++) {
      const uint32_t t = queue[i];
      const double dist = glm::length(
          mesh::Bvh::ClosestOnTriangle(r.pointMm, m.Corner(t, 0),
                                       m.Corner(t, 1), m.Corner(t, 2)) -
          r.pointMm);
      if (dist > reach)
        continue;
      if (t != hit.triangle)
        near.emplace_back(dist, t);
      for (int e = 0; e < 3; e++) {
        const uint32_t u = m.neighbour[t][e];
        if (u != mesh::kNone && !seen[u]) {
          seen[u] = 1;
          queue.push_back(u);
        }
      }
    }
    std::sort(near.begin(), near.end());
    for (const auto& [dist, t] : near)
      seeds.push_back(t);
  }
  mesh::GrowOptions options;
  options.clipSigma = r.clipSigma;
  options.hints = r.hints;
  std::vector<mesh::Region> grown;
  std::vector<size_t> seedsTried;
  for (mesh::Kind k : kinds) {
    std::string why;
    mesh::Region g;
    std::vector<uint8_t> covered(seeds.size() > 1 ? m.triangles.size() : 0, 0);
    size_t attempts = 0;
    std::vector<glm::dvec3> triedFacing;
    for (uint32_t seed : seeds) {
      if (seeds.size() > 1 && covered[seed])
        continue;
      // One seed a surface: a triangle turned less than a crease (the
      // fewest-sided polygon's turn, mesh::kMinSidesLowest) from one
      // already tried faces the way that surface did.
      const glm::dvec3 an = m.AreaNormal(seed);
      const glm::dvec3 facing =
          glm::length(an) > 0.0 ? glm::normalize(an) : glm::dvec3(0.0);
      bool same = false;
      for (const glm::dvec3& f : triedFacing)
        same = same || glm::dot(f, facing) >= kCreaseCos;
      if (same)
        continue;
      if (attempts == kMostSeeds)
        break;
      attempts++;
      std::string thisWhy;
      std::vector<uint32_t> reached;
      g = mesh::GrowFrom(m, seed, k, out.toleranceMm, &thisWhy, options,
                         &reached);
      if (g.fit.ok)
        break;
      if (why.empty())
        why = thisWhy; // the nearest surface's reason
      if (seeds.size() == 1)
        break;
      triedFacing.push_back(facing);
      covered[seed] = 1;
      for (uint32_t t : reached)
        covered[t] = 1;
    }
    if (!g.fit.ok && attempts > 1)
      why += " (and on the " + std::to_string(attempts - 1) +
             (attempts == 2 ? " other surface" : " other surfaces") +
             " within " + Mm(diag / 50.0) + " mm of the point, none meets " +
             r.hints.Describe() + ")";
    seedsTried.push_back(attempts);
    Candidate c;
    c.kind = k;
    c.ok = g.fit.ok;
    c.areaMm2 = g.area;
    c.rmsMm = g.fit.rms;
    c.why = why;
    out.tried.push_back(c);
    grown.push_back(std::move(g));
  }
  if (kinds.empty()) {
    error = "no kind takes every hint given (" + r.hints.Describe() + ")";
    return false;
  }
  if (r.kind) {
    if (!grown[0].fit.ok) {
      error = out.tried[0].why + " (at point_mm " + Point(r.pointMm) + " on '" +
              out.body + "', tolerance_mm " + Mm(out.toleranceMm) + ")";
      return false;
    }
    out.region = std::move(grown[0]);
    out.seedsTried = seedsTried[0];
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
  out.seedsTried = seedsTried[size_t(pick)];
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
  const mesh::Region& g = r.region;
  j["sigma_mm"] = g.sigma;
  j["form_mm"] = g.residualHi - g.residualLo;
  j["used"] = g.used;
  j["clip_sigma"] = ClipSigmaJson(r.clipSigma);
  nlohmann::json warnings = nlohmann::json::array();
  if (f.kind == mesh::Kind::Cylinder || f.kind == mesh::Kind::Sphere) {
    j["diameter_min_mm"] = 2.0 * (f.radius + g.residualLo);
    j["diameter_max_mm"] = 2.0 * (f.radius + g.residualHi);
    const double arc = angles::ToDegrees(g.arc);
    j["arc_deg"] = arc;
    if (arc < kPartialArcDeg)
      warnings.push_back(
          "the fit covers " + Mm(arc) + " deg of arc, under " +
          Mm(kPartialArcDeg) +
          ": a radius from a partial arc is the least "
          "certain size there is -- fit more of the surface, or give "
          "radius_mm");
  }
  if (f.kind == mesh::Kind::Cylinder)
    j["length_mm"] = g.length;
  if (f.kind == mesh::Kind::Plane)
    j["extent_mm"] = {g.extent.x, g.extent.y};
  if (!warnings.empty())
    j["warnings"] = std::move(warnings);
  if (r.seedsTried > 1)
    j["seeds_tried"] = r.seedsTried;
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
  r.clipSigma = ref.clipSigma.value_or(kDefaultClipSigma);
  r.hints = ref.hints;
  try {
    return Fit(bodies, r, out, error);
  } catch (const std::exception& e) {
    error = e.what();
    return false;
  }
}

} // namespace meshfit
