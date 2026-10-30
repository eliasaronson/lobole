// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/Serialization.h"
#include "core/io/SerializationInternal.h"

#include "core/Document.h"
#include "core/InputHash.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/fem/FemTopo.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <limits>
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
  // Which program solves it (v204): absent when auto, so every study saved
  // before -- and its input hash -- is what it was.
  if (s.solver != fem::Solver::Auto)
    j["solver"] = fem::SolverToString(s.solver);
  // Buckling (v207): num_modes is its number of factors.
  if (s.type == fem::AnalysisType::Modal ||
      s.type == fem::AnalysisType::Buckling)
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
  // Whether a run checks its peak has settled (v206): absent at the
  // default, auto, so earlier files re-save byte-identically.
  if (s.converge != fem::ConvergeMode::Auto)
    j["converge"] = fem::ConvergeModeToString(s.converge);
  // Solved past linear (v208): absent when linear, so every study saved
  // before -- and its input hash -- is what it was. A contact study's
  // increments without large deformation (v209).
  if (s.nonlinear.On() || s.nonlinear.increments > 0) {
    const fem::Nonlinear& nl = s.nonlinear;
    json n = json::object();
    if (nl.largeDeformation)
      n["large_deformation"] = true;
    if (nl.plasticity) // v210
      n["plasticity"] = true;
    if (nl.increments > 0)
      n["increments"] = nl.increments;
    if (nl.snapThrough)
      n["snap_through"] = true;
    if (nl.hasImperfection)
      n["imperfection_mm"] = nl.imperfectionMm;
    if (nl.hasDeflectionLimit)
      n["deflection_limit_mm"] = nl.deflectionLimitMm;
    j["nonlinear"] = std::move(n);
  }
  // A thermal load (v212): absent without one.
  if (s.thermalLoad.On()) {
    const fem::ThermalLoad& tl = s.thermalLoad;
    json t = json::object();
    if (tl.kind == fem::ThermalLoad::Kind::FromStudy)
      t["from_study"] = tl.fromStudy;
    else
      t["uniform_c"] = tl.uniformC;
    if (tl.hasStressFree)
      t["stress_free_c"] = tl.stressFreeC;
    j["thermal_load"] = std::move(t);
  }

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
      if (pt.role == fem::ParticipantRole::Contact && pt.friction != 0.0)
        pj["friction"] = pt.friction; // v209
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
      else if (l.type != fem::LoadType::HeatGeneration)
        lj["face"] = ElementRefJson(l.face);
      if (l.occurrenceId != 0)
        lj["occurrence"] = l.occurrenceId;
      if (!l.magnitudeExpr.empty())
        lj["magnitude_expr"] = l.magnitudeExpr;
      if (l.type == fem::LoadType::Force || l.type == fem::LoadType::Remote)
        lj["direction"] = {l.direction.x, l.direction.y, l.direction.z};
      // v170: a torque's axis (omitted: the cylindrical face's own), and a
      // remote load's point.
      if (l.type == fem::LoadType::Torque && l.HasAxis()) {
        lj["axis_origin"] = {l.axisOrigin.x, l.axisOrigin.y, l.axisOrigin.z};
        lj["axis_dir"] = {l.axisDirection.x, l.axisDirection.y,
                          l.axisDirection.z};
      }
      if (l.type == fem::LoadType::Remote && l.hasPoint)
        lj["point"] = {l.point.x, l.point.y, l.point.z};
      if (l.type == fem::LoadType::Pin && l.fixRotation) // v196
        lj["fix_rotation"] = true;
      if (l.type == fem::LoadType::Contact && l.friction != 0.0) // v209
        lj["friction"] = l.friction;
      // A thermal load's quantities (v212), each only where given.
      const auto given = [&](const char* key, double v) {
        if (!std::isnan(v))
          lj[key] = v;
      };
      given("temperature_c", l.temperatureC);
      given("power_w", l.powerW);
      given("flux_w_m2", l.fluxWm2);
      given("film_w_m2k", l.filmWm2K);
      given("ambient_c", l.ambientC);
      if (!l.body.empty())
        lj["body"] = l.body;
      if (!l.loadCase.empty())
        lj["case"] = l.loadCase;
      // v183: part of the face; omitted for the whole of it.
      if (l.region.kind == fem::LoadRegion::Kind::Band)
        lj["region"] = {
            {"normal",
             {l.region.normal.x, l.region.normal.y, l.region.normal.z}},
            {"from_mm", l.region.fromMm},
            {"to_mm", l.region.toMm}};
      else if (l.region.kind == fem::LoadRegion::Kind::Sphere)
        lj["region"] = {
            {"center_mm",
             {l.region.center.x, l.region.center.y, l.region.center.z}},
            {"radius_mm", l.region.radiusMm}};
      j["loads"].push_back(std::move(lj));
    }
  }
  return j;
}

fem::LoadRegion LoadRegionFromJson(const json& r, const std::string& who) {
  const auto vec = [&](const char* key) {
    const json& v = r.at(key);
    if (!v.is_array() || v.size() != 3 || !v[0].is_number() ||
        !v[1].is_number() || !v[2].is_number())
      throw std::runtime_error(who + ": region " + key + " is [x, y, z]");
    return glm::dvec3(v[0].get<double>(), v[1].get<double>(),
                      v[2].get<double>());
  };
  const auto num = [&](const char* key) {
    const json& v = r.at(key);
    if (!v.is_number())
      throw std::runtime_error(who + ": region " + key + " is a number");
    return v.get<double>();
  };
  if (!r.is_object())
    throw std::runtime_error(who + ": a region is {normal, from_mm, to_mm} "
                                   "or {center_mm, radius_mm}");
  const bool band =
      r.contains("normal") || r.contains("from_mm") || r.contains("to_mm");
  const bool sphere = r.contains("center_mm") || r.contains("radius_mm");
  for (const auto& [key, _] : r.items())
    if (key != "normal" && key != "from_mm" && key != "to_mm" &&
        key != "center_mm" && key != "radius_mm")
      throw std::runtime_error(
          who + ": a region does not take `" + key +
          "`; it is {normal, from_mm, to_mm} (a band between two planes) "
          "or {center_mm, radius_mm} (within a radius of a point)");
  if (band == sphere)
    throw std::runtime_error(
        who + ": a region is EITHER {normal, from_mm, to_mm} (a band "
              "between two planes) OR {center_mm, radius_mm} (within a "
              "radius of a point)");
  fem::LoadRegion g;
  if (band) {
    g.kind = fem::LoadRegion::Kind::Band;
    g.normal = vec("normal");
    g.fromMm = num("from_mm");
    g.toMm = num("to_mm");
  } else {
    g.kind = fem::LoadRegion::Kind::Sphere;
    g.center = vec("center_mm");
    g.radiusMm = num("radius_mm");
  }
  if (const std::string why = g.Why(); !why.empty())
    throw std::runtime_error(who + ": " + why);
  return g;
}

