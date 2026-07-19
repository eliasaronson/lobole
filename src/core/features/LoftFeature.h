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
class LoftFeature : public Feature {
public:
  LoftFeature(const std::string& name, std::vector<uint64_t> sketchIds,
              ExtrudeMode mode = ExtrudeMode::NewBody);

  std::string TypeName() const override { return "Loft"; }
  bool Execute(RegenContext& ctx) override;

  std::vector<uint64_t> sketchIds; // >= 2 sections, in order
  ExtrudeMode mode = ExtrudeMode::NewBody;
};
