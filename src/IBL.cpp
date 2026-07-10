#include "IBL.h"

#include "Logger.h"

#define STB_IMAGE_IMPLEMENTATION
#include "thirdparty/stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

namespace {

constexpr float kPI = 3.14159265358979323846f;

// Cache format version — bump when sizes/params/math below change so stale
// caches are rejected and recomputed.
constexpr uint32_t kCacheMagic = 0x49424C31; // "IBL1"
constexpr uint32_t kCacheVersion = 1;

// Map sizes (small: IBL inputs are low-frequency). Prefilter is a mip chain.
constexpr int kIrradW = 64, kIrradH = 32;
constexpr int kPrefilterBaseW = 128, kPrefilterBaseH = 64;
constexpr int kPrefilterMips = 5; // roughness 0 .. 1
constexpr int kBrdfSize = 128;

// Sample counts (traded against startup time; the disk cache makes it one-off).
constexpr float kIrradSampleDelta = 0.045f;
constexpr int kPrefilterSamples = 128;
constexpr int kBrdfSamples = 1024;

using Image = IBLPrecompute::Image;

glm::vec2 DirToEquirectUV(const glm::vec3& d) {
  float u = std::atan2(d.z, d.x) / (2.0f * kPI) + 0.5f;
  float v = std::acos(std::clamp(d.y, -1.0f, 1.0f)) / kPI; // 0 at +Y (top)
  return {u, v};
}

glm::vec3 EquirectUVToDir(float u, float v) {
  float phi = (u - 0.5f) * 2.0f * kPI;
  float theta = v * kPI;
  float sinT = std::sin(theta);
  return {sinT * std::cos(phi), std::cos(theta), sinT * std::sin(phi)};
}

// Bilinear sample of an equirectangular image: wrap in u (longitude), clamp in
// v (latitude).
glm::vec4 Sample(const Image& img, const glm::vec3& dir) {
  glm::vec2 uv = DirToEquirectUV(glm::normalize(dir));
  float x = uv.x * img.width - 0.5f;
  float y = uv.y * img.height - 0.5f;
  int x0 = static_cast<int>(std::floor(x));
  int y0 = static_cast<int>(std::floor(y));
  float fx = x - x0, fy = y - y0;
  auto at = [&](int xi, int yi) -> const glm::vec4& {
    xi = ((xi % img.width) + img.width) % img.width;
    yi = std::clamp(yi, 0, img.height - 1);
    return img.pixels[static_cast<size_t>(yi) * img.width + xi];
  };
  const glm::vec4& a = at(x0, y0);
  const glm::vec4& b = at(x0 + 1, y0);
  const glm::vec4& c = at(x0, y0 + 1);
  const glm::vec4& d = at(x0 + 1, y0 + 1);
  return glm::mix(glm::mix(a, b, fx), glm::mix(c, d, fx), fy);
}

// Low-discrepancy Hammersley sequence for importance sampling.
float RadicalInverseVdC(uint32_t bits) {
  bits = (bits << 16u) | (bits >> 16u);
  bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
  bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
  bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
  bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
  return static_cast<float>(bits) * 2.3283064365386963e-10f;
}
glm::vec2 Hammersley(uint32_t i, uint32_t n) {
  return {static_cast<float>(i) / static_cast<float>(n), RadicalInverseVdC(i)};
}

glm::vec3 ImportanceSampleGGX(const glm::vec2& Xi, const glm::vec3& N,
                              float roughness) {
  float a = roughness * roughness;
  float phi = 2.0f * kPI * Xi.x;
  float cosT = std::sqrt((1.0f - Xi.y) / (1.0f + (a * a - 1.0f) * Xi.y));
  float sinT = std::sqrt(1.0f - cosT * cosT);
  glm::vec3 H{std::cos(phi) * sinT, std::sin(phi) * sinT, cosT};
  glm::vec3 up = std::abs(N.z) < 0.999f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
  glm::vec3 tangent = glm::normalize(glm::cross(up, N));
  glm::vec3 bitangent = glm::cross(N, tangent);
  return glm::normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float GeometrySchlickGGX(float NdotV, float k) {
  return NdotV / (NdotV * (1.0f - k) + k);
}
float GeometrySmith(float NdotV, float NdotL, float roughness) {
  float k = (roughness * roughness) / 2.0f; // IBL variant
  return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// Runs `fn(row)` for row in [0,height) across hardware threads.
template <typename Fn> void ParallelRows(int height, Fn fn) {
  unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  unsigned n = std::min<unsigned>(hw, static_cast<unsigned>(height));
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < n; t++) {
    pool.emplace_back([=] {
      for (int y = static_cast<int>(t); y < height; y += static_cast<int>(n))
        fn(y);
    });
  }
  for (auto& th : pool) th.join();
}

} // namespace

bool IBLPrecompute::LoadHDR(const std::string& path) {
  int w = 0, h = 0, comp = 0;
  float* data = stbi_loadf(path.c_str(), &w, &h, &comp, 3);
  if (!data) {
    LOG_ERROR("IBL: failed to load HDR '{}': {}", path, stbi_failure_reason());
    return false;
  }
  m_env.width = w;
  m_env.height = h;
  m_env.pixels.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < m_env.pixels.size(); i++)
    m_env.pixels[i] = {data[i * 3 + 0], data[i * 3 + 1], data[i * 3 + 2], 1.0f};
  stbi_image_free(data);
  LOG_INFO("IBL: loaded environment {}x{}", w, h);
  return true;
}

