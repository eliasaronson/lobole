// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/MeshImportCost.h"

#include "core/io/ImportFormats.h"
#include "core/mesh/MeshReaders.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace interchange {

namespace {

// The law, t = kCostAt10k * (n / 10^4)^kCostExponent seconds, fitted (least
// squares in log-log) to the whole `import_step` calls of the slowest meshes
// measured, at 40k, 70k and 100k triangles, where the limit falls: the
// table is docs/interchange/README.md#what-a-mesh-costs. The exponent is the
// measured one, not a guess at the algorithm's: what grows is the sewing's
// joining of faces into shells (BRepTools_Quilt), which is most of a large
// import. A mesh in grid order costs a third to a fifth of this; a whole
// closed scan took longer than it says. The law is what an import may take.
constexpr double kCostAt10k = 0.36;
constexpr double kCostExponent = 2.2;

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Whitespace-separated words of `line`.
size_t Words(std::string_view line) {
  size_t n = 0;
  bool in = false;
  for (char c : line) {
    const bool word = !IsSpace(c);
    if (word && !in)
      n++;
    in = word;
  }
  return n;
}

std::string_view TrimLeft(std::string_view s) {
  size_t i = 0;
  while (i < s.size() && IsSpace(s[i]))
    i++;
  return s.substr(i);
}

bool StartsWordNoCase(std::string_view s, std::string_view word) {
  if (s.size() < word.size())
    return false;
  for (size_t i = 0; i < word.size(); i++)
    if (std::tolower(static_cast<unsigned char>(s[i])) != word[i])
      return false;
  return s.size() == word.size() || IsSpace(s[word.size()]);
}

std::optional<uint64_t> CountStl(const std::string& path) {
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec)
    return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  if (size >= 84) {
    char header[84];
    in.read(header, 84);
    if (in.gcount() == 84) {
      const auto* b = reinterpret_cast<const unsigned char*>(header + 80);
      const uint64_t count = uint64_t{b[0]} | uint64_t{b[1]} << 8 |
                             uint64_t{b[2]} << 16 | uint64_t{b[3]} << 24;
      // A binary file is exactly its header and its records. An ASCII one
      // that starts "solid" and happens to fit is not likely; one whose size
      // disagrees is read as text below.
      if (84 + 50 * count == size)
        return count;
    }
  }
  in.clear();
  in.seekg(0);
  uint64_t facets = 0;
  std::string line;
  while (std::getline(in, line))
    if (StartsWordNoCase(TrimLeft(line), "facet"))
      facets++;
  return facets;
}

std::optional<uint64_t> CountObj(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  uint64_t triangles = 0;
  std::string line;
  while (std::getline(in, line)) {
    const std::string_view s = TrimLeft(line);
    if (s.size() < 2 || s[0] != 'f' || !IsSpace(s[1]))
      continue;
    const size_t corners = Words(s.substr(1));
    if (corners >= 3)
      triangles += corners - 2;
  }
  return triangles;
}

} // namespace

bool IsMeshFile(const std::string& path) {
  return IsMeshFormat(FormatOf(path));
}

std::optional<uint64_t> CountMeshTriangles(const std::string& path) {
  switch (FormatOf(path)) {
  case ImportFormat::Stl:
    return CountStl(path);
  case ImportFormat::Obj:
    return CountObj(path);
  case ImportFormat::Ply:
    return mesh::CountPlyFaces(path);
  case ImportFormat::Gltf:
    return mesh::CountGltfTriangles(path);
  default:
    return std::nullopt;
  }
}

double ExpectedMeshImportSeconds(uint64_t triangles) {
  return kCostAt10k *
         std::pow(static_cast<double>(triangles) / 1e4, kCostExponent);
}

uint64_t MeshTriangleLimit() {
  return static_cast<uint64_t>(
      1e4 * std::pow(kPatientImportSeconds / kCostAt10k, 1.0 / kCostExponent));
}

std::string DescribeSeconds(double seconds) {
  char buf[48];
  if (seconds < 1.0)
    return "under a second";
  if (seconds < 90.0)
    std::snprintf(buf, sizeof buf, "about %.0f s", seconds);
  else if (seconds < 90.0 * 60.0)
    std::snprintf(buf, sizeof buf, "about %.0f min", seconds / 60.0);
  else
    std::snprintf(buf, sizeof buf, "about %.1f h", seconds / 3600.0);
  return buf;
}

std::string DescribeTriangles(uint64_t triangles) {
  std::string digits = std::to_string(triangles);
  std::string out;
  for (size_t i = 0; i < digits.size(); i++) {
    if (i > 0 && (digits.size() - i) % 3 == 0)
      out += ',';
    out += digits[i];
  }
  return out + (triangles == 1 ? " triangle" : " triangles");
}

namespace {
// The figure's segmentation, the slowest organic fixture: fitted to its 10k
// and 30k points (docs/plans/organic-meshes-and-scans.md, stage 0).
constexpr double kRecognitionAt10k = 2.0;
constexpr double kRecognitionExponent = 2.2;
// What a mesh body puts in the tessellation budget a triangle: a closed
// mesh has about half as many vertices as triangles, a vertex is 24 bytes
// and a triangle three 4-byte indices, and the outline asset uploads the
// buffers again.
constexpr double kTessellationBudgetBytes = kMeshBodyBudgetBytes;
constexpr double kMeshBodyBytesPerTriangle = 2.0 * (0.5 * 24.0 + 12.0);
} // namespace

double ExpectedRecognitionSeconds(uint64_t triangles) {
  return kRecognitionAt10k *
         std::pow(double(triangles) / 1e4, kRecognitionExponent);
}

uint64_t RecognitionTriangleLimit() {
  return uint64_t(1e4 * std::pow(kPatientImportSeconds / kRecognitionAt10k,
                                 1.0 / kRecognitionExponent));
}

uint64_t MeshBodyTriangleLimit() {
  return uint64_t(kTessellationBudgetBytes / kMeshBodyBytesPerTriangle);
}

std::string MeshBodyTooLarge(const std::string& path, uint64_t triangles) {
  return "'" + path + "' is a mesh of " + DescribeTriangles(triangles) +
         ", past the " + DescribeTriangles(MeshBodyTriangleLimit()) +
         " a mesh body can hold on screen here: decimate it in MeshLab "
         "(Quadric Edge Collapse Decimation) or Blender (the Decimate "
         "modifier) to under " +
         DescribeTriangles(MeshBodyTriangleLimit()) + " and import that";
}

std::optional<MeshCost> MeshImportCost(const std::string& path) {
  if (!IsMeshFile(path))
    return std::nullopt;
  const std::optional<uint64_t> n = CountMeshTriangles(path);
  if (!n)
    return std::nullopt;
  MeshCost cost;
  cost.triangles = *n;
  cost.expectedSeconds = ExpectedMeshImportSeconds(*n);
  cost.overLimit = *n > MeshTriangleLimit();
  return cost;
}

std::string LargeMeshRefusal(const std::string& path, const MeshCost& cost,
                             const std::string& override) {
  return "'" + path + "' is a mesh of " + DescribeTriangles(cost.triangles) +
         ", which may take " + DescribeSeconds(cost.expectedSeconds) +
         " to import (every triangle becomes a face, and the faces are "
         "sewn); past " +
         DescribeTriangles(MeshTriangleLimit()) + " (about " +
         std::to_string(static_cast<int>(kPatientImportSeconds)) +
         " s) an import is refused unless asked for with " + override +
         ". A mesh decimated below the limit imports in less.";
}

} // namespace interchange
