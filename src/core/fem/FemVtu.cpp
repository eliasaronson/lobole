// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/FemVtu.h"

#include <charconv>
#include <cstdio>

namespace fem {

namespace {

// Shortest text that reads back as the float the value is (fields are
// float: docs/conventions.md#the-model-is-double).
void Num(std::string& out, double v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof buf, static_cast<float>(v));
  out.append(buf, r.ptr);
}
void Coord(std::string& out, double v) {
  char buf[40];
  const auto r = std::to_chars(buf, buf + sizeof buf, v);
  out.append(buf, r.ptr);
}

void OpenArray(std::string& out, const char* type, const std::string& name,
               int components) {
  out += "        <DataArray type=\"";
  out += type;
  out += "\" Name=\"" + name + "\"";
  if (components > 1)
    out += " NumberOfComponents=\"" + std::to_string(components) + "\"";
  out += " format=\"ascii\">\n";
}
void CloseArray(std::string& out) { out += "        </DataArray>\n"; }

} // namespace

std::string VtuRefusal(const StudyRuntime& rt) {
  if (rt.bodies.empty())
    return "the result has no bodies";
  for (const StudyBodyResult& b : rt.bodies)
    if (b.mesh.tets.empty())
      // docs/simulation/README.md#what-a-saved-result-keeps
      return "this result keeps its boundary surface only, as every stored "
             "result does, so it has no elements to write; run the study in "
             "this session (run_study {recompute: true}) and export while "
             "the app is open";
  return "";
}

