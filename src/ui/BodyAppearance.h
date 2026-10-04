#pragma once

// How a body LOOKS, resolved once for everything that shows it: the part-tab
// and assembly-tab renderers, glTF export and STEP export. They each used to
// resolve it themselves, and the exports had fallen behind — glTF ignored an
// appearance override and a colour imported from STEP, and coloured two
// occurrences of one unpainted part differently
// (docs/interchange/README.md#exporting-step).
//
// Resolve order: appearance > material > displayColor > palette.

#include "cad/CADMaterials.h"
#include "core/Feature.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

struct BodyLook {
  // The fallback colour: displayColor, else the palette entry.
  glm::vec3 palette{0.7f};
  // Set when a material or an appearance override applies; `mat` is then the
  // full PBR set the viewport draws with (a colour-only override on an
  // unassigned body is a neutral dielectric carrying the palette colour).
  bool hasMaterial = false;
  CADMaterials::MaterialProperties mat{};
  // The one colour an exporter writes. Linear RGB, as the shaders light it.
  glm::vec3 Color() const { return hasMaterial ? mat.baseColor : palette; }
};

// `palette` is the fallback counter, advanced when the body has no
// displayColor — the renderers' rule. A part tab starts it at 0 for the
// document; an assembly tab seeds it with the PART's id so every occurrence of
// a part is the same colour, whatever else is in the assembly.
//
// `wears` is a placement's material when it overrides the part's (an
// assembly occurrence of another material, v135); empty = the body's own.
BodyLook ResolveBodyLook(const Body& body, size_t& palette,
                         const std::string& wears = {});

// The look of a face carrying its own override (Body::faceAppearances, v164):
// the body's resolved look with the override's set fields on top, so an
// unset field inherits from the BODY — its appearance, then its material.
// A colour-only override on an unassigned body is a neutral dielectric, as
// ResolveBodyLook makes it.
BodyLook ResolveFaceLook(const BodyLook& body, const Appearance& face);

// A run of consecutive triangles that share one look. `face` is the override
// they wear, nullptr for the body's own look.
struct FaceLookRun {
  uint32_t firstTriangle = 0;
  uint32_t triangleCount = 0;
  const FaceAppearance* face = nullptr;
};
// The runs tiling a tessellation whose triangles lie on `triangleFaces`
// (CADShape::Mesh::triangleFaces). EMPTY when the body has no face override
// that resolved — the one-draw path every other body keeps. A tessellation
// emits each face contiguously, so this is one run per overridden face plus
// the gaps between them; it does not rely on that, only gets longer without it.
std::vector<FaceLookRun>
FaceLookRuns(const Body& body, const std::vector<uint32_t>& triangleFaces);

} // namespace ui
