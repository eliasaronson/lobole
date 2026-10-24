// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/BaseFlangeFeature.h"

#include "core/BodyMode.h"
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h"

BaseFlangeFeature::BaseFlangeFeature(const std::string& name, uint64_t sketchId,
                                     bool reversed)
    : sketchId(sketchId), reversed(reversed) {
  m_name = name;
}

bool BaseFlangeFeature::Execute(RegenContext& ctx) {
  if (ctx.sheetMetal == nullptr) {
    // Refuse, don't guess. A default gauge here would be a literal millimetre
    // dressed up as a decision, and every bend allowance in the part would be
    // computed from it.
    SetError("this part has no sheet-metal rules - set the material and gauge "
             "first (set_sheet_metal_rules), since a flange has no thickness "
             "of its own");
    return false;
  }
  const double thickness = ctx.sheetMetal->thickness;
  if (thickness <= 1e-9) {
    SetError("sheet thickness is zero");
    return false;
  }

  const Feature* ref = ctx.FindById(sketchId);
  const auto* sketch = dynamic_cast<const SketchFeature*>(ref);
  if (!sketch) {
    SetError("profile sketch (id " + std::to_string(sketchId) +
             ") not found earlier in history");
    return false;
  }
  // A sketch that failed this regeneration still holds its last plane and
  // profile; building from those puts the feature where the reference used
  // to be (a rib on a plane 9e15 mm out spun a boolean for minutes).
  if (!sketch->GetError().empty()) {
    SetError("profile sketch '" + sketch->GetName() +
             "' failed: " + sketch->GetError());
    return false;
  }

  std::vector<sketch::Loop> loops = sketch::BuildLoops(sketch->entities);
  if (loops.empty()) {
    SetError("sketch has no closed profile");
    return false;
  }
  if (!regions.empty() && !sketch::FilterLoopsToRegions(loops, regions)) {
    SetError("a selected region no longer exists in the sketch - re-pick "
             "the profiles");
    return false;
  }
  if (loops.empty()) {
    SetError("the selected regions leave no profile");
    return false;
  }

  ElementMap map;
  std::string buildErr;
  CADShape wall = sketch::BuildPrismNamed(sketch->plane, loops, thickness,
                                          reversed, GetId(), map, &buildErr);
  if (!wall.IsValid()) {
    SetError(buildErr.empty() ? "profile could not be thickened into a wall"
                              : buildErr);
    return false;
  }

  // The wall is made OF the sheet, so it wears the sheet's material unless
  // this feature was given one of its own.
  const std::string material =
      GetMaterial().empty() ? ctx.sheetMetal->material : GetMaterial();
  std::string warning, error;
  if (!ApplyBodyMode(ctx.bodies, std::move(wall), std::move(map),
                     ExtrudeMode::NewBody, GetName(), material, GetId(),
                     &warning, &error)) {
    SetError(error.empty() ? "base flange could not be added" : error);
    return false;
  }
  if (!warning.empty())
    SetWarning(warning);
  return true;
}
