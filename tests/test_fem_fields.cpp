// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The fields a result is read by, and its export
// (docs/simulation/README.md#results-display,
// docs/simulation/README.md#vtu-export): the principal stresses of a bar
// and a pressurised cylinder against their closed forms, the factors that
// read a stress against an allowable, and a .vtu read back with a parser of
// its own -- counts, arrays, and on a curved element the node order VTK
// expects.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADKernel.h"
#include "core/fem/FemFields.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemVtu.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("Principal stresses of known tensors", "[fem][fields]") {
  // Diagonal: the entries, sorted.
  auto p = fem::Principals({30.0, -10.0, 5.0, 0.0, 0.0, 0.0});
  CHECK_THAT(p[0], WithinAbs(30.0, 1e-9));
  CHECK_THAT(p[1], WithinAbs(5.0, 1e-9));
  CHECK_THAT(p[2], WithinAbs(-10.0, 1e-9));
  // Pure shear tau in xy: +tau, 0, -tau.
  p = fem::Principals({0.0, 0.0, 0.0, 12.0, 0.0, 0.0});
  CHECK_THAT(p[0], WithinAbs(12.0, 1e-9));
  CHECK_THAT(p[1], WithinAbs(0.0, 1e-9));
  CHECK_THAT(p[2], WithinAbs(-12.0, 1e-9));
  // Hydrostatic: all three the pressure.
  p = fem::Principals({-7.0, -7.0, -7.0, 0.0, 0.0, 0.0});
  CHECK_THAT(p[0], WithinAbs(-7.0, 1e-9));
  CHECK_THAT(p[2], WithinAbs(-7.0, 1e-9));
  // A full tensor: the invariants hold, and the largest is MaxPrincipal's.
  const fem::Stress s = {40.0, -15.0, 22.0, 9.0, -6.0, 13.0};
  p = fem::Principals(s);
  CHECK(p[0] >= p[1]);
  CHECK(p[1] >= p[2]);
  CHECK_THAT(p[0] + p[1] + p[2], WithinAbs(s[0] + s[1] + s[2], 1e-9));
  CHECK_THAT(p[0], WithinAbs(fem::MaxPrincipal(s), 1e-9));
  const double det = s[0] * (s[1] * s[2] - s[4] * s[4]) -
                     s[3] * (s[3] * s[2] - s[4] * s[5]) +
                     s[5] * (s[3] * s[4] - s[1] * s[5]);
  CHECK_THAT(p[0] * p[1] * p[2], WithinRel(det, 1e-9));
}

TEST_CASE("A field's key is refused when it is not one", "[fem][fields]") {
  CHECK(fem::ResultFieldFromKey("principal_min") ==
        fem::ResultField::PrincipalMin);
  CHECK_FALSE(fem::ResultFieldFromKey("principal").has_value());
  CHECK_FALSE(fem::ResultFieldFromKey("").has_value());
  CHECK_THAT(fem::ResultFieldKeys(), ContainsSubstring("factor_of_safety"));
  // Every field has a key that reads back, and a stress has its unit.
  for (const fem::ResultFieldSpec& spec : fem::ResultFieldSpecs())
    CHECK(fem::ResultFieldFromKey(spec.key) == spec.field);
  CHECK(std::string(fem::SpecOf(fem::ResultField::PrincipalMid).unit) == "MPa");
  CHECK(
      std::string(fem::SpecOf(fem::ResultField::FactorOfSafety).unit).empty());
}

