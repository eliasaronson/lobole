#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/Document.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <nlohmann/json.hpp>

#include <array>
#include <vector>

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
  if (s.type == fem::AnalysisType::Modal)
    j["num_modes"] = s.numModes;
  if (s.gravity.x != 0 || s.gravity.y != 0 || s.gravity.z != 0)
    j["gravity"] = {s.gravity.x, s.gravity.y, s.gravity.z};
  // Topology parameters (v33) — written only for a topology study so lower
  // versions stay byte-identical.
  if (s.type == fem::AnalysisType::Topology) {
    j["volume_fraction"] = s.volumeFraction;
    if (!s.volumeFractionExpr.empty())
      j["volume_fraction_expr"] = s.volumeFractionExpr;
    j["filter_radius"] = s.filterRadius;
    if (!s.filterRadiusExpr.empty())
      j["filter_radius_expr"] = s.filterRadiusExpr;
    j["max_iterations"] = s.maxIterations;
    if (!s.keepFaces.empty()) {
      j["keep_faces"] = json::array();
      for (const ElementRef& f : s.keepFaces)
        j["keep_faces"].push_back(ElementRefJson(f));
    }
  }
  j["mesh_size"] = s.meshSize;
  if (!s.meshSizeExpr.empty())
    j["mesh_size_expr"] = s.meshSizeExpr;

  if (!s.refinements.empty()) {
    j["refinements"] = json::array();
    for (const fem::Refinement& r : s.refinements) {
      json rj = {{"face", ElementRefJson(r.face)}, {"size", r.size}};
      if (!r.sizeExpr.empty())
        rj["size_expr"] = r.sizeExpr;
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
      if (!l.magnitudeExpr.empty())
        lj["magnitude_expr"] = l.magnitudeExpr;
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
  // A study whose analysis type did not parse used to load as STATIC and run
  // happily — the wrong physics, reported with the same confidence as the
  // right physics. Same for a load below: a force silently becoming a fixed
  // constraint changes the answer completely and looks legitimate.
  const std::string analysisStr = j.value("analysis", std::string{"static"});
  if (!fem::AnalysisTypeFromString(analysisStr, s.type))
    throw std::runtime_error("unknown study analysis type '" + analysisStr +
                             "' (expected one of: static, modal, topology)");
  s.numModes = j.value("num_modes", 6);
  if (j.contains("gravity")) {
    const auto& g = j.at("gravity");
    s.gravity = glm::dvec3(g.at(0).get<double>(), g.at(1).get<double>(),
                           g.at(2).get<double>());
  }
  s.volumeFraction = j.value("volume_fraction", 0.3);
  s.volumeFractionExpr = j.value("volume_fraction_expr", std::string{});
  s.filterRadius = j.value("filter_radius", 0.0);
  s.filterRadiusExpr = j.value("filter_radius_expr", std::string{});
  s.maxIterations = j.value("max_iterations", 60);
  for (const json& fj : j.value("keep_faces", json::array()))
    s.keepFaces.push_back(ElementRefFromJson(fj));
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
    const std::string loadStr = lj.value("type", std::string{"fixed"});
    if (!fem::LoadTypeFromString(loadStr, l.type))
      throw std::runtime_error("unknown load type '" + loadStr +
                               "' (expected one of: fixed, force, pressure)");
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

// ---- Topology result (v33) ------------------------------------------------
// Persisted (unlike FEM results): the optimized density + its mesh + summary.
// Flat numeric arrays keep it compact; solvedAtVersion is stamped on load.

bool StudyResultToJson(const fem::StudyRuntime& rt, json& out) {
  if (!rt.topology)
    return false;
  out = json::object();
  out["topology"] = {{"iterations", rt.iterations},
                     {"converged", rt.converged},
                     {"cancelled", rt.cancelled},
                     {"volume_fraction", rt.volumeFractionAchieved},
                     {"compliance", rt.complianceHistory}};
  json bodies = json::array();
  for (const fem::StudyBodyResult& b : rt.bodies) {
    json nodes = json::array();
    for (const glm::dvec3& n : b.mesh.nodes) {
      nodes.push_back(n.x);
      nodes.push_back(n.y);
      nodes.push_back(n.z);
    }
    json tets = json::array();
    for (const std::array<int, 10>& t : b.mesh.tets)
      for (int v : t)
        tets.push_back(v);
    json boundary = json::array();
    for (const fem::FemMesh::BoundaryTri& tr : b.mesh.boundary) {
      for (int v : tr.nodes)
        boundary.push_back(v);
      boundary.push_back(tr.faceIndex);
    }
    bodies.push_back({{"body_index", b.bodyIndex},
                      {"body", b.bodyName},
                      {"nodes", std::move(nodes)},
                      {"tets", std::move(tets)},
                      {"boundary", std::move(boundary)},
                      {"density", b.density}});
  }
  out["bodies"] = std::move(bodies);
  return true;
}

fem::StudyRuntime StudyResultFromJson(const json& j) {
  fem::StudyRuntime rt;
  rt.topology = true;
  const json& s = j.at("topology");
  rt.iterations = s.value("iterations", 0);
  rt.converged = s.value("converged", false);
  rt.cancelled = s.value("cancelled", false);
  rt.volumeFractionAchieved = s.value("volume_fraction", 0.0);
  rt.complianceHistory = s.value("compliance", std::vector<double>{});
  for (const json& bj : j.value("bodies", json::array())) {
    fem::StudyBodyResult b;
    b.bodyIndex = bj.value("body_index", -1);
    b.bodyName = bj.value("body", std::string{});
    const auto nodes = bj.value("nodes", std::vector<double>{});
    for (size_t i = 0; i + 3 <= nodes.size(); i += 3)
      b.mesh.nodes.push_back(glm::dvec3(nodes[i], nodes[i + 1], nodes[i + 2]));
    const auto tets = bj.value("tets", std::vector<int>{});
    for (size_t i = 0; i + 10 <= tets.size(); i += 10) {
      std::array<int, 10> t;
      for (int k = 0; k < 10; ++k)
        t[k] = tets[i + k];
      b.mesh.tets.push_back(t);
    }
    const auto boundary = bj.value("boundary", std::vector<int>{});
    for (size_t i = 0; i + 7 <= boundary.size(); i += 7) {
      fem::FemMesh::BoundaryTri tr;
      for (int k = 0; k < 6; ++k)
        tr.nodes[k] = boundary[i + k];
      tr.faceIndex = boundary[i + 6];
      b.mesh.boundary.push_back(tr);
    }
    b.density = bj.value("density", std::vector<double>{});
    b.mesh.stats.nodeCount = b.mesh.nodes.size();
    b.mesh.stats.tetCount = b.mesh.tets.size();
    rt.bodies.push_back(std::move(b));
  }
  return rt;
}

} // namespace DocumentIO
