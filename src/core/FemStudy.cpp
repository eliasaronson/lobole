#include "core/FemStudy.h"

#include "CADMaterials.h"
#include "core/Document.h"
#include "core/FemSolver.h"
#include "core/FemTopo.h"

#include <map>
#include <set>
#include <utility>

namespace fem {

const char* LoadTypeToString(LoadType type) {
  switch (type) {
    case LoadType::Fixed:
      return "fixed";
    case LoadType::Force:
      return "force";
    case LoadType::Pressure:
      return "pressure";
  }
  return "fixed";
}

bool LoadTypeFromString(const std::string& s, LoadType& out) {
  if (s == "fixed") {
    out = LoadType::Fixed;
    return true;
  }
  if (s == "force") {
    out = LoadType::Force;
    return true;
  }
  if (s == "pressure") {
    out = LoadType::Pressure;
    return true;
  }
  return false;
}

const char* AnalysisTypeToString(AnalysisType type) {
  switch (type) {
    case AnalysisType::Modal: return "modal";
    case AnalysisType::Topology: return "topology";
    case AnalysisType::Static: break;
  }
  return "static";
}

bool AnalysisTypeFromString(const std::string& s, AnalysisType& out) {
  if (s == "static") { out = AnalysisType::Static; return true; }
  if (s == "modal") { out = AnalysisType::Modal; return true; }
  if (s == "topology") { out = AnalysisType::Topology; return true; }
  return false;
}

bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols, std::string* error) {
  bool ok = true;
  // Evaluate one value+expr pair; on failure records the first error but keeps
  // going so every bad field is left with its last good value.
  auto eval = [&](const std::string& text, double& value, const std::string& what) {
    if (text.empty()) return;
    ExprError err;
    const auto v = ExpressionEngine::Default().EvaluateText(text, symbols, &err);
    if (v.has_value()) {
      value = *v;
    } else if (ok) {
      ok = false;
      if (error) *error = what + " expression '" + text + "': " + err.message;
    }
  };

  eval(study.meshSizeExpr, study.meshSize, "mesh size");
  for (auto& r : study.refinements) eval(r.sizeExpr, r.size, "refinement size");
  for (auto& l : study.loads) eval(l.magnitudeExpr, l.magnitude, "load magnitude");
  if (study.type == AnalysisType::Topology) {
    eval(study.volumeFractionExpr, study.volumeFraction, "volume fraction");
    eval(study.filterRadiusExpr, study.filterRadius, "filter radius");
  }
  return ok;
}

bool ResolveFace(const Document& doc, const ElementRef& ref, const std::string& what,
                 ResolvedFace& out, std::string* error) {
  const auto& bodies = doc.GetBodies();
  for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
    if (bodies[i].createdBy != ref.featureId) continue;
    if (const auto idx = bodies[i].elementMap.Resolve(ref.name)) {
      out.bodyIndex = i;
      out.faceIndex = *idx;
      return true;
    }
  }
  if (error) *error = what + ": face '" + ref.name + "' no longer exists";
  return false;
}

bool MeshStudy(const Document& doc, Study& study, std::vector<StudyBodyMesh>& out,
               std::string* error) {
  out.clear();
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error) *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err)) return fail(err);

  // Resolve every ref, grouping per body. Per-face refinement sizes are keyed
  // by resolved face index (what fem::Mesh's MeshSettings.faceSizes expects).
  std::map<int, MeshSettings> perBody;  // bodyIndex -> settings
  auto touch = [&](int bodyIndex) -> MeshSettings& {
    MeshSettings& s = perBody[bodyIndex];
    s.maxSize = study.meshSize;  // 0 => auto, per body
    return s;
  };

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face,
                     "load " + std::to_string(i + 1), rf, &err))
      return fail(err);
    touch(rf.bodyIndex);  // ensure the loaded body is meshed
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    touch(rf.bodyIndex).faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  if (perBody.empty())
    return fail("study has no loads or refinements to mesh");

  const auto& bodies = doc.GetBodies();
  for (const auto& [bodyIndex, settings] : perBody) {
    auto mesh = Mesh(bodies[bodyIndex].shape.GetShape(), settings, &err);
    if (!mesh)
      return fail("body '" + bodies[bodyIndex].name + "': " + err);
    StudyBodyMesh r;
    r.bodyIndex = bodyIndex;
    r.bodyName = bodies[bodyIndex].name;
    r.mesh = std::move(*mesh);
    out.push_back(std::move(r));
  }
  return true;
}

bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt, std::string* error) {
  rt = StudyRuntime{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error) *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err)) return fail(err);

  const auto& bodies = doc.GetBodies();

  // Group every load by the body it resolves to, and collect per-face
  // refinement sizes. A refinement alone does not make a body "loaded".
  struct BodyPlan {
    std::vector<std::pair<int, size_t>> loads;  // (faceIndex, load index)
    MeshSettings settings;
  };
  std::map<int, BodyPlan> perBody;

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1), rf, &err))
      return fail(err);
    BodyPlan& bp = perBody[rf.bodyIndex];
    bp.settings.maxSize = study.meshSize;
    bp.loads.push_back({rf.faceIndex, i});
  }
  if (perBody.empty()) return fail("study has no loads to solve");

  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    // Only refine bodies that are actually solved.
    auto it = perBody.find(rf.bodyIndex);
    if (it != perBody.end())
      it->second.settings.faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  for (auto& [bodyIndex, plan] : perBody) {
    const Body& body = bodies[bodyIndex];

    // Material must have stiffness data (0 = unknown, like density).
    const auto matType = CADMaterials::MaterialLibrary::FromSlug(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    if (mp.youngGPa <= 0.0f)
      return fail("body '" + body.name + "': material '" +
                  (body.material.empty() ? "default" : body.material) +
                  "' has no stiffness data");

    // Each loaded body needs at least one fixed support.
    bool hasFixed = false;
    for (const auto& [faceIndex, loadIdx] : plan.loads)
      if (study.loads[loadIdx].type == LoadType::Fixed) hasFixed = true;
    if (!hasFixed)
      return fail("body '" + body.name + "' has loads but no fixed support");

    auto mesh = Mesh(body.shape.GetShape(), plan.settings, &err);
    if (!mesh) return fail("body '" + body.name + "': " + err);

    // Build fixed-node set and face tractions from the resolved faces. Fixed
    // faces contribute all their boundary-tri nodes; force/pressure faces
    // contribute their tri indices.
    std::set<int> fixedSet;
    std::vector<FaceLoad> faceLoads;
    for (const auto& [faceIndex, loadIdx] : plan.loads) {
      const Load& load = study.loads[loadIdx];
      if (load.type == LoadType::Fixed) {
        for (const auto& tri : mesh->boundary)
          if (tri.faceIndex == faceIndex)
            for (int n : tri.nodes) fixedSet.insert(n);
      } else {
        FaceLoad fl;
        fl.type = load.type;
        fl.magnitude = load.magnitude;
        fl.direction = load.direction;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex) fl.tris.push_back(t);
        faceLoads.push_back(std::move(fl));
      }
    }
    if (fixedSet.empty())
      return fail("body '" + body.name + "': fixed face meshed to no nodes");

    // GPa -> MPa; density kg/m^3 -> tonne/mm^3 for the consistent mm-N-t-s
    // system (so gravity loads and modal frequencies come out right).
    Material mat{mp.youngGPa * 1000.0, mp.poisson, mp.density * 1e-12};
    std::vector<int> fixedNodes(fixedSet.begin(), fixedSet.end());

    StudyBodyResult sbr;
    sbr.bodyIndex = bodyIndex;
    sbr.bodyName = body.name;

    if (study.type == AnalysisType::Modal) {
      if (mp.density <= 0.0f)
        return fail("body '" + body.name + "': material has no density (needed "
                    "for modal analysis)");
      auto modes = SolveModal(*mesh, mat, fixedNodes, study.numModes, &err);
      if (!modes) return fail("body '" + body.name + "': " + err);
      sbr.results.modes = std::move(*modes);
    } else {
      const glm::dvec3 g = study.gravity * 1000.0;  // m/s^2 -> mm/s^2
      if ((g.x != 0 || g.y != 0 || g.z != 0) && mp.density <= 0.0f)
        return fail("body '" + body.name + "': material has no density (needed "
                    "for gravity)");
      auto res = Solve(*mesh, mat, fixedNodes, faceLoads, &err, g);
      if (!res) return fail("body '" + body.name + "': " + err);
      if (res->maxVonMises > rt.maxVonMises) {
        rt.maxVonMises = res->maxVonMises;
        rt.maxVmAt = res->maxVmAt;
        rt.maxVmBody = body.name;
      }
      if (res->maxDisplacement > rt.maxDisplacement) {
        rt.maxDisplacement = res->maxDisplacement;
        rt.maxDispAt = res->maxDispAt;
        rt.maxDispBody = body.name;
      }
      sbr.results = std::move(*res);
    }
    sbr.mesh = std::move(*mesh);
    rt.bodies.push_back(std::move(sbr));
  }
  return true;
}

