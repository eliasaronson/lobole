#include "core/FemStudy.h"

#include "core/Document.h"

#include <map>

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

}  // namespace fem
