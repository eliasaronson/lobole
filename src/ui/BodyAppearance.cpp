#include "ui/BodyAppearance.h"

#include "ui/AppActionsInternal.h"

#include <iterator>

namespace ui {

BodyLook ResolveBodyLook(const Body& body, size_t& palette) {
  BodyLook look;
  look.palette = body.hasDisplayColor
                     ? body.displayColor
                     : kBodyPalette[palette++ % std::size(kBodyPalette)];
  if (body.material.empty() && !body.appearance.Any())
    return look;
  // The override edits the CPU-resolved params, so the renderer's 128-byte
  // push constant never grows (the appearance-plan constraint).
  CADMaterials::MaterialProperties mat =
      CADMaterials::MaterialLibrary::GetMaterialOrDefault(body.material);
  if (body.material.empty()) {
    // No physical material: a neutral dielectric carrying the palette
    // color, so a color-only override doesn't inherit DEFAULT's metal.
    mat.baseColor = look.palette;
    mat.metallic = 0.0f;
    mat.roughness = 0.6f;
    mat.specular = 0.5f;
    mat.reflectance = 0.04f;
    mat.pattern = CADMaterials::SurfacePattern::Flat;
  }
  if (body.appearance.color)
    mat.baseColor = *body.appearance.color;
  if (body.appearance.roughness)
    mat.roughness = *body.appearance.roughness;
  if (body.appearance.metallic)
    mat.metallic = *body.appearance.metallic;
  look.hasMaterial = true;
  look.mat = mat;
  return look;
}

} // namespace ui
