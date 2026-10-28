// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/RecognitionJson.h"

#include "core/Angles.h"
#include "core/Feature.h"

#include <cmath>

namespace interchange {

namespace {

nlohmann::json Vec(const glm::dvec3& v) {
  return nlohmann::json::array({v.x, v.y, v.z});
}

} // namespace

nlohmann::json RecognitionJson(const ImportFeature& imp, bool lists) {
  using nlohmann::json;
  if (!ImportFeature::IsMesh(imp.path))
    return nullptr;
  json out = {{"representation", mesh::ToString(imp.representation)}};
  if (imp.representation != mesh::Representation::Recognize)
    return out;
  out["min_sides"] = imp.minSides;
  json bodies = json::array();
  for (const ImportFeature::Recognized& b : imp.RecognizedBodies()) {
    const mesh::Recognition& r = b.recognition;
    json row = {{"body", b.body},
                {"summary", r.Summary()},
                {"triangles", r.triangles},
                {"planes", r.planes},
                {"facets_left", r.facetsLeft},
                {"tolerance_mm", r.toleranceMm}};
    if (r.demoted > 0)
      row["demoted"] = r.demoted;
    if (!r.fellBack.empty())
      row["fell_back"] = r.fellBack;
    if (!lists) {
      row["cylinders"] = r.Count(mesh::Kind::Cylinder);
      row["cones"] = r.Count(mesh::Kind::Cone);
      row["spheres"] = r.Count(mesh::Kind::Sphere);
      row["polygonal"] = r.polygonal.size();
      bodies.push_back(std::move(row));
      continue;
    }
    json cylinders = json::array(), cones = json::array(),
         spheres = json::array(), polygons = json::array();
    for (const mesh::RecognizedRound& c : r.rounds) {
      json one = {{"radius_mm", c.radius},
                  {"centre_mm", Vec(c.centre)},
                  {"sides", c.sides},
                  {"sagitta_mm", c.sagitta}};
      if (c.kind != mesh::Kind::Sphere)
        one["axis"] = Vec(c.axis);
      if (c.kind == mesh::Kind::Cone)
        one["half_angle_deg"] = angles::ToDegrees(c.halfAngle);
      (c.kind == mesh::Kind::Cylinder ? cylinders
       : c.kind == mesh::Kind::Cone   ? cones
                                      : spheres)
          .push_back(std::move(one));
    }
    for (const mesh::Polygon& p : r.polygonal)
      polygons.push_back({{"sides", p.sides},
                          {"circumradius_mm", p.circumradius},
                          {"centre_mm", Vec(p.centre)},
                          {"axis", Vec(p.axis)}});
    row["cylinders"] = std::move(cylinders);
    row["cones"] = std::move(cones);
    row["spheres"] = std::move(spheres);
    row["polygonal"] = std::move(polygons);
    bodies.push_back(std::move(row));
  }
  out["bodies"] = std::move(bodies);
  return out;
}

} // namespace interchange
