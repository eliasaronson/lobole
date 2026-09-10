#include "core/fem/FemStudy.h"

#include <BRepBuilderAPI_Copy.hxx>

#include "cad/CADMaterials.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemTopo.h"

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
  case AnalysisType::Modal:
    return "modal";
  case AnalysisType::Topology:
    return "topology";
  case AnalysisType::Static:
    break;
  }
  return "static";
}

bool AnalysisTypeFromString(const std::string& s, AnalysisType& out) {
  if (s == "static") {
    out = AnalysisType::Static;
    return true;
  }
  if (s == "modal") {
    out = AnalysisType::Modal;
    return true;
  }
  if (s == "topology") {
    out = AnalysisType::Topology;
    return true;
  }
  return false;
}

bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols,
                        std::string* error) {
  bool ok = true;
  // Evaluate one value+expr pair; on failure records the first error but keeps
  // going so every bad field is left with its last good value.
  auto eval = [&](const std::string& text, double& value,
                  const std::string& what) {
    if (text.empty())
      return;
    const auto v = ExpressionEngine::Default().EvaluateText(text, symbols);
    if (v.has_value()) {
      value = *v;
    } else if (ok) {
      ok = false;
      if (error)
        *error = what + " expression '" + text + "': " + v.error().message;
    }
  };

  eval(study.meshSizeExpr, study.meshSize, "mesh size");
  for (auto& r : study.refinements)
    eval(r.sizeExpr, r.size, "refinement size");
  for (auto& l : study.loads)
    eval(l.magnitudeExpr, l.magnitude, "load magnitude");
  if (study.type == AnalysisType::Topology) {
    eval(study.volumeFractionExpr, study.volumeFraction, "volume fraction");
    eval(study.filterRadiusExpr, study.filterRadius, "filter radius");
  }
  return ok;
}

// featureId 0 = "whichever body has this element". An element name looks
// like it carries its feature ("3/face7"), but that prefix is the feature
// that CREATED the element, not the body's `created_by` — after one boolean
// the two differ, so a caller who reads the id off the name picks a body
// that does not exist. Searching is the honest resolution, and ambiguity is
// refused rather than guessed.
bool ResolveFace(const Document& doc, const ElementRef& ref,
                 const std::string& what, ResolvedFace& out,
                 std::string* error) {
  const auto& bodies = doc.GetBodies();
  std::vector<int> hits;
  for (int i = 0; i < static_cast<int>(bodies.size()); ++i) {
    if (ref.featureId != 0 && bodies[i].createdBy != ref.featureId)
      continue;
    if (const auto idx = bodies[i].elementMap.Resolve(ref.name)) {
      if (ref.featureId != 0) {
        out.bodyIndex = i;
        out.faceIndex = *idx;
        return true;
      }
      hits.push_back(i);
      out.bodyIndex = i;
      out.faceIndex = *idx;
    }
  }
  if (hits.size() == 1)
    return true;
  if (error) {
    if (hits.size() > 1) {
      *error = what + ": '" + ref.name + "' is on " +
               std::to_string(hits.size()) +
               " bodies — say which with feature_id (its created_by):";
      for (int i : hits)
        *error += " " + std::to_string(bodies[i].createdBy) + " (" +
                  bodies[i].name + ")";
    } else {
      *error = what + ": face '" + ref.name + "' no longer exists";
      if (ref.featureId != 0) {
        bool onAnother = false;
        for (const auto& b : bodies)
          if (b.elementMap.Resolve(ref.name))
            onAnother = true;
        if (onAnother)
          *error += " on the body created by feature " +
                    std::to_string(ref.featureId) +
                    " — it is on another body (drop feature_id and it will be "
                    "found, or read created_by from get_state)";
      }
    }
  }
  return false;
}

