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

    // Wood & engineered wood. Base tone only here; the grain figure is applied
    // procedurally in the shader (see the "procedural grain" block below).
    // Colors are the light early-wood tone — the shader darkens toward the
    // grain lines using grainContrast, so these read as the overall hue.
    // NOTE: these are pre-darkened to survive the scene's bright lighting rig
    // (primary intensity 1.8 + IBL + 1.1 boost + ACES tonemap wash light
    // albedos out ~1.4x); tuned against a render, not raw perceptual targets.
    materials[MaterialType::WOOD_OAK] = {
        glm::vec3(0.42f, 0.28f, 0.14f),  // Warm honey brown
        0.0f, 0.55f, 0.4f, 0.04f, "Oak"
    };
    materials[MaterialType::WOOD_PINE] = {
        glm::vec3(0.60f, 0.45f, 0.25f),  // Pale yellow
        0.0f, 0.6f, 0.35f, 0.04f, "Pine"
    };
    materials[MaterialType::WOOD_WALNUT] = {
        glm::vec3(0.085f, 0.052f, 0.033f), // Dark chocolate brown
        0.0f, 0.5f, 0.45f, 0.04f, "Walnut"
    };
    materials[MaterialType::PLYWOOD_BIRCH] = {
        glm::vec3(0.60f, 0.48f, 0.30f),  // Pale cream birch face
        0.0f, 0.55f, 0.4f, 0.04f, "Birch Plywood"
    };
    materials[MaterialType::PLYWOOD_PINE] = {
        glm::vec3(0.55f, 0.41f, 0.23f),  // Softwood ply face
        0.0f, 0.6f, 0.35f, 0.04f, "Pine Plywood"
    };
    materials[MaterialType::MDF] = {
        glm::vec3(0.40f, 0.29f, 0.19f),  // Uniform tan-brown, matte
        0.0f, 0.75f, 0.25f, 0.04f, "MDF"
    };

    // Default material
    materials[MaterialType::DEFAULT] = materials[MaterialType::ALUMINUM];

    // Densities (kg/m^3), for mass properties (inspection I2). Set after the
    // DEFAULT copy above so DEFAULT keeps density 0 = unknown: an unassigned
    // body reports no mass instead of a silently-guessed aluminum one.
    materials[MaterialType::ALUMINUM].density = 2700.0f;
    materials[MaterialType::STEEL].density = 7850.0f;
    materials[MaterialType::STAINLESS_STEEL].density = 8000.0f;
    materials[MaterialType::BRASS].density = 8530.0f;
    materials[MaterialType::BRONZE].density = 8800.0f;
    materials[MaterialType::COPPER].density = 8960.0f;
    materials[MaterialType::CHROME].density = 7190.0f;
    materials[MaterialType::GOLD].density = 19300.0f;
    materials[MaterialType::SILVER].density = 10490.0f;
    materials[MaterialType::TITANIUM].density = 4506.0f;
    materials[MaterialType::PLASTIC_WHITE].density = 1040.0f;
    materials[MaterialType::PLASTIC_BLACK].density = 1040.0f;
    materials[MaterialType::RUBBER].density = 1200.0f;
    materials[MaterialType::GLASS].density = 2500.0f;
    materials[MaterialType::CONCRETE].density = 2400.0f;
    // Wood densities are moisture-dependent; these are typical air-dry (~12% MC)
    // values, well-defined enough for mass properties.
    materials[MaterialType::WOOD_OAK].density = 700.0f;
    materials[MaterialType::WOOD_PINE].density = 500.0f;
    materials[MaterialType::WOOD_WALNUT].density = 610.0f;
    materials[MaterialType::PLYWOOD_BIRCH].density = 680.0f;
    materials[MaterialType::PLYWOOD_PINE].density = 500.0f;
    materials[MaterialType::MDF].density = 750.0f;

    // Linear-elastic properties for FEM (Young's modulus GPa, Poisson ratio,
    // yield strength MPa). Representative handbook values; like density they
    // are left 0 on DEFAULT so a study on an unassigned body fails loudly.
    // Yield is 0 where it isn't a meaningful single number (brittle/soft
    // materials) — the study then reports stress without a factor of safety.
    auto elastic = [&](MaterialType t, float e, float nu, float yield) {
        materials[t].youngGPa = e;
        materials[t].poisson = nu;
        materials[t].yieldMPa = yield;
    };
    elastic(MaterialType::ALUMINUM, 69.0f, 0.33f, 95.0f);
    elastic(MaterialType::STEEL, 200.0f, 0.30f, 250.0f);
    elastic(MaterialType::STAINLESS_STEEL, 193.0f, 0.30f, 215.0f);
    elastic(MaterialType::BRASS, 100.0f, 0.34f, 200.0f);
    elastic(MaterialType::BRONZE, 110.0f, 0.34f, 125.0f);
    elastic(MaterialType::COPPER, 117.0f, 0.34f, 70.0f);
    elastic(MaterialType::CHROME, 279.0f, 0.21f, 0.0f);
    elastic(MaterialType::GOLD, 79.0f, 0.44f, 0.0f);
    elastic(MaterialType::SILVER, 83.0f, 0.37f, 0.0f);
    elastic(MaterialType::TITANIUM, 116.0f, 0.32f, 880.0f);
    elastic(MaterialType::PLASTIC_WHITE, 2.5f, 0.35f, 50.0f);
    elastic(MaterialType::PLASTIC_BLACK, 2.5f, 0.35f, 50.0f);
    elastic(MaterialType::GLASS, 70.0f, 0.22f, 0.0f);
    elastic(MaterialType::CONCRETE, 30.0f, 0.20f, 0.0f);
    // Wood is strongly anisotropic (along-grain stiffness is ~15-20x the
    // transverse), but our FEM is linear-isotropic. These are representative
    // ALONG-GRAIN values (modulus, and modulus-of-rupture as a yield proxy);
    // a study treats the part as isotropic, so results are approximate and
    // conservative only when the load happens to run along the grain. Plywood
    // is cross-laminated so its in-plane response is closer to isotropic.
    elastic(MaterialType::WOOD_OAK, 11.0f, 0.37f, 95.0f);
    elastic(MaterialType::WOOD_PINE, 9.0f, 0.35f, 80.0f);
    elastic(MaterialType::WOOD_WALNUT, 11.5f, 0.38f, 100.0f);
    elastic(MaterialType::PLYWOOD_BIRCH, 10.0f, 0.30f, 55.0f);
    elastic(MaterialType::PLYWOOD_PINE, 8.0f, 0.30f, 45.0f);
    elastic(MaterialType::MDF, 3.6f, 0.25f, 18.0f);

    // Procedural grain: pattern id, rings/plies per local unit, late-wood
    // contrast (0..1). Shaded in mesh.frag from local object-space position;
    // MDF stays flat (pattern 0). Scale is in per-mm; tune against a render.
    auto grain = [&](MaterialType t, int pattern, float scale, float contrast) {
        materials[t].procPattern = pattern;
        materials[t].grainScale = scale;
        materials[t].grainContrast = contrast;
    };
    grain(MaterialType::WOOD_OAK, 1, 0.15f, 0.35f);
    grain(MaterialType::WOOD_PINE, 1, 0.09f, 0.5f);   // wide rings, strong lines
    grain(MaterialType::WOOD_WALNUT, 1, 0.22f, 0.3f); // finer, figure-rich
    grain(MaterialType::PLYWOOD_BIRCH, 2, 0.7f, 0.3f); // plies per mm on edges
    grain(MaterialType::PLYWOOD_PINE, 2, 0.5f, 0.35f);
    // MDF: pattern 0 (flat) — no grain to apply.

    // RUBBER and DEFAULT intentionally left at 0 (near-incompressible /
    // unassigned): a linear-static study on them errors rather than guessing.
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