void IBLPrecompute::ComputeIrradiance() {
  m_irradiance.width = kIrradW;
  m_irradiance.height = kIrradH;
  m_irradiance.pixels.resize(static_cast<size_t>(kIrradW) * kIrradH);
  ParallelRows(kIrradH, [&](int y) {
    for (int x = 0; x < kIrradW; x++) {
      float u = (x + 0.5f) / kIrradW;
      float v = (y + 0.5f) / kIrradH;
      glm::vec3 N = EquirectUVToDir(u, v);
      glm::vec3 up = std::abs(N.y) < 0.999f ? glm::vec3(0, 1, 0)
                                            : glm::vec3(1, 0, 0);
      glm::vec3 right = glm::normalize(glm::cross(up, N));
      up = glm::cross(N, right);

      glm::vec3 irradiance(0.0f);
      int samples = 0;
      for (float phi = 0.0f; phi < 2.0f * kPI; phi += kIrradSampleDelta) {
        for (float theta = 0.0f; theta < 0.5f * kPI; theta += kIrradSampleDelta) {
          glm::vec3 tan{std::sin(theta) * std::cos(phi),
                        std::sin(theta) * std::sin(phi), std::cos(theta)};
          glm::vec3 dir = right * tan.x + up * tan.y + N * tan.z;
          glm::vec4 s = Sample(m_env, dir);
          irradiance += glm::vec3(s) * std::cos(theta) * std::sin(theta);
          samples++;
        }
      }
      irradiance = kPI * irradiance / static_cast<float>(samples);
      m_irradiance.pixels[static_cast<size_t>(y) * kIrradW + x] = {
          irradiance, 1.0f};
    }
  });
}

void IBLPrecompute::ComputePrefilter() {
  m_prefilter.clear();
  for (int mip = 0; mip < kPrefilterMips; mip++) {
    int w = std::max(1, kPrefilterBaseW >> mip);
    int h = std::max(1, kPrefilterBaseH >> mip);
    float roughness = static_cast<float>(mip) / (kPrefilterMips - 1);
    Image out;
    out.width = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h);
    ParallelRows(h, [&](int y) {
      for (int x = 0; x < w; x++) {
        float u = (x + 0.5f) / w;
        float v = (y + 0.5f) / h;
        glm::vec3 N = EquirectUVToDir(u, v);
        glm::vec3 V = N; // split-sum assumption N=V=R
        glm::vec3 sum(0.0f);
        float weight = 0.0f;
        for (int i = 0; i < kPrefilterSamples; i++) {
          glm::vec2 Xi = Hammersley(i, kPrefilterSamples);
          glm::vec3 H = ImportanceSampleGGX(Xi, N, roughness);
          glm::vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);
          float NdotL = glm::dot(N, L);
          if (NdotL > 0.0f) {
            sum += glm::vec3(Sample(m_env, L)) * NdotL;
            weight += NdotL;
          }
        }
        glm::vec3 c = weight > 0.0f ? sum / weight : glm::vec3(Sample(m_env, N));
        out.pixels[static_cast<size_t>(y) * w + x] = {c, 1.0f};
      }
    });
    m_prefilter.push_back(std::move(out));
  }
}

void IBLPrecompute::ComputeBRDF() {
  m_brdf.width = kBrdfSize;
  m_brdf.height = kBrdfSize;
  m_brdf.pixels.resize(static_cast<size_t>(kBrdfSize) * kBrdfSize);
  const glm::vec3 N(0, 0, 1);
  ParallelRows(kBrdfSize, [&](int y) {
    float roughness = (y + 0.5f) / kBrdfSize;
    for (int x = 0; x < kBrdfSize; x++) {
      float NdotV = (x + 0.5f) / kBrdfSize;
      glm::vec3 V{std::sqrt(1.0f - NdotV * NdotV), 0.0f, NdotV};
      float A = 0.0f, B = 0.0f;
      for (int i = 0; i < kBrdfSamples; i++) {
        glm::vec2 Xi = Hammersley(i, kBrdfSamples);
        glm::vec3 H = ImportanceSampleGGX(Xi, N, roughness);
        glm::vec3 L = glm::normalize(2.0f * glm::dot(V, H) * H - V);
        float NdotL = std::max(L.z, 0.0f);
        float NdotH = std::max(H.z, 0.0f);
        float VdotH = std::max(glm::dot(V, H), 0.0f);
        if (NdotL > 0.0f) {
          float G = GeometrySmith(NdotV, NdotL, roughness);
          float Gvis = G * VdotH / (NdotH * NdotV);
          float Fc = std::pow(1.0f - VdotH, 5.0f);
          A += (1.0f - Fc) * Gvis;
          B += Fc * Gvis;
        }
      }
      m_brdf.pixels[static_cast<size_t>(y) * kBrdfSize + x] = {
          A / kBrdfSamples, B / kBrdfSamples, 0.0f, 1.0f};
    }
  });
}

