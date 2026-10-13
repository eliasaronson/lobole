// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/BodyMode.h" // ExtrudeMode (new_body / add / cut)
#include "core/Feature.h"

#include <cstdint>
#include <string>
#include <vector>

// Lofts a solid through two or more sketch profiles (modeling M8): each
// referenced sketch contributes its outer boundary as a section, and OCC's
// BRepOffsetAPI_ThruSections skins a solid between them (a frustum from a big
// square to a small one, a boat hull from ribs, ...). Sections are ordered as
// listed. Modes reuse ApplyBodyMode (new-body / fuse / cut), shared with
// extrude and revolve. Regeneration fails per-feature when a referenced
// sketch is missing/empty or the sections don't skin a valid solid.
//
// Guided loft (curves-3d Phase 3): when `pathId` names a spine (a sketch or a
// Curve3D), the sections are skinned along that path with MakePipeShell instead
// of ThruSections, and an optional `guideId` guide curve shapes them as they
// travel (sections scale to keep contact with the guide) — mirrors FreeCAD's
// PartDesign Pipe, which is how guide curves reach OCC (ThruSections has none).
class LoftFeature : public Feature {
public:
  LoftFeature(const std::string& name, std::vector<uint64_t> sketchIds,
              ExtrudeMode mode = ExtrudeMode::NewBody);

  std::string TypeName() const override { return "Loft"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<LoftFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<LoftFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitFeatureRefs(const RefVisitor& visit) const override {
    for (const uint64_t id : sketchIds)
      visit(id);
    visit(pathId); // optional spine
    visit(guideId);
  }

  std::vector<uint64_t> sketchIds; // >= 2 sections, in order
  uint64_t pathId =
      0; // optional spine (sketch/Curve3D); 0 = classic loft (v45)
  uint64_t guideId = 0; // optional guide curve; requires pathId (v45)
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
