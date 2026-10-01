#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/Document.h"
#include "core/InputHash.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"

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
    // Keep-outs (v152): a body of the part by name, or a box.
    if (!s.keepOuts.empty()) {
      j["keep_outs"] = json::array();
      for (const fem::KeepOut& k : s.keepOuts)
        j["keep_outs"].push_back(
            k.body.empty()
                ? json{{"box_min", {k.boxMin.x, k.boxMin.y, k.boxMin.z}},
                       {"box_max", {k.boxMax.x, k.boxMax.y, k.boxMax.z}}}
                : json{{"body", k.body}});
    }
  }
  // Load cases (v152): absent on a study without, so earlier files re-save
  // byte-identically.
  if (!s.loadCases.empty()) {
    j["load_cases"] = json::array();
    for (const fem::LoadCase& c : s.loadCases) {
      json cj = {{"name", c.name}, {"weight", c.weight}};
      if (!c.weightExpr.empty())
        cj["weight_expr"] = c.weightExpr;
      j["load_cases"].push_back(std::move(cj));
    }
  }
  // A static study of a topology result (v152).
  if (s.shapeFrom.study != 0) {
    json sj = {{"study", s.shapeFrom.study}};
    if (s.shapeFrom.whole)
      sj["whole"] = true;
    else if (s.shapeFrom.threshold != 0.0)
      sj["threshold"] = s.shapeFrom.threshold;
    j["shape_from"] = std::move(sj);
  }
  j["mesh_size"] = s.meshSize;
  if (!s.meshSizeExpr.empty())
    j["mesh_size_expr"] = s.meshSizeExpr;

  // Assembly studies (v106): the participants and their roles. Absent on a
  // part study, so every earlier study file re-saves byte-identically.
  if (!s.participants.empty()) {
    j["participants"] = json::array();
    for (const fem::Participant& pt : s.participants) {
      json pj = {{"occurrence", pt.occurrenceId},
                 {"role", fem::ParticipantRoleToString(pt.role)}};
      if (pt.meshSize != 0.0 || !pt.meshSizeExpr.empty())
        pj["mesh_size"] = pt.meshSize;
      if (!pt.meshSizeExpr.empty())
        pj["mesh_size_expr"] = pt.meshSizeExpr;
      j["participants"].push_back(std::move(pj));
    }
  }

  if (!s.refinements.empty()) {
    j["refinements"] = json::array();
    for (const fem::Refinement& r : s.refinements) {
      json rj = {{"face", ElementRefJson(r.face)}, {"size", r.size}};
      if (r.occurrenceId != 0)
        rj["occurrence"] = r.occurrenceId;
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
                 {"magnitude", l.magnitude}};
      // A contact anchor has no face to name (v106); a face anchor on an
      // assembly study says whose face it is.
      if (l.contactWith != 0)
        lj["contact_with"] = l.contactWith;
      else
        lj["face"] = ElementRefJson(l.face);
      if (l.occurrenceId != 0)
        lj["occurrence"] = l.occurrenceId;
      if (!l.magnitudeExpr.empty())
        lj["magnitude_expr"] = l.magnitudeExpr;
      if (l.type == fem::LoadType::Force)
        lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
      if (!l.loadCase.empty())
        lj["case"] = l.loadCase;
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
  const auto vec3 = [](const json& v, const char* what) {
    if (!v.is_array() || v.size() != 3)
      throw std::runtime_error(std::string("keep-out ") + what +
                               " is not [x, y, z]");
    return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                      v.at(2).get<double>());
  };
  for (const json& kj : j.value("keep_outs", json::array())) {
    fem::KeepOut k;
    if (kj.contains("body")) {
      k.body = kj.at("body").get<std::string>();
      if (k.body.empty())
        throw std::runtime_error("a keep-out's body is named by an empty "
                                 "string");
    } else if (kj.contains("box_min") && kj.contains("box_max")) {
      k.boxMin = vec3(kj.at("box_min"), "box_min");
      k.boxMax = vec3(kj.at("box_max"), "box_max");
    } else {
      throw std::runtime_error("a keep-out is {body} or {box_min, box_max}: " +
                               kj.dump());
    }
    s.keepOuts.push_back(std::move(k));
  }
  for (const json& cj : j.value("load_cases", json::array())) {
    fem::LoadCase c;
    c.name = cj.at("name").get<std::string>();
    c.weight = cj.value("weight", 1.0);
    c.weightExpr = cj.value("weight_expr", std::string{});
    s.loadCases.push_back(std::move(c));
  }
  if (j.contains("shape_from")) {
    const json& sj = j.at("shape_from");
    s.shapeFrom.study = sj.at("study").get<uint64_t>();
    s.shapeFrom.whole = sj.value("whole", false);
    s.shapeFrom.threshold = sj.value("threshold", 0.0);
    if (s.type != fem::AnalysisType::Static)
      throw std::runtime_error("study " + std::to_string(s.id) +
                               ": only a static study takes shape_from");
  }
  s.meshSize = j.value("mesh_size", 0.0);
  s.meshSizeExpr = j.value("mesh_size_expr", std::string{});

  for (const json& pj : j.value("participants", json::array())) {
    fem::Participant pt;
    pt.occurrenceId = pj.at("occurrence").get<uint64_t>();
    // A role that does not parse is refused, not defaulted to rigid: a
    // meshed panel silently read as a support is a different study.
    const std::string roleStr = pj.value("role", std::string{});
    if (!fem::ParticipantRoleFromString(roleStr, pt.role))
      throw std::runtime_error("unknown participant role '" + roleStr +
                               "' (expected one of: meshed, rigid, ignored)");
    pt.meshSize = pj.value("mesh_size", 0.0);
    pt.meshSizeExpr = pj.value("mesh_size_expr", std::string{});
    s.participants.push_back(std::move(pt));
  }

  for (const json& rj : j.value("refinements", json::array())) {
    fem::Refinement r;
    r.face = ElementRefFromJson(rj.at("face"));
    r.occurrenceId = rj.value("occurrence", uint64_t{0});
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
    l.contactWith = lj.value("contact_with", uint64_t{0});
    l.occurrenceId = lj.value("occurrence", uint64_t{0});
    if (l.contactWith == 0)
      l.face = ElementRefFromJson(lj.at("face"));
    else if (l.type != fem::LoadType::Fixed)
      throw std::runtime_error("load " + std::to_string(l.id) +
                               ": only a fixed support anchors on a contact");
    l.magnitude = lj.value("magnitude", 0.0);
    l.magnitudeExpr = lj.value("magnitude_expr", std::string{});
    l.loadCase = lj.value("case", std::string{});
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
  // What produced it (v148): a reopened project reuses the result for a run
  // whose inputs hash the same, and reads it stale when they do not.
  if (rt.inputHash != 0)
    out["input_hash"] = inputhash::Hex(rt.inputHash);
  // The load cases and what each one came to (v152).
  if (!rt.caseNames.empty()) {
    json cases = json::array();
    for (size_t k = 0; k < rt.caseNames.size(); ++k)
      cases.push_back(
          {{"name", rt.caseNames[k]},
           {"weight", k < rt.caseWeights.size() ? rt.caseWeights[k] : 1.0},
           {"compliance",
            k < rt.caseCompliance.size() ? rt.caseCompliance[k] : 0.0}});
    out["cases"] = std::move(cases);
  }
  // What the keep-outs took and the connectivity filter removed (v152).
  if (!rt.keepOuts.empty()) {
    json ko = json::array();
    for (const fem::KeepOutReport& k : rt.keepOuts)
      ko.push_back({{"what", k.what}, {"tets", k.tets}, {"volume", k.volume}});
    out["keep_outs"] = std::move(ko);
  }
  out["islands"] = {{"threshold", rt.islands.threshold},
                    {"components", rt.islands.components},
                    {"tets", rt.islands.tets},
                    {"volume", rt.islands.volume},
                    {"keep_faces", rt.islands.keepFaces},
                    {"load_path_broken", rt.islands.loadPathBroken}};
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
  if (j.contains("input_hash") &&
      !inputhash::FromHex(j.at("input_hash").get<std::string>(), rt.inputHash))
    throw std::runtime_error("topology result: input_hash " +
                             j.at("input_hash").dump() +
                             " is not 0x and 16 hex digits");
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
  for (const json& cj : j.value("cases", json::array())) {
    rt.caseNames.push_back(cj.at("name").get<std::string>());
    rt.caseWeights.push_back(cj.value("weight", 1.0));
    rt.caseCompliance.push_back(cj.value("compliance", 0.0));
  }
  for (const json& kj : j.value("keep_outs", json::array()))
    rt.keepOuts.push_back({kj.value("what", std::string{}), kj.value("tets", 0),
                           kj.value("volume", 0.0)});
  if (j.contains("islands")) {
    const json& ij = j.at("islands");
    rt.islands.threshold = ij.value("threshold", 0.0);
    rt.islands.components = ij.value("components", 0);
    rt.islands.tets = ij.value("tets", 0);
    rt.islands.volume = ij.value("volume", 0.0);
    rt.islands.keepFaces = ij.value("keep_faces", std::vector<int>{});
    rt.islands.loadPathBroken = ij.value("load_path_broken", false);
  }
  if (!rt.bodies.empty())
    rt.densityDigest =
        fem::DensityDigest(rt.bodies.front().mesh, rt.bodies.front().density);
  return rt;
}

} // namespace DocumentIO