TEST_CASE("Factors read a stress against its allowable, unbounded where "
          "there is none",
          "[fem][fields]") {
  fem::FemResults r;
  r.vonMises = {100.0, 0.0, 250.0};
  r.stress = {{0, 0, 10.0, 0, 0, 0}, {0, 0, -5.0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}};
  r.displacement.assign(3, glm::dvec3(0.0));
  fem::FieldInputs in;
  std::string why;
  // No yield: refused, saying so.
  CHECK(fem::NodalField(r, fem::ResultField::FactorOfSafety, in, &why).empty());
  CHECK_THAT(why, ContainsSubstring("no yield"));
  in.yieldMPa = 250.0;
  auto fos = fem::NodalField(r, fem::ResultField::FactorOfSafety, in, &why);
  REQUIRE(fos.size() == 3);
  CHECK_THAT(fos[0], WithinAbs(2.5, 1e-12));
  CHECK(fos[1] >= fem::kUnboundedFactor);
  CHECK_THAT(fos[2], WithinAbs(1.0, 1e-12));
  // The range leaves the unbounded node out.
  const fem::FieldRange range = fem::RangeOf(fos);
  CHECK(range.loNode == 2);
  CHECK(range.hiNode == 0);

  // Across layers needs the direction, the factor the strength.
  CHECK(fem::NodalField(r, fem::ResultField::AcrossLayers, in, &why).empty());
  CHECK_THAT(why, ContainsSubstring("layer_normal"));
  in.layerNormal = {0, 0, 2}; // need not be unit
  auto across = fem::NodalField(r, fem::ResultField::AcrossLayers, in, &why);
  REQUIRE(across.size() == 3);
  CHECK_THAT(across[0], WithinAbs(10.0, 1e-12));
  CHECK_THAT(across[1], WithinAbs(-5.0, 1e-12));
  CHECK(
      fem::NodalField(r, fem::ResultField::InterlayerFactor, in, &why).empty());
  CHECK_THAT(why, ContainsSubstring("interlayer"));
  in.interlayerMPa = 17.0;
  auto inter = fem::NodalField(r, fem::ResultField::InterlayerFactor, in, &why);
  REQUIRE(inter.size() == 3);
  CHECK_THAT(inter[0], WithinAbs(1.7, 1e-12));
  CHECK(inter[1] >= fem::kUnboundedFactor); // pushed together: no factor
  CHECK(inter[2] >= fem::kUnboundedFactor);

  // What the result does not hold is refused, not zero.
  CHECK(fem::NodalField(r, fem::ResultField::PlasticStrain, in, &why).empty());
  CHECK(
      fem::NodalField(r, fem::ResultField::ContactPressure, in, &why).empty());
  CHECK(fem::NodalField(r, fem::ResultField::Temperature, in, &why).empty());
}

namespace {

// A .vtu's DataArrays by name, as text tokens: enough of a parser to read
// what VtuText writes without sharing a line of it.
struct Vtu {
  size_t points = 0, cells = 0;
  struct Array {
    std::string type;
    int components = 1;
    std::vector<double> values;
  };
  std::map<std::string, Array> arrays;
};

std::string Attr(const std::string& tag, const std::string& name) {
  const std::string key = name + "=\"";
  const size_t a = tag.find(key);
  if (a == std::string::npos)
    return "";
  const size_t b = tag.find('"', a + key.size());
  return tag.substr(a + key.size(), b - a - key.size());
}

Vtu ParseVtu(const std::string& text) {
  Vtu out;
  size_t at = 0;
  const size_t piece = text.find("<Piece ");
  REQUIRE(piece != std::string::npos);
  const std::string pieceTag =
      text.substr(piece, text.find('>', piece) - piece);
  out.points = std::stoul(Attr(pieceTag, "NumberOfPoints"));
  out.cells = std::stoul(Attr(pieceTag, "NumberOfCells"));
  while ((at = text.find("<DataArray ", at)) != std::string::npos) {
    const size_t close = text.find('>', at);
    const std::string tag = text.substr(at, close - at);
    const size_t end = text.find("</DataArray>", close);
    REQUIRE(end != std::string::npos);
    Vtu::Array a;
    a.type = Attr(tag, "type");
    const std::string comps = Attr(tag, "NumberOfComponents");
    a.components = comps.empty() ? 1 : std::stoi(comps);
    REQUIRE(Attr(tag, "format") == "ascii");
    std::istringstream in(text.substr(close + 1, end - close - 1));
    double v;
    while (in >> v)
      a.values.push_back(v);
    out.arrays[Attr(tag, "Name")] = std::move(a);
    at = end;
  }
  return out;
}

} // namespace

