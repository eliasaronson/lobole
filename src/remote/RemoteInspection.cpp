#include "remote/RemoteInspection.h"

#include "core/Document.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

// Inspection RPCs (section clip, interference, mass properties, measure,
// element listing) carved out of RemoteServer::HandleSemantic.
std::optional<json> HandleInspectionRpc(ui::AppState& state, Document& document,
                                        const std::string& m, const json& p) {
  Document* doc = &document;
  if (m == "set_section") {
    // Toggle/position the viewport section clip plane (I3 raster). params:
    // {enabled, base_plane: "xy"|"xz"|"yz", offset?} or {enabled, origin,
    // normal}. Disabled clears it.
    if (!p.value("enabled", true)) {
      state.sectionPlane = glm::vec4(0.0f);
      return json::object();
    }
    glm::vec3 nrm(0, 0, 1), origin(0.0f);
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      nrm = bp == "xz" ? glm::vec3(0, 1, 0)
            : bp == "yz" ? glm::vec3(1, 0, 0)
                         : glm::vec3(0, 0, 1);
      origin = nrm * static_cast<float>(p.value("offset", 0.0));
    } else {
      const auto& o = p.at("origin");
      const auto& n = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      nrm = glm::normalize(glm::vec3(n[0].get<float>(), n[1].get<float>(),
                                     n[2].get<float>()));
    }
    state.sectionPlane = glm::vec4(nrm, glm::dot(origin, nrm));
    return json::object();
  }
  if (m == "section") {
    // Cross-section area where a plane cuts through all bodies (I3). Plane:
    // {base_plane: "xy"|"xz"|"yz", offset?: 0} OR {origin: [x,y,z],
    // normal: [x,y,z]}.
    glm::vec3 origin(0.0f), normal(0.0f, 0.0f, 1.0f);
    if (p.contains("base_plane")) {
      const std::string bp = p.at("base_plane").get<std::string>();
      normal = bp == "xz" ? glm::vec3(0, 1, 0)
               : bp == "yz" ? glm::vec3(1, 0, 0)
                            : glm::vec3(0, 0, 1);
      origin = normal * static_cast<float>(p.value("offset", 0.0));
    } else {
      const auto& o = p.at("origin");
      const auto& nrm = p.at("normal");
      origin = {o[0].get<float>(), o[1].get<float>(), o[2].get<float>()};
      normal = {nrm[0].get<float>(), nrm[1].get<float>(),
                nrm[2].get<float>()};
    }
    const auto area = Measure::CrossSectionArea(doc->GetBodies(), origin,
                                                normal);
    if (!area) throw std::runtime_error("section failed");
    return json{{"area", *area}};
  }
  if (m == "interference") {
    // Clash detection (inspection I4): two bodies interfere when their
    // intersection has volume. params: {a: index, b: index}
    const auto& bodies = doc->GetBodies();
    const int ia = p.at("a").get<int>();
    const int ib = p.at("b").get<int>();
    if (ia < 0 || ib < 0 || ia >= static_cast<int>(bodies.size()) ||
        ib >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    CADShape common = bodies[ia].shape.Intersection(bodies[ib].shape);
    const double v = common.IsValid() ? common.Volume() : 0.0;
    return json{{"clash", v > 1e-9}, {"volume", v}};
  }
  if (m == "mass_properties") {
    // Volume, surface area, center of mass, and inertia (about the CoM) per
    // body, plus a document total. Mass assumes model units are millimetres:
    // grams = density[kg/m^3] * volume[mm^3] * 1e-6; bodies without an
    // assigned material report mass null (density unknown), and so does the
    // total if any contributor is unknown. params: {body?: index}
    const auto& bodies = doc->GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    // The math is shared with the BOM (A4): Measure::MassOfBodies. A single
    // body's Body copy is cheap (the shape is a refcounted OCC handle).
    std::vector<Body> subset;
    if (only >= 0) subset.push_back(bodies[only]);
    std::string err;
    const auto mass = Measure::MassOfBodies(only >= 0 ? subset : bodies, &err);
    if (!mass) throw std::runtime_error(err);
    if (mass->bodies.empty()) throw std::runtime_error("no bodies");
    json out = json::array();
    for (size_t i = 0; i < mass->bodies.size(); i++) {
      const Measure::BodyMass& bm = mass->bodies[i];
      json inertia = json::array();
      for (int r = 0; r < 3; r++)
        inertia.push_back({bm.inertia[r][0], bm.inertia[r][1],
                           bm.inertia[r][2]});
      json entry = {{"body", only >= 0 ? static_cast<size_t>(only) : i},
                    {"name", bm.name},
                    {"volume", bm.volume},
                    {"surface_area", bm.surfaceArea},
                    {"center_of_mass",
                     {bm.centerOfMass.x, bm.centerOfMass.y, bm.centerOfMass.z}},
                    {"inertia", inertia},
                    {"material", bm.material}};
      if (bm.massKnown) {
        entry["density"] = bm.density;
        entry["mass_g"] = bm.massG;
      } else {
        entry["mass_g"] = nullptr;
      }
      out.push_back(entry);
    }
    const glm::dvec3& com = mass->centerOfMass;
    return json{{"bodies", out},
            {"total",
             {{"volume", mass->volume},
              {"center_of_mass", {com.x, com.y, com.z}},
              {"mass_g", mass->massKnown ? json(mass->massG) : json(nullptr)}}}};
  }
  if (m == "measure") {
    // Geometry queries over stable element names (I1). params:
    // {a: {feature_id, element}, b?: {feature_id, element}}.
    // With `b`: minimum distance between the two elements. Without: the
    // element's own measure (face area / edge length).
    auto refOf = [](const json& r) {
      return ElementRef{r.at("feature_id").get<uint64_t>(),
                        r.at("element").get<std::string>()};
    };
    const ElementRef a = refOf(p.at("a"));
    const auto& bodies = doc->GetBodies();
    if (p.contains("b")) {
      const auto d = Measure::Distance(bodies, a, refOf(p.at("b")));
      if (!d) throw std::runtime_error("measure: an element did not resolve");
      return json{{"distance", *d}};
    }
    const auto e = Measure::Describe(bodies, a);
    if (!e) throw std::runtime_error("measure: element did not resolve");
    json out = {{"kind", e->kind}};
    if (e->kind == "face") {
      out["area"] = e->area;
    } else {
      out["length"] = e->length;
      if (e->hasRadius) out["radius"] = e->radius; // circular/arc edges
    }
    return out;
  }
  if (m == "list_elements") {
    // Stable element names (topological naming) for each body's faces and
    // edges — what Fillet/Chamfer edge refs {feature_id, element} point at
    // (feature_id = the body's created_by). Document-only: works headless.
    // params: {body?: index}
    const auto& bodies = doc->GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    json out = json::array();
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only)) continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names)
        faces.push_back({{"index", idx}, {"name", name}});
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames)
        edges.push_back({{"index", idx}, {"name", name}});
      out.push_back({{"body", i},
                     {"name", bodies[i].name},
                     {"created_by", bodies[i].createdBy},
                     {"faces", faces},
                     {"edges", edges}});
    }
    return json{{"bodies", out}};
  }
  return std::nullopt;
}

}  // namespace remote
