// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Booleans of closed triangle meshes, by manifold (deps/manifold): the one
// file that calls it (docs/plans/organic-meshes-and-scans.md). Pure data
// and no OpenCASCADE, so it runs on any thread; manifold is built serial
// (MANIFOLD_PAR off), so one call is one thread.

#include "core/mesh/TriMesh.h"

#include <string>

namespace mesh {

enum class BooleanOp { Union, Subtract, Intersect };

// `a` op `b`. Both must be Closed(): an open or non-manifold operand is
// refused, naming which and why, and `out` is left alone. The result is
// welded and its adjacency built; it is Closed() by manifold's
// construction, or empty when the operands do not overlap (an Intersect)
// or `b` swallows `a` (a Subtract).
bool Boolean(const TriMesh& a, const TriMesh& b, BooleanOp op, TriMesh& out,
             std::string& error);

// "manifold 3.5.4": what a build carries, for `ping`'s features and logs.
const char* BooleanLibrary();

} // namespace mesh
