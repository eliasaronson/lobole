// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/MassProps.h"

#include "cad/CADMaterials.h"
#include "core/mesh/TriMesh.h"

namespace Measure {

std::optional<DocumentMass> MassOfBodies(const std::vector<Body>& bodies,
                                         std::string* error,
                                         const std::string& materialOverride) {
  DocumentMass out;
  glm::dvec3 weightedCom(0.0);
  for (const Body& body : bodies) {
    if (body.reference)
      continue; // a Derived ghost weighs nothing here (it is another part)
    CADShape::MassProps mp;
    if (body.IsMesh()) {
      // A mesh body's own triangles, by the divergence theorem: defined only
      // when they close (docs/interchange/README.md#mesh-bodies).
      if (!body.mesh->Closed()) {
        if (error) {
          const std::vector<double> loops = mesh::BoundaryLoops(*body.mesh);
          *error = "body " + body.name + " is an open mesh (" +
                   std::to_string(loops.size()) + " boundary loop" +
                   (loops.size() == 1 ? "" : "s") +
                   "), which encloses no volume: close the holes in "
                   "MeshLab, Blender or the scanning app and re-import it";
        }
        return std::nullopt;
      }
      const mesh::MassProperties m = mesh::ComputeMassProperties(*body.mesh);
      mp.volume = m.volume;
      mp.surfaceArea = m.area;
      mp.centerOfMass = m.centroid;
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          mp.inertia[r][c] = m.inertia[r][c];
    } else if (!body.shape.ComputeMassProps(mp)) {
      if (error)
        *error = "mass properties failed for body " + body.name;
      return std::nullopt;
    }
    BodyMass bm;
    bm.name = body.name;
    bm.material = materialOverride.empty() ? body.material : materialOverride;
    bm.volume = mp.volume;
    bm.surfaceArea = mp.surfaceArea;
    bm.centerOfMass = mp.centerOfMass;
    for (int r = 0; r < 3; r++)
      for (int c = 0; c < 3; c++)
        bm.inertia[r][c] = mp.inertia[r][c];
    const double density =
        CADMaterials::MaterialLibrary::GetMaterialOrDefault(bm.material)
            .density;
    if (!bm.material.empty() && density > 0.0) {
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
  if (!out.massKnown)
    out.massG = 0.0;
  return out;
}

} // namespace Measure
