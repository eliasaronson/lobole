// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// One boolean of two organic fixture meshes, as a program of its own: the
// mingw cross build has no test suite, so this is what proves manifold
// builds, links and is right there (packaging/windows/manifold-probe.nix,
// run under Wine). tests/test_mesh_organic.cpp checks the same identities on
// Linux. Exits 0 when they hold, 1 when not.

#include "core/mesh/MeshBoolean.h"
#include "mesh_organic.h"

#include <cmath>
#include <cstdio>
#include <string>

int main() {
  const auto sphere = fixtures::NoisySphere(10000);
  const mesh::TriMesh torus =
      fixtures::Translated(fixtures::Torus(10000).mesh, {0.0, 0.0, 36.0});
  mesh::TriMesh cut, common, both;
  std::string error;
  if (!mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Subtract, cut,
                     error) ||
      !mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Intersect, common,
                     error) ||
      !mesh::Boolean(sphere.mesh, torus, mesh::BooleanOp::Union, both, error)) {
    std::printf("mesh_boolean_probe: FAIL: %s\n", error.c_str());
    return 1;
  }
  const double vs = sphere.mesh.Volume(), vt = torus.Volume();
  const bool closed = cut.Closed() && common.Closed() && both.Closed();
  const bool real = common.Volume() > 0.05 * vt && common.Volume() < 0.95 * vt;
  const bool adds =
      std::abs(cut.Volume() + common.Volume() - vs) < 1e-9 * vs &&
      std::abs(both.Volume() + common.Volume() - vs - vt) < 1e-9 * vs;
  std::printf("mesh_boolean_probe: %s; sphere %zu + torus %zu triangles; "
              "cut %zu, common %zu, union %zu; volumes %.3f - %.3f = %.3f "
              "mm3; closed %s, a real cut %s, the volumes add up %s\n",
              mesh::BooleanLibrary(), sphere.mesh.TriangleCount(),
              torus.TriangleCount(), cut.TriangleCount(),
              common.TriangleCount(), both.TriangleCount(), vs, common.Volume(),
              cut.Volume(), closed ? "yes" : "NO", real ? "yes" : "NO",
              adds ? "yes" : "NO");
  return closed && real && adds ? 0 : 1;
}
