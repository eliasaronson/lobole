// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/MeshReaders.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>

namespace mesh {

namespace {

constexpr size_t kCancelEvery = 1u << 16; // triangles between cancel polls

bool IsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
         c == '\v';
}

// Significant digits printed in a number's mantissa ("1.234560e+01" is 7,
// "0.0125" is 3, "6" is 1).
int SignificantDigits(std::string_view s) {
  int digits = 0;
  bool leading = true;
  for (char c : s) {
    if (c == 'e' || c == 'E')
      break;
    if (c < '0' || c > '9')
      continue;
    if (leading && c == '0')
      continue;
    leading = false;
    digits++;
  }
  return digits;
}

struct AsciiReader {
  std::string_view text;
  size_t pos = 0;

  std::string_view Word() {
    while (pos < text.size() && IsSpace(text[pos]))
      pos++;
    const size_t start = pos;
    while (pos < text.size() && !IsSpace(text[pos]))
      pos++;
    return text.substr(start, pos - start);
  }
};

bool EqualNoCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

bool ParseDouble(std::string_view w, double& v) {
  const char* first = w.data();
  if (!w.empty() && w.front() == '+')
    first++;
  const auto r = std::from_chars(first, w.data() + w.size(), v);
  return r.ec == std::errc() && r.ptr == w.data() + w.size() &&
         std::isfinite(v);
}

bool ReadAscii(std::string_view text, const std::string& name,
               std::vector<glm::dvec3>& soup, int& digits, std::string& error,
               const Cancelled& cancelled) {
  AsciiReader r{text};
  if (!EqualNoCase(r.Word(), "solid")) {
    error = "'" + name + "' is not an STL: it neither starts with `solid` " +
            "nor has the size its binary header says";
    return false;
  }
  // The solid's name runs to the end of the line.
  while (r.pos < text.size() && text[r.pos] != '\n')
    r.pos++;
  size_t facets = 0;
  for (;;) {
    const std::string_view w = r.Word();
    if (w.empty()) {
      error = "'" + name + "' ends before `endsolid`";
      return false;
    }
    if (EqualNoCase(w, "endsolid"))
      break;
    if (!EqualNoCase(w, "facet")) {
      error = "'" + name + "': expected `facet` or `endsolid`, found `" +
              std::string(w.substr(0, 32)) + "` after " +
              std::to_string(facets) + " facets";
      return false;
    }
    // facet normal nx ny nz -- the normal is not used: the corners'
    // order is what says which way a triangle faces.
    if (!EqualNoCase(r.Word(), "normal")) {
      error = "'" + name + "': facet " + std::to_string(facets + 1) +
              " has no `normal`";
      return false;
    }
    for (int i = 0; i < 3; i++)
      r.Word();
    if (!EqualNoCase(r.Word(), "outer") || !EqualNoCase(r.Word(), "loop")) {
      error = "'" + name + "': facet " + std::to_string(facets + 1) +
              " has no `outer loop`";
      return false;
    }
    for (int c = 0; c < 3; c++) {
      if (!EqualNoCase(r.Word(), "vertex")) {
        error = "'" + name + "': facet " + std::to_string(facets + 1) +
                " has fewer than three vertices";
        return false;
      }
      glm::dvec3 p;
      for (int k = 0; k < 3; k++) {
        const std::string_view num = r.Word();
        if (!ParseDouble(num, p[k])) {
          error = "'" + name + "': facet " + std::to_string(facets + 1) +
                  " has a vertex coordinate that is not a number (`" +
                  std::string(num.substr(0, 32)) + "`)";
          return false;
        }
        digits = std::max(digits, SignificantDigits(num));
      }
      soup.push_back(p);
    }
    if (!EqualNoCase(r.Word(), "endloop") ||
        !EqualNoCase(r.Word(), "endfacet")) {
      error = "'" + name + "': facet " + std::to_string(facets + 1) +
              " is not closed by `endloop endfacet` (more than three "
              "vertices?)";
      return false;
    }
    if (++facets % kCancelEvery == 0 && cancelled && cancelled()) {
      error = "cancelled";
      return false;
    }
  }
  return true;
}

} // namespace

