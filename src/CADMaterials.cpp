#include "CADMaterials.h"
#include <algorithm>
#include <cctype>

namespace CADMaterials {

std::unordered_map<MaterialType, MaterialProperties> MaterialLibrary::materials;

void MaterialLibrary::InitializeMaterials() {
    if (!materials.empty()) return;
    
    // Metals - High metallic, low roughness, realistic colors
    materials[MaterialType::ALUMINUM] = {
        glm::vec3(0.91f, 0.92f, 0.92f),  // Light silver-white
        1.0f,   // Fully metallic
        0.1f,   // Very smooth
        0.9f,   // High specular
        0.04f,  // Standard metal reflectance
        "Aluminum"
    };
    
    materials[MaterialType::STEEL] = {
        glm::vec3(0.65f, 0.67f, 0.68f),  // Dark metallic gray
        1.0f,   // Fully metallic
        0.2f,   // Slightly rougher than aluminum
        0.8f,   // High specular
        0.04f,  // Standard metal reflectance
        "Steel"
    };
    
    materials[MaterialType::STAINLESS_STEEL] = {
        glm::vec3(0.75f, 0.77f, 0.78f),  // Brighter than regular steel
        1.0f,   // Fully metallic
        0.15f,  // Smoother than regular steel
        0.85f,  // High specular
        0.04f,  // Standard metal reflectance
        "Stainless Steel"
    };
    
    materials[MaterialType::BRASS] = {
        glm::vec3(0.91f, 0.78f, 0.35f),  // Golden yellow
        1.0f,   // Fully metallic
        0.15f,  // Smooth
        0.85f,  // High specular
        0.04f,  // Standard metal reflectance
        "Brass"
    };
    
    materials[MaterialType::BRONZE] = {
        glm::vec3(0.80f, 0.50f, 0.20f),  // Reddish-brown
        1.0f,   // Fully metallic
        0.2f,   // Slightly rough
        0.8f,   // High specular
        0.04f,  // Standard metal reflectance
        "Bronze"
    };
    
    materials[MaterialType::COPPER] = {
        glm::vec3(0.95f, 0.64f, 0.54f),  // Reddish orange
        1.0f,   // Fully metallic
        0.1f,   // Smooth
        0.9f,   // Very high specular
        0.04f,  // Standard metal reflectance
        "Copper"
    };
    
    materials[MaterialType::CHROME] = {
        glm::vec3(0.55f, 0.56f, 0.59f),  // Cool metallic gray
        1.0f,   // Fully metallic
        0.05f,  // Very smooth (mirror-like)
        0.95f,  // Very high specular
        0.04f,  // Standard metal reflectance
        "Chrome"
    };
    
    materials[MaterialType::GOLD] = {
        glm::vec3(1.0f, 0.84f, 0.0f),    // Pure gold color
        1.0f,   // Fully metallic
        0.1f,   // Smooth
        0.9f,   // High specular
        0.04f,  // Standard metal reflectance
        "Gold"
    };
    
    materials[MaterialType::SILVER] = {
        glm::vec3(0.97f, 0.96f, 0.91f),  // Bright silver
        1.0f,   // Fully metallic
        0.05f,  // Very smooth
        0.95f,  // Very high specular
        0.04f,  // Standard metal reflectance
        "Silver"
    };
    
    materials[MaterialType::TITANIUM] = {
        glm::vec3(0.54f, 0.50f, 0.50f),  // Dark metallic
        1.0f,   // Fully metallic
        0.25f,  // Slightly rougher
        0.75f,  // Good specular
        0.04f,  // Standard metal reflectance
        "Titanium"
    };
    
    // Non-metals - Low metallic, varied roughness
    materials[MaterialType::PLASTIC_WHITE] = {
        glm::vec3(0.95f, 0.95f, 0.95f),  // Pure white
        0.0f,   // Non-metallic
        0.5f,   // Medium roughness
        0.5f,   // Medium specular
        0.04f,  // Plastic reflectance
        "White Plastic"
    };
    
    materials[MaterialType::PLASTIC_BLACK] = {
        glm::vec3(0.05f, 0.05f, 0.05f),  // Dark black
        0.0f,   // Non-metallic
        0.6f,   // Slightly rough
        0.4f,   // Lower specular
        0.04f,  // Plastic reflectance
        "Black Plastic"
    };
    
    materials[MaterialType::RUBBER] = {
        glm::vec3(0.1f, 0.1f, 0.1f),     // Dark rubber
        0.0f,   // Non-metallic
        0.9f,   // Very rough
        0.1f,   // Low specular
        0.04f,  // Low reflectance
        "Rubber"
    };
    
    materials[MaterialType::GLASS] = {
        glm::vec3(0.95f, 0.98f, 1.0f),   // Clear glass tint
        0.0f,   // Non-metallic
        0.0f,   // Perfectly smooth
        1.0f,   // Maximum specular
        0.08f,  // Glass reflectance
        "Glass"
    };
    
    materials[MaterialType::CONCRETE] = {
        glm::vec3(0.7f, 0.7f, 0.65f),    // Gray concrete
        0.0f,   // Non-metallic
        0.8f,   // Rough surface
        0.2f,   // Low specular
        0.04f,  // Low reflectance
        "Concrete"
    };
    
    // Default material
    materials[MaterialType::DEFAULT] = materials[MaterialType::ALUMINUM];
}

const MaterialProperties& MaterialLibrary::GetMaterial(MaterialType type) {
    InitializeMaterials();
    auto it = materials.find(type);
    if (it != materials.end()) {
        return it->second;
    }
    return materials[MaterialType::DEFAULT];
}

MaterialType MaterialLibrary::GetMaterialFromFilename(const std::string& filename) {
    // Convert filename to lowercase for case-insensitive matching
    std::string lowerFilename = filename;
    std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
    
    // Check for material keywords in filename
    if (lowerFilename.find("aluminum") != std::string::npos || 
        lowerFilename.find("aluminium") != std::string::npos) {
        return MaterialType::ALUMINUM;
    }
    if (lowerFilename.find("steel") != std::string::npos) {
        if (lowerFilename.find("stainless") != std::string::npos) {
            return MaterialType::STAINLESS_STEEL;
        }
        return MaterialType::STEEL;
    }
    if (lowerFilename.find("brass") != std::string::npos) {
        return MaterialType::BRASS;
    }
    if (lowerFilename.find("bronze") != std::string::npos) {
        return MaterialType::BRONZE;
    }
    if (lowerFilename.find("copper") != std::string::npos) {
        return MaterialType::COPPER;
    }
    if (lowerFilename.find("chrome") != std::string::npos) {
        return MaterialType::CHROME;
    }
    if (lowerFilename.find("titanium") != std::string::npos) {
        return MaterialType::TITANIUM;
    }
    if (lowerFilename.find("plastic") != std::string::npos) {
        if (lowerFilename.find("white") != std::string::npos) {
            return MaterialType::PLASTIC_WHITE;
        }
        if (lowerFilename.find("black") != std::string::npos) {
            return MaterialType::PLASTIC_BLACK;
        }
        return MaterialType::PLASTIC_WHITE;
    }
    if (lowerFilename.find("rubber") != std::string::npos) {
        return MaterialType::RUBBER;
    }
    if (lowerFilename.find("glass") != std::string::npos) {
        return MaterialType::GLASS;
    }
    
    // Default to aluminum for CAD files (common in mechanical engineering)
    return MaterialType::ALUMINUM;
}

MaterialType MaterialLibrary::GetMaterialFromShapeIndex(int index) {
    // Cycle through different metals for variety
    const MaterialType metalMaterials[] = {
        MaterialType::ALUMINUM,
        MaterialType::STEEL,
        MaterialType::BRASS,
        MaterialType::COPPER,
        MaterialType::STAINLESS_STEEL,
        MaterialType::TITANIUM
    };
    
    int materialCount = sizeof(metalMaterials) / sizeof(metalMaterials[0]);
    return metalMaterials[index % materialCount];
}

} // namespace CADMaterials