fem::Study StudyFromJson(const json& j) {
  readctx::RequireTag(j, "Study", "study");
  readctx::RefuseUnknownKeys(j,
                             {"type",
                              "id",
                              "name",
                              "analysis",
                              "solver",
                              "num_modes",
                              "gravity",
                              "volume_fraction",
                              "volume_fraction_expr",
                              "filter_radius",
                              "filter_radius_expr",
                              "max_iterations",
                              "keep_faces",
                              "keep_outs",
                              "load_cases",
                              "shape_from",
                              "mesh_size",
                              "mesh_size_expr",
                              "converge",
                              "nonlinear",
                              "thermal_load",
                              "participants",
                              "refinements",
                              "loads"},
                             "study");
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
                             "' (expected one of: " + fem::kAnalysisTypeNames +
                             ")");
  // Each analysis's own keys are written only for it: on another, the next
  // save would drop them (a modal study that lost its `analysis` line read
  // as static, its num_modes gone).
  const auto refuseUnless = [&](fem::AnalysisType type,
                                std::initializer_list<const char*> keys) {
    if (s.type == type)
      return;
    for (const char* key : keys)
      if (j.contains(key))
        throw std::runtime_error(std::string("study ") + key + " is a " +
                                 fem::AnalysisTypeToString(type) +
                                 " study's, and this one is " + analysisStr);
  };
  if (j.contains("num_modes") && s.type != fem::AnalysisType::Modal &&
      s.type != fem::AnalysisType::Buckling)
    throw std::runtime_error("study num_modes is a modal study's (or a "
                             "buckling one's), and this one is " +
                             analysisStr);
  if (j.contains("solver")) {
    const std::string solverStr = j.at("solver").get<std::string>();
    if (!fem::SolverFromString(solverStr, s.solver))
      throw std::runtime_error("unknown study solver '" + solverStr +
                               "' (expected one of: " + fem::kSolverNames +
                               ")");
    if (const std::string why = fem::SolverRefusal(s.solver, s.type);
        !why.empty())
      throw std::runtime_error("study " + std::to_string(s.id) + ": " + why);
  }
  refuseUnless(fem::AnalysisType::Topology,
               {"volume_fraction", "volume_fraction_expr", "filter_radius",
                "filter_radius_expr", "max_iterations", "keep_faces",
                "keep_outs"});
  s.numModes = j.value("num_modes", s.type == fem::AnalysisType::Buckling
                                        ? fem::kBucklingFactorsDefault
                                        : 6);
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
  for (const json& fj : j.value("keep_faces", json::array())) {
    readctx::RefuseUnknownKeys(fj, {"feature_id", "element"},
                               "study keep face");
    s.keepFaces.push_back(ElementRefFromJson(fj));
  }
  const auto vec3 = [](const json& v, const char* what) {
    if (!v.is_array() || v.size() != 3)
      throw std::runtime_error(std::string("keep-out ") + what +
                               " is not [x, y, z]");
    return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                      v.at(2).get<double>());
  };
  for (const json& kj : j.value("keep_outs", json::array())) {
    readctx::RefuseUnknownKeys(kj, {"body", "box_min", "box_max"},
                               "study keep-out");
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
    readctx::RefuseUnknownKeys(cj, {"name", "weight", "weight_expr"},
                               "study load case");
    fem::LoadCase c;
    c.name = cj.at("name").get<std::string>();
    c.weight = cj.value("weight", 1.0);
    c.weightExpr = cj.value("weight_expr", std::string{});
    s.loadCases.push_back(std::move(c));
  }
  if (j.contains("shape_from")) {
    const json& sj = j.at("shape_from");
    readctx::RefuseUnknownKeys(sj, {"study", "whole", "threshold"},
                               "study shape_from");
    s.shapeFrom.study = sj.at("study").get<uint64_t>();
    s.shapeFrom.whole = sj.value("whole", false);
    s.shapeFrom.threshold = sj.value("threshold", 0.0);
    if (s.type != fem::AnalysisType::Static)
      throw std::runtime_error("study " + std::to_string(s.id) +
                               ": only a static study takes shape_from");
  }
  s.meshSize = j.value("mesh_size", 0.0);
  s.meshSizeExpr = j.value("mesh_size_expr", std::string{});
  if (j.contains("converge")) { // v206
    const std::string c = j.at("converge").get<std::string>();
    if (!fem::ConvergeModeFromString(c, s.converge))
      throw std::runtime_error("study converge '" + c +
                               "' is not one of: " + fem::kConvergeModeNames);
    if (s.type != fem::AnalysisType::Static &&
        s.type != fem::AnalysisType::Buckling &&
        s.type != fem::AnalysisType::Thermal)
      throw std::runtime_error("study " + std::to_string(s.id) +
                               ": only a static, buckling or thermal study "
                               "takes converge");
  }

  if (j.contains("nonlinear")) { // v208
    const json& n = j.at("nonlinear");
    readctx::RefuseUnknownKeys(n,
                               {"large_deformation", "plasticity", "increments",
                                "snap_through", "imperfection_mm",
                                "deflection_limit_mm"},
                               "study nonlinear");
    fem::Nonlinear& nl = s.nonlinear;
    nl.largeDeformation = n.value("large_deformation", false);
    nl.plasticity = n.value("plasticity", false); // v210
    nl.increments = n.value("increments", 0);
    nl.snapThrough = n.value("snap_through", false);
    nl.hasImperfection = n.contains("imperfection_mm");
    nl.imperfectionMm = n.value("imperfection_mm", 0.0);
    nl.hasDeflectionLimit = n.contains("deflection_limit_mm");
    nl.deflectionLimitMm = n.value("deflection_limit_mm", 0.0);
    // Without large_deformation, refused below unless the study has a
    // contact (NonlinearRefusal).
  }
  if (j.contains("thermal_load")) { // v212
    const json& t = j.at("thermal_load");
    readctx::RefuseUnknownKeys(t, {"from_study", "uniform_c", "stress_free_c"},
                               "study thermal_load");
    fem::ThermalLoad& tl = s.thermalLoad;
    if (t.contains("from_study") == t.contains("uniform_c"))
      throw std::runtime_error("study " + std::to_string(s.id) +
                               ": thermal_load is {from_study} or "
                               "{uniform_c, stress_free_c}");
    const auto number = [&](const char* key) {
      if (!t.at(key).is_number())
        throw std::runtime_error("study thermal_load " + std::string(key) +
                                 " is a number");
      return t.at(key).get<double>();
    };
    if (t.contains("from_study")) {
      tl.kind = fem::ThermalLoad::Kind::FromStudy;
      tl.fromStudy = t.at("from_study").get<uint64_t>();
    } else {
      tl.kind = fem::ThermalLoad::Kind::Uniform;
      tl.uniformC = number("uniform_c");
    }
    if (t.contains("stress_free_c")) {
      tl.hasStressFree = true;
      tl.stressFreeC = number("stress_free_c");
    }
  }

  for (const json& pj : j.value("participants", json::array())) {
    readctx::RefuseUnknownKeys(
        pj, {"occurrence", "role", "mesh_size", "mesh_size_expr", "friction"},
        "study participant");
    fem::Participant pt;
    pt.occurrenceId = pj.at("occurrence").get<uint64_t>();
    // A role that does not parse is refused, not defaulted to rigid: a
    // meshed panel silently read as a support is a different study.
    const std::string roleStr = pj.value("role", std::string{});
    if (!fem::ParticipantRoleFromString(roleStr, pt.role))
      throw std::runtime_error("unknown participant role '" + roleStr +
                               "' (expected one of: meshed, rigid, bonded, "
                               "contact, ignored)");
    pt.meshSize = pj.value("mesh_size", 0.0);
    pt.meshSizeExpr = pj.value("mesh_size_expr", std::string{});
    if (pj.contains("friction")) { // v209
      if (pt.role != fem::ParticipantRole::Contact)
        throw std::runtime_error("participant " +
                                 std::to_string(pt.occurrenceId) +
                                 ": friction is a contact participant's");
      if (!pj.at("friction").is_number())
        throw std::runtime_error("participant " +
                                 std::to_string(pt.occurrenceId) +
                                 ": friction is not a number");
      pt.friction = pj.at("friction").get<double>();
    }
    s.participants.push_back(std::move(pt));
  }

  for (const json& rj : j.value("refinements", json::array())) {
    readctx::RefuseUnknownKeys(rj, {"face", "occurrence", "size", "size_expr"},
                               "study refinement");
    fem::Refinement r;
    readctx::RefuseUnknownKeys(rj.at("face"), {"feature_id", "element"},
                               "study refinement face");
    r.face = ElementRefFromJson(rj.at("face"));
    r.occurrenceId = rj.value("occurrence", uint64_t{0});
    r.size = rj.value("size", 0.0);
    r.sizeExpr = rj.value("size_expr", std::string{});
    s.refinements.push_back(std::move(r));
  }

  for (const json& lj : j.value("loads", json::array())) {
    readctx::RefuseUnknownKeys(
        lj, {"id",           "type",         "magnitude",  "magnitude_expr",
             "contact_with", "face",         "occurrence", "direction",
             "axis_origin",  "axis_dir",     "point",      "case",
             "region",       "fix_rotation", "friction",   "temperature_c",
             "power_w",      "flux_w_m2",    "film_w_m2k", "ambient_c",
             "body"},
        "study load");
    fem::Load l;
    l.id = lj.at("id").get<uint64_t>();
    const std::string loadStr = lj.value("type", std::string{"fixed"});
    if (!fem::LoadTypeFromString(loadStr, l.type))
      throw std::runtime_error("unknown load type '" + loadStr +
                               "' (expected one of: " + fem::kLoadTypeNames +
                               ")");
    l.contactWith = lj.value("contact_with", uint64_t{0});
    l.occurrenceId = lj.value("occurrence", uint64_t{0});
    if (l.type == fem::LoadType::HeatGeneration) {
      if (lj.contains("face") || l.contactWith != 0)
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": a heat_generation heats a body, named by "
                                 "body, not a face");
    } else if (l.contactWith == 0) {
      readctx::RefuseUnknownKeys(lj.at("face"), {"feature_id", "element"},
                                 "study load face");
      l.face = ElementRefFromJson(lj.at("face"));
    } else if (l.type != fem::LoadType::Fixed &&
               l.type != fem::LoadType::Contact)
      throw std::runtime_error("load " + std::to_string(l.id) +
                               ": only a fixed support or a contact "
                               "support anchors on a contact");
    l.magnitude = lj.value("magnitude", 0.0);
    l.magnitudeExpr = lj.value("magnitude_expr", std::string{});
    l.loadCase = lj.value("case", std::string{});
    const auto point = [&](const char* key) {
      const json& v = lj.at(key);
      if (!v.is_array() || v.size() != 3)
        throw std::runtime_error("load " + std::to_string(l.id) + ": " + key +
                                 " is not [x, y, z]");
      return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                        v.at(2).get<double>());
    };
    if (lj.contains("direction"))
      l.direction = point("direction");
    // What a load type does not use is not written back: one stated on
    // another type would be dropped.
    if (l.type != fem::LoadType::Torque &&
        (lj.contains("axis_origin") || lj.contains("axis_dir")))
      throw std::runtime_error("load " + std::to_string(l.id) +
                               ": axis_origin and axis_dir are a torque's");
    if (l.type != fem::LoadType::Force && l.type != fem::LoadType::Remote &&
        lj.contains("direction"))
      throw std::runtime_error("load " + std::to_string(l.id) +
                               ": direction is a force's or a remote load's");
    if (l.type != fem::LoadType::Remote && lj.contains("point"))
      throw std::runtime_error("load " + std::to_string(l.id) +
                               ": point is a remote load's");
    if (lj.contains("fix_rotation")) { // v196
      if (l.type != fem::LoadType::Pin)
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": fix_rotation is a pin's");
      if (!lj.at("fix_rotation").is_boolean())
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": fix_rotation is not true or false");
      l.fixRotation = lj.at("fix_rotation").get<bool>();
    }
    if (lj.contains("friction")) { // v209
      if (l.type != fem::LoadType::Contact)
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": friction is a contact support's");
      if (!lj.at("friction").is_number())
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": friction is not a number");
      l.friction = lj.at("friction").get<double>();
    }
    if (l.type == fem::LoadType::Torque &&
        (lj.contains("axis_origin") || lj.contains("axis_dir"))) {
      if (!lj.contains("axis_origin") || !lj.contains("axis_dir"))
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": a torque's axis is axis_origin AND "
                                 "axis_dir");
      l.axisOrigin = point("axis_origin");
      l.axisDirection = point("axis_dir");
      if (!l.HasAxis())
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": a torque's axis_dir is zero");
    }
    if (l.type == fem::LoadType::Remote) {
      if (!lj.contains("point"))
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": a remote load needs the point it acts "
                                 "at");
      l.point = point("point");
      l.hasPoint = true;
    }
    if (lj.contains("region")) // v183
      l.region =
          LoadRegionFromJson(lj.at("region"), "load " + std::to_string(l.id));
    // A thermal load's quantities (v212), each its own type's.
    const auto thermalKey = [&](const char* key, fem::LoadType type1,
                                fem::LoadType type2, double& into) {
      if (!lj.contains(key))
        return;
      if (l.type != type1 && l.type != type2)
        throw std::runtime_error(
            "load " + std::to_string(l.id) + ": " + key + " is a " +
            fem::LoadTypeToString(type1) +
            (type1 != type2 ? std::string(" or ") +
                                  fem::LoadTypeToString(type2) + " load's"
                            : std::string(" load's")));
      if (!lj.at(key).is_number())
        throw std::runtime_error("load " + std::to_string(l.id) + ": " + key +
                                 " is a number");
      into = lj.at(key).get<double>();
    };
    using LT = fem::LoadType;
    thermalKey("temperature_c", LT::Temperature, LT::Temperature,
               l.temperatureC);
    thermalKey("power_w", LT::HeatFlux, LT::HeatGeneration, l.powerW);
    thermalKey("flux_w_m2", LT::HeatFlux, LT::HeatFlux, l.fluxWm2);
    thermalKey("film_w_m2k", LT::Convection, LT::Convection, l.filmWm2K);
    thermalKey("ambient_c", LT::Convection, LT::Convection, l.ambientC);
    if (lj.contains("body")) {
      if (l.type != LT::HeatGeneration)
        throw std::runtime_error("load " + std::to_string(l.id) +
                                 ": body is a heat_generation's");
      l.body = lj.at("body").get<std::string>();
    }
    s.loads.push_back(std::move(l));
  }
  // A nonlinear option or a contact the study cannot take (an in-house
  // solver, load cases, another analysis) is refused here as set_study
  // refuses it.
  if (const std::string why = fem::ContactRefusal(s); !why.empty())
    throw std::runtime_error("study " + std::to_string(s.id) + ": " + why);
  // A thermal load where it cannot go, a structural one on a thermal study,
  // a missing or impossible quantity (docs/simulation/README.md#thermal).
  if (const std::string why = fem::ThermalRefusal(s); !why.empty())
    throw std::runtime_error("study " + std::to_string(s.id) + ": " + why);
  return s;
}

} // namespace DocumentIO

