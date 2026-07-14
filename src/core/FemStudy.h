#pragma once

// FEM study model: a document-level analysis definition (NOT a Feature). A
// Study consumes the finished bodies and produces no geometry, so it lives on
// Document as a sibling to the parameter table rather than in the regeneration
// history. Mesh and results are derived data, never serialized (see
// FemMesher.h / the FEM2 solver); only this configuration persists.

#include "core/ElementMap.h"
#include "core/Expression.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace fem {

// A boundary condition applied to one face (resolved through an ElementRef, the
// toponaming contract — a dangling ref is a study error, not a silent skip).
enum class LoadType {
  Fixed,     // zero all three DOF on the face's nodes
  Force,     // a total force vector (N) spread as uniform traction
  Pressure,  // a scalar pressure (MPa) along the outward face normal
};

// A per-face mesh size override (mm). size follows the value+expr convention:
// `sizeExpr` empty means `size` is a literal, else it is the evaluated result.
struct Refinement {
  ElementRef face;
  double size = 0.0;
  std::string sizeExpr;
};

// One boundary condition. `magnitude` is N (Force) or MPa (Pressure), ignored
// for Fixed. `direction` is the force direction (Force only; need not be
// normalized — it is normalized at solve time). Magnitude follows the value+
// expr convention so it can read "bolt_preload * 1.5".
struct Load {
  uint64_t id = 0;
  LoadType type = LoadType::Fixed;
  ElementRef face;
  double magnitude = 0.0;
  std::string magnitudeExpr;
  glm::dvec3 direction{0.0, 0.0, -1.0};
};

// A single-part linear-static analysis definition.
struct Study {
  uint64_t id = 0;
  std::string name;

  // Global maximum element size (mm); 0 = auto from the body bounding box.
  double meshSize = 0.0;
  std::string meshSizeExpr;

  std::vector<Refinement> refinements;
  std::vector<Load> loads;

  // Transient: the most recent mesh/solve error (dangling ref, no fixed
  // support, singular matrix, ...). Never serialized. Empty when healthy.
  std::string error;
};

const char* LoadTypeToString(LoadType type);
bool LoadTypeFromString(const std::string& s, LoadType& out);

// Evaluates every expression-driven field (mesh size, refinement sizes, load
// magnitudes) against `symbols`, writing results into the value fields. On a
// parse/unknown-name error, sets *error (naming the offending text) and returns
// false; the value fields keep their last good values. Lazy: called at mesh/
// solve time, not during regeneration (a Study is not visited by the feature
// loop, and results are never auto-solved).
bool EvaluateStudyExprs(Study& study, const SymbolTable& symbols, std::string* error);

}  // namespace fem
