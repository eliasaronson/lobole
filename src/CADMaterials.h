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

private:
    static std::unordered_map<MaterialType, MaterialProperties> materials;
    static void InitializeMaterials();
};

} // namespace CADMaterials