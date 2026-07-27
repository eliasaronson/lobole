#pragma once

#include "cad/CADKernel.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>
#include <vector>

// Assembly STEP import (interchange I2, docs/interchange/import-plan.md):
// turn a structured STEP into a *plan* the app materializes as part documents
// plus an assembly. Pure core logic — file extraction and transform math live
// here so they are testable without AppState; the ui wrapper only creates
// tabs/occurrences from the result.
namespace interchange {

struct PlannedPart {
  std::string name;     // unique among planned parts ("Widget", "Widget 2")
  std::string brepPath; // extracted geometry, CENTERED at the part origin
  bool hasColor = false;
  glm::vec3 color{0.0f};
};

struct PlannedOccurrence {
  size_t part = 0; // index into parts
  std::string name;
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

struct AssemblyImportPlan {
  std::vector<PlannedPart> parts;
  std::vector<PlannedOccurrence> occurrences;
  std::string error; // non-empty = the plan failed; nothing was extracted
};

// Reads `stepPath` (XCAF) and extracts one centered .brep per unique product
// into `destDir` (created if missing). Each occurrence's placement composes
// the instance transform with the centering offset, so materialized parts
// land exactly where the file put them. Non-rigid instance transforms import
// at their rigid part (rotation+translation of the closest similarity) — in
// practice real CAD writes rigid placements only.
AssemblyImportPlan BuildAssemblyImportPlan(const std::string& stepPath,
                                           const std::string& destDir);

} // namespace interchange
