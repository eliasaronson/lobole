// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/MeshShape.h"

#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Version.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>

namespace cad {

CADShape TriangulationShape(const std::vector<glm::dvec3>& positions,
                            const std::vector<std::array<uint32_t, 3>>& tris) {
  if (positions.empty() || tris.empty())
    return CADShape();
  Handle(Poly_Triangulation) tri =
      new Poly_Triangulation(int(positions.size()), int(tris.size()), false);
  for (size_t v = 0; v < positions.size(); v++)
    tri->SetNode(int(v) + 1,
                 gp_Pnt(positions[v].x, positions[v].y, positions[v].z));
  for (size_t t = 0; t < tris.size(); t++)
    tri->SetTriangle(int(t) + 1,
                     Poly_Triangle(int(tris[t][0]) + 1, int(tris[t][1]) + 1,
                                   int(tris[t][2]) + 1));
  BRep_Builder b;
  TopoDS_Face face;
  b.MakeFace(face, tri);
  // In a shell: a lone triangulated face is dropped by the STEP writer even
  // in AP242 (tests/test_mesh_writers.cpp).
  TopoDS_Shell shell;
  b.MakeShell(shell);
  b.Add(shell, face);
  return CADShape(shell);
}

CADShape TriangulationShape(const mesh::TriMesh& m) {
  return TriangulationShape(m.positions, m.triangles);
}

bool IsTriangulationShape(const CADShape& shape) {
  if (!shape.IsValid())
    return false;
  bool any = false;
  for (TopExp_Explorer ex(shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face& f = TopoDS::Face(ex.Current());
    TopLoc_Location loc;
    if (!BRep_Tool::Surface(f).IsNull() ||
        BRep_Tool::Triangulation(f, loc).IsNull())
      return false;
    any = true;
  }
  return any;
}

bool StepWritesTessellation() { return OCC_VERSION_HEX >= 0x070700; }

} // namespace cad
