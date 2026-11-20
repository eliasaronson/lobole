// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// FEM tetrahedral meshing (Netgen, gated HAVE_NETGEN). GUI-free core code.
//
// Confines every Netgen type to FemMesher.cpp: this header exposes only plain
// data (glm/std) so the solver, RPCs and tests never see netgen headers. When
// the app is built without Netgen (HAVE_NETGEN undefined) Mesh() returns
// std::nullopt with a "built without FEM support" error, mirroring the
// HAVE_PLANEGCS stub discipline.

#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fem {

// Meshing controls for one body. Sizes are in millimetres (the document unit).
struct MeshSettings {
  // Global maximum element size; 0 = auto (bounding-box diagonal / 15, the
  // adaptive-deflection spirit of CADKernel tessellation).
  double maxSize = 0.0;
  // Per-face overrides keyed by TopExp_Explorer(TopAbs_FACE) 0-based index
  // (the same face order picking, ElementMap and the rest of the code use).
  // Set as the face's Netgen shape property, which grades the size into its
  // edges and the volume behind it; any refinement also takes Netgen's
  // grading from 0.3 to 0.2.
  std::vector<std::pair<int, double>> faceSizes;
};

// A second-order tetrahedral mesh. Node coordinates are millimetres. Element
// and boundary node indices are 0-based into `nodes`.
struct FemMesh {
  // tet10 corner+midside node ordering, canonical for the solver:
  //   0..3   = corner nodes
  //   4 = edge(0,1), 5 = edge(1,2), 6 = edge(2,0),
  //   7 = edge(0,3), 8 = edge(1,3), 9 = edge(2,3)
  std::vector<glm::dvec3> nodes;
  std::vector<std::array<int, 10>> tets;
  // The 0-based domain (netgen: the solid) each tet lies in, parallel to
  // `tets`; empty means one domain. A mesh over several glued solids (an
  // assembly study's bonded group, S2) carries a material per domain.
  std::vector<int> tetDomain;

  // Boundary surface triangles (6-node tri6: 3 corners then 3 edge-midsides),
  // each tagged with the 0-based OCC face index it lies on — the anchor for
  // applying face boundary conditions.
  struct BoundaryTri {
    std::array<int, 6> nodes;
    int faceIndex;
  };
  std::vector<BoundaryTri> boundary;

  struct Stats {
    std::size_t nodeCount = 0;
    std::size_t tetCount = 0;
    // Minimum normalized shape quality across all tets, in [0,1]:
    // 1 = regular tetrahedron, -> 0 = degenerate sliver. Surfaced so a study
    // can warn on a bad mesh before trusting its stresses.
    double worstQuality = 0.0;
    // Midside nodes the fold check moved back to their straight edges
    // (StraightenFoldedTets); 0 on a mesh that needed none.
    std::size_t straightenedNodes = 0;
    // The mesh's volume and the B-Rep's it was made from (mm^3), as Mesh()
    // measured them before anything was straightened; 0 on a mesh not made
    // by Mesh() (a hand-built one, a result read back).
    double volumeMm3 = 0.0;
    double shapeVolumeMm3 = 0.0;
    // How well the B-Rep's own volume is known (mm^3): the error its
    // integration over curved faces reports.
    double shapeVolumeErrorMm3 = 0.0;
  } stats;
};

// Meshes `shape` (a single solid) to second order. Returns std::nullopt and
// sets *error (when non-null) on failure or when built without Netgen.
std::optional<FemMesh> Mesh(const TopoDS_Shape& shape,
                            const MeshSettings& settings, std::string* error);

// True when the build links Netgen; lets callers surface a clear message
// instead of a generic meshing failure.
bool Available();

// The element size Mesh() uses for `shape` when its settings leave maxSize
// at 0: a fifteenth of its bounding-box diagonal.
double AutoMeshSize(const TopoDS_Shape& shape);

// How many entries Netgen's process-wide shape property table holds for
// `shape` or any of its sub-shapes. Mesh() takes out every entry it made
// before it returns, so this reads 0 after a mesh (a test's check; 0 without
// Netgen). docs/simulation/README.md#meshing.
int NetgenPropertyEntries(const TopoDS_Shape& shape);

// Curved tet10 that fold (docs/simulation/README.md#curved-elements-that-fold).
// A tet10 whose midside nodes sit on a curved surface can turn its Jacobian's
// sign somewhere inside while its corners look sound; every integrator takes
// |detJ|, so such an element would be integrated as if it were whole. The
// mesh's orientation is the sign most tets' corner volumes have (Netgen's is
// negative); an element is sound when det J keeps that sign over all of it,
// proven from the Bernstein coefficients of det J (a cubic), subdividing
// where they are inconclusive (Johnen, Remacle and Geuzaine). `at` is a point
// where det J has the wrong sign, or is zero.
struct TetFold {
  int tet = -1;
  glm::dvec3 at{0.0};
};
// +1 or -1; 0 for a mesh with no tets.
int MeshOrientation(const FemMesh& mesh);
// Every folded tet, at most `limit` of them (in tet order).
std::vector<TetFold> FoldedTets(const FemMesh& mesh, std::size_t limit = 64);
// The refusal for the first folded tet, naming where it is; empty when none.
[[nodiscard]] std::string FoldedTetRefusal(const FemMesh& mesh);
// Straightens every folded tet: its midside nodes back to the middles of its
// straight edges (a node shared with a sound neighbour moves too, and its
// neighbours are checked again). Returns how many midside nodes moved; a
// tet still folded after (one whose corners are inverted) is left to
// FoldedTetRefusal.
std::size_t StraightenFoldedTets(FemMesh& mesh);

// What one element measures (docs/simulation/README.md#the-mesh-you-look-at).
// `shape` is the corner tetrahedron's normalized quality (1 = regular, -> 0 a
// sliver: Stats::worstQuality is its minimum). `jacobian` is the scaled
// Jacobian of the curved element, the least det J over the largest on the
// degree-3 lattice the fold certification samples: 1 for a straight-sided
// element, falling as its curved edges crowd a corner, <= 0 folded. `size`
// is the mean length of its six straight edges (mm). `sign` is
// MeshOrientation's.
struct TetMeasure {
  double shape = 0.0;
  double jacobian = 0.0;
  double size = 0.0;
  // The worse of shape and jacobian: the one number an element is coloured
  // and listed by.
  double Quality() const { return shape < jacobian ? shape : jacobian; }
};
TetMeasure MeasureTet(const FemMesh& mesh, std::size_t tet, int sign);

// The mesh's volume (mm^3): each tet10's |det J| integrated exactly (det J
// is a cubic, and the rule used is exact for one).
double MeshVolume(const FemMesh& mesh);
// How far a sound mesh's volume may sit from its B-Rep's (mm^3): the volume
// between the curved boundary triangles and their flat chords -- each
// triangle's area times the sag of its midside nodes, which is what the
// mesh's own deflection leaves room for -- then `shapeError`, how well the
// B-Rep's volume is itself known, and a hundred-thousandth of the volume
// for rounding.
double MeshVolumeSlack(const FemMesh& mesh, double volume,
                       double shapeError = 0.0);
// The refusal for a mesh whose volume is not its shape's (a region the
// mesher left empty, a solid it dropped); empty when it is, and when
// `shapeVolume` is not positive (not a solid).
[[nodiscard]] std::string MeshVolumeRefusal(const FemMesh& mesh,
                                            double shapeVolume,
                                            double shapeError = 0.0);

} // namespace fem