namespace {
// Single source of truth for slug <-> type and UI ordering. Grouped metals
// first, then non-metals; DEFAULT is intentionally excluded from AllTypes() (it
// is the empty-slug fallback, not a user-picked material).
const std::vector<std::pair<MaterialType, const char*>>& SlugTable() {
    static const std::vector<std::pair<MaterialType, const char*>> table = {
        {MaterialType::ALUMINUM, "aluminum"},
        {MaterialType::STEEL, "steel"},
        {MaterialType::STAINLESS_STEEL, "stainless_steel"},
        {MaterialType::BRASS, "brass"},
        {MaterialType::BRONZE, "bronze"},
        {MaterialType::COPPER, "copper"},
        {MaterialType::CHROME, "chrome"},
        {MaterialType::GOLD, "gold"},
        {MaterialType::SILVER, "silver"},
        {MaterialType::TITANIUM, "titanium"},
        {MaterialType::PLASTIC_WHITE, "plastic_white"},
        {MaterialType::PLASTIC_BLACK, "plastic_black"},
        {MaterialType::RUBBER, "rubber"},
        {MaterialType::GLASS, "glass"},
        {MaterialType::CONCRETE, "concrete"},
        {MaterialType::WOOD_OAK, "wood_oak"},
        {MaterialType::WOOD_PINE, "wood_pine"},
        {MaterialType::WOOD_WALNUT, "wood_walnut"},
        {MaterialType::PLYWOOD_BIRCH, "plywood_birch"},
        {MaterialType::PLYWOOD_PINE, "plywood_pine"},
        {MaterialType::MDF, "mdf"},
    };
    return table;
}
} // namespace

std::string MaterialLibrary::Slug(MaterialType type) {
    for (const auto& [t, slug] : SlugTable()) {
        if (t == type) return slug;
    }
    return ""; // DEFAULT (and anything unmapped) serializes as empty
}

MaterialType MaterialLibrary::FromSlug(const std::string& slug) {
    if (slug.empty()) return MaterialType::DEFAULT;
    for (const auto& [t, s] : SlugTable()) {
        if (slug == s) return t;
    }
    return MaterialType::DEFAULT;
}

const std::vector<MaterialType>& MaterialLibrary::AllTypes() {
    static const std::vector<MaterialType> types = [] {
        std::vector<MaterialType> v;
        for (const auto& [t, slug] : SlugTable()) v.push_back(t);
        return v;
    }();
    return types;
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