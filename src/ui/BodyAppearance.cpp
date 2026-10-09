// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/BodyAppearance.h"

#include "ui/AppActionsInternal.h"

#include <iterator>

namespace ui {

namespace {
// A neutral dielectric carrying `color`: what an override paints on a body
// with no physical material, so a colour-only override does not inherit
// DEFAULT's metal.
CADMaterials::MaterialProperties NeutralWith(CADMaterials::MaterialProperties m,
                                             const glm::vec3& color) {
  m.baseColor = color;
  m.metallic = 0.0f;
  m.roughness = 0.6f;
  m.specular = 0.5f;
  m.reflectance = 0.04f;
  m.pattern = CADMaterials::SurfacePattern::Flat;
  return m;
}

void Apply(CADMaterials::MaterialProperties& mat, const Appearance& a) {
  if (a.color)
    mat.baseColor = *a.color;
  if (a.roughness)
    mat.roughness = *a.roughness;
  if (a.metallic)
    mat.metallic = *a.metallic;
}
} // namespace

BodyLook ResolveBodyLook(const Body& body, size_t& palette,
                         const std::string& wears) {
  const std::string& material = wears.empty() ? body.material : wears;
  BodyLook look;
  look.palette = body.hasDisplayColor
                     ? body.displayColor
                     : kBodyPalette[palette++ % std::size(kBodyPalette)];
  if (material.empty() && !body.appearance.Any())
    return look;
  // The override edits the CPU-resolved params, so the renderer's 128-byte
  // push constant never grows (the appearance-plan constraint).
  CADMaterials::MaterialProperties mat =
      CADMaterials::MaterialLibrary::GetMaterialOrDefault(material);
  if (material.empty())
    mat = NeutralWith(mat, look.palette);
  Apply(mat, body.appearance);
  look.hasMaterial = true;
  look.mat = mat;
  return look;
}

BodyLook ResolveFaceLook(const BodyLook& body, const Appearance& face) {
  BodyLook look = body;
  if (!face.Any())
    return look;
  if (!look.hasMaterial)
    look.mat = NeutralWith(
        CADMaterials::MaterialLibrary::GetMaterialOrDefault(""), body.palette);
  Apply(look.mat, face);
  look.hasMaterial = true;
  return look;
}

std::vector<FaceLookRun>
FaceLookRuns(const Body& body, const std::vector<uint32_t>& triangleFaces) {
  std::vector<FaceLookRun> runs;
  if (body.faceAppearances.empty())
    return runs;
  bool any = false;
  for (uint32_t t = 0; t < triangleFaces.size(); t++) {
    const auto it =
        body.faceAppearances.find(static_cast<int>(triangleFaces[t]));
    const FaceAppearance* face =
        it == body.faceAppearances.end() ? nullptr : &it->second;
    any |= face != nullptr;
    if (!runs.empty() && runs.back().face == face)
      runs.back().triangleCount++;
    else
      runs.push_back({t, 1, face});
  }
  if (!any) // overrides on faces this tessellation has no triangles of
    runs.clear();
  return runs;
}

} // namespace ui