// ---- A result's numeric arrays (v154) --------------------------------------

static_assert(std::endian::native == std::endian::little,
              "a result's .bin is little-endian, written as the bytes are");

json ResultArrayWriter::Append(const char* type, const void* data, size_t count,
                               size_t size) {
  // Every array starts on a multiple of its element size.
  while (m_bytes.size() % size != 0)
    m_bytes.push_back(0);
  const size_t offset = m_bytes.size();
  const auto* b = static_cast<const std::uint8_t*>(data);
  m_bytes.insert(m_bytes.end(), b, b + count * size);
  return {{"type", type}, {"offset", offset}, {"count", count}};
}

json ResultArrayWriter::F32(const std::vector<double>& v) {
  std::vector<float> f(v.begin(), v.end());
  return Append("f32", f.data(), f.size(), sizeof(float));
}

json ResultArrayWriter::F32(const std::vector<glm::dvec3>& v) {
  std::vector<float> f;
  f.reserve(v.size() * 3);
  for (const glm::dvec3& p : v) {
    f.push_back(static_cast<float>(p.x));
    f.push_back(static_cast<float>(p.y));
    f.push_back(static_cast<float>(p.z));
  }
  return Append("f32", f.data(), f.size(), sizeof(float));
}

json ResultArrayWriter::F64(const std::vector<double>& v) {
  return Append("f64", v.data(), v.size(), sizeof(double));
}

json ResultArrayWriter::Index(const std::vector<int>& v) {
  int top = 0;
  bool negative = false;
  for (int i : v) {
    negative = negative || i < 0;
    top = std::max(top, i);
  }
  if (negative) // a cut's own triangles lie on no face (-1)
    return Append("i32", v.data(), v.size(), sizeof(std::int32_t));
  if (top <= 0xFFFF) {
    std::vector<std::uint16_t> u(v.begin(), v.end());
    return Append("u16", u.data(), u.size(), sizeof(std::uint16_t));
  }
  std::vector<std::uint32_t> u(v.begin(), v.end());
  return Append("u32", u.data(), u.size(), sizeof(std::uint32_t));
}

json ResultArrayWriter::Take() { return json::binary(std::move(m_bytes)); }

ResultArrayReader::ResultArrayReader(const json& owner, std::string what)
    : m_what(std::move(what)) {
  if (owner.contains(kResultData) && owner.at(kResultData).is_binary())
    m_bytes = &owner.at(kResultData).get_binary();
}

const std::uint8_t* ResultArrayReader::Locate(const json& ref, const char* name,
                                              std::string& type,
                                              size_t& count) const {
  if (!ref.is_object() || !ref.contains("type") || !ref.contains("offset") ||
      !ref.contains("count"))
    throw std::runtime_error(m_what + ": " + name +
                             " is not {type, offset, count}");
  if (m_bytes == nullptr)
    throw std::runtime_error(m_what + ": " + name +
                             " names its .result.bin, which was not read");
  type = ref.at("type").get<std::string>();
  size_t size = 0;
  if (type == "f32" || type == "u32" || type == "i32")
    size = 4;
  else if (type == "f64")
    size = 8;
  else if (type == "u16")
    size = 2;
  else
    throw std::runtime_error(m_what + ": " + name + " has type '" + type +
                             "' (expected one of: f32, f64, u16, u32, i32)");
  const size_t offset = ref.at("offset").get<size_t>();
  count = ref.at("count").get<size_t>();
  if (offset % size != 0 || count > m_bytes->size() / size ||
      offset > m_bytes->size() - count * size)
    throw std::runtime_error(m_what + ": " + name + " (" +
                             std::to_string(count) + " " + type + " at byte " +
                             std::to_string(offset) + ") lies outside its " +
                             std::to_string(m_bytes->size()) + "-byte .bin");
  return m_bytes->data() + offset;
}

std::vector<double> ResultArrayReader::Reals(const json& ref,
                                             const char* name) const {
  std::string type;
  size_t count = 0;
  const std::uint8_t* p = Locate(ref, name, type, count);
  std::vector<double> out(count);
  if (type == "f32") {
    for (size_t i = 0; i < count; ++i) {
      float f;
      std::memcpy(&f, p + i * sizeof(float), sizeof(float));
      out[i] = f;
    }
  } else if (type == "f64") {
    if (count > 0) // an empty array has no data pointer to copy from
      std::memcpy(out.data(), p, count * sizeof(double));
  } else {
    throw std::runtime_error(m_what + ": " + name + " is " + type +
                             " (expected f32 or f64)");
  }
  return out;
}

std::vector<glm::dvec3> ResultArrayReader::Vec3s(const json& ref,
                                                 const char* name) const {
  const std::vector<double> flat = Reals(ref, name);
  if (flat.size() % 3 != 0)
    throw std::runtime_error(m_what + ": " + name +
                             " is not a whole number of xyz triples");
  std::vector<glm::dvec3> out;
  out.reserve(flat.size() / 3);
  for (size_t i = 0; i + 3 <= flat.size(); i += 3)
    out.emplace_back(flat[i], flat[i + 1], flat[i + 2]);
  return out;
}

