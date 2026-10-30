// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Placing an import from what it is
// (docs/interchange/README.md#placing-an-import): lay a floor on z = 0, lay the
// plane through three points there, turn the principal axes onto the world's,
// or scale so two points stand a known distance apart. Each answers a new
// Import placement, composed onto the one the import already has, so the points
// are where the bodies stand now. The `align_import` RPC and the GUI Align tool
// both call this.

#include "core/Feature.h"
#include "core/ImportPlacement.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

class Document;

namespace importalign {

enum class Mode { Floor, ThreePoints, Principal, Scale };

// "floor", "three_points", "principal", "scale".
const char* ToString(Mode m);
std::optional<Mode> ModeFromString(const std::string& s);
// "floor, three_points, principal, scale": what a refusal names.
std::string AcceptedModes();
// How many points a mode takes: floor 1, three_points 3, principal 0, scale 2.
size_t PointsFor(Mode m);

struct Request {
  Mode mode = Mode::Floor;
  std::vector<glm::dvec3> pointsMm; // where the bodies stand now
  double distanceMm = 0.0;          // scale: the true distance
  // floor: how far a vertex may stand off the plane and still be on it;
  // unset = a five-hundredth of the import's bounding diagonal, never less
  // than four quanta of the file's precision.
  std::optional<double> toleranceMm;
};

struct Result {
  importplace::Placement placement; // the Import's new placement
  // What was fitted, for the reply and the tool window: floor {fitted:
  // "region" | "boundary_loop", body, triangles | loop_vertices, area_mm2,
  // rms_mm, max_mm, tolerance_mm}; three_points {}; principal {spread_mm
  // [x, y, z]}; scale {measured_mm, factor}. Each with `normal` for a plane
  // mode: the plane's, where it stood before.
  nlohmann::json report;
};

// {translate_mm: [x, y, z], rotate_deg: [x, y, z]}: how a reply says where
// an import stands (its `scale` beside it).
nlohmann::json PlacementJson(const importplace::Placement& p);

// The bodies `import` made, as they stood after it ran: from the regen
// cache when it reaches the import, else the document's bodies it created.
std::vector<const Body*> ImportBodies(const Document& doc,
                                      const ImportFeature& import);

// Refuses a request whose inputs are wrong for its mode, naming what it
// takes, before anything is read: the wrong number of points, a distance
// that is not a positive number, a tolerance that is not.
void CheckRequest(const Request& request);

// The new placement for an import standing at `current` with `bodies`.
// False with `error` naming why (a point off every body, three points in a
// line, two points in one place, an import with no triangles).
bool Align(const std::vector<const Body*>& bodies,
           const importplace::Placement& current, const Request& request,
           Result& out, std::string& error);

} // namespace importalign
