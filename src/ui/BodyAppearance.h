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
BodyLook ResolveBodyLook(const Body& body, size_t& palette);

} // namespace ui
