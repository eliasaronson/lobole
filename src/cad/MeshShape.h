// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A mesh body's triangles as the one OpenCASCADE shape that carries them
// unchanged: a shell of one face holding only a Poly_Triangulation -- no
// surface, no edges (docs/interchange/README.md#mesh-bodies). What the
// writers take (stage 0 measured each: STL, BREP, glTF, OBJ and PLY write it
// whole; STEP in AP242 with tessellation on, from OpenCASCADE 7.7) and what a
// section cap is drawn from. Never a body's shape: a B-Rep operation on it
// would answer about a surface it does not have.

#include "cad/CADKernel.h"
#include "core/mesh/TriMesh.h"

#include <array>
#include <vector>

namespace cad {

CADShape TriangulationShape(const mesh::TriMesh& m);
CADShape TriangulationShape(const std::vector<glm::dvec3>& positions,
                            const std::vector<std::array<uint32_t, 3>>& tris);

// Is `shape` one TriangulationShape made: faces with a triangulation and no
// surface?
bool IsTriangulationShape(const CADShape& shape);

// Can this build write a mesh body to STEP? OpenCASCADE 7.6's writer crashed
// on a triangulation-only face, so there it is refused by name.
bool StepWritesTessellation();

} // namespace cad
