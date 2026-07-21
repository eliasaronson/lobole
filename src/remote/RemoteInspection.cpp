#include "remote/RemoteInspection.h"

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"
#include "remote/RemoteHelpers.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <glm/glm.hpp>

#include <string>
#include <vector>

using nlohmann::json;

namespace remote {

namespace {

// Resolve an optional `part` RPC param (slot name, or stable PartSlot id as a
// number or numeric string) to that part's document WITHOUT switching the
// active tab — kills the set_active_part round-trips for cross-part queries.
Document* ResolvePartParam(ui::AppState& state, const json& part) {
  ui::EnsureParts(state);
  uint64_t id = 0;
  // A numeric `part` is tried as a STABLE id first (what occurrences reference);
  // if no part carries that id it falls back to the slot INDEX from list_parts,
  // so passing the visible 0..N index no longer silently returns another part.
  std::optional<int> indexFallback;
  if (part.is_number_unsigned() || part.is_number_integer()) {
    id = part.get<uint64_t>();
    indexFallback = part.get<int>();
  } else if (part.is_string()) {
    const std::string s = part.get<std::string>();
    id = ui::PartIdByName(state, s);
    if (id == 0) { // allow a stable id / index passed as a string
      try {
        id = std::stoull(s);
        indexFallback = std::stoi(s);
      } catch (...) {
        id = 0;
      }
    }
  }
  Document* d = id != 0 ? ui::PartDocById(state, id) : nullptr;
  if (d == nullptr && indexFallback && *indexFallback >= 0 &&
      *indexFallback < static_cast<int>(state.parts.size())) {
    d = ui::PartDocById(state, state.parts[*indexFallback].id);
  }
  if (d == nullptr)
    throw std::runtime_error("no part with id or index " + part.dump() +
                             " — pass a part name, stable id or slot index "
                             "(see list_parts)");
  return d;
}

// The active part document, required by the part-tab-only inspection RPCs.
Document& RequirePartDoc(Document* doc, const char* what) {
  if (doc == nullptr)
    throw std::runtime_error(std::string(what) +
                             ": no active part document (on an assembly tab, "
                             "pass `part` or occurrence refs)");
  return *doc;
}

} // namespace

// Inspection RPCs (section clip, interference, mass properties, measure,
// element listing) carved out of RemoteServer::HandleSemantic. `docIn` is
// null on an assembly/drawing tab.
std::optional<json> HandleInspectionRpc(ui::AppState& state, Document* docIn,
                                        const std::string& m, const json& p) {
  Document* doc = docIn;
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
    const auto area = Measure::CrossSectionArea(
        RequirePartDoc(doc, "section").GetBodies(), origin, normal);
    if (!area) throw std::runtime_error("section failed");
    return json{{"area", *area}};
  }
  if (m == "interference") {
    // Clash detection (inspection I4). Part tab: two bodies interfere when
    // their intersection has volume — params: {a: index, b: index}. Active
    // ASSEMBLY tab: a/b are occurrence ids (numbers) or names (strings); each
    // occurrence's part bodies are placed by its solved occurrence transform
    // and the same check runs in world space. Headless-safe (pure geometry).
    if (doc == nullptr && state.assembly) {
      auto occOf = [&](const json& v) -> const Occurrence& {
        if (v.is_string()) {
          const std::string name = v.get<std::string>();
          for (const Occurrence& o : state.assembly->GetOccurrences())
            if (o.name == name) return o;
          throw std::runtime_error("occurrence '" + name +
                                   "' not found (see list_occurrences)");
        }
        const Occurrence* o = state.assembly->FindById(v.get<uint64_t>());
        if (o == nullptr)
          throw std::runtime_error("occurrence id " + v.dump() +
                                   " not found (see list_occurrences)");
        return *o;
      };
      const Occurrence& oa = occOf(p.at("a"));
      const Occurrence& ob = occOf(p.at("b"));
      if (oa.id == ob.id)
        throw std::runtime_error("pick two different occurrences");
      Document* pa = ui::PartDocById(state, oa.partId);
      Document* pb = ui::PartDocById(state, ob.partId);
      if (pa == nullptr || pb == nullptr)
        throw std::runtime_error("an occurrence's part document is missing");
      const auto v = Measure::InterferenceVolume(
          pa->GetBodies(), ui::OccurrenceMatrix(oa), pb->GetBodies(),
          ui::OccurrenceMatrix(ob));
      if (!v) throw std::runtime_error("interference check failed (see log)");
      return json{{"clash", *v > 1e-9},
                  {"volume", *v},
                  {"a", oa.name},
                  {"b", ob.name}};
    }
    const auto& bodies = RequirePartDoc(doc, "interference").GetBodies();
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
    //
    // On an ASSEMBLY tab there is no part doc: aggregate every visible,
    // non-suppressed occurrence's part mass in WORLD space (each part's CoM
    // carried through its solved occurrence transform — the same placement the
    // interference/BOM paths use). The reply's `bodies` array carries one row
    // per occurrence; `total` is the combined world mass/volume/CoM.
    if (doc == nullptr && state.assembly) {
      const assembly::PartResolver resolve = [&](uint64_t id) {
        return ui::PartDocById(state, id);
      };
      const assembly::AssemblyMass am =
          assembly::MassProperties(*state.assembly, resolve);
      json rows = json::array();
      for (const assembly::OccurrenceMass& om : am.occurrences) {
        json entry = {{"occurrence", om.occurrenceId},
                      {"name", om.name},
                      {"part_id", om.partId},
                      {"volume", om.volume},
                      {"center_of_mass",
                       {om.centerOfMass.x, om.centerOfMass.y,
                        om.centerOfMass.z}}};
        entry["mass_g"] = om.massKnown ? json(om.massG) : json(nullptr);
        rows.push_back(entry);
      }
      const glm::dvec3& com = am.centerOfMass;
      json out = {{"occurrences", rows},
                  {"total",
                   {{"volume", am.volume},
                    {"center_of_mass", {com.x, com.y, com.z}},
                    {"mass_g", am.massKnown ? json(am.massG) : json(nullptr)}}}};
      if (!am.skipped.empty()) out["skipped"] = am.skipped;
      return out;
    }
    const auto& bodies = RequirePartDoc(doc, "mass_properties").GetBodies();
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
    // {a: {feature_id, element}, b?: {feature_id, element}, part?}.
    // With `b`: minimum distance between the two elements. Without: the
    // element's own measure (face area / edge length). `part` (name or stable
    // id) resolves the elements against that part's bodies without switching
    // the active tab.
    auto refOf = [](const json& r) {
      return ElementRef{r.at("feature_id").get<uint64_t>(),
                        r.at("element").get<std::string>()};
    };
    const ElementRef a = refOf(p.at("a"));
    const auto& bodies =
        p.contains("part")
            ? ResolvePartParam(state, p.at("part"))->GetBodies()
            : RequirePartDoc(doc, "measure").GetBodies();
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
    // (feature_id = the body's created_by). Each face carries its surface
    // kind + area, each edge its curve kind + length (+ radius when
    // circular), so agents can identify bore rims / seams without
    // measure-probing every name. Document-only: works headless.
    // params: {body?: index, part?: name|stable-id (cross-part, no tab
    // switch)}
    const auto& bodies =
        p.contains("part")
            ? ResolvePartParam(state, p.at("part"))->GetBodies()
            : RequirePartDoc(doc, "list_elements").GetBodies();
    const int only = p.value("body", -1);
    if (only >= static_cast<int>(bodies.size()))
      throw std::runtime_error("body index out of range");
    json out = json::array();
    for (size_t i = 0; i < bodies.size(); i++) {
      if (only >= 0 && i != static_cast<size_t>(only)) continue;
      json faces = json::array(), edges = json::array();
      for (const auto& [idx, name] : bodies[i].elementMap.names) {
        json f = {{"index", idx}, {"name", name}};
        if (const auto fi = Measure::DescribeFaceAt(bodies[i], idx)) {
          f["kind"] = fi->kind;
          f["area"] = fi->area;
          // Centroid: lets an importer match a specific face geometrically
          // (Onshape shell/draft face queries -> our stable face names).
          f["centroid"] = {fi->centroid.x, fi->centroid.y, fi->centroid.z};
          // Outward unit surface normal at the centroid: lets an agent tell a
          // +X wall from a -X wall without a separate select_face round-trip.
          f["normal"] = {fi->normal.x, fi->normal.y, fi->normal.z};
        }
        faces.push_back(f);
      }
      for (const auto& [idx, name] : bodies[i].elementMap.edgeNames) {
        json e = {{"index", idx}, {"name", name}};
        if (const auto ei = Measure::DescribeEdgeAt(bodies[i], idx)) {
          e["kind"] = ei->kind;
          e["length"] = ei->length;
          if (ei->hasRadius) e["radius"] = ei->radius;
          // Midpoint: lets an importer match a specific edge geometrically
          // (Onshape fillet/chamfer edge queries → our stable edge names).
          e["midpoint"] = {ei->midpoint.x, ei->midpoint.y, ei->midpoint.z};
        }
        edges.push_back(e);
      }
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
