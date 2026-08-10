#pragma once

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace CADMaterials {

enum class MaterialType {
  // Metals
  ALUMINUM,
  STEEL,
  STAINLESS_STEEL,
  BRASS,
  BRONZE,
  COPPER,
  CHROME,
  GOLD,
  SILVER,
  TITANIUM,

  // Non-metals
  PLASTIC_WHITE,
  PLASTIC_BLACK,
  RUBBER,
  GLASS,
  CONCRETE,

  // Wood & engineered wood. Appearance is a procedural surface pattern shaded
  // in mesh.frag from local object-space position (no textures / UVs); see
  // MaterialProperties::procPattern.
  WOOD_OAK,
  WOOD_PINE,
  WOOD_WALNUT,
  PLYWOOD_BIRCH,
  PLYWOOD_PINE,
  MDF,

  // Default
  DEFAULT
};

struct MaterialProperties {
  glm::vec3 baseColor;
  float metallic;
  float roughness;
  float specular;
  float reflectance;
  std::string name;
  // kg/m^3; 0 = unknown, so mass_properties reports no mass rather than a
  // guessed one (DEFAULT deliberately stays 0).
  float density = 0.0f;
  // Linear-elastic properties for FEM (same 0 = unknown convention as
  // density; DEFAULT stays 0 so a study on an unassigned body fails with
  // "material has no stiffness data" rather than guessing).
  float youngGPa = 0.0f; // Young's modulus, GPa
  float poisson = 0.0f;  // Poisson's ratio (dimensionless)
  float yieldMPa = 0.0f; // yield strength, MPa (0 = no factor-of-safety)

  // Procedural surface pattern, shaded in mesh.frag from local object-space
  // position (no textures/UVs). 0 = flat (the default for every non-wood
  // material, and for MDF, which really is near-uniform). 1 = solid-wood
  // grain (rings + turbulence). 2 = plywood (face veneer + ply-stripe edges).
  // grainScale is rings/plies per local unit; grainContrast is the 0..1
  // darkening of the late-wood bands / dark plies. Both 0 when procPattern 0.
  int procPattern = 0;
  float grainScale = 0.0f;
  float grainContrast = 0.0f;
};

class MaterialLibrary {
public:
  static const MaterialProperties& GetMaterial(MaterialType type);
  static MaterialType GetMaterialFromFilename(const std::string& filename);
  static MaterialType GetMaterialFromShapeIndex(int index);

  // Stable serialization token (e.g. "stainless_steel"), independent of the
  // human-facing display name in MaterialProperties::name.
  static std::string Slug(MaterialType type);
  // Reverse of Slug; DEFAULT for empty or unknown slugs (so pre-v10 files and
  // future/unknown materials degrade gracefully).
  static MaterialType FromSlug(const std::string& slug);
  // All assignable materials in display order (for UI enumeration).
  static const std::vector<MaterialType>& AllTypes();
  // All assignable slugs in display order (RPC/describe enumeration).
  static const std::vector<std::string>& AllSlugs();

private:
  static std::unordered_map<MaterialType, MaterialProperties> materials;
  static void InitializeMaterials();
};

} // namespace CADMaterials