bool RunTopology(const Document& doc, Study& study, StudyRuntime& rt,
                 std::string* error) {
  rt = StudyRuntime{};
  rt.topology = true;
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error) *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err)) return fail(err);
  if (study.volumeFraction <= 0.0 || study.volumeFraction >= 1.0)
    return fail("volume fraction must be in (0, 1)");

  const auto& bodies = doc.GetBodies();

  // Group loads by body (v1 optimizes exactly one loaded body).
  std::map<int, std::vector<std::pair<int, size_t>>> bodyLoads;  // (faceIndex, loadIdx)
  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1), rf, &err))
      return fail(err);
    bodyLoads[rf.bodyIndex].push_back({rf.faceIndex, i});
  }
  if (bodyLoads.empty()) return fail("study has no loads");
  if (bodyLoads.size() > 1)
    return fail("topology optimization supports a single loaded body (v1)");
  const int bodyIndex = bodyLoads.begin()->first;
  const Body& body = bodies[bodyIndex];

  MeshSettings settings;
  settings.maxSize = study.meshSize;
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      settings.faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  const auto matType = CADMaterials::MaterialLibrary::FromSlug(body.material);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
  if (mp.youngGPa <= 0.0f)
    return fail("body '" + body.name + "': material '" +
                (body.material.empty() ? "default" : body.material) +
                "' has no stiffness data");

  // Keep faces (frozen solid beyond the auto-frozen support/load skin).
  std::set<int> keepFaceIdx;
  for (size_t i = 0; i < study.keepFaces.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.keepFaces[i], "keep face " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex) keepFaceIdx.insert(rf.faceIndex);
  }

  auto mesh = Mesh(body.shape.GetShape(), settings, &err);
  if (!mesh) return fail("body '" + body.name + "': " + err);

  // Fixed nodes + face tractions + keep nodes, exactly as the static solve
  // resolves them (fixed faces contribute their nodes; force/pressure faces
  // their tri indices; keep faces their nodes).
  std::set<int> fixedSet, keepSet;
  std::vector<FaceLoad> faceLoads;
  bool hasFixed = false, hasApplied = false;
  for (const auto& [faceIndex, loadIdx] : bodyLoads[bodyIndex]) {
    const Load& load = study.loads[loadIdx];
    if (load.type == LoadType::Fixed) {
      hasFixed = true;
      for (const auto& tri : mesh->boundary)
        if (tri.faceIndex == faceIndex)
          for (int n : tri.nodes) fixedSet.insert(n);
    } else {
      hasApplied = true;
      FaceLoad fl;
      fl.type = load.type;
      fl.magnitude = load.magnitude;
      fl.direction = load.direction;
      for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
        if (mesh->boundary[t].faceIndex == faceIndex) fl.tris.push_back(t);
      faceLoads.push_back(std::move(fl));
    }
  }
  if (!hasFixed) return fail("body '" + body.name + "' has no fixed support");
  if (!hasApplied)
    return fail("topology optimization needs a force or pressure load");
  if (fixedSet.empty())
    return fail("body '" + body.name + "': fixed face meshed to no nodes");
  for (int fi : keepFaceIdx)
    for (const auto& tri : mesh->boundary)
      if (tri.faceIndex == fi)
        for (int n : tri.nodes) keepSet.insert(n);

  // Density is unused (gravity is intentionally excluded from the TO objective).
  Material mat{mp.youngGPa * 1000.0, mp.poisson, 0.0};
  const std::vector<int> fixedNodes(fixedSet.begin(), fixedSet.end());
  const std::vector<int> keepNodes(keepSet.begin(), keepSet.end());

  TopoParams params;
  params.volumeFraction = study.volumeFraction;
  params.filterRadius = study.filterRadius;
  params.maxIterations = study.maxIterations > 0 ? study.maxIterations : 60;

  auto res = TopologyOptimize(*mesh, mat, fixedNodes, faceLoads, params, &err, {},
                              keepNodes);
  if (!res) return fail("body '" + body.name + "': " + err);

  StudyBodyResult sbr;
  sbr.bodyIndex = bodyIndex;
  sbr.bodyName = body.name;
  sbr.density = std::move(res->density);
  sbr.mesh = std::move(*mesh);
  rt.bodies.push_back(std::move(sbr));
  rt.iterations = res->iterations;
  rt.converged = res->converged;
  rt.cancelled = res->cancelled;
  rt.volumeFractionAchieved = res->volumeFractionAchieved;
  rt.complianceHistory = std::move(res->complianceHistory);
  return true;
}

}  // namespace fem