// ---- disk cache ----------------------------------------------------------

static void WriteImage(std::ofstream& f, const Image& img) {
  int32_t wh[2] = {img.width, img.height};
  f.write(reinterpret_cast<const char*>(wh), sizeof(wh));
  f.write(reinterpret_cast<const char*>(img.pixels.data()),
          static_cast<std::streamsize>(img.pixels.size() * sizeof(glm::vec4)));
}
static bool ReadImage(std::ifstream& f, Image& img) {
  int32_t wh[2] = {0, 0};
  f.read(reinterpret_cast<char*>(wh), sizeof(wh));
  if (!f || wh[0] <= 0 || wh[1] <= 0) return false;
  img.width = wh[0];
  img.height = wh[1];
  img.pixels.resize(static_cast<size_t>(wh[0]) * wh[1]);
  f.read(reinterpret_cast<char*>(img.pixels.data()),
         static_cast<std::streamsize>(img.pixels.size() * sizeof(glm::vec4)));
  return static_cast<bool>(f);
}

bool IBLPrecompute::LoadCache(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  uint32_t magic = 0, version = 0, mips = 0;
  f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  f.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (!f || magic != kCacheMagic || version != kCacheVersion) return false;
  f.read(reinterpret_cast<char*>(&mips), sizeof(mips));
  if (!f || mips != static_cast<uint32_t>(kPrefilterMips)) return false;
  if (!ReadImage(f, m_irradiance)) return false;
  m_prefilter.resize(mips);
  for (auto& m : m_prefilter)
    if (!ReadImage(f, m)) return false;
  if (!ReadImage(f, m_brdf)) return false;
  LOG_INFO("IBL: loaded precompute from cache '{}'", path);
  return true;
}

void IBLPrecompute::SaveCache(const std::string& path) const {
  std::ofstream f(path, std::ios::binary);
  if (!f) return;
  uint32_t mips = static_cast<uint32_t>(m_prefilter.size());
  f.write(reinterpret_cast<const char*>(&kCacheMagic), sizeof(kCacheMagic));
  f.write(reinterpret_cast<const char*>(&kCacheVersion), sizeof(kCacheVersion));
  f.write(reinterpret_cast<const char*>(&mips), sizeof(mips));
  WriteImage(f, m_irradiance);
  for (const auto& m : m_prefilter) WriteImage(f, m);
  WriteImage(f, m_brdf);
}

bool IBLPrecompute::Build(const std::string& hdrPath,
                          const std::string& cacheDir) {
  std::string cachePath;
  if (!cacheDir.empty()) {
    // Cache basename derived from the HDR filename only (params ride the
    // version); good enough for the handful of bundled HDRIs.
    size_t slash = hdrPath.find_last_of("/\\");
    std::string base =
        slash == std::string::npos ? hdrPath : hdrPath.substr(slash + 1);
    cachePath = cacheDir + "/" + base + ".iblcache";
  }

  // The full-res source is needed downstream too (offline-render background /
  // sharp reflections), so load it even on a cache hit — the cache only skips
  // the convolution.
  if (!LoadHDR(hdrPath)) return false;
  if (!cachePath.empty() && LoadCache(cachePath)) return true;

  LOG_INFO("IBL: precomputing irradiance / prefilter / BRDF (one-time)...");
  ComputeIrradiance();
  ComputePrefilter();
  ComputeBRDF();
  if (!cachePath.empty()) SaveCache(cachePath);
  return true;
}

// ---- debug dump ----------------------------------------------------------

// Writes a binary PPM (P6) — no image-encoder dependency, viewable after a
// trivial convert. `tonemap` applies Reinhard+gamma for HDR maps; the BRDF LUT
// (data, not color) is written raw with the two channels in R,G.
static void WritePPM(const std::string& path, const Image& img, bool tonemap) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return;
  f << "P6\n" << img.width << " " << img.height << "\n255\n";
  std::vector<uint8_t> rgb(static_cast<size_t>(img.width) * img.height * 3);
  for (size_t i = 0; i < img.pixels.size(); i++) {
    glm::vec3 c = glm::vec3(img.pixels[i]);
    if (tonemap) {
      c = c / (c + glm::vec3(1.0f));
      c = glm::pow(c, glm::vec3(1.0f / 2.2f));
    }
    for (int k = 0; k < 3; k++)
      rgb[i * 3 + k] =
          static_cast<uint8_t>(std::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
  }
  f.write(reinterpret_cast<const char*>(rgb.data()),
          static_cast<std::streamsize>(rgb.size()));
}

void IBLPrecompute::DumpDebugPNGs(const std::string& dir) const {
  WritePPM(dir + "/ibl_irradiance.ppm", m_irradiance, true);
  for (size_t i = 0; i < m_prefilter.size(); i++)
    WritePPM(dir + "/ibl_prefilter_mip" + std::to_string(i) + ".ppm",
             m_prefilter[i], true);
  WritePPM(dir + "/ibl_brdf.ppm", m_brdf, false);
}
