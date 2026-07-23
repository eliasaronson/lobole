#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Image-based lighting precompute (CPU). Loads an equirectangular HDR
// environment and derives the split-sum IBL inputs:
//   - irradiance: diffuse environment lighting, sampled by the surface normal
//   - prefiltered specular: a roughness mip chain, sampled by the reflection
//   - BRDF LUT: the environment BRDF (scale, bias) over (NdotV, roughness)
//
// All maps are held as float RGBA (irradiance/prefilter) or RG (BRDF). The
// Vulkan upload lives in the renderer; this stays GPU-free so the math is
// testable and dumpable to PNG. Results are cached to disk keyed by the source
// + parameters, so the (multi-second) convolution runs only once per HDR.
class IBLPrecompute {
public:
  // A single equirectangular float image (RGBA, row-major, origin top-left).
  struct Image {
    int width = 0;
    int height = 0;
    std::vector<glm::vec4> pixels; // size = width*height
  };

  // Loads the HDR and computes all maps (from cache when available). Returns
  // false if the HDR can't be loaded. `cacheDir` may be empty to disable the
  // cache.
  bool Build(const std::string& hdrPath, const std::string& cacheDir);

  const Image& Environment() const { return m_env; } // full-res source HDR
  const Image& Irradiance() const { return m_irradiance; }
  const std::vector<Image>& Prefilter() const { return m_prefilter; } // mip 0..N-1
  const Image& BrdfLut() const { return m_brdf; }
  int PrefilterMipCount() const { return static_cast<int>(m_prefilter.size()); }

  // Writes irradiance / each prefilter mip / BRDF to PNGs under dir (tone-mapped
  // for HDR maps). For eyeballing the precompute in isolation.
  void DumpDebugPNGs(const std::string& dir) const;

private:
  Image m_env;         // source equirectangular environment
  Image m_irradiance;
  std::vector<Image> m_prefilter;
  Image m_brdf;

  bool LoadHDR(const std::string& path);
  void ComputeIrradiance();
  void ComputePrefilter();
  void ComputeBRDF();

  bool LoadCache(const std::string& path);
  void SaveCache(const std::string& path) const;
};
