// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "core/io/ImportUpAxis.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <functional>
#include <string>
#include <vector>

// Assembly STEP import (docs/interchange/README.md#importing-an-assembly):
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
  // The import's resolved up axis, carried explicitly: a .brep has no header,
  // so the part tab stands up on its own. With a turn the part also keeps
  // the .brep's origin (`keepOrigin`), which is what the occurrence's
  // R·P·R⁻¹ placement assumes.
  UpAxis upAxis = UpAxis::None;
  double turnDeg = 0.0;
  bool keepOrigin = false;
};

struct PlannedOccurrence {
  size_t part = 0; // index into parts
  std::string name;
  glm::dvec3 position{0.0};
  glm::dquat orientation{1.0, 0.0, 0.0, 0.0};
};

struct AssemblyImportPlan {
  std::vector<PlannedPart> parts;
  std::vector<PlannedOccurrence> occurrences;
  std::string error; // non-empty = the plan failed; nothing was extracted
  UpDecision up;     // what the whole import was turned by, and why
};

// Extracts one centered .brep per unique product of an ALREADY-PARSED model
// into `destDir` (created if missing). Each occurrence's placement composes
// the instance transform with the centering offset, so materialized parts
// land exactly where the file put them. Non-rigid instance transforms import
// at their rigid part (rotation+translation of the closest similarity) — in
// practice real CAD writes rigid placements only.
//
// `stepPath` names the source in error messages and the log only — nothing is
// read from it. Parsing is the caller's, because it is by far the expensive
// half: on a 48 MB / 299-product file the XCAF read is ~12 s against ~0.9 s
// for all of the extraction below, so a caller that already holds the model
// (the GUI import worker in ui/ImportJob.cpp probes the structure to decide
// part-vs-assembly) must never pay for it twice.
//
// `onProduct(done, total)` is called after each product is extracted; return
// false from it to abort (the plan then comes back with an error). Null by
// default — the synchronous callers want neither progress nor cancellation.
//
// `up` and `turnDeg` turn the whole assembly (DecideUpAxis against the
// model's header): R is applied to every part in its own tab, and each
// placement P becomes R·P·R⁻¹, so occurrences stay where the file put them,
// turned.
using ExtractionProgress = std::function<bool(size_t done, size_t total)>;
AssemblyImportPlan
BuildAssemblyImportPlan(const CADKernel::ImportedModel& model,
                        const std::string& stepPath, const std::string& destDir,
                        const ExtractionProgress& onProduct = nullptr,
                        UpAxis up = UpAxis::Auto, double turnDeg = 0.0);

// Convenience overload: parse `stepPath` (STEP or IGES, by extension), then
// the above. For callers that do not already hold the model.
AssemblyImportPlan BuildAssemblyImportPlan(const std::string& stepPath,
                                           const std::string& destDir,
                                           UpAxis up = UpAxis::Auto,
                                           double turnDeg = 0.0);

// Reads a STEP or IGES file by its extension (.step/.stp, .igs/.iges) into
// `out`; false when it is neither or cannot be read.
bool LoadStructured(const std::string& path, CADKernel::ImportedModel& out,
                    CADKernel::ImportProgress* progress = nullptr);

} // namespace interchange