std::optional<glm::dvec3> DefaultForceDirection(const Document& doc,
                                                const ElementRef& face) {
  ResolvedFace resolved;
  if (!ResolveFace(doc, face, "force", resolved, nullptr))
    return std::nullopt;
  const auto info = Measure::DescribeFaceAt(doc.GetBodies()[resolved.bodyIndex],
                                            resolved.faceIndex);
  if (!info)
    return std::nullopt;
  const glm::vec3 n = info->normal;
  if (glm::dot(n, n) < 1e-12f)
    return std::nullopt;
  // Into the face: pushing on a face is what placing a force on it means.
  const glm::vec3 into = -glm::normalize(n);
  return glm::dvec3(into.x, into.y, into.z);
}

bool PlanStudyMesh(const Document& doc, Study& study, MeshPlan& out,
                   std::string* error) {
  out = MeshPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);

  // Resolve every ref, grouping per body. Per-face refinement sizes are keyed
  // by resolved face index (what fem::Mesh's MeshSettings.faceSizes expects).
  std::map<int, MeshSettings> perBody; // bodyIndex -> settings
  auto touch = [&](int bodyIndex) -> MeshSettings& {
    MeshSettings& s = perBody[bodyIndex];
    s.maxSize = study.meshSize; // 0 => auto, per body
    return s;
  };

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    touch(rf.bodyIndex); // ensure the loaded body is meshed
  }
  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    touch(rf.bodyIndex)
        .faceSizes.push_back({rf.faceIndex, study.refinements[i].size});
  }

  if (perBody.empty())
    return fail("study has no loads or refinements to mesh");

  const auto& bodies = doc.GetBodies();
  for (const auto& [bodyIndex, settings] : perBody) {
    if (bodies[bodyIndex].reference)
      return fail("body '" + bodies[bodyIndex].name +
                  "' is a derived reference (another part's geometry); set "
                  "reference: false on its Derived feature to analyse it");
    MeshBodyPlan b;
    b.bodyIndex = bodyIndex;
    b.bodyName = bodies[bodyIndex].name;
    b.shape = BRepBuilderAPI_Copy(bodies[bodyIndex].shape.GetShape()).Shape();
    b.settings = settings;
    out.bodies.push_back(std::move(b));
  }
  return true;
}

bool MeshPlanned(const MeshPlan& plan, std::vector<StudyBodyMesh>& out,
                 std::string* error) {
  out.clear();
  std::string err;
  for (const MeshBodyPlan& b : plan.bodies) {
    auto mesh = Mesh(b.shape, b.settings, &err);
    if (!mesh) {
      if (error)
        *error = "body '" + b.bodyName + "': " + err;
      return false;
    }
    StudyBodyMesh r;
    r.bodyIndex = b.bodyIndex;
    r.bodyName = b.bodyName;
    r.mesh = std::move(*mesh);
    out.push_back(std::move(r));
  }
  return true;
}