bool ReadStlBytes(const std::string& bytes, const std::string& name,
                  TriMesh& out, std::string& error,
                  const Cancelled& cancelled) {
  out = {};
  std::vector<glm::dvec3> soup;
  bool binary = false;
  if (bytes.size() >= 84) {
    uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4); // little-endian, as STL is
    binary = bytes.size() == 84 + uint64_t{count} * 50;
    if (binary) {
      soup.reserve(size_t{count} * 3);
      for (uint32_t t = 0; t < count; t++) {
        const char* rec = bytes.data() + 84 + size_t{t} * 50 + 12;
        for (int c = 0; c < 3; c++) {
          float xyz[3];
          std::memcpy(xyz, rec + 12 * c, 12);
          if (!std::isfinite(xyz[0]) || !std::isfinite(xyz[1]) ||
              !std::isfinite(xyz[2])) {
            error = "'" + name + "': triangle " + std::to_string(t + 1) +
                    " has a corner that is not a number";
            return false;
          }
          soup.emplace_back(xyz[0], xyz[1], xyz[2]);
        }
        if ((t + 1) % kCancelEvery == 0 && cancelled && cancelled()) {
          error = "cancelled";
          return false;
        }
      }
    }
  }
  int digits = 0;
  if (!binary && !ReadAscii(bytes, name, soup, digits, error, cancelled))
    return false;
  if (soup.empty()) {
    error = "'" + name + "' holds no triangles";
    return false;
  }
  double quantum = Float32Quantum(soup);
  if (!binary && digits > 0) {
    // The place value of the last digit printed at the largest magnitude.
    double largest = 0.0;
    for (const glm::dvec3& p : soup)
      largest =
          std::max({largest, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
    if (largest > 0.0) {
      const double place =
          std::pow(10.0, std::floor(std::log10(largest)) + 1.0 - digits);
      quantum = std::max(quantum, place);
    }
  }
  out = FromSoup(soup, quantum);
  if (out.triangles.empty()) {
    error = "'" + name + "' holds only degenerate triangles";
    return false;
  }
  return true;
}

bool ReadStl(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "'" + path + "' cannot be read";
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  return ReadStlBytes(bytes, path, out, error, cancelled);
}

namespace {

// The quantum of numbers printed with `digits` significant digits, at the
// largest magnitude in `soup` -- or float32's, whichever is coarser.
double PrintedQuantum(const std::vector<glm::dvec3>& soup, int digits) {
  double quantum = Float32Quantum(soup);
  double largest = 0.0;
  for (const glm::dvec3& p : soup)
    largest = std::max({largest, std::abs(p.x), std::abs(p.y), std::abs(p.z)});
  if (largest > 0.0 && digits > 0)
    quantum = std::max(quantum, std::pow(10.0, std::floor(std::log10(largest)) +
                                                   1.0 - digits));
  return quantum;
}

} // namespace

bool ReadObjBytes(const std::string& bytes, const std::string& name,
                  TriMesh& out, std::string& error,
                  const Cancelled& cancelled) {
  out = {};
  std::vector<glm::dvec3> verts, soup;
  int digits = 0;
  size_t line = 0, faces = 0;
  std::string_view text(bytes);
  size_t pos = 0;
  std::vector<int64_t> corner;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos)
      end = text.size();
    const std::string_view ln = text.substr(pos, end - pos);
    pos = end + 1;
    line++;
    AsciiReader r{ln};
    const std::string_view key = r.Word();
    if (key == "v") {
      glm::dvec3 p;
      for (int k = 0; k < 3; k++) {
        const std::string_view num = r.Word();
        if (!ParseDouble(num, p[k])) {
          error = "'" + name + "' line " + std::to_string(line) +
                  ": a vertex coordinate that is not a number (`" +
                  std::string(num.substr(0, 32)) + "`)";
          return false;
        }
        digits = std::max(digits, SignificantDigits(num));
      }
      verts.push_back(p);
    } else if (key == "f") {
      corner.clear();
      for (std::string_view w = r.Word(); !w.empty(); w = r.Word()) {
        const std::string_view idx = w.substr(0, w.find('/'));
        int64_t i = 0;
        const auto res =
            std::from_chars(idx.data(), idx.data() + idx.size(), i);
        if (res.ec != std::errc() || res.ptr != idx.data() + idx.size() ||
            i == 0) {
          error = "'" + name + "' line " + std::to_string(line) +
                  ": a face corner that is not a vertex number (`" +
                  std::string(w.substr(0, 32)) + "`)";
          return false;
        }
        const int64_t at = i > 0 ? i - 1 : int64_t(verts.size()) + i;
        if (at < 0 || at >= int64_t(verts.size())) {
          error = "'" + name + "' line " + std::to_string(line) +
                  ": a face names vertex " + std::to_string(i) + " of " +
                  std::to_string(verts.size());
          return false;
        }
        corner.push_back(at);
      }
      if (corner.size() < 3) {
        error = "'" + name + "' line " + std::to_string(line) +
                ": a face of fewer than three corners";
        return false;
      }
      for (size_t k = 1; k + 1 < corner.size(); k++) {
        soup.push_back(verts[size_t(corner[0])]);
        soup.push_back(verts[size_t(corner[k])]);
        soup.push_back(verts[size_t(corner[k + 1])]);
      }
      if (++faces % kCancelEvery == 0 && cancelled && cancelled()) {
        error = "cancelled";
        return false;
      }
    }
  }
  if (soup.empty()) {
    error = "'" + name + "' holds no faces";
    return false;
  }
  // An OBJ shares its vertices by index, so the soup welds back exactly;
  // the second weld is only half a printed digit, which never joins two
  // values printed differently (a cube of integer corners stays a cube).
  // The precision the fits read is still the printed digit's.
  const double printed = PrintedQuantum(soup, digits);
  out = FromSoup(soup, 0.5 * printed);
  out.quantum = printed;
  if (out.triangles.empty()) {
    error = "'" + name + "' holds only degenerate triangles";
    return false;
  }
  return true;
}

bool ReadObj(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "'" + path + "' cannot be read";
    return false;
  }
  const std::string bytes((std::istreambuf_iterator<char>(in)),
                          std::istreambuf_iterator<char>());
  return ReadObjBytes(bytes, path, out, error, cancelled);
}

} // namespace mesh
