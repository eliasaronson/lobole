// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/PartMeasures.h"

#include "core/Measure.h"

#include <stdexcept>

namespace partmeasure {

namespace {

std::string Known(const std::vector<std::string>& known, size_t cap) {
  if (known.empty())
    return "none exist yet";
  std::string out;
  const size_t shown = std::min(known.size(), cap);
  for (size_t i = 0; i < shown; i++)
    out += (i ? ", " : "") + known[i];
  if (known.size() > shown)
    out += ", +" + std::to_string(known.size() - shown) + " more";
  return out;
}

} // namespace

std::set<uint64_t> ResolveBodies(const Document& doc,
                                 const nlohmann::json& list, const char* what) {
  if (!list.is_array() || list.empty())
    throw std::runtime_error(std::string("`") + what +
                             "` is a non-empty list of body names or the "
                             "feature ids that created them");
  const auto known = [&] {
    std::vector<std::string> v;
    for (const Body& b : doc.GetBodies())
      v.push_back("'" + b.name + "' (" + std::to_string(b.createdBy) + ")");
    return Known(v, 40);
  };
  std::set<uint64_t> out;
  for (const nlohmann::json& e : list) {
    bool hit = false;
    if (e.is_number_unsigned()) {
      const uint64_t id = e.get<uint64_t>();
      for (const Body& b : doc.GetBodies())
        if (b.createdBy == id) {
          out.insert(id);
          hit = true;
        }
      if (!hit)
        throw std::runtime_error(std::string(what) +
                                 ": no body was created "
                                 "by feature " +
                                 std::to_string(id) + "; bodies: " + known());
    } else if (e.is_string()) {
      const std::string name = e.get<std::string>();
      for (const Body& b : doc.GetBodies())
        if (b.name == name) {
          out.insert(b.createdBy);
          hit = true;
        }
      if (!hit)
        throw std::runtime_error(std::string(what) + ": no body '" + name +
                                 "'; bodies: " + known());
    } else {
      throw std::runtime_error(std::string(what) +
                               " entries are body names or feature ids, not " +
                               e.dump());
    }
  }
  return out;
}

Measure::DocumentMass MassOf(const std::vector<Body>& bodies) {
  std::string err;
  const auto mass = Measure::MassOfBodies(bodies, &err);
  if (!mass)
    throw std::runtime_error(err);
  if (mass->bodies.empty())
    throw std::runtime_error(
        bodies.empty() ? "no bodies"
                       : "only reference bodies (derived ghosts), which "
                         "weigh nothing here; set reference: false on the "
                         "Derived feature to count one");
  return *mass;
}

Section SectionOf(const std::vector<Body>& bodies, const glm::dvec3& origin,
                  const glm::dvec3& normal, bool enclosed) {
  const auto area = Measure::CrossSectionArea(bodies, origin, normal);
  if (!area)
    throw std::runtime_error(
        "section failed — the plane may cut no body at all; check the "
        "bodies' bbox_min/bbox_max in get_state against the plane");
  Section s;
  s.area = *area;
  if (enclosed) {
    const auto inside = Measure::SectionEnclosedArea(bodies, origin, normal);
    if (!inside)
      throw std::runtime_error("enclosed area failed -- see the log");
    s.enclosed = *inside;
    // Zero with material cut is the ring not closing HERE, which is worth
    // saying: a wall with a door or a window in it at this height is
    // open, and a plane above the openings encloses the room.
    if (*inside <= 0.0 && *area > 0.0)
      s.enclosedNote =
          "the cut material rings nothing at this plane: an opening (a "
          "door, a window, a gap between bodies) breaks the ring. Cut "
          "where it is whole, e.g. above the openings";
  }
  return s;
}

SectionReading SectionAreaMeasure(const Document& doc,
                                  const nlohmann::json& m) {
  glm::dvec3 origin(0.0), normal(0.0, 0.0, 1.0);
  if (m.contains("base_plane")) {
    const std::string bp = m.at("base_plane").get<std::string>();
    normal = bp == "xz"   ? glm::dvec3(0, 1, 0)
             : bp == "yz" ? glm::dvec3(1, 0, 0)
                          : glm::dvec3(0, 0, 1);
    origin = normal * m.at("offset").get<double>();
  } else {
    const auto& o = m.at("origin");
    const auto& n = m.at("normal");
    origin = {o[0].get<double>(), o[1].get<double>(), o[2].get<double>()};
    normal = {n[0].get<double>(), n[1].get<double>(), n[2].get<double>()};
  }
  std::vector<Body> chosen;
  const std::vector<Body>* bodies = &doc.GetBodies();
  if (m.contains("bodies")) {
    const std::set<uint64_t> ids = ResolveBodies(doc, m.at("bodies"), "bodies");
    for (const Body& b : doc.GetBodies())
      if (ids.count(b.createdBy) != 0)
        chosen.push_back(b);
    bodies = &chosen;
  }
  const bool enclosed = m.at("of").get<std::string>() == "enclosed";
  const Section s = SectionOf(*bodies, origin, normal, enclosed);
  SectionReading r;
  r.value = enclosed ? *s.enclosed : s.area;
  if (enclosed)
    r.detail["material_area"] = s.area;
  if (!s.enclosedNote.empty())
    r.detail["note"] = s.enclosedNote;
  return r;
}

} // namespace partmeasure
