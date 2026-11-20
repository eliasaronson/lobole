// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemCheck.h"

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/fem/FemSolver.h"

#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>

#include <map>

namespace fem {

namespace {

using json = nlohmann::json;

TopoDS_Shape FaceAt(const TopoDS_Shape& shape, int index) {
  int i = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next(), ++i)
    if (i == index)
      return ex.Current();
  return {};
}

} // namespace

std::vector<StudyProblem> CheckStudy(const Document& doc, const Study& original,
                                     const std::vector<CheckMesh>* meshes,
                                     bool backstop) {
  std::vector<StudyProblem> out;
  Study study = original; // expressions are evaluated into the copy
  const uint64_t sid = study.id;
  const auto add = [&](const char* code, std::string message) -> StudyProblem& {
    StudyProblem p;
    p.code = code;
    p.message = std::move(message);
    out.push_back(std::move(p));
    return out.back();
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err)) {
    StudyProblem& p = add("expression", err);
    p.fixNote = "correct the expression where it was typed (set_study, "
                "add_load or add_refinement), or define the variable it "
                "reads (set_variable)";
  }
  err.clear();
  if (!ValidateLoadCases(study, &err)) {
    StudyProblem& p = add("load_cases", err);
    p.fixMethod = "set_study";
    p.fixParams = {{"id", sid}, {"load_cases", nullptr}};
    p.fixNote = "load_cases: the list of {name, weight} every load's `case` "
                "names";
  }
  if (const std::string why = ContactRefusal(study); !why.empty()) {
    StudyProblem& p = add("setup", why);
    p.fixMethod = "set_study";
    p.fixParams = {{"id", sid}};
    p.fixNote = "the message names the setting to change";
  }
  if (const std::string why = ThermalRefusal(study); !why.empty()) {
    StudyProblem& p = add("setup", why);
    p.fixMethod = "set_study";
    p.fixParams = {{"id", sid}};
    p.fixNote = "the message names the setting to change";
  }
  // A roller, held or cylindrical support that cannot mean what it says
  // (docs/simulation/README.md
  // #rollers-held-directions-and-cylindrical-supports).
  for (const Load& l : study.loads) {
    if (!IsFrameSupport(l.type))
      continue;
    Study one = study;
    one.loads = {l};
    if (const std::string why = FrameSupportRefusal(one); !why.empty()) {
      StudyProblem& p = add("setup", why);
      p.loadId = l.id;
      p.hasFace = true;
      p.face = l.face;
      p.fixMethod = "remove_load";
      p.fixParams = {{"study", sid}, {"id", l.id}};
      p.fixNote = "then add_load it again with what the message says it "
                  "takes";
    }
  }

  if (study.loads.empty()) {
    StudyProblem& p = add("no_loads", "the study has no loads or supports");
    p.fixMethod = "add_load";
    p.fixParams = {{"study", sid}, {"type", "fixed"}, {"face", nullptr}};
    p.fixNote = "face: the face that is held (list_elements names them); "
                "then a force, pressure, torque or remote load the same way";
    return out;
  }

  // --- Every anchor, and the loads grouped by the body they land on.
  const auto& bodies = doc.GetBodies();
  struct Placed {
    int faceIndex;
    size_t load;
  };
  std::map<int, std::vector<Placed>> perBody;
  for (size_t i = 0; i < study.loads.size(); ++i) {
    const Load& l = study.loads[i];
    ResolvedFace rf;
    if (l.type == LoadType::HeatGeneration) {
      int index = -1, hits = 0;
      for (size_t b = 0; b < bodies.size(); ++b)
        if (!bodies[b].reference && bodies[b].name == l.body) {
          index = static_cast<int>(b);
          ++hits;
        }
      if (hits != 1) {
        StudyProblem& p =
            add("anchor", "heat_generation " + std::to_string(l.id) +
                              (hits == 0 ? ": no body is named '"
                                         : ": more than one body is named '") +
                              l.body + "'");
        p.loadId = l.id;
        p.fixMethod = "remove_load";
        p.fixParams = {{"study", sid}, {"id", l.id}};
        p.fixNote = "then add_load it again with `body` naming one body";
        continue;
      }
      perBody[index].push_back({-1, i});
      continue;
    }
    err.clear();
    if (!ResolveFace(doc, l.face, "load " + std::to_string(i + 1), rf, &err)) {
      StudyProblem& p = add("anchor", err);
      p.loadId = l.id;
      p.fixMethod = "remove_load";
      p.fixParams = {{"study", sid}, {"id", l.id}};
      p.fixNote = "its face is gone from the part; then add_load it again "
                  "on a face that exists (list_elements names them)";
      continue;
    }
    perBody[rf.bodyIndex].push_back({rf.faceIndex, i});
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    err.clear();
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err)) {
      StudyProblem& p = add("refinement_anchor", err);
      p.fixMethod = "remove_refinement";
      p.fixParams = {{"study", sid}, {"face", study.refinements[i].face.name}};
      p.fixNote = "its face is gone from the part";
    }
  }

  const bool thermalStudy = study.type == AnalysisType::Thermal;
  const bool gravity =
      study.gravity.x != 0 || study.gravity.y != 0 || study.gravity.z != 0;
  for (const auto& [bodyIndex, placed] : perBody) {
    const Body& body = bodies[bodyIndex];
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    const auto material = [&](std::string why) {
      StudyProblem& p = add("material", std::move(why));
      p.body = body.name;
      p.fixMethod = "set_material";
      p.fixParams = {{"body_name", body.name}, {"material", nullptr}};
      p.fixNote = "material: a library material that has the property "
                  "(describe's `materials` lists each one's numbers)";
    };
    if (!thermalStudy)
      if (const std::string why = CADMaterials::ElasticDataRefusal(mp);
          !why.empty())
        material("body '" + body.name + "': material '" +
                 (body.material.empty() ? "default" : body.material) + "' " +
                 why);
    if (mp.density <= 0.0f && (study.type == AnalysisType::Modal || gravity))
      material(
          "body '" + body.name + "': material has no density (needed for " +
          (study.type == AnalysisType::Modal ? "modal analysis" : "gravity") +
          ")");

    bool held = false, sink = false;
    for (const Placed& pl : placed) {
      const Load& l = study.loads[pl.load];
      held = held || IsSupport(l.type);
      sink = sink || l.type == LoadType::Temperature ||
             l.type == LoadType::Convection;
    }
    if (thermalStudy && !sink) {
      StudyProblem& p =
          add("no_support",
              "body '" + body.name +
                  "': its heat has nowhere to go -- no face of it is held at "
                  "a temperature or cooled by convection, so its temperature "
                  "is not determined");
      p.body = body.name;
      p.fixMethod = "add_load";
      p.fixParams = {{"study", sid},
                     {"type", "temperature"},
                     {"face", nullptr},
                     {"temperature_c", nullptr}};
      p.fixNote = "face and temperature_c: the face that is held and what "
                  "at; or a convection load {film_w_m2k, ambient_c}";
    }
    if (!thermalStudy && !held) {
      StudyProblem& p = add("no_support", "body '" + body.name +
                                              "' has loads but no fixed "
                                              "support");
      p.body = body.name;
      p.fixMethod = "add_load";
      p.fixParams = {{"study", sid}, {"type", "fixed"}, {"face", nullptr}};
      p.fixNote = "face: the face of '" + body.name +
                  "' that is held (list_elements names them)";
    }

    for (const Placed& pl : placed) {
      const Load& l = study.loads[pl.load];
      if (pl.faceIndex < 0 || IsThermalLoad(l.type))
        continue;
      if (l.type == LoadType::Pin && study.type == AnalysisType::Modal) {
        StudyProblem& p =
            add("load_geometry",
                "load " + std::to_string(l.id) +
                    ": a modal study's supports are fixed; a pin holds where "
                    "a load presses the part onto it, and a mode has none");
        p.loadId = l.id;
        p.hasFace = true;
        p.face = l.face;
        p.fixMethod = "remove_load";
        p.fixParams = {{"study", sid}, {"id", l.id}};
        p.fixNote = "then add_load {type: fixed} on the same face";
      }
      if (l.type != LoadType::Contact) {
        Load copy = l;
        err.clear();
        if (!CompleteLoadGeometry(FaceAt(body.shape.GetShape(), pl.faceIndex),
                                  copy, &err)) {
          StudyProblem& p = add("load_geometry", err);
          p.loadId = l.id;
          p.hasFace = true;
          p.face = l.face;
          p.fixMethod = "remove_load";
          p.fixParams = {{"study", sid}, {"id", l.id}};
          p.fixNote =
              l.type == LoadType::Torque
                  ? "then add_load {type: torque} again with axis_origin_mm "
                    "and axis_dir, or on a cylindrical face"
              : l.type == LoadType::Remote
                  ? "then add_load {type: remote} again with point_mm"
                  : "then add_load it again on a cylindrical face";
        }
      }
      // What only a mesh shows: a region that takes none of its face.
      if (meshes != nullptr && !l.region.IsWhole())
        for (const CheckMesh& bm : *meshes) {
          if (bm.bodyIndex != bodyIndex || bm.mesh == nullptr)
            continue;
          bool any = false;
          for (const FemMesh::BoundaryTri& tri : bm.mesh->boundary) {
            if (tri.faceIndex != pl.faceIndex)
              continue;
            glm::dvec3 c(0.0);
            for (int k = 0; k < 3; ++k)
              c += bm.mesh->nodes[tri.nodes[k]];
            if (l.region.Contains(c / 3.0)) {
              any = true;
              break;
            }
          }
          if (!any) {
            StudyProblem& p =
                add("region", "body '" + body.name + "': load " +
                                  std::to_string(l.id) +
                                  "'s region holds none of its face's mesh "
                                  "facets");
            p.loadId = l.id;
            p.hasFace = true;
            p.face = l.face;
            p.fixMethod = "remove_load";
            p.fixParams = {{"study", sid}, {"id", l.id}};
            p.fixNote = "then add_load it again with a `region` that "
                        "reaches the face, or a finer mesh there "
                        "(add_refinement)";
          }
        }
    }
  }

  // The backstop: what the planner refuses that is not itemised above.
  if (out.empty() && backstop) {
    StudyPlan plan;
    err.clear();
    Study scratch = original;
    if (study.type == AnalysisType::Topology) {
      // A topology run plans inside its own job; its setup rules above are
      // the ones shared with it.
    } else if (!PlanStudy(doc, scratch, plan, &err)) {
      add("plan", err);
    }
  }
  return out;
}

} // namespace fem
