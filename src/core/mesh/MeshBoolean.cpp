// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/mesh/MeshBoolean.h"

#include <manifold/manifold.h>

#include <algorithm>
#include <string>

#ifndef LOBOLE_MANIFOLD_VERSION
#error "CMakeLists.txt defines LOBOLE_MANIFOLD_VERSION from deps/manifold"
#endif

namespace mesh {

namespace {

const char* ErrorName(manifold::Manifold::Error e) {
  using E = manifold::Manifold::Error;
  switch (e) {
  case E::NoError:
    return "no error";
  case E::NonFiniteVertex:
    return "a vertex that is not finite";
  case E::NotManifold:
    return "not manifold";
  case E::VertexOutOfBounds:
    return "a triangle naming a vertex that is not there";
  case E::ResultTooLarge:
    return "a result too large";
  case E::Cancelled:
    return "cancelled";
  default:
    return "malformed input";
  }
}

manifold::MeshGL64 ToMeshGL(const TriMesh& m) {
  manifold::MeshGL64 gl;
  gl.numProp = 3;
  gl.vertProperties.reserve(m.positions.size() * 3);
  for (const glm::dvec3& p : m.positions) {
    gl.vertProperties.push_back(p.x);
    gl.vertProperties.push_back(p.y);
    gl.vertProperties.push_back(p.z);
  }
  gl.triVerts.reserve(m.triangles.size() * 3);
  for (const auto& t : m.triangles)
    for (uint32_t v : t)
      gl.triVerts.push_back(v);
  return gl;
}

// Why `m` is not an operand, or "".
std::string NotSolid(const TriMesh& m) {
  if (m.triangles.empty())
    return "it has no triangles";
  if (m.neighbour.empty())
    return "its adjacency was never built";
  if (m.boundaryEdges)
    return "it is open: " + std::to_string(m.boundaryEdges) +
           " edges on one triangle";
  if (m.nonManifoldEdges)
    return std::to_string(m.nonManifoldEdges) +
           " edges are shared by three or more triangles";
  if (m.flippedEdges)
    return std::to_string(m.flippedEdges) +
           " edges join triangles facing opposite ways";
  return "";
}

} // namespace

bool Boolean(const TriMesh& a, const TriMesh& b, BooleanOp op, TriMesh& out,
             std::string& error) {
  for (const auto& [m, name] : {std::pair{&a, "first"}, {&b, "second"}}) {
    if (const std::string why = NotSolid(*m); !why.empty()) {
      error = std::string("the ") + name + " mesh is not a solid: " + why;
      return false;
    }
  }
  const manifold::Manifold ma(ToMeshGL(a)), mb(ToMeshGL(b));
  for (const auto& [m, name] : {std::pair{&ma, "first"}, {&mb, "second"}}) {
    if (m->Status() != manifold::Manifold::Error::NoError) {
      error = std::string("the ") + name +
              " mesh was refused by manifold: " + ErrorName(m->Status());
      return false;
    }
  }
  const manifold::OpType kind = op == BooleanOp::Union ? manifold::OpType::Add
                                : op == BooleanOp::Subtract
                                    ? manifold::OpType::Subtract
                                    : manifold::OpType::Intersect;
  const manifold::Manifold r = ma.Boolean(mb, kind);
  if (r.Status() != manifold::Manifold::Error::NoError) {
    error = std::string("the boolean failed: ") + ErrorName(r.Status());
    return false;
  }
  const manifold::MeshGL64 gl = r.GetMeshGL64();
  TriMesh result;
  result.quantum = std::max(a.quantum, b.quantum);
  result.positions.reserve(gl.NumVert());
  for (size_t v = 0; v < gl.NumVert(); v++)
    result.positions.emplace_back(gl.vertProperties[v * gl.numProp],
                                  gl.vertProperties[v * gl.numProp + 1],
                                  gl.vertProperties[v * gl.numProp + 2]);
  result.triangles.reserve(gl.NumTri());
  for (size_t t = 0; t < gl.NumTri(); t++)
    result.triangles.push_back({static_cast<uint32_t>(gl.triVerts[3 * t]),
                                static_cast<uint32_t>(gl.triVerts[3 * t + 1]),
                                static_cast<uint32_t>(gl.triVerts[3 * t + 2])});
  BuildAdjacency(result);
  out = std::move(result);
  return true;
}

const char* BooleanLibrary() { return "manifold " LOBOLE_MANIFOLD_VERSION; }

} // namespace mesh