TEST_CASE("A result cut to its surface is refused for export, saying why",
          "[fem][vtu]") {
  fem::StudyRuntime rt;
  CHECK_THAT(fem::VtuRefusal(rt), ContainsSubstring("no bodies"));
  rt.bodies.emplace_back();
  rt.bodies[0].mesh.nodes = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
  CHECK_THAT(fem::VtuRefusal(rt), ContainsSubstring("boundary surface only"));
  CHECK_THAT(fem::VtuRefusal(rt), ContainsSubstring("recompute"));
}

#ifdef HAVE_NETGEN

namespace {
std::vector<int> NodesAt(const fem::FemMesh& m, int axis, double value) {
  std::vector<int> out;
  for (int i = 0; i < (int)m.nodes.size(); ++i)
    if (std::abs(m.nodes[i][axis] - value) < 1e-6)
      out.push_back(i);
  return out;
}
std::vector<int> TrisAt(const fem::FemMesh& m, int axis, double value) {
  std::vector<int> out;
  for (int t = 0; t < (int)m.boundary.size(); ++t) {
    bool on = true;
    for (int k = 0; k < 6; ++k)
      on = on && std::abs(m.nodes[m.boundary[t].nodes[k]][axis] - value) < 1e-6;
    if (on)
      out.push_back(t);
  }
  return out;
}
int NodeNearest(const fem::FemMesh& m, const glm::dvec3& p) {
  int best = -1;
  double bestD = 1e300;
  for (int i = 0; i < (int)m.nodes.size(); ++i)
    if (const double d = glm::length(m.nodes[i] - p); d < bestD) {
      bestD = d;
      best = i;
    }
  return best;
}
} // namespace

