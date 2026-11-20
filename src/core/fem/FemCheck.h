// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Everything a run of a study would refuse, at once
// (docs/simulation/README.md#checking-a-study). A run stops at its first
// refusal, so a setup with three things wrong took three runs to hear of
// them; this reads the same rules and collects, each problem with the call
// that fixes it. Read-only: the study and the document are not touched.

#include "core/fem/FemStudy.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class Document;

namespace fem {

struct StudyProblem {
  // expression | load_cases | setup | no_loads | anchor | refinement_anchor |
  // material | no_support | load_geometry | region | stale_mesh | solver |
  // plan
  std::string code;
  std::string message;
  uint64_t loadId = 0; // the load it is about; 0 = none
  std::string body;    // the body it is about; empty = none
  // The face it is about, when it has one that still resolves.
  bool hasFace = false;
  ElementRef face;
  // The call that fixes it (empty method = none to offer), and what the
  // caller still has to choose in it.
  std::string fixMethod;
  nlohmann::json fixParams = nlohmann::json::object();
  std::string fixNote;
};

// A mesh of one of the study's bodies, by reference.
struct CheckMesh {
  int bodyIndex = -1;
  const FemMesh* mesh = nullptr;
};

// A part study's problems. `meshes`, when given, is a current mesh of the
// study's bodies (the Mesh action's): what only a mesh shows -- a load
// region that holds none of its face's facets -- is then checked too.
// With `backstop`, when nothing itemised is wrong the planner itself is run
// on a copy, so an empty answer means PlanStudy accepts the study; it
// copies every body's shape, which a panel drawing the list each time the
// study moves does without.
std::vector<StudyProblem>
CheckStudy(const Document& doc, const Study& study,
           const std::vector<CheckMesh>* meshes = nullptr,
           bool backstop = true);

} // namespace fem
