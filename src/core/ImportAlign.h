// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Placing an import from what it is
// (docs/interchange/README.md#placing-an-import): lay a floor on z = 0, lay the
// plane through three points there, turn the principal axes onto the world's,
// scale so two points stand a known distance apart, or lay it onto another
// body by a best fit (docs/interchange/README.md#laying-a-scan-onto-a-body).
// Each answers a new
// Import placement, composed onto the one the import already has, so the points
// are where the bodies stand now. The `align_import` RPC and the GUI Align tool
// both call this.

#include "core/Feature.h"
#include "core/ImportPlacement.h"

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <functional>
#include <memory>

#include <optional>
#include <string>
#include <vector>

class Document;

namespace mesh {
struct TriMesh;
}

namespace importalign {

enum class Mode { Floor, ThreePoints, Principal, Scale, BestFit };

// "floor", "three_points", "principal", "scale", "best_fit".
const char* ToString(Mode m);
std::optional<Mode> ModeFromString(const std::string& s);
// "floor, three_points, principal, scale, best_fit": what a refusal names.
std::string AcceptedModes();
// How many points a mode takes: floor 1, three_points 3, principal 0, scale 2,
// best_fit 0 (its picked pairs are `pairsMm`).
size_t PointsFor(Mode m);

struct Request {
  Mode mode = Mode::Floor;
  std::vector<glm::dvec3> pointsMm; // where the bodies stand now
  double distanceMm = 0.0;          // scale: the true distance
  // floor: how far a vertex may stand off the plane and still be on it;
  // unset = a five-hundredth of the import's bounding diagonal, never less
  // than four quanta of the file's precision.
  std::optional<double> toleranceMm;
  // best_fit: three picked pairs, each [on the import as it stands now, on
  // the body it goes onto], which give the one start instead of the 24.
  std::vector<std::array<glm::dvec3, 2>> pairsMm;
  // best_fit: the fit limited to the surface within `radiusMm` of
  // `pointMm`, on the import or on the body it goes onto (`onScan`).
  struct Region {
    bool onScan = true;
    glm::dvec3 pointMm{0.0};
    double radiusMm = 0.0;
  };
  std::optional<Region> region;
};

struct Result {
  importplace::Placement placement; // the Import's new placement
  // What was fitted, for the reply and the tool window: floor {fitted:
  // "region" | "boundary_loop", body, triangles | loop_vertices, area_mm2,
  // rms_mm, max_mm, tolerance_mm}; three_points {}; principal {spread_mm
  // [x, y, z]}; scale {measured_mm, factor}. Each with `normal` for a plane
  // mode: the plane's, where it stood before.
  nlohmann::json report;
  bool cancelled = false; // best_fit: stopped between passes
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
// line, two points in one place, an import with no triangles). Not for
// best_fit, which has a body to go onto: BestFit.
bool Align(const std::vector<const Body*>& bodies,
           const importplace::Placement& current, const Request& request,
           Result& out, std::string& error);

// What a best fit reads, gathered on the main thread so that the fit can
// run on another: the import's triangles and those of the body it goes
// onto, a B-Rep's tessellated at a deflection derived from its size
// (meshop::Triangles). Shared, immutable meshes; nothing of the document.
struct BestFitInput {
  std::vector<std::shared_ptr<const mesh::TriMesh>> scan, onto;
  std::string ontoName;
  double ontoChordMm = 0.0; // a B-Rep's deflection; 0 for a mesh body
};
BestFitInput GatherBestFit(const std::vector<const Body*>& scan,
                           const std::vector<const Body*>& onto);

struct Progress {
  const std::atomic<bool>* cancel = nullptr; // read between passes
  std::function<void(double)> fraction;      // 0-1, between passes
};

// best_fit's refusals that need the triangles but not the fit, for a caller
// to make before it starts one (a refused call starts no job): an import or
// a body with none, a region with no surface of the import -- or, picked on
// the body, of the body -- within it, three pairs in a line. "" when none;
// throws what CheckRequest throws.
std::string CheckBestFit(const BestFitInput& input, const Request& request);

// best_fit: the rigid placement that lays the import's triangles onto the
// target's (docs/interchange/README.md#laying-a-scan-onto-a-body), composed
// onto `current`; never a scale. Starts at the placement as it stands and
// the 24 attitudes that lay the principal axes on the target's, or (pairs)
// at the three pairs laid on each other; point-to-plane ICP on a thinned
// sample from each, the winner by its capped mean distance over every
// sample. `out.report`: {rotate_deg, translate_mm (the motion applied),
// moved_mm, mean_mm, p95_mm, within_mm, within, onto, starts, winner,
// runner_up?, note?}. False with `error` naming why; true with
// `out.cancelled` when the cancel was asked.
bool BestFit(const BestFitInput& input, const importplace::Placement& current,
             const Request& request, const Progress& progress, Result& out,
             std::string& error);

} // namespace importalign
