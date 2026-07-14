#include "core/MassProps.h"

#include "CADMaterials.h"

namespace Measure {

std::optional<DocumentMass> MassOfBodies(const std::vector<Body>& bodies,
                                         std::string* error) {
  DocumentMass out;
  glm::dvec3 weightedCom(0.0);
  for (const Body& body : bodies) {
    CADShape::MassProps mp;
    if (!body.shape.ComputeMassProps(mp)) {
      if (error) *error = "mass properties failed for body " + body.name;
      return std::nullopt;
    }
    BodyMass bm;
    bm.name = body.name;
    bm.material = body.material;
    bm.volume = mp.volume;
    bm.surfaceArea = mp.surfaceArea;
    bm.centerOfMass = glm::dvec3(mp.centerOfMass);
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++) bm.inertia[r][c] = mp.inertia[r][c];
    const float density = CADMaterials::MaterialLibrary::GetMaterial(
                              CADMaterials::MaterialLibrary::FromSlug(
                                  body.material))
                              .density;
    if (!body.material.empty() && density > 0.0f) {
      bm.density = density;
      bm.massKnown = true;
      bm.massG = density * mp.volume * 1e-6;
      out.massG += bm.massG;
    } else {
      out.massKnown = false;
    }
    out.volume += mp.volume;
    weightedCom += bm.centerOfMass * mp.volume;
    out.bodies.push_back(std::move(bm));
  }
  out.centerOfMass =
      out.volume > 0.0 ? weightedCom / out.volume : glm::dvec3(0.0);
  if (!out.massKnown) out.massG = 0.0;
  return out;
}

} // namespace Measure