TEST_CASE("A pulled bar's principal stresses are F/A and nothing",
          "[fem][fields][solver]") {
  const double L = 100, W = 10, F = 10000.0;
  CADShape bar = CADKernel::Instance().CreateBox(L, W, W);
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(bar.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  fem::FaceLoad load;
  load.type = fem::LoadType::Force;
  load.tris = TrisAt(*mesh, 0, L);
  load.magnitude = F;
  load.direction = {1, 0, 0};
  auto res = fem::Solve(*mesh, fem::Material{200000.0, 0.3},
                        NodesAt(*mesh, 0, 0.0), {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());
  const int mid = NodeNearest(*mesh, {L / 2, W / 2, W / 2});
  fem::FieldInputs in;
  in.yieldMPa = 250.0;
  in.layerNormal = {1, 0, 0};
  std::string why;
  const double sigma = F / (W * W); // 100 MPa
  const auto at = [&](fem::ResultField f) {
    const std::vector<double> v = fem::NodalField(*res, f, in, &why);
    INFO(why);
    REQUIRE(v.size() == mesh->nodes.size());
    return v[mid];
  };
  CHECK_THAT(at(fem::ResultField::PrincipalMax), WithinRel(sigma, 0.03));
  CHECK_THAT(at(fem::ResultField::PrincipalMid), WithinAbs(0.0, 0.03 * sigma));
  CHECK_THAT(at(fem::ResultField::PrincipalMin), WithinAbs(0.0, 0.03 * sigma));
  // Along the pull the layers carry all of it; the yield factor is 2.5.
  CHECK_THAT(at(fem::ResultField::AcrossLayers), WithinRel(sigma, 0.03));
  CHECK_THAT(at(fem::ResultField::FactorOfSafety), WithinRel(2.5, 0.05));
  CHECK_THAT(at(fem::ResultField::Displacement),
             WithinRel(F * (L / 2) / (200000.0 * W * W), 0.05));
  // Layers at 45 degrees to the pull carry n.sigma.n = sigma cos^2 45 =
  // sigma / 2, and layers along it (stacked across the bar) none.
  in.layerNormal = {1, 0, 1};
  CHECK_THAT(at(fem::ResultField::AcrossLayers), WithinRel(sigma / 2, 0.03));
  in.layerNormal = {0, 0, 1};
  CHECK_THAT(at(fem::ResultField::AcrossLayers), WithinAbs(0.0, 0.03 * sigma));
  // A solve given no yield keeps no factor of its own; the field then
  // reads the body's yield over von Mises, as above.
  CHECK(res->factorOfSafety == 0.0);
  CHECK(res->nodeFactorOfSafety.empty());
}

TEST_CASE("A pressurised cylinder's principal stresses are Lame's, and its "
          "export keeps the curve",
          "[fem][fields][vtu][solver][slow]") {
  // The tube of the Lame von Mises case: a = 10, b = 20, p = 10 MPa, both
  // ends clamped, read at the bore's mid-plane (plane strain there).
  const double a = 10, b = 20, H = 60, p = 10, nu = 0.3;
  CADShape outer = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), b, H);
  CADShape bore = CADKernel::Instance().CreateCylinder(
      glm::vec3(0, 0, -1), glm::vec3(0, 0, 1), a, H + 2);
  CADShape tube = outer.Difference(bore);
  REQUIRE(tube.IsValid());
  fem::MeshSettings ms;
  ms.maxSize = 4.0;
  std::string err;
  auto mesh = fem::Mesh(tube.GetShape(), ms, &err);
  INFO(err);
  REQUIRE(mesh.has_value());
  const auto radius = [&](int n) {
    return std::hypot(mesh->nodes[n].x, mesh->nodes[n].y);
  };
  std::vector<int> fixed;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i)
    if (std::abs(mesh->nodes[i].z) < 1e-6 ||
        std::abs(mesh->nodes[i].z - H) < 1e-6)
      fixed.push_back(i);
  fem::FaceLoad load;
  load.type = fem::LoadType::Pressure;
  load.magnitude = p;
  for (int t = 0; t < (int)mesh->boundary.size(); ++t) {
    bool onBore = true;
    for (int k = 0; k < 6; ++k)
      onBore = onBore && std::abs(radius(mesh->boundary[t].nodes[k]) - a) < 0.6;
    if (onBore)
      load.tris.push_back(t);
  }
  REQUIRE_FALSE(load.tris.empty());
  auto res =
      fem::Solve(*mesh, fem::Material{200000.0, nu}, fixed, {load}, &err);
  INFO(err);
  REQUIRE(res.has_value());

  // Lame at r = a: hoop tension, the pressure in compression, and between
  // them the axial stress plane strain leaves.
  const double hoop = p * (b * b + a * a) / (b * b - a * a); // 16.67
  const double radial = -p;
  const double axial = nu * (hoop + radial); // 2.0
  int inner = -1;
  double best = 1e30;
  for (int i = 0; i < (int)mesh->nodes.size(); ++i)
    if (const double d =
            std::abs(radius(i) - a) + std::abs(mesh->nodes[i].z - H / 2);
        d < best) {
      best = d;
      inner = i;
    }
  fem::FieldInputs in;
  std::string why;
  const auto at = [&](fem::ResultField f) {
    return fem::NodalField(*res, f, in, &why).at(inner);
  };
  INFO("principals " << at(fem::ResultField::PrincipalMax) << ", "
                     << at(fem::ResultField::PrincipalMid) << ", "
                     << at(fem::ResultField::PrincipalMin));
  CHECK_THAT(at(fem::ResultField::PrincipalMax), WithinRel(hoop, 0.08));
  CHECK_THAT(at(fem::ResultField::PrincipalMin), WithinRel(radial, 0.15));
  CHECK_THAT(at(fem::ResultField::PrincipalMid), WithinAbs(axial, 0.15 * hoop));

  // --- The export, read back.
  fem::StudyRuntime rt;
  rt.bodies.emplace_back();
  rt.bodies[0].bodyName = "tube";
  rt.bodies[0].mesh = *mesh;
  rt.bodies[0].results = *res;
  REQUIRE(fem::VtuRefusal(rt).empty());
  fem::VtuSummary summary;
  const std::string text = fem::VtuText(rt, -1, &summary);
  CHECK(summary.points == mesh->nodes.size());
  CHECK(summary.cells == mesh->tets.size());
  CHECK(summary.bytes == text.size());
  const Vtu vtu = ParseVtu(text);
  CHECK(vtu.points == mesh->nodes.size());
  CHECK(vtu.cells == mesh->tets.size());
  REQUIRE(vtu.arrays.count("Points"));
  REQUIRE(vtu.arrays.count("connectivity"));
  REQUIRE(vtu.arrays.count("offsets"));
  REQUIRE(vtu.arrays.count("types"));
  REQUIRE(vtu.arrays.count("displacement"));
  REQUIRE(vtu.arrays.count("von_mises"));
  REQUIRE(vtu.arrays.count("stress"));
  CHECK_FALSE(vtu.arrays.count("temperature")); // not held, not written
  const auto& pts = vtu.arrays.at("Points").values;
  const auto& conn = vtu.arrays.at("connectivity").values;
  REQUIRE(pts.size() == 3 * mesh->nodes.size());
  REQUIRE(conn.size() == 10 * mesh->tets.size());
  CHECK(vtu.arrays.at("stress").components == 6);
  CHECK(vtu.arrays.at("stress").values.size() == 6 * mesh->nodes.size());
  for (double t : vtu.arrays.at("types").values)
    REQUIRE(t == 24.0); // VTK_QUADRATIC_TETRA
  CHECK(vtu.arrays.at("offsets").values.back() == 10.0 * mesh->tets.size());
  // The fields at the node read above are the ones written.
  CHECK_THAT(vtu.arrays.at("von_mises").values[inner],
             WithinRel(res->vonMises[inner], 1e-6));
  for (int k = 0; k < 6; ++k)
    CHECK_THAT(vtu.arrays.at("stress").values[6 * inner + k],
               WithinAbs(res->stress[inner][k], 1e-4));
  CHECK_THAT(vtu.arrays.at("displacement").values[3 * inner + 0],
             WithinAbs(res->displacement[inner].x, 1e-9));

  // Node order, as VTK defines a quadratic tetra: corners 0-3 right-handed,
  // then midside nodes on edges (0,1)(1,2)(2,0)(0,3)(1,3)(2,3). Every
  // element is checked for its orientation and for each midside node lying
  // by the middle of ITS edge; and on the bore, where an edge is curved,
  // that the node is off the chord toward the surface it was projected to
  // -- which only the right node is.
  const auto P = [&](double index) {
    const size_t i = static_cast<size_t>(index);
    return glm::dvec3(pts[3 * i], pts[3 * i + 1], pts[3 * i + 2]);
  };
  static const int kEdge[6][2] = {{0, 1}, {1, 2}, {2, 0},
                                  {0, 3}, {1, 3}, {2, 3}};
  size_t curvedEdges = 0;
  for (size_t c = 0; c < vtu.cells; ++c) {
    const double* n = &conn[10 * c];
    const glm::dvec3 p0 = P(n[0]);
    const double vol =
        glm::dot(glm::cross(P(n[1]) - p0, P(n[2]) - p0), P(n[3]) - p0);
    REQUIRE(vol > 0.0);
    for (int e = 0; e < 6; ++e) {
      const glm::dvec3 A = P(n[kEdge[e][0]]), B = P(n[kEdge[e][1]]);
      const glm::dvec3 M = P(n[4 + e]);
      const double len = glm::length(B - A);
      // A node on another edge is at least half an edge from this one's
      // middle; the sag of the right one is a small fraction of it.
      REQUIRE(glm::length(M - 0.5 * (A + B)) < 0.2 * len);
      const double rA = std::hypot(A.x, A.y), rB = std::hypot(B.x, B.y);
      if (std::abs(rA - a) < 1e-6 && std::abs(rB - a) < 1e-6 &&
          std::abs(A.z - B.z) < 0.5 * len) {
        // A chord of the bore: its middle is inside the circle, the node
        // on it.
        const glm::dvec3 chord = 0.5 * (A + B);
        REQUIRE(std::hypot(chord.x, chord.y) < a - 1e-4);
        REQUIRE(std::abs(std::hypot(M.x, M.y) - a) < 1e-6);
        ++curvedEdges;
      }
    }
  }
  CHECK(curvedEdges > 20);
  // And the mesh's own orientation is the one that needed the exchange:
  // Netgen's is negative, so this export is not the identity.
  CHECK(fem::MeshOrientation(*mesh) == -1);
}

#endif // HAVE_NETGEN