std::string VtuText(const StudyRuntime& rt, int caseIndex,
                    VtuSummary* summary) {
  VtuSummary sum;
  for (const StudyBodyResult& b : rt.bodies) {
    sum.points += b.mesh.nodes.size();
    sum.cells += b.mesh.tets.size();
  }
  const auto shown = [&](const StudyBodyResult& b) -> const FemResults& {
    return b.Case(caseIndex);
  };
  // An array is written when every body holds it, one entry per node.
  const auto every = [&](auto&& sizeOf) {
    for (const StudyBodyResult& b : rt.bodies)
      if (sizeOf(b) != b.mesh.nodes.size() || b.mesh.nodes.empty())
        return false;
    return !rt.bodies.empty();
  };

  std::string out;
  out.reserve(sum.points * 160 + sum.cells * 90 + 4096);
  out += "<?xml version=\"1.0\"?>\n"
         "<VTKFile type=\"UnstructuredGrid\" version=\"0.1\" "
         "byte_order=\"LittleEndian\">\n"
         "  <UnstructuredGrid>\n";
  out += "    <Piece NumberOfPoints=\"" + std::to_string(sum.points) +
         "\" NumberOfCells=\"" + std::to_string(sum.cells) + "\">\n";

  // --- Point data.
  out += "      <PointData>\n";
  const auto vec3 = [&](const std::string& name, auto&& of) {
    OpenArray(out, "Float32", name, 3);
    for (const StudyBodyResult& b : rt.bodies)
      for (const glm::dvec3& v : of(b)) {
        Num(out, v.x);
        out += ' ';
        Num(out, v.y);
        out += ' ';
        Num(out, v.z);
        out += '\n';
      }
    CloseArray(out);
    sum.pointData.push_back(name);
  };
  const auto scalar = [&](const std::string& name, auto&& of) {
    OpenArray(out, "Float32", name, 1);
    for (const StudyBodyResult& b : rt.bodies)
      for (double v : of(b)) {
        Num(out, v);
        out += '\n';
      }
    CloseArray(out);
    sum.pointData.push_back(name);
  };
  if (every([&](const StudyBodyResult& b) {
        return shown(b).displacement.size();
      }))
    vec3("displacement", [&](const StudyBodyResult& b) -> const auto& {
      return shown(b).displacement;
    });
  if (every([&](const StudyBodyResult& b) { return shown(b).vonMises.size(); }))
    scalar("von_mises", [&](const StudyBodyResult& b) -> const auto& {
      return shown(b).vonMises;
    });
  if (every([&](const StudyBodyResult& b) { return shown(b).stress.size(); })) {
    // Ours is xx yy zz xy yz zx, which is VTK's XX YY ZZ XY YZ XZ.
    OpenArray(out, "Float32", "stress", 6);
    for (const StudyBodyResult& b : rt.bodies)
      for (const Stress& s : shown(b).stress) {
        for (int k = 0; k < 6; ++k) {
          Num(out, s[k]);
          out += k < 5 ? ' ' : '\n';
        }
      }
    CloseArray(out);
    sum.pointData.push_back("stress");
  }
  if (every([&](const StudyBodyResult& b) {
        return shown(b).plasticStrain.size();
      }))
    scalar("plastic_strain", [&](const StudyBodyResult& b) -> const auto& {
      return shown(b).plasticStrain;
    });
  if (every([&](const StudyBodyResult& b) {
        return shown(b).temperature.size();
      }))
    scalar("temperature", [&](const StudyBodyResult& b) -> const auto& {
      return shown(b).temperature;
    });
  if (every([&](const StudyBodyResult& b) { return shown(b).heatFlux.size(); }))
    vec3("heat_flux", [&](const StudyBodyResult& b) -> const auto& {
      return shown(b).heatFlux;
    });
  {
    std::size_t modes =
        rt.bodies.empty() ? 0 : rt.bodies[0].results.modes.size();
    for (const StudyBodyResult& b : rt.bodies)
      modes = std::min(modes, b.results.modes.size());
    for (std::size_t k = 0; k < modes; ++k)
      if (every([&](const StudyBodyResult& b) {
            return b.results.modes[k].displacement.size();
          }))
        vec3("mode_" + std::to_string(k + 1),
             [&](const StudyBodyResult& b) -> const auto& {
               return b.results.modes[k].displacement;
             });
  }
  out += "      </PointData>\n";

  // --- Cell data.
  out += "      <CellData>\n";
  OpenArray(out, "Int32", "body", 1);
  for (std::size_t i = 0; i < rt.bodies.size(); ++i)
    for (std::size_t t = 0; t < rt.bodies[i].mesh.tets.size(); ++t)
      out += std::to_string(i) + "\n";
  CloseArray(out);
  sum.cellData.push_back("body");
  OpenArray(out, "Int32", "domain", 1);
  for (const StudyBodyResult& b : rt.bodies)
    for (std::size_t t = 0; t < b.mesh.tets.size(); ++t)
      out += std::to_string(t < b.mesh.tetDomain.size() ? b.mesh.tetDomain[t]
                                                        : 0) +
             "\n";
  CloseArray(out);
  sum.cellData.push_back("domain");
  bool density = !rt.bodies.empty();
  for (const StudyBodyResult& b : rt.bodies)
    density =
        density && !b.density.empty() && b.density.size() == b.mesh.tets.size();
  if (density) {
    OpenArray(out, "Float32", "density", 1);
    for (const StudyBodyResult& b : rt.bodies)
      for (double d : b.density) {
        Num(out, d);
        out += '\n';
      }
    CloseArray(out);
    sum.cellData.push_back("density");
  }
  out += "      </CellData>\n";

  // --- The grid.
  out += "      <Points>\n";
  OpenArray(out, "Float64", "Points", 3);
  for (const StudyBodyResult& b : rt.bodies)
    for (const glm::dvec3& p : b.mesh.nodes) {
      Coord(out, p.x);
      out += ' ';
      Coord(out, p.y);
      out += ' ';
      Coord(out, p.z);
      out += '\n';
    }
  CloseArray(out);
  out += "      </Points>\n      <Cells>\n";
  OpenArray(out, "Int64", "connectivity", 1);
  std::size_t base = 0;
  for (const StudyBodyResult& b : rt.bodies) {
    for (const auto& t : b.mesh.tets) {
      const glm::dvec3 a = b.mesh.nodes[t[0]];
      const double v =
          glm::dot(b.mesh.nodes[t[1]] - a,
                   glm::cross(b.mesh.nodes[t[2]] - a, b.mesh.nodes[t[3]] - a));
      const std::array<int, 10>& order =
          v >= 0.0 ? kVtkTet10Positive : kVtkTet10Negative;
      for (int k = 0; k < 10; ++k) {
        out += std::to_string(base + static_cast<std::size_t>(t[order[k]]));
        out += k < 9 ? ' ' : '\n';
      }
    }
    base += b.mesh.nodes.size();
  }
  CloseArray(out);
  OpenArray(out, "Int64", "offsets", 1);
  for (std::size_t c = 1; c <= sum.cells; ++c)
    out += std::to_string(c * 10) + "\n";
  CloseArray(out);
  OpenArray(out, "UInt8", "types", 1);
  for (std::size_t c = 0; c < sum.cells; ++c)
    out += "24\n"; // VTK_QUADRATIC_TETRA
  CloseArray(out);
  out += "      </Cells>\n    </Piece>\n  </UnstructuredGrid>\n</VTKFile>\n";
  sum.bytes = out.size();
  if (summary)
    *summary = std::move(sum);
  return out;
}

} // namespace fem