std::vector<int> ResultArrayReader::Indices(const json& ref,
                                            const char* name) const {
  std::string type;
  size_t count = 0;
  const std::uint8_t* p = Locate(ref, name, type, count);
  std::vector<int> out(count);
  if (type == "u16") {
    for (size_t i = 0; i < count; ++i) {
      std::uint16_t u;
      std::memcpy(&u, p + i * sizeof(u), sizeof(u));
      out[i] = u;
    }
  } else if (type == "u32") {
    for (size_t i = 0; i < count; ++i) {
      std::uint32_t u;
      std::memcpy(&u, p + i * sizeof(u), sizeof(u));
      if (u > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error(m_what + ": " + name + " holds index " +
                                 std::to_string(u));
      out[i] = static_cast<int>(u);
    }
  } else if (type == "i32") {
    std::memcpy(out.data(), p, count * sizeof(std::int32_t));
  } else {
    throw std::runtime_error(m_what + ": " + name + " is " + type +
                             " (expected u16, u32 or i32)");
  }
  return out;
}

namespace DocumentIO {

// ---- Study results (topology v33, every kind v153, binary v154) ------------
// Saved with the project, each beside its study, so a reopened project's run
// of the same inputs is answered from it (docs/simulation/README.md
// #studies-as-jobs). What is kept is what fem::KeepWhatIsRead leaves -- what
// the panel and the RPCs read back -- and never the solver's scratch
// (docs/simulation/README.md#what-a-saved-result-keeps). The numeric arrays
// go to the .result.bin (ResultArrayWriter); solvedAtVersion is stamped on
// load.

namespace {

std::vector<glm::dvec3> Vec3sFromFlat(const json& j, const char* what) {
  const auto flat = j.get<std::vector<double>>();
  if (flat.size() % 3 != 0)
    throw std::runtime_error(std::string("study result: ") + what +
                             " is not a whole number of xyz triples");
  std::vector<glm::dvec3> out;
  out.reserve(flat.size() / 3);
  for (size_t i = 0; i + 3 <= flat.size(); i += 3)
    out.emplace_back(flat[i], flat[i + 1], flat[i + 2]);
  return out;
}

// A field is a reference into the .bin (v154), or the json array a v153
// file held inline.
struct FieldReader {
  const ResultArrayReader& bin;
  std::vector<double> Reals(const json& j, const char* name) const {
    return j.is_array() ? j.get<std::vector<double>>() : bin.Reals(j, name);
  }
  std::vector<glm::dvec3> Vec3s(const json& j, const char* name) const {
    return j.is_array() ? Vec3sFromFlat(j, name) : bin.Vec3s(j, name);
  }
  std::vector<int> Indices(const json& j, const char* name) const {
    return j.is_array() ? j.get<std::vector<int>>() : bin.Indices(j, name);
  }
};

json FieldsToJson(const fem::FemResults& r, ResultArrayWriter& w) {
  json out = {{"max_von_mises", r.maxVonMises},
              {"max_vm_node", r.maxVmNode},
              {"max_vm_at", Vec3ToJson(r.maxVmAt)},
              {"max_displacement", r.maxDisplacement},
              {"max_disp_node", r.maxDispNode},
              {"max_disp_at", Vec3ToJson(r.maxDispAt)}};
  if (!r.displacement.empty())
    out["displacement"] = w.F32(r.displacement);
  if (!r.vonMises.empty())
    out["von_mises"] = w.F32(r.vonMises);
  if (!r.stress.empty()) { // v172: six a node, Voigt order
    std::vector<double> flat;
    flat.reserve(6 * r.stress.size());
    for (const fem::Stress& st : r.stress)
      flat.insert(flat.end(), st.begin(), st.end());
    out["stress"] = w.F32(flat);
  }
  // v210: a plastic result's plastic strain, and what unloading left.
  if (!r.plasticStrain.empty()) {
    out["plastic_strain"] = w.F32(r.plasticStrain);
    out["max_plastic_strain"] = r.maxPlasticStrain;
    out["max_pe_node"] = r.maxPeNode;
    out["max_pe_at"] = Vec3ToJson(r.maxPeAt);
  }
  if (!r.residualDisplacement.empty()) {
    out["residual_displacement"] = w.F32(r.residualDisplacement);
    out["max_residual_displacement"] = r.maxResidualDisplacement;
    out["max_residual_at"] = Vec3ToJson(r.maxResidualAt);
  }
  // v212: a thermal result's temperature (C) and heat flux (W/m^2).
  if (!r.temperature.empty())
    out["temperature"] = w.F32(r.temperature);
  if (!r.heatFlux.empty())
    out["heat_flux"] = w.F32(r.heatFlux);
  // v196: what each support carried, and a peak on a support's edge.
  if (!r.reactions.empty()) {
    json rs = json::array();
    for (const fem::FemResults::SupportReaction& sr : r.reactions) {
      json rj = {{"support", sr.support},
                 {"type", fem::LoadTypeToString(sr.type)},
                 {"force_n", Vec3ToJson(sr.forceN)}};
      if (sr.hasMoment) // v202
        rj["moment_n_mm"] = Vec3ToJson(sr.momentNmm);
      if (sr.type == fem::LoadType::Pin) {
        rj["contact_area_mm2"] = sr.contactAreaMm2;
        rj["face_area_mm2"] = sr.faceAreaMm2;
      }
      rs.push_back(std::move(rj));
    }
    out["reactions"] = std::move(rs);
  }
  if (r.supportEdgePeak.flagged)
    out["support_edge_peak"] = {
        {"support", r.supportEdgePeak.support},
        {"type", fem::LoadTypeToString(r.supportEdgePeak.type)},
        {"away_mpa", r.supportEdgePeak.awayMPa},
        {"away_at", Vec3ToJson(r.supportEdgePeak.awayAt)}};
  if (!r.modes.empty()) {
    json modes = json::array();
    for (const fem::FemResults::ModeShape& m : r.modes) {
      json mj = {{"frequency_hz", m.frequencyHz},
                 {"shape", w.F32(m.displacement)}};
      if (m.factor != 0.0) // v207: a buckling mode's load factor
        mj["factor"] = m.factor;
      modes.push_back(std::move(mj));
    }
    out["modes"] = std::move(modes);
  }
  return out;
}

fem::FemResults FieldsFromJson(const json& j, const FieldReader& f) {
  fem::FemResults r;
  if (j.contains("displacement"))
    r.displacement = f.Vec3s(j.at("displacement"), "displacement");
  if (j.contains("von_mises"))
    r.vonMises = f.Reals(j.at("von_mises"), "von_mises");
  if (j.contains("stress")) { // v172
    const std::vector<double> flat = f.Reals(j.at("stress"), "stress");
    if (flat.size() % 6 != 0)
      throw std::runtime_error("a result's stress has " +
                               std::to_string(flat.size()) +
                               " values, not six a node");
    r.stress.resize(flat.size() / 6);
    for (size_t i = 0; i < r.stress.size(); ++i)
      for (int k = 0; k < 6; ++k)
        r.stress[i][k] = flat[6 * i + k];
  }
  if (j.contains("plastic_strain")) { // v210
    r.plasticStrain = f.Reals(j.at("plastic_strain"), "plastic_strain");
    r.maxPlasticStrain = j.value("max_plastic_strain", 0.0);
    r.maxPeNode = j.value("max_pe_node", -1);
    if (j.contains("max_pe_at"))
      r.maxPeAt = Vec3FromJson(j.at("max_pe_at"));
  }
  if (j.contains("residual_displacement")) { // v210
    r.residualDisplacement =
        f.Vec3s(j.at("residual_displacement"), "residual_displacement");
    r.maxResidualDisplacement = j.value("max_residual_displacement", 0.0);
    if (j.contains("max_residual_at"))
      r.maxResidualAt = Vec3FromJson(j.at("max_residual_at"));
  }
  if (j.contains("temperature")) // v212
    r.temperature = f.Reals(j.at("temperature"), "temperature");
  if (j.contains("heat_flux"))
    r.heatFlux = f.Vec3s(j.at("heat_flux"), "heat_flux");
  r.maxVonMises = j.value("max_von_mises", 0.0);
  r.maxVmNode = j.value("max_vm_node", -1);
  if (j.contains("max_vm_at"))
    r.maxVmAt = Vec3FromJson(j.at("max_vm_at"));
  r.maxDisplacement = j.value("max_displacement", 0.0);
  r.maxDispNode = j.value("max_disp_node", -1);
  if (j.contains("max_disp_at"))
    r.maxDispAt = Vec3FromJson(j.at("max_disp_at"));
  for (const json& mj : j.value("modes", json::array()))
    r.modes.push_back({mj.at("frequency_hz").get<double>(),
                       f.Vec3s(mj.at("shape"), "a mode shape"),
                       mj.value("factor", 0.0)});
  const auto supportType = [](const json& v) {
    const std::string t = v.get<std::string>();
    fem::LoadType out;
    if (!fem::LoadTypeFromString(t, out) || !fem::IsSupport(out))
      throw std::runtime_error("a result's support type '" + t +
                               "' is not a support (fixed, pin)");
    return out;
  };
  for (const json& rj : j.value("reactions", json::array())) { // v196
    fem::FemResults::SupportReaction sr;
    sr.support = rj.at("support").get<uint64_t>();
    sr.type = supportType(rj.at("type"));
    sr.forceN = Vec3FromJson(rj.at("force_n"));
    if (rj.contains("moment_n_mm")) { // v202
      sr.momentNmm = Vec3FromJson(rj.at("moment_n_mm"));
      sr.hasMoment = true;
    }
    sr.contactAreaMm2 = rj.value("contact_area_mm2", 0.0);
    sr.faceAreaMm2 = rj.value("face_area_mm2", 0.0);
    r.reactions.push_back(sr);
  }
  if (j.contains("support_edge_peak")) { // v196
    const json& pj = j.at("support_edge_peak");
    r.supportEdgePeak.flagged = true;
    r.supportEdgePeak.support = pj.at("support").get<uint64_t>();
    r.supportEdgePeak.type = supportType(pj.at("type"));
    r.supportEdgePeak.awayMPa = pj.value("away_mpa", 0.0);
    if (pj.contains("away_at"))
      r.supportEdgePeak.awayAt = Vec3FromJson(pj.at("away_at"));
  }
  return r;
}

const char* KindOf(const fem::StudyRuntime& rt) {
  if (rt.topology)
    return "topology";
  for (const fem::StudyBodyResult& b : rt.bodies)
    if (b.buckling.solved)
      return "buckling";
  for (const fem::StudyBodyResult& b : rt.bodies)
    if (b.thermal.solved)
      return "thermal";
  for (const fem::StudyBodyResult& b : rt.bodies)
    if (!b.results.modes.empty())
      return "modal";
  return "static";
}

// A loaded body that indexes past its own nodes is refused: the panel would
// read out of bounds.
void CheckBody(const fem::StudyBodyResult& b, const std::string& what) {
  const size_t n = b.mesh.nodes.size();
  const auto fail = [&](const std::string& m) {
    throw std::runtime_error(what + ": body '" + b.bodyName + "' " + m);
  };
  for (const auto& t : b.mesh.tets)
    for (int v : t)
      if (v < 0 || static_cast<size_t>(v) >= n)
        fail("has a tet on node " + std::to_string(v) + " of " +
             std::to_string(n));
  for (const auto& tr : b.mesh.boundary)
    for (int v : tr.nodes)
      if (v < 0 || static_cast<size_t>(v) >= n)
        fail("has a boundary triangle on node " + std::to_string(v) + " of " +
             std::to_string(n));
  if (!b.mesh.tetDomain.empty() &&
      b.mesh.tetDomain.size() != b.mesh.tets.size())
    fail("has " + std::to_string(b.mesh.tetDomain.size()) +
         " tet domains for " + std::to_string(b.mesh.tets.size()) + " tets");
  if (!b.density.empty() && b.density.size() != b.mesh.tets.size())
    fail("has " + std::to_string(b.density.size()) + " densities for " +
         std::to_string(b.mesh.tets.size()) + " tets");
  const auto perNode = [&](const fem::FemResults& r) {
    if (!r.displacement.empty() && r.displacement.size() != n)
      fail("has " + std::to_string(r.displacement.size()) +
           " displacements for " + std::to_string(n) + " nodes");
    if (!r.plasticStrain.empty() && r.plasticStrain.size() != n)
      fail("has " + std::to_string(r.plasticStrain.size()) +
           " plastic strains for " + std::to_string(n) + " nodes");
    if (!r.residualDisplacement.empty() && r.residualDisplacement.size() != n)
      fail("has " + std::to_string(r.residualDisplacement.size()) +
           " residual displacements for " + std::to_string(n) + " nodes");
    if (!r.vonMises.empty() && r.vonMises.size() != n)
      fail("has " + std::to_string(r.vonMises.size()) + " stresses for " +
           std::to_string(n) + " nodes");
    if (!r.stress.empty() && r.stress.size() != n)
      fail("has " + std::to_string(r.stress.size()) + " stress tensors for " +
           std::to_string(n) + " nodes");
    for (const auto& m : r.modes)
      if (m.displacement.size() != n)
        fail("has a mode shape of " + std::to_string(m.displacement.size()) +
             " for " + std::to_string(n) + " nodes");
  };
  perNode(b.results);
  for (const fem::FemResults& c : b.cases)
    perNode(c);
}

} // namespace

namespace {

// What a buckling body's result says beyond its modes (v207).
// A thermal body's summary (v212): what was read off its field.
json ThermalToJson(const fem::StudyBodyResult::Thermal& t) {
  json j = {{"max_c", t.maxC},
            {"max_at", Vec3ToJson(t.maxAt)},
            {"min_c", t.minC},
            {"min_at", Vec3ToJson(t.minAt)},
            {"base_c", t.baseC}};
  if (t.solved) {
    j["max_heat_flux_w_m2"] = t.maxFluxWm2;
    j["max_heat_flux_at"] = Vec3ToJson(t.maxFluxAt);
    j["in_w"] = t.inW;
    j["out_w"] = t.outW;
    j["imbalance_w"] = t.imbalanceW;
    json flows = json::array();
    for (const fem::HeatFlow& f : t.flows)
      flows.push_back({{"load", f.id},
                       {"type", fem::LoadTypeToString(f.type)},
                       {"watts", f.watts}});
    j["flows"] = std::move(flows);
  }
  if (t.load) {
    j["stress_free_c"] = t.stressFreeC;
    if (t.fromStudy != 0)
      j["from_study"] = t.fromStudy;
  }
  if (t.hasService) {
    j["service_max_c"] = t.serviceMaxC;
    j["service_basis"] = t.serviceBasis;
    j["margin_k"] = t.marginK;
  }
  if (t.bulkConductivity)
    j["bulk_conductivity"] = true;
  return j;
}

fem::StudyBodyResult::Thermal
ThermalFromJson(const json& j, const std::string& what, bool solved) {
  readctx::RefuseUnknownKeys(j,
                             {"max_c", "max_at", "min_c", "min_at", "base_c",
                              "max_heat_flux_w_m2", "max_heat_flux_at", "in_w",
                              "out_w", "imbalance_w", "flows", "stress_free_c",
                              "from_study", "service_max_c", "service_basis",
                              "margin_k", "bulk_conductivity"},
                             what + " thermal summary");
  fem::StudyBodyResult::Thermal t;
  t.solved = solved;
  t.load = !solved;
  t.maxC = j.value("max_c", 0.0);
  if (j.contains("max_at"))
    t.maxAt = Vec3FromJson(j.at("max_at"));
  t.minC = j.value("min_c", 0.0);
  if (j.contains("min_at"))
    t.minAt = Vec3FromJson(j.at("min_at"));
  t.baseC = j.value("base_c", 0.0);
  t.maxFluxWm2 = j.value("max_heat_flux_w_m2", 0.0);
  if (j.contains("max_heat_flux_at"))
    t.maxFluxAt = Vec3FromJson(j.at("max_heat_flux_at"));
  t.inW = j.value("in_w", 0.0);
  t.outW = j.value("out_w", 0.0);
  t.imbalanceW = j.value("imbalance_w", 0.0);
  for (const json& fj : j.value("flows", json::array())) {
    fem::HeatFlow f;
    f.id = fj.at("load").get<uint64_t>();
    const std::string type = fj.at("type").get<std::string>();
    if (!fem::LoadTypeFromString(type, f.type) || !fem::IsThermalLoad(f.type))
      throw std::runtime_error(
          what + ": a heat flow's type '" + type +
          "' is not one of: " + fem::kThermalLoadTypeNames);
    f.watts = fj.at("watts").get<double>();
    t.flows.push_back(f);
  }
  t.stressFreeC = j.value("stress_free_c", 0.0);
  t.fromStudy = j.value("from_study", uint64_t{0});
  if (j.contains("service_max_c")) {
    t.hasService = true;
    t.serviceMaxC = j.at("service_max_c").get<double>();
    t.serviceBasis = j.value("service_basis", std::string{});
    t.marginK = j.value("margin_k", 0.0);
  }
  t.bulkConductivity = j.value("bulk_conductivity", false);
  return t;
}

json BucklingToJson(const fem::StudyBodyResult::Buckling& k) {
  json out = {{"buckling_factor", k.buckles ? json(k.factor) : json()},
              {"knockdown", k.knockdown},
              {"class", k.shapeClass},
              {"yield_factor", k.yieldFactor},
              {"interlayer_factor", k.interlayerFactor},
              {"governing_factor", k.governingFactor}};
  if (!k.buckles)
    out["why_none"] = k.none;
  if (k.face >= 0)
    out["face"] = k.face;
  if (k.radiusMm > 0)
    out["radius_mm"] = k.radiusMm;
  if (k.wallMm > 0)
    out["wall_mm"] = k.wallMm;
  if (k.wallLayers > 0)
    out["wall_layers"] = k.wallLayers;
  if (k.atLoadEdge)
    out["mode_at_load_edge"] = {
        {"load", k.edgeLoad}, {"type", k.edgeType}, {"face", k.edgeFace}};
  return out;
}

// Each contact of a body (v209; docs/simulation/README.md#contact).
json ContactsToJson(
    const std::vector<fem::StudyBodyResult::ContactResult>& contacts) {
  json out = json::array();
  for (const auto& c : contacts)
    out.push_back(
        {{"id", c.id},
         {"participant", c.participant},
         {"name", c.name},
         {"friction", c.friction},
         {"penalty_mpa_per_mm", c.penaltyMPaPerMm},
         {"area_mm2", c.areaMm2},
         {"closed_mm2", c.closedMm2},
         {"force_n", {c.forceN.x, c.forceN.y, c.forceN.z}},
         {"moment_n_mm", {c.momentNmm.x, c.momentNmm.y, c.momentNmm.z}},
         {"normal_n", c.normalN},
         {"shear_n", c.shearN},
         {"max_pressure_mpa", c.maxPressureMPa},
         {"mean_pressure_mpa", c.meanPressureMPa}});
  return out;
}

std::vector<fem::StudyBodyResult::ContactResult>
ContactsFromJson(const json& j, const std::string& what) {
  std::vector<fem::StudyBodyResult::ContactResult> out;
  const auto vec = [&](const json& v, const char* key) {
    if (!v.is_array() || v.size() != 3)
      throw std::runtime_error(what + ": a contact's " + key +
                               " is not [x, y, z]");
    return glm::dvec3(v.at(0).get<double>(), v.at(1).get<double>(),
                      v.at(2).get<double>());
  };
  for (const json& cj : j) {
    readctx::RefuseUnknownKeys(cj,
                               {"id", "participant", "name", "friction",
                                "penalty_mpa_per_mm", "area_mm2", "closed_mm2",
                                "force_n", "moment_n_mm", "normal_n", "shear_n",
                                "max_pressure_mpa", "mean_pressure_mpa"},
                               what + " contact");
    fem::StudyBodyResult::ContactResult c;
    c.id = cj.at("id").get<uint64_t>();
    c.participant = cj.value("participant", false);
    c.name = cj.value("name", std::string{});
    c.friction = cj.value("friction", 0.0);
    c.penaltyMPaPerMm = cj.value("penalty_mpa_per_mm", 0.0);
    c.areaMm2 = cj.value("area_mm2", 0.0);
    c.closedMm2 = cj.value("closed_mm2", 0.0);
    c.forceN = vec(cj.at("force_n"), "force_n");
    c.momentNmm = vec(cj.at("moment_n_mm"), "moment_n_mm");
    c.normalN = cj.value("normal_n", 0.0);
    c.shearN = cj.value("shear_n", 0.0);
    c.maxPressureMPa = cj.value("max_pressure_mpa", 0.0);
    c.meanPressureMPa = cj.value("mean_pressure_mpa", 0.0);
    out.push_back(c);
  }
  return out;
}

// How a nonlinear body's run went (v208).
json NonlinearToJson(const fem::StudyBodyResult::Nonlinear& n) {
  json curve = json::array();
  for (const auto& [f, u] : n.curve)
    curve.push_back({f, u});
  json out = {
      {"factor", n.factor},         {"max_factor", n.maxFactor},
      {"increments", n.increments}, {"ccx_runs", n.ccxRuns},
      {"curve", std::move(curve)},  {"residual_criterion", n.residualCriterion},
      {"imbalance", n.imbalance}};
  if (n.firstYieldFactor > 0)
    out["first_yield_factor"] = n.firstYieldFactor;
  // A contact run's (v209): solved at small deformation, and what its
  // stabilizing springs carried.
  if (!n.largeDeformation)
    out["large_deformation"] = false;
  if (n.stabilization > 0)
    out["stabilization"] = n.stabilization;
  // A plastic run's (v210).
  if (n.plasticity) {
    json p = {{"unloaded", n.unloaded}, {"strain_limit", n.strainLimit}};
    if (n.collapsed)
      p["collapse_bracket"] = {n.factor, n.collapseHigh};
    out["plasticity"] = std::move(p);
  }
  if (n.snapped)
    out["snapped"] = {n.snappedFrom, n.snappedTo};
  if (n.snapThrough)
    out["snap_through"] = {{"linear_factor", n.linearFactor},
                           {"imperfection_mm", n.imperfectionMm},
                           {"imperfection_given", n.imperfectionGiven},
                           {"deflection_limit_mm", n.deflectionLimitMm},
                           {"peak_at", n.peakAt},
                           {"peak_factor", n.peakFactor},
                           {"peak_bracket", {n.peakLow, n.peakHigh}},
                           {"why_none", n.none}};
  return out;
}

fem::StudyBodyResult::Nonlinear NonlinearFromJson(const json& j,
                                                  const std::string& what) {
  readctx::RefuseUnknownKeys(
      j,
      {"factor", "max_factor", "increments", "ccx_runs", "curve",
       "residual_criterion", "imbalance", "first_yield_factor", "snapped",
       "snap_through", "large_deformation", "stabilization", "plasticity"},
      what + " nonlinear");
  fem::StudyBodyResult::Nonlinear n;
  n.solved = true;
  n.factor = j.at("factor").get<double>();
  n.maxFactor = j.value("max_factor", 1.0);
  n.increments = j.value("increments", 0);
  n.ccxRuns = j.value("ccx_runs", 0);
  for (const json& p : j.value("curve", json::array()))
    n.curve.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
  n.residualCriterion = j.value("residual_criterion", 0.0);
  n.imbalance = j.value("imbalance", 0.0);
  n.firstYieldFactor = j.value("first_yield_factor", 0.0);
  n.largeDeformation = j.value("large_deformation", true);
  n.stabilization = j.value("stabilization", 0.0);
  if (j.contains("plasticity")) { // v210
    const json& p = j.at("plasticity");
    readctx::RefuseUnknownKeys(p,
                               {"unloaded", "strain_limit", "collapse_bracket"},
                               what + " plasticity");
    n.plasticity = true;
    n.unloaded = p.value("unloaded", false);
    n.strainLimit = p.value("strain_limit", 0.0);
    if (p.contains("collapse_bracket")) {
      n.collapsed = true;
      n.collapseHigh = p.at("collapse_bracket").at(1).get<double>();
    }
  }
  if (j.contains("snapped")) {
    n.snapped = true;
    n.snappedFrom = j.at("snapped").at(0).get<double>();
    n.snappedTo = j.at("snapped").at(1).get<double>();
  }
  if (j.contains("snap_through")) {
    const json& s = j.at("snap_through");
    n.snapThrough = true;
    n.linearFactor = s.value("linear_factor", 0.0);
    n.imperfectionMm = s.value("imperfection_mm", 0.0);
    n.imperfectionGiven = s.value("imperfection_given", false);
    n.deflectionLimitMm = s.value("deflection_limit_mm", 0.0);
    n.peakAt = s.value("peak_at", std::string{});
    n.peakFactor = s.value("peak_factor", 0.0);
    if (s.contains("peak_bracket")) {
      n.peakLow = s.at("peak_bracket").at(0).get<double>();
      n.peakHigh = s.at("peak_bracket").at(1).get<double>();
    }
    n.none = s.value("why_none", std::string{});
    if (n.peakAt != "limit_point" && n.peakAt != "snap" &&
        n.peakAt != "deflection_limit" && n.peakAt != "none")
      throw std::runtime_error(what + ": a snap-through peak_at '" + n.peakAt +
                               "' (expected one of: limit_point, snap, "
                               "deflection_limit, none)");
  }
  return n;
}

fem::StudyBodyResult::Buckling BucklingFromJson(const json& j,
                                                const std::string& what) {
  fem::StudyBodyResult::Buckling k;
  k.solved = true;
  if (!j.contains("buckling_factor"))
    throw std::runtime_error(what +
                             ": a buckling summary without buckling_factor");
  k.buckles = !j.at("buckling_factor").is_null();
  if (k.buckles)
    k.factor = j.at("buckling_factor").get<double>();
  k.none = j.value("why_none", std::string{});
  k.knockdown = j.value("knockdown", 1.0);
  k.shapeClass = j.value("class", std::string{});
  k.yieldFactor = j.value("yield_factor", 0.0);
  k.interlayerFactor = j.value("interlayer_factor", 0.0);
  k.governingFactor = j.value("governing_factor", 0.0);
  k.face = j.value("face", -1);
  k.radiusMm = j.value("radius_mm", 0.0);
  k.wallMm = j.value("wall_mm", 0.0);
  k.wallLayers = j.value("wall_layers", 0);
  if (j.contains("mode_at_load_edge")) {
    const json& e = j.at("mode_at_load_edge");
    k.atLoadEdge = true;
    k.edgeLoad = e.value("load", uint64_t{0});
    k.edgeType = e.value("type", std::string{});
    k.edgeFace = e.value("face", -1);
  }
  return k;
}

// A static run's convergence check (v206), saved with its result.
json ConvergenceToJson(const fem::Convergence& c) {
  json out = {{"checked", c.checked}};
  if (!c.why.empty())
    out["why"] = c.why;
  if (!c.checked)
    return out;
  out["converged"] = c.converged;
  if (!c.stopped.empty())
    out["stopped"] = c.stopped;
  if (c.stoppedSeconds > 0.0)
    out["stopped_seconds"] = c.stoppedSeconds;
  out["last_change_pct"] = c.lastChangePct;
  if (c.hasLayerNormal)
    out["layer_normal"] = Vec3ToJson(c.layerNormal);
  json steps = json::array();
  for (const fem::ConvergenceStep& st : c.steps) {
    json refined = json::array();
    for (const fem::ConvergenceRefinement& r : st.refined)
      refined.push_back(
          {{"body", r.body}, {"face", r.face}, {"size_mm", r.sizeMm}});
    json peaks = json::array();
    for (const fem::ConvergencePeak& p : st.peaks) {
      json pj = {{"body", p.body}, {"quantity", p.quantity}, {"mpa", p.mpa}};
      if (p.away)
        pj["away"] = true;
      peaks.push_back(std::move(pj));
    }
    steps.push_back({{"refined", std::move(refined)},
                     {"nodes", st.nodes},
                     {"seconds", st.seconds},
                     {"peaks", std::move(peaks)}});
  }
  out["steps"] = std::move(steps);
  return out;
}

fem::Convergence ConvergenceFromJson(const json& j) {
  fem::Convergence c;
  c.checked = j.value("checked", false);
  c.why = j.value("why", std::string{});
  c.converged = j.value("converged", false);
  c.stopped = j.value("stopped", std::string{});
  c.stoppedSeconds = j.value("stopped_seconds", 0.0);
  c.lastChangePct = j.value("last_change_pct", 0.0);
  if (j.contains("layer_normal")) {
    c.hasLayerNormal = true;
    c.layerNormal = Vec3FromJson(j.at("layer_normal"));
  }
  for (const json& sj : j.value("steps", json::array())) {
    fem::ConvergenceStep st;
    for (const json& rj : sj.value("refined", json::array()))
      st.refined.push_back({rj.at("body").get<std::string>(),
                            rj.at("face").get<int>(),
                            rj.at("size_mm").get<double>()});
    st.nodes = sj.value("nodes", std::size_t{0});
    st.seconds = sj.value("seconds", 0.0);
    for (const json& pj : sj.value("peaks", json::array()))
      st.peaks.push_back({pj.at("body").get<std::string>(),
                          pj.at("quantity").get<std::string>(),
                          pj.value("away", false), pj.at("mpa").get<double>()});
    c.steps.push_back(std::move(st));
  }
  return c;
}

} // namespace

bool StudyResultToJson(const fem::StudyRuntime& runtime, json& out) {
  if (runtime.bodies.empty())
    return false; // nothing was solved
  // What is saved is what is kept (a stored runtime already is).
  fem::StudyRuntime kept = runtime;
  fem::KeepWhatIsRead(kept);
  const fem::StudyRuntime& rt = kept;
  ResultArrayWriter w;
  out = json::object();
  out["kind"] = KindOf(rt);
  if (rt.topology)
    out["topology"] = {{"iterations", rt.iterations},
                       {"converged", rt.converged},
                       {"cancelled", rt.cancelled},
                       {"volume_fraction", rt.volumeFractionAchieved},
                       {"compliance", rt.complianceHistory}};
  // What produced it (v148): a reopened project reuses the result for a run
  // whose inputs hash the same, and reads it stale when they do not.
  if (rt.inputHash != 0)
    out["input_hash"] = inputhash::Hex(rt.inputHash);
  // A CalculiX answer says so, with the version (v204): it is a second,
  // independent answer, and its receipt names that oracle after a reopen
  // too (docs/simulation/README.md#calculix). An in-house result writes
  // nothing here, so every one saved before re-saves byte for byte.
  if (rt.solver.rfind("calculix", 0) == 0)
    out["solver"] = rt.solver;
  if (rt.convergence.checked || !rt.convergence.why.empty())
    out["convergence"] = ConvergenceToJson(rt.convergence);
  if (!rt.topology)
    out["maxima"] = {{"von_mises", rt.maxVonMises},
                     {"von_mises_at", Vec3ToJson(rt.maxVmAt)},
                     {"von_mises_body", rt.maxVmBody},
                     {"displacement", rt.maxDisplacement},
                     {"displacement_at", Vec3ToJson(rt.maxDispAt)},
                     {"displacement_body", rt.maxDispBody}};
  // The load cases (v152): a topology run's weight and compliance each, a
  // static run's names (its fields are per body, below).
  if (!rt.caseNames.empty()) {
    json cases = json::array();
    for (size_t k = 0; k < rt.caseNames.size(); ++k) {
      json c = {{"name", rt.caseNames[k]}};
      if (rt.topology) {
        c["weight"] = k < rt.caseWeights.size() ? rt.caseWeights[k] : 1.0;
        c["compliance"] =
            k < rt.caseCompliance.size() ? rt.caseCompliance[k] : 0.0;
      }
      cases.push_back(std::move(c));
    }
    out["cases"] = std::move(cases);
  }
  if (rt.topology) {
    // What the keep-outs took and the connectivity filter removed (v152).
    if (!rt.keepOuts.empty()) {
      json ko = json::array();
      for (const fem::KeepOutReport& k : rt.keepOuts)
        ko.push_back(
            {{"what", k.what}, {"tets", k.tets}, {"volume", k.volume}});
      out["keep_outs"] = std::move(ko);
    }
    out["islands"] = {{"threshold", rt.islands.threshold},
                      {"components", rt.islands.components},
                      {"tets", rt.islands.tets},
                      {"volume", rt.islands.volume},
                      {"keep_faces", rt.islands.keepFaces},
                      {"load_path_broken", rt.islands.loadPathBroken}};
  }
  // A static study of a topology result: the cut it solved, and which
  // result it was cut from (its density digest), so it stays current after
  // a reopen only while that result does.
  if (rt.sourceDigest != 0)
    out["shape"] = {{"source", inputhash::Hex(rt.sourceDigest)},
                    {"threshold", rt.shapeThreshold},
                    {"island_tets", rt.shapeIslandTets},
                    {"volume", rt.shapeVolume},
                    {"threshold_lowered_from", rt.shapeThresholdLoweredFrom}};
  json bodies = json::array();
  for (const fem::StudyBodyResult& b : rt.bodies) {
    // The solved mesh's counts (v154): a static body keeps its surface only.
    json bj = {{"body_index", b.bodyIndex},
               {"body", b.bodyName},
               {"node_count", b.mesh.stats.nodeCount},
               {"tet_count", b.mesh.stats.tetCount},
               {"nodes", w.F32(b.mesh.nodes)}};
    if (!b.mesh.tets.empty()) {
      std::vector<int> tets;
      tets.reserve(b.mesh.tets.size() * 10);
      for (const std::array<int, 10>& t : b.mesh.tets)
        tets.insert(tets.end(), t.begin(), t.end());
      bj["tets"] = w.Index(tets);
    }
    std::vector<int> tris, faces;
    tris.reserve(b.mesh.boundary.size() * 6);
    faces.reserve(b.mesh.boundary.size());
    for (const fem::FemMesh::BoundaryTri& tr : b.mesh.boundary) {
      tris.insert(tris.end(), tr.nodes.begin(), tr.nodes.end());
      faces.push_back(tr.faceIndex);
    }
    bj["boundary"] = w.Index(tris);
    bj["boundary_face"] = w.Index(faces);
    if (!b.mesh.tetDomain.empty())
      bj["tet_domain"] = w.Index(b.mesh.tetDomain);
    if (b.mesh.stats.worstQuality != 0.0)
      bj["worst_quality"] = b.mesh.stats.worstQuality;
    // v206: which support's edge each node lies near, one past its index
    // (0 = none), so a field read later is flagged as von Mises was.
    if (b.supportEdgeZone.size() == b.mesh.nodes.size() &&
        !b.supportEdgeZone.empty()) {
      std::vector<int> zone(b.supportEdgeZone.size());
      for (size_t i = 0; i < zone.size(); ++i)
        zone[i] = b.supportEdgeZone[i] + 1;
      bj["support_edge_zone"] = w.Index(zone);
      // Whose edge each zone is: a support's, or a load's on part of a face.
      json owners = json::array();
      for (const fem::FemResults::SupportReaction& o : b.edgeOwners)
        owners.push_back(
            {{"load", o.support}, {"type", fem::LoadTypeToString(o.type)}});
      bj["support_edge_owners"] = std::move(owners);
    }
    if (rt.topology) {
      bj["density"] = w.F32(b.density);
    } else if (!b.cases.empty()) {
      // `results` is the first case's copy: written once.
      json cases = json::array();
      for (const fem::FemResults& c : b.cases)
        cases.push_back(FieldsToJson(c, w));
      bj["cases"] = std::move(cases);
    } else if (!b.results.displacement.empty() || !b.results.modes.empty() ||
               !b.results.temperature.empty()) {
      bj["results"] = FieldsToJson(b.results, w);
    }
    if (b.buckling.solved)
      bj["buckling"] = BucklingToJson(b.buckling);
    if (b.thermal.solved || b.thermal.load) // v212
      bj["thermal"] = ThermalToJson(b.thermal);
    if (b.nonlinear.solved)
      bj["nonlinear"] = NonlinearToJson(b.nonlinear);
    if (!b.contacts.empty())
      bj["contacts"] = ContactsToJson(b.contacts);
    bodies.push_back(std::move(bj));
  }
  out["bodies"] = std::move(bodies);
  out[kResultData] = w.Take();
  out["bin_bytes"] = out[kResultData].get_binary().size();
  return true;
}

fem::StudyRuntime StudyResultFromJson(const json& j) {
  fem::StudyRuntime rt;
  const std::string kind =
      j.value("kind", std::string(j.contains("topology") ? "topology" : ""));
  if (kind != "topology" && kind != "static" && kind != "modal" &&
      kind != "buckling" && kind != "thermal")
    throw std::runtime_error("study result: kind '" + kind +
                             "' (expected one of: static, modal, topology, "
                             "buckling, thermal)");
  const std::string what = kind + " result";
  const ResultArrayReader bin(j, what);
  const FieldReader f{bin};
  rt.topology = kind == "topology";
  if (rt.topology) {
    const json& s = j.at("topology");
    rt.iterations = s.value("iterations", 0);
    rt.converged = s.value("converged", false);
    rt.cancelled = s.value("cancelled", false);
    rt.volumeFractionAchieved = s.value("volume_fraction", 0.0);
    rt.complianceHistory = s.value("compliance", std::vector<double>{});
  }
  if (j.contains("input_hash") &&
      !inputhash::FromHex(j.at("input_hash").get<std::string>(), rt.inputHash))
    throw std::runtime_error(what + ": input_hash " +
                             j.at("input_hash").dump() +
                             " is not 0x and 16 hex digits");
  if (j.contains("solver")) {
    rt.solver = j.at("solver").get<std::string>();
    if (rt.solver.rfind("calculix ", 0) != 0)
      throw std::runtime_error(what + ": solver '" + rt.solver +
                               "' (a saved result names only a CalculiX "
                               "solve, as 'calculix 2.NN')");
  }
  if (j.contains("convergence")) // v206
    rt.convergence = ConvergenceFromJson(j.at("convergence"));
  if (j.contains("maxima")) {
    const json& m = j.at("maxima");
    rt.maxVonMises = m.value("von_mises", 0.0);
    if (m.contains("von_mises_at"))
      rt.maxVmAt = Vec3FromJson(m.at("von_mises_at"));
    rt.maxVmBody = m.value("von_mises_body", std::string{});
    rt.maxDisplacement = m.value("displacement", 0.0);
    if (m.contains("displacement_at"))
      rt.maxDispAt = Vec3FromJson(m.at("displacement_at"));
    rt.maxDispBody = m.value("displacement_body", std::string{});
  }
  if (j.contains("shape")) {
    const json& s = j.at("shape");
    if (!inputhash::FromHex(s.at("source").get<std::string>(), rt.sourceDigest))
      throw std::runtime_error("shape result: source " + s.at("source").dump() +
                               " is not 0x and 16 hex digits");
    rt.shapeThreshold = s.value("threshold", 0.0);
    rt.shapeIslandTets = s.value("island_tets", 0);
    rt.shapeVolume = s.value("volume", 0.0);
    rt.shapeThresholdLoweredFrom = s.value("threshold_lowered_from", 0.0);
  }
  for (const json& bj : j.value("bodies", json::array())) {
    fem::StudyBodyResult b;
    b.bodyIndex = bj.value("body_index", -1);
    b.bodyName = bj.value("body", std::string{});
    b.mesh.nodes = f.Vec3s(bj.at("nodes"), "nodes");
    const auto tets = bj.contains("tets") ? f.Indices(bj.at("tets"), "tets")
                                          : std::vector<int>{};
    if (tets.size() % 10 != 0)
      throw std::runtime_error(what + ": tets is not a whole number of tet10s");
    for (size_t i = 0; i + 10 <= tets.size(); i += 10) {
      std::array<int, 10> t;
      for (int k = 0; k < 10; ++k)
        t[k] = tets[i + k];
      b.mesh.tets.push_back(t);
    }
    const auto boundary = bj.contains("boundary")
                              ? f.Indices(bj.at("boundary"), "boundary")
                              : std::vector<int>{};
    if (bj.contains("boundary_face")) {
      // v154: six nodes a triangle, and its face apart.
      const auto faces = f.Indices(bj.at("boundary_face"), "boundary_face");
      if (boundary.size() != faces.size() * 6)
        throw std::runtime_error(what + ": " + std::to_string(faces.size()) +
                                 " boundary faces for " +
                                 std::to_string(boundary.size()) +
                                 " boundary nodes (six a triangle)");
      for (size_t t = 0; t < faces.size(); ++t) {
        fem::FemMesh::BoundaryTri tr;
        for (int k = 0; k < 6; ++k)
          tr.nodes[k] = boundary[t * 6 + k];
        tr.faceIndex = faces[t];
        b.mesh.boundary.push_back(tr);
      }
    } else {
      // v153: six nodes and the face, inline.
      for (size_t i = 0; i + 7 <= boundary.size(); i += 7) {
        fem::FemMesh::BoundaryTri tr;
        for (int k = 0; k < 6; ++k)
          tr.nodes[k] = boundary[i + k];
        tr.faceIndex = boundary[i + 6];
        b.mesh.boundary.push_back(tr);
      }
    }
    if (bj.contains("tet_domain"))
      b.mesh.tetDomain = f.Indices(bj.at("tet_domain"), "tet_domain");
    b.mesh.stats.nodeCount = bj.value("node_count", b.mesh.nodes.size());
    b.mesh.stats.tetCount = bj.value("tet_count", b.mesh.tets.size());
    b.mesh.stats.worstQuality = bj.value("worst_quality", 0.0);
    if (bj.contains("density"))
      b.density = f.Reals(bj.at("density"), "density");
    if (bj.contains("support_edge_zone")) { // v206
      b.supportEdgeZone =
          f.Indices(bj.at("support_edge_zone"), "support_edge_zone");
      if (b.supportEdgeZone.size() != b.mesh.nodes.size())
        throw std::runtime_error(what + ": body '" + b.bodyName + "' has " +
                                 std::to_string(b.supportEdgeZone.size()) +
                                 " support-edge zones for " +
                                 std::to_string(b.mesh.nodes.size()) +
                                 " nodes");
      for (int& z : b.supportEdgeZone)
        z -= 1;
      for (const json& oj : bj.value("support_edge_owners", json::array())) {
        fem::FemResults::SupportReaction o;
        o.support = oj.at("load").get<uint64_t>();
        const std::string t = oj.at("type").get<std::string>();
        if (!fem::LoadTypeFromString(t, o.type))
          throw std::runtime_error(what + ": an edge's load type '" + t +
                                   "' is not one of: " + fem::kLoadTypeNames);
        b.edgeOwners.push_back(o);
      }
      for (int z : b.supportEdgeZone)
        if (z >= static_cast<int>(b.edgeOwners.size()))
          throw std::runtime_error(
              what + ": body '" + b.bodyName + "' names edge " +
              std::to_string(z) + " of " + std::to_string(b.edgeOwners.size()));
    }
    for (const json& cj : bj.value("cases", json::array()))
      b.cases.push_back(FieldsFromJson(cj, f));
    if (!b.cases.empty())
      b.results = b.cases.front();
    else if (bj.contains("results"))
      b.results = FieldsFromJson(bj.at("results"), f);
    if (bj.contains("nonlinear")) {
      if (kind != "static")
        throw std::runtime_error(what + ": body '" + b.bodyName +
                                 "' carries a nonlinear summary, which is a "
                                 "static result's");
      b.nonlinear = NonlinearFromJson(bj.at("nonlinear"), what);
    }
    if (bj.contains("contacts")) {
      if (kind != "static")
        throw std::runtime_error(what + ": body '" + b.bodyName +
                                 "' carries contacts, which are a static "
                                 "result's");
      b.contacts = ContactsFromJson(bj.at("contacts"), what);
    }
    if (bj.contains("thermal")) { // v212
      if (kind != "thermal" && kind != "static")
        throw std::runtime_error(what + ": body '" + b.bodyName +
                                 "' carries a thermal summary, which is a "
                                 "thermal result's or a thermally loaded "
                                 "one's");
      b.thermal = ThermalFromJson(bj.at("thermal"), what, kind == "thermal");
    } else if (kind == "thermal") {
      throw std::runtime_error(what + ": body '" + b.bodyName +
                               "' has no thermal summary");
    }
    if (kind == "buckling") {
      if (!bj.contains("buckling"))
        throw std::runtime_error(what + ": body '" + b.bodyName +
                                 "' has no buckling summary");
      b.buckling = BucklingFromJson(bj.at("buckling"), what);
    }
    CheckBody(b, what);
    rt.bodies.push_back(std::move(b));
  }
  for (const json& cj : j.value("cases", json::array())) {
    rt.caseNames.push_back(cj.at("name").get<std::string>());
    if (rt.topology) {
      rt.caseWeights.push_back(cj.value("weight", 1.0));
      rt.caseCompliance.push_back(cj.value("compliance", 0.0));
    }
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
  // A v153 file's doubles, and its whole static mesh, become what a run
  // keeps now.
  fem::KeepWhatIsRead(rt);
  if (rt.topology && !rt.bodies.empty())
    rt.densityDigest =
        fem::DensityDigest(rt.bodies.front().mesh, rt.bodies.front().density);
  return rt;
}

} // namespace DocumentIO
