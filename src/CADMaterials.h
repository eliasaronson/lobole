#pragma once

#include <glm/glm.hpp>
#include <string>
#include <unordered_map>

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
    
private:
    static std::unordered_map<MaterialType, MaterialProperties> materials;
    static void InitializeMaterials();
};

} // namespace CADMaterials