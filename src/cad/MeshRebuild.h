// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A B-Rep rebuilt from a segmented mesh: one face per region, on its fitted
// plane, cylinder, cone or sphere
// (docs/interchange/README.md#rebuilding-the-solid).
//
// Faces are built on SHARED edges -- the boundary between two regions is cut
// at the vertices where three or more meet, and each piece is one edge whose
// curve comes from its own points, snapped onto both surfaces: a line, a
// circle or arc, or else an interpolated B-spline. Nothing is sewn, so the
// quilt that is most of a faceted import never runs. The faces go into the
// shell in region order (each region's lowest file triangle), so the same
// file always names the same faces.
//
// Checked before it is used: valid (BRepCheck), closed, and its volume
// within what the fits allow of the mesh's. Failing any, the result says
// which, and the caller sews the facets as before.

#include "cad/CADKernel.h"
#include "core/mesh/Segment.h"
#include "core/mesh/TriMesh.h"

#include <string>

namespace cad {

struct Rebuilt {
  CADShape shape;     // valid when `failed` is empty
  std::string failed; // which check, and why
  // A curved region whose face would not build: demoted and tried again
  // (mesh::Demote) by the caller, rather than the whole mesh given up.
  uint32_t badRegion = mesh::kNone;
};

Rebuilt RebuildFromRegions(const mesh::TriMesh& mesh,
                           const mesh::Segmentation& segmentation);

struct RecognizeOptions {
  int minSides = 12;
};

// Reads the STL at `path` and recognises it: segmented, rebuilt, checked.
// A curved region that will not make a face is demoted and the rebuild
// tried again; a mesh that fails a check is sewn as facets
// (CADKernel::LoadSTL) and `report.fellBack` says why. False only when the
// file cannot be read at all, or on a cancel.
bool LoadStlRecognized(const std::string& path, const RecognizeOptions& options,
                       CADShape& shape, mesh::Recognition& report,
                       std::string& error,
                       CADKernel::ImportProgress* progress = nullptr);

} // namespace cad
