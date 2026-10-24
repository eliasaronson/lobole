// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The part-only measures a requirement reads -- a part's mass and volume,
// the area a plane cuts through it -- on a `const Document&`, so the RPCs
// (mass_properties, section), a requirement's check and a design sweep's
// point (core/DesignSweep.h), which measures a copy of the part on a worker,
// call the same function and agree by construction.

#include "core/Document.h"
#include "core/MassProps.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace partmeasure {

// A part's bodies named by the caller: each entry a body NAME or the id of
// the feature that created a body (its `created_by`), answered as the set of
// creating-feature ids. Ids, not body indices, so the choice outlives a
// regeneration that reorders bodies. A feature that created several bodies
// names them all. An entry matching no body is refused naming the bodies
// that exist -- a section or a sheet that silently left one out would look
// right and be wrong. `what` is the parameter's name, for the message.
std::set<uint64_t> ResolveBodies(const Document& doc,
                                 const nlohmann::json& list, const char* what);

// Measure::MassOfBodies over `bodies`, refused as mass_properties refuses:
// a computation that failed, no bodies, or only reference bodies.
Measure::DocumentMass MassOf(const std::vector<Body>& bodies);

// The area a plane cuts through `bodies`, and -- `enclosed` -- the area the
// cut material RINGS (docs/inspect/README.md#enclosed-area), with a note
// when material is cut but rings nothing. Throws when the plane cuts no
// body.
struct Section {
  double area = 0.0;
  std::optional<double> enclosed;
  std::string enclosedNote;
};
Section SectionOf(const std::vector<Body>& bodies, const glm::dvec3& origin,
                  const glm::dvec3& normal, bool enclosed);

// A section_area requirement's measure ({of, base_plane + offset, or origin
// + normal, bodies?}) on a part: what MeasureSectionArea and a sweep's point
// read. `value` is the area the measure asks for (material or enclosed);
// `detail` what a reader needs beside it.
struct SectionReading {
  double value = 0.0;
  nlohmann::json detail = nlohmann::json::object();
};
SectionReading SectionAreaMeasure(const Document& doc,
                                  const nlohmann::json& measure);

} // namespace partmeasure
