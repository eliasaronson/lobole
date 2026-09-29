#pragma once

#include "core/Feature.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

// Mass properties over a body list (inspection I2), shared by the
// mass_properties RPC and the assembly BOM (A4). Mass assumes model units are
// millimetres: grams = density[kg/m^3] * volume[mm^3] * 1e-6. A body without
// an assigned material (or one whose material has no density) reports its
// mass as unknown, and taints every total it contributes to.
namespace Measure {

struct BodyMass {
  std::string name;
  std::string material; // slug; may be empty
  double volume = 0.0;  // mm^3
  double surfaceArea = 0.0;
  glm::dvec3 centerOfMass{0.0};
  double inertia[3][3] = {}; // unit-density, about the CoM (world axes)
  double density = 0.0;      // kg/m^3; 0 = unknown
  bool massKnown = false;
  double massG = 0.0; // valid when massKnown
};

struct DocumentMass {
  std::vector<BodyMass> bodies;
  double volume = 0.0;
  glm::dvec3 centerOfMass{0.0}; // volume-weighted
  bool massKnown = true;        // false when any body's density is unknown
  double massG = 0.0;
};

// Unit-density OCC mass properties x material density for every body --
// `materialOverride`'s density for all of them when it is set (an
// occurrence made of another material than its part). nullopt (with `error`
// set) when a shape's computation fails.
std::optional<DocumentMass>
MassOfBodies(const std::vector<Body>& bodies, std::string* error = nullptr,
             const std::string& materialOverride = {});

} // namespace Measure
