#include "core/FemStudy.h"

#include "CADMaterials.h"
#include "core/Document.h"
#include "core/FemSolver.h"

#include <map>
#include <set>

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

    Material mat{mp.youngGPa * 1000.0, mp.poisson};  // GPa -> MPa
    std::vector<int> fixedNodes(fixedSet.begin(), fixedSet.end());
    auto res = Solve(*mesh, mat, fixedNodes, faceLoads, &err);
    if (!res) return fail("body '" + body.name + "': " + err);

    StudyBodyResult sbr;
    sbr.bodyIndex = bodyIndex;
    sbr.bodyName = body.name;
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
    sbr.mesh = std::move(*mesh);
    sbr.results = std::move(*res);
    rt.bodies.push_back(std::move(sbr));
  }
  return true;
}

}  // namespace fem
