// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A body as triangles, for the mesh operations a B-Rep body takes part in
// (a Cradle, a Boolean with a mesh operand; docs/modeling/README.md#cradle):
// a mesh body's own, or a B-Rep's tessellated ON A COPY at a deflection
// derived from its own size and welded, so a closed solid arrives Closed().

#include "core/Feature.h"
#include "core/mesh/TriMesh.h"

#include <memory>

namespace meshop {

// The deflection a B-Rep operand is tessellated at when nothing finer is
// asked for: a thousandth of its bounding diagonal, as a mesh measure's.
double DerivedDeflectionMm(const Body& body);

// `body`'s triangles, adjacency built. A mesh body's own (deflection 0); a
// B-Rep's at `deflectionMm` (0 derives it), which `used` reports. Empty
// (no triangles) when the body has neither.
std::shared_ptr<const mesh::TriMesh>
Triangles(const Body& body, double deflectionMm, double& used);

// Why `m` cannot be a solid operand of `what`, naming its boundary loops
// and the way on, or "" when it is Closed().
std::string OpenOperandRefusal(const mesh::TriMesh& m, const std::string& body,
                               const std::string& what);

} // namespace meshop
