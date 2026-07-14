#include "core/FemStudy.h"

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

}  // namespace fem
