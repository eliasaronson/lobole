// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// One drawing view's projection as a unit of work and of storage
// (docs/drawings/README.md#projection-off-the-frame): everything the HLR and
// a section's cut read, gathered on the main thread; the projection that
// comes back, kept between updates and saved with the drawing; and the hash
// that says whether a saved one still answers the view.
//
// GUI-free core code, like DrawingProjection. Nothing here touches a
// document: the inputs are shapes the caller already owns (in the app, the
// worker's own deep copies), so a projection can run on any thread that
// holds them and its caches.

#include "core/drawing/DrawingProjection.h"
#include "core/drawing/DrawingSection.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace drawing {

// Everything ProjectView and the section cut read for one view. `bodies` are
// in gather order and their `sourceId` is that order (0, 1, ...), which is
// what a projected curve's SourceRef then names.
struct ProjectionInputs {
  std::vector<InputBody> bodies;
  glm::dvec3 viewDir{0.0, 0.0, -1.0};
  glm::dvec3 up{0.0, 1.0, 0.0};
  ProjectionMode mode = ProjectionMode::Exact;
  bool hiddenLines = true;
  double deflection = 0.05;
  // A section view: the plane through `sectionPoint` (world) square to
  // `viewDir`, keeping what lies beyond it.
  bool section = false;
  glm::dvec3 sectionPoint{0.0};

  // Whether these project to the same curves as `other` up to a turn in the
  // view plane: the same shapes (by identity), placements, revisions and
  // groups, the same direction, mode, options and section. `up` is left
  // out on purpose -- hidden-line removal does not depend on which way is up,
  // so a view turned on the sheet is its old projection turned (TurnedTo).
  [[nodiscard]] bool SameAs(const ProjectionInputs& other) const;
};

// A view's projection, as kept and as saved.
struct ViewProjection {
  ProjectionResult result;
  // The frame `result`'s curves are drawn in (ProjectView's: X = dir x up).
  glm::dvec3 viewDir{0.0, 0.0, -1.0};
  glm::dvec3 up{0.0, 1.0, 0.0};
  // ProjectionInputHash of what produced it; 0 when it is not known.
  uint64_t inputHash = 0;
  // A section view's cuts, one per body the plane left something of, in the
  // order the curves' SourceRef.sourceId counts them: `body` is that body's
  // index in the gathered inputs, and the rest is the cut's bookkeeping
  // (drawing::SectionCut without its shape).
  struct Cut {
    int body = -1;
    std::vector<int> edgeOrigin, edgeFromFace, faceOrigin;
    std::vector<std::vector<glm::dvec3>> cutLoops;
  };
  bool sectioned = false;
  std::vector<Cut> cuts;
};

// Section cuts, kept while their body and plane are the same: the boolean is
// the dear part, and a new cut every update would also be a new shape to the
// projection cache, which keys on shape identity. Bounded; single-threaded --
// whoever holds it (the app hands it to the projecting job for the job's
// life, as it does the ProjectionCache).
class CutCache {
public:
  std::shared_ptr<const SectionCut> Get(const TopoDS_Shape& shape,
                                        const glm::dvec3& point,
                                        const glm::dvec3& normal,
                                        std::string* error);
  void Clear() { m_entries.clear(); }
  [[nodiscard]] size_t Size() const { return m_entries.size(); }

private:
  struct Entry {
    TopoDS_Shape source; // held, so its identity cannot be reused
    glm::dvec3 point{0.0}, normal{0.0};
    std::shared_ptr<const SectionCut> cut;
    uint64_t used = 0;
  };
  std::vector<Entry> m_entries;
  uint64_t m_clock = 0;
};

// 64 bits over exactly what produces a view's projection: per body in order,
// its exact B-Rep (inputhash::Hasher::Shape -- the geometry, as a reopened
// project reads it back), its placement and its group; the direction; the
// mode, deflection and hidden lines; the section; and the OCC version and
// kProjectionRevision. Not `up` (see SameAs) and not the session's revision
// counters. The B-Rep is what makes a saved projection's edge indices still
// name the same edges. Costs a B-Rep write per shape the first time
// (memoized per shape identity), so it is computed off the frame.
[[nodiscard]] uint64_t ProjectionInputHash(const ProjectionInputs& inputs);

struct ProjectRun {
  ProjectionCache* cache = nullptr; // optional
  CutCache* cuts = nullptr;         // optional
  std::function<bool()> cancelled;
  std::function<void(const ProjectionProgress&)> progress;
};

// Cuts (a section view) and projects. The error is the projection's or the
// cut's ("cancelled" when `cancelled` said so), or that a section left
// nothing of the source behind its plane.
std::optional<ViewProjection> ProjectInputs(const ProjectionInputs& inputs,
                                            const ProjectRun& run,
                                            std::string* error);

// `projection`'s curves as ProjectView would have drawn them looking the
// same way with `up` as up: a turn in the view plane, exact.
[[nodiscard]] ProjectionResult TurnedTo(const ViewProjection& projection,
                                        const glm::dvec3& up);

// A digest of a result's curves: kinds, visibility, types, points, conics
// and sources. What the kProjectionRevision guard compares.
[[nodiscard]] uint64_t ProjectionDigest(const ProjectionResult& result);

} // namespace drawing
