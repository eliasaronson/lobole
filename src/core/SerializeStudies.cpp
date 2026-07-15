#include "core/Serialization.h"
#include "core/SerializationInternal.h"

#include "core/Document.h"
#include "core/FemStudy.h"

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace DocumentIO {

// ---- FEM studies (v29) ----------------------------------------------------
// A study is document-level configuration, not geometry; mesh and results are
// derived and never appear here. Face refs use the {feature_id, element}
// shape shared with feature files and the RPCs. Expression fields follow the
// value+expr convention: the numeric field is always written, the expression
// only when non-empty (a plain literal round-trips byte-identically).


json StudyToJson(const fem::Study& s) {
  json j;
  j["type"] = "Study";
  j["id"] = s.id;
  j["name"] = s.name;
  // Analysis type / modal / gravity (v32) — written only when non-default so
  // v31 static studies stay byte-identical.
  if (s.type != fem::AnalysisType::Static)
    j["analysis"] = fem::AnalysisTypeToString(s.type);
  if (s.type == fem::AnalysisType::Modal) j["num_modes"] = s.numModes;
  if (s.gravity.x != 0 || s.gravity.y != 0 || s.gravity.z != 0)
    j["gravity"] = {s.gravity.x, s.gravity.y, s.gravity.z};
  j["mesh_size"] = s.meshSize;
  if (!s.meshSizeExpr.empty()) j["mesh_size_expr"] = s.meshSizeExpr;

  if (!s.refinements.empty()) {
    j["refinements"] = json::array();
    for (const fem::Refinement& r : s.refinements) {
      json rj = {{"face", ElementRefJson(r.face)}, {"size", r.size}};
      if (!r.sizeExpr.empty()) rj["size_expr"] = r.sizeExpr;
      j["refinements"].push_back(std::move(rj));
    }
  }

  if (!s.loads.empty()) {
    j["loads"] = json::array();
    for (const fem::Load& l : s.loads) {
      json lj = {{"id", l.id},
                 {"type", fem::LoadTypeToString(l.type)},
                 {"face", ElementRefJson(l.face)},
                 {"magnitude", l.magnitude}};
      if (!l.magnitudeExpr.empty()) lj["magnitude_expr"] = l.magnitudeExpr;
      if (l.type == fem::LoadType::Force)
        lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
      j["loads"].push_back(std::move(lj));
    }
  }
  return j;
}

fem::Study StudyFromJson(const json& j) {
  fem::Study s;
  s.id = j.at("id").get<uint64_t>();
  s.name = j.value("name", std::string{});
  fem::AnalysisTypeFromString(j.value("analysis", std::string{"static"}), s.type);
  s.numModes = j.value("num_modes", 6);
  if (j.contains("gravity")) {
    const auto& g = j.at("gravity");
    s.gravity = glm::dvec3(g.at(0).get<double>(), g.at(1).get<double>(),
                           g.at(2).get<double>());
  }
  s.meshSize = j.value("mesh_size", 0.0);
  s.meshSizeExpr = j.value("mesh_size_expr", std::string{});

  for (const json& rj : j.value("refinements", json::array())) {
    fem::Refinement r;
    r.face = ElementRefFromJson(rj.at("face"));
    r.size = rj.value("size", 0.0);
    r.sizeExpr = rj.value("size_expr", std::string{});
    s.refinements.push_back(std::move(r));
  }

  for (const json& lj : j.value("loads", json::array())) {
    fem::Load l;
    l.id = lj.at("id").get<uint64_t>();
    fem::LoadTypeFromString(lj.value("type", std::string{"fixed"}), l.type);
    l.face = ElementRefFromJson(lj.at("face"));
    l.magnitude = lj.value("magnitude", 0.0);
    l.magnitudeExpr = lj.value("magnitude_expr", std::string{});
    if (lj.contains("direction")) {
      const auto& d = lj.at("direction");
      l.direction = glm::dvec3(d.at(0).get<double>(), d.at(1).get<double>(),
                               d.at(2).get<double>());
    }
    s.loads.push_back(std::move(l));
  }
  return s;
}

}  // namespace DocumentIO
