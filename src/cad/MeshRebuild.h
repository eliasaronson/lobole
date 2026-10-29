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
  // The curved regions to blame when it failed: a face that would not
  // build, one with an open edge or one BRepCheck faults (or, for a flat
  // one, the curved regions round it). The caller demotes them all
  // (mesh::Demote) and tries again, rather than giving the whole mesh up.
  std::vector<uint32_t> badRegions;
};

Rebuilt RebuildFromRegions(const mesh::TriMesh& mesh,
                           const mesh::Segmentation& segmentation);

struct RecognizeOptions {
  int minSides = 12;
  // Whether a mesh the rebuild cannot take may be sewn as facets. A caller
  // that let a mesh past the triangle limit because it would be recognised
  // says no, and the read then fails with `facetsRefused` set, for it to
  // refuse naming the limit (docs/interchange/README.md#what-a-mesh-costs).
  bool facetsAllowed = true;
};

// Reads the STL at `path` and recognises it: segmented, rebuilt, checked.
// A curved region that will not make a face is demoted and the rebuild
// tried again; a mesh that fails a check is sewn as facets
// (CADKernel::LoadSTL) and `report.fellBack` says why. False only when the
// file cannot be read at all, or on a cancel.
// The rebuild of a mesh already read and segmented (SegmentAll's result,
// which it demotes in place as rounds fail): true with `shape` a solid of
// recognised faces, false with `report.fellBack` naming the check that
// failed. `report` is filled either way.
bool RebuildRecognized(const mesh::TriMesh& m, mesh::Segmentation& seg,
                       int minSides, CADShape& shape, mesh::Recognition& report,
                       const std::function<bool()>& cancelled = {});

bool LoadStlRecognized(const std::string& path, const RecognizeOptions& options,
                       CADShape& shape, mesh::Recognition& report,
                       std::string& error,
                       CADKernel::ImportProgress* progress = nullptr,
                       bool* facetsRefused = nullptr);

} // namespace cad