bool MeshStudy(const Document& doc, Study& study,
               std::vector<StudyBodyMesh>& out, std::string* error) {
  out.clear();
  MeshPlan plan;
  if (!PlanStudyMesh(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlanned(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

bool PlanStudy(const Document& doc, Study& study, StudyPlan& out,
               std::string* error) {
  out = StudyPlan{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  out.type = study.type;
  out.numModes = study.numModes;
  out.gravity = study.gravity;

  const auto& bodies = doc.GetBodies();

  // Group every load by the body it resolves to, and collect per-face
  // refinement sizes. A refinement alone does not make a body "loaded".
  struct BodyPlan {
    std::vector<std::pair<int, size_t>> loads; // (faceIndex, load index)
    MeshSettings settings;
  };
  std::map<int, BodyPlan> perBody;

  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    BodyPlan& bp = perBody[rf.bodyIndex];
    bp.settings.maxSize = study.meshSize;
    bp.loads.push_back({rf.faceIndex, i});
  }
  if (perBody.empty())
    return fail("study has no loads to solve");

  for (size_t i = 0; i < study.refinements.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.refinements[i].face,
                     "refinement " + std::to_string(i + 1), rf, &err))
      return fail(err);
    // Only refine bodies that are actually solved.
    auto it = perBody.find(rf.bodyIndex);
    if (it != perBody.end())
      it->second.settings.faceSizes.push_back(
          {rf.faceIndex, study.refinements[i].size});
  }

  for (auto& [bodyIndex, plan] : perBody) {
    const Body& body = bodies[bodyIndex];

    // Material must have stiffness data (0 = unknown, like density).
    const auto matType =
        CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
    const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
    if (mp.youngGPa <= 0.0f)
      return fail("body '" + body.name + "': material '" +
                  (body.material.empty() ? "default" : body.material) +
                  "' has no stiffness data");

    // Each loaded body needs at least one fixed support.
    bool hasFixed = false;
    for (const auto& [faceIndex, loadIdx] : plan.loads)
      if (study.loads[loadIdx].type == LoadType::Fixed)
        hasFixed = true;
    if (!hasFixed)
      return fail("body '" + body.name + "' has loads but no fixed support");

    // Density-dependent analyses must fail HERE (prepare has the material
    // library and study), not on the worker.
    if (study.type == AnalysisType::Modal && mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for modal analysis)");
    if (study.type != AnalysisType::Modal &&
        (study.gravity.x != 0 || study.gravity.y != 0 ||
         study.gravity.z != 0) &&
        mp.density <= 0.0f)
      return fail("body '" + body.name +
                  "': material has no density (needed "
                  "for gravity)");

    // GPa -> MPa; density kg/m^3 -> tonne/mm^3 for the consistent mm-N-t-s
    // system (so gravity loads and modal frequencies come out right).
    StudyBodyPlan bp;
    bp.bodyIndex = bodyIndex;
    bp.bodyName = body.name;
    bp.mat = Material{mp.youngGPa * 1000.0, mp.poisson, mp.density * 1e-12};
    // A deep copy: the mesher writes triangulation into the faces it
    // meshes, and this shape may be meshed on a worker while the document's
    // own is being drawn.
    bp.shape = BRepBuilderAPI_Copy(body.shape.GetShape()).Shape();
    bp.settings = plan.settings;
    for (const auto& [faceIndex, loadIdx] : plan.loads)
      bp.loads.push_back({faceIndex, study.loads[loadIdx]});
    out.bodies.push_back(std::move(bp));
  }
  return true;
}

bool MeshPlannedStudy(const StudyPlan& plan, StudyJobInput& out,
                      std::string* error) {
  out = StudyJobInput{};
  out.type = plan.type;
  out.numModes = plan.numModes;
  out.gravity = plan.gravity;
  std::string err;
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  for (const StudyBodyPlan& bp : plan.bodies) {
    auto mesh = Mesh(bp.shape, bp.settings, &err);
    if (!mesh)
      return fail("body '" + bp.bodyName + "': " + err);

    // Build fixed-node set and face tractions from the resolved faces. Fixed
    // faces contribute all their boundary-tri nodes; force/pressure faces
    // contribute their tri indices.
    std::set<int> fixedSet;
    std::vector<FaceLoad> faceLoads;
    for (const auto& [faceIndex, load] : bp.loads) {
      if (load.type == LoadType::Fixed) {
        for (const auto& tri : mesh->boundary)
          if (tri.faceIndex == faceIndex)
            for (int n : tri.nodes)
              fixedSet.insert(n);
      } else {
        FaceLoad fl;
        fl.type = load.type;
        fl.magnitude = load.magnitude;
        fl.direction = load.direction;
        for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
          if (mesh->boundary[t].faceIndex == faceIndex)
            fl.tris.push_back(t);
        faceLoads.push_back(std::move(fl));
      }
    }
    if (fixedSet.empty())
      return fail("body '" + bp.bodyName + "': fixed face meshed to no nodes");

    StudyJobBody jb;
    jb.bodyIndex = bp.bodyIndex;
    jb.bodyName = bp.bodyName;
    jb.mat = bp.mat;
    jb.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
    jb.faceLoads = std::move(faceLoads);
    jb.mesh = std::move(*mesh);
    out.bodies.push_back(std::move(jb));
  }
  return true;
}

bool PrepareStudy(const Document& doc, Study& study, StudyJobInput& out,
                  std::string* error) {
  out = StudyJobInput{};
  StudyPlan plan;
  if (!PlanStudy(doc, study, plan, error))
    return false;
  std::string err;
  if (!MeshPlannedStudy(plan, out, &err)) {
    study.error = err;
    if (error)
      *error = err;
    return false;
  }
  return true;
}

bool SolvePreparedStudy(StudyJobInput& in, StudyRuntime& rt,
                        std::string* error) {
  rt = StudyRuntime{};
  std::string err;
  auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  for (StudyJobBody& jb : in.bodies) {
    StudyBodyResult sbr;
    sbr.bodyIndex = jb.bodyIndex;
    sbr.bodyName = jb.bodyName;

    if (in.type == AnalysisType::Modal) {
      auto modes =
          SolveModal(jb.mesh, jb.mat, jb.fixedNodes, in.numModes, &err);
      if (!modes)
        return fail("body '" + jb.bodyName + "': " + err);
      sbr.results.modes = std::move(*modes);
    } else {
      const glm::dvec3 g = in.gravity * 1000.0; // m/s^2 -> mm/s^2
      auto res = Solve(jb.mesh, jb.mat, jb.fixedNodes, jb.faceLoads, &err, g);
      if (!res)
        return fail("body '" + jb.bodyName + "': " + err);
      if (res->maxVonMises > rt.maxVonMises) {
        rt.maxVonMises = res->maxVonMises;
        rt.maxVmAt = res->maxVmAt;
        rt.maxVmBody = jb.bodyName;
      }
      if (res->maxDisplacement > rt.maxDisplacement) {
        rt.maxDisplacement = res->maxDisplacement;
        rt.maxDispAt = res->maxDispAt;
        rt.maxDispBody = jb.bodyName;
      }
      sbr.results = std::move(*res);
    }
    sbr.mesh = std::move(jb.mesh);
    rt.bodies.push_back(std::move(sbr));
  }
  return true;
}

bool RunStudy(const Document& doc, Study& study, StudyRuntime& rt,
              std::string* error) {
  StudyJobInput input;
  if (!PrepareStudy(doc, study, input, error))
    return false;
  if (!SolvePreparedStudy(input, rt, error)) {
    // Solve failures set study.error too, matching the pre-split behavior.
    study.error = error ? *error : "solve failed";
    return false;
  }
  return true;
}

bool PrepareTopology(const Document& doc, Study& study, TopoInput& out,
                     std::string* error) {
  out = TopoInput{};
  study.error.clear();
  auto fail = [&](const std::string& m) {
    study.error = m;
    if (error)
      *error = m;
    return false;
  };

  std::string err;
  if (!EvaluateStudyExprs(study, doc.Symbols(), &err))
    return fail(err);
  if (study.volumeFraction <= 0.0 || study.volumeFraction >= 1.0)
    return fail("volume fraction must be in (0, 1)");

  const auto& bodies = doc.GetBodies();

  // Group loads by body (v1 optimizes exactly one loaded body).
  std::map<int, std::vector<std::pair<int, size_t>>>
      bodyLoads; // (faceIndex, loadIdx)
  for (size_t i = 0; i < study.loads.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.loads[i].face, "load " + std::to_string(i + 1),
                     rf, &err))
      return fail(err);
    bodyLoads[rf.bodyIndex].push_back({rf.faceIndex, i});
  }
  if (bodyLoads.empty())
    return fail("study has no loads");
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

  const auto matType =
      CADMaterials::MaterialLibrary::MaterialTypeFromString(body.material);
  const auto& mp = CADMaterials::MaterialLibrary::GetMaterial(matType);
  if (mp.youngGPa <= 0.0f)
    return fail("body '" + body.name + "': material '" +
                (body.material.empty() ? "default" : body.material) +
                "' has no stiffness data");

  // Keep faces (frozen solid beyond the auto-frozen support/load skin).
  std::set<int> keepFaceIdx;
  for (size_t i = 0; i < study.keepFaces.size(); ++i) {
    ResolvedFace rf;
    if (!ResolveFace(doc, study.keepFaces[i],
                     "keep face " + std::to_string(i + 1), rf, &err))
      return fail(err);
    if (rf.bodyIndex == bodyIndex)
      keepFaceIdx.insert(rf.faceIndex);
  }

  auto mesh = Mesh(body.shape.GetShape(), settings, &err);
  if (!mesh)
    return fail("body '" + body.name + "': " + err);

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
          for (int n : tri.nodes)
            fixedSet.insert(n);
    } else {
      hasApplied = true;
      FaceLoad fl;
      fl.type = load.type;
      fl.magnitude = load.magnitude;
      fl.direction = load.direction;
      for (int t = 0; t < static_cast<int>(mesh->boundary.size()); ++t)
        if (mesh->boundary[t].faceIndex == faceIndex)
          fl.tris.push_back(t);
      faceLoads.push_back(std::move(fl));
    }
  }
  if (!hasFixed)
    return fail("body '" + body.name + "' has no fixed support");
  if (!hasApplied)
    return fail("topology optimization needs a force or pressure load");
  if (fixedSet.empty())
    return fail("body '" + body.name + "': fixed face meshed to no nodes");
  for (int fi : keepFaceIdx)
    for (const auto& tri : mesh->boundary)
      if (tri.faceIndex == fi)
        for (int n : tri.nodes)
          keepSet.insert(n);

  out.bodyIndex = bodyIndex;
  out.bodyName = body.name;
  out.mesh = std::move(*mesh);
  // Density is unused (gravity is intentionally excluded from the TO
  // objective).
  out.material = Material{mp.youngGPa * 1000.0, mp.poisson, 0.0};
  out.fixedNodes.assign(fixedSet.begin(), fixedSet.end());
  out.loads = std::move(faceLoads);
  out.keepNodes.assign(keepSet.begin(), keepSet.end());
  out.params.volumeFraction = study.volumeFraction;
  out.params.filterRadius = study.filterRadius;
  out.params.maxIterations = study.maxIterations > 0 ? study.maxIterations : 60;
  return true;
}

StudyRuntime MakeTopoRuntime(TopoInput& input, const TopoResult& result) {
  StudyRuntime rt;
  rt.topology = true;
  StudyBodyResult sbr;
  sbr.bodyIndex = input.bodyIndex;
  sbr.bodyName = input.bodyName;
  sbr.density = result.density;
  sbr.mesh = std::move(input.mesh);
  rt.bodies.push_back(std::move(sbr));
  rt.iterations = result.iterations;
  rt.converged = result.converged;
  rt.cancelled = result.cancelled;
  rt.volumeFractionAchieved = result.volumeFractionAchieved;
  rt.complianceHistory = result.complianceHistory;
  return rt;
}

bool RunTopology(const Document& doc, Study& study, StudyRuntime& rt,
                 std::string* error) {
  TopoInput input;
  if (!PrepareTopology(doc, study, input, error))
    return false;
  std::string err;
  auto res =
      TopologyOptimize(input.mesh, input.material, input.fixedNodes,
                       input.loads, input.params, &err, {}, input.keepNodes);
  if (!res) {
    study.error = "body '" + input.bodyName + "': " + err;
    if (error)
      *error = study.error;
    return false;
  }
  rt = MakeTopoRuntime(input, *res);
  return true;
}

} // namespace fem
