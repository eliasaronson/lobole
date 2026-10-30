// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Booleans of closed triangle meshes, by manifold (deps/manifold), which
// core/mesh/Field.cpp also calls for its level set
// (docs/modeling/README.md#mesh-booleans). Pure data
// and no OpenCASCADE, so it runs on any thread; manifold is built serial
// (MANIFOLD_PAR off), so one call is one thread.

#include "core/mesh/TriMesh.h"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <vector>

namespace mesh {

enum class BooleanOp { Union, Subtract, Intersect };

// `a` op `b`. Both must be Closed(): an open or non-manifold operand is
// refused, naming which and why, and `out` is left alone. The result is
// welded and its adjacency built; it is Closed() by manifold's
// construction, or empty when the operands do not overlap (an Intersect)
// or `b` swallows `a` (a Subtract).
bool Boolean(const TriMesh& a, const TriMesh& b, BooleanOp op, TriMesh& out,
             std::string& error);

// Triangulates planar polygons with holes (outer loops counter-clockwise,
// holes clockwise) by manifold's triangulator. Indices count through the
// loops' points in order. False when it cannot (a self-crossing loop).
bool TriangulatePolygons(const std::vector<std::vector<glm::dvec2>>& loops,
                         std::vector<std::array<uint32_t, 3>>& out);

// "manifold 3.5.4": what a build carries, for `ping`'s features and logs.
const char* BooleanLibrary();

} // namespace mesh
