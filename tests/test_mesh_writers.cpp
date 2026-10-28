// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What OpenCASCADE's writers do with a face that carries ONLY a
// Poly_Triangulation -- no surface, no edges: the shape a mesh body would
// hand them (docs/plans/organic-meshes-and-scans.md, stage 0, "the OCC
// writers"). Each check is the outcome read back from the file, not a
// writer's return value.

#include "mesh_organic.h"

#include "cad/CADKernel.h"
#include "cad/MeshExport.h"
#include "cad/XcafDocument.h"

#include <catch2/catch_test_macros.hpp>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Interface_Static.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Version.hxx>
#include <StlAPI_Writer.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

TopoDS_Face TriangulationFace(const mesh::TriMesh& m) {
  Handle(Poly_Triangulation) tri = new Poly_Triangulation(
      int(m.positions.size()), int(m.triangles.size()), false);
  for (size_t v = 0; v < m.positions.size(); v++)
    tri->SetNode(int(v) + 1,
                 gp_Pnt(m.positions[v].x, m.positions[v].y, m.positions[v].z));
  for (size_t t = 0; t < m.triangles.size(); t++)
    tri->SetTriangle(int(t) + 1, Poly_Triangle(int(m.triangles[t][0]) + 1,
                                               int(m.triangles[t][1]) + 1,
                                               int(m.triangles[t][2]) + 1));
  TopoDS_Face face;
  BRep_Builder().MakeFace(face, tri);
  return face;
}

int TrianglesOf(const TopoDS_Shape& s) {
  int n = 0;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
    TopLoc_Location loc;
    const auto t = BRep_Tool::Triangulation(TopoDS::Face(ex.Current()), loc);
    n += t.IsNull() ? 0 : t->NbTriangles();
  }
  return n;
}

std::string Slurp(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::filesystem::path Temp(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("lobole_mesh_writer_" + std::to_string(::getpid()) + "_" + name);
}

// Sets an Interface_Static for the scope and puts the old value back: these
// are process-wide, and every other STEP test in the binary reads them.
struct StaticValue {
  std::string name;
  int old = 0;
  StaticValue(const char* n, int value) : name(n) {
    old = Interface_Static::IVal(n);
    REQUIRE(Interface_Static::SetIVal(n, value));
  }
  ~StaticValue() { Interface_Static::SetIVal(name.c_str(), old); }
};

} // namespace

TEST_CASE("Mesh writers: a triangulation-only face, through every writer",
          "[mesh][export]") {
  const auto figure = fixtures::Figurine(10000);
  const int n = int(figure.mesh.TriangleCount());
  const TopoDS_Face face = TriangulationFace(figure.mesh);
  REQUIRE(BRep_Tool::Surface(face).IsNull());
  REQUIRE(TrianglesOf(face) == n);

  SECTION("BRepMesh leaves it alone, at any deflection") {
    for (double d : {1.0, 0.1, 0.001}) {
      BRepMesh_IncrementalMesh mesher(face, d);
      CHECK(TrianglesOf(face) == n);
    }
  }

  SECTION("glTF, OBJ and PLY take it through MeshExport unchanged") {
    MeshExport::Item item;
    item.shape = CADShape(face);
    item.name = "figure";
    // PLY's writer arrived in OpenCASCADE 7.7 (MeshExport refuses it before).
    const std::vector<const char*> exts =
        OCC_VERSION_HEX >= 0x070700
            ? std::vector<const char*>{".glb", ".obj", ".ply"}
            : std::vector<const char*>{".glb", ".obj"};
    for (const char* ext : exts) {
      const auto path = Temp(std::string("visual") + ext);
      std::string error;
      INFO(ext << ": " << error);
      REQUIRE(MeshExport::SaveVisual(path.string(), {item}, 0.1, &error));
      const std::string text = Slurp(path);
      std::filesystem::remove(path);
      if (std::string(ext) == ".obj") {
        size_t faces = 0;
        for (size_t at = 0; (at = text.find("\nf ", at)) != std::string::npos;
             at++)
          faces++;
        CHECK(int(faces) == n);
      } else if (std::string(ext) == ".ply") {
        CHECK(text.find("element face " + std::to_string(n) + "\n") !=
              std::string::npos);
      } else {
        // The index accessor counts corners: three a triangle.
        CHECK(text.find("\"count\":" + std::to_string(3 * n)) !=
              std::string::npos);
      }
    }
  }

  SECTION("STL writes its triangles") {
    const auto path = Temp("plain.stl");
    StlAPI_Writer writer;
    writer.ASCIIMode() = false;
    REQUIRE(writer.Write(face, path.string().c_str()));
    CHECK(std::filesystem::file_size(path) == size_t(84 + 50 * n));
    std::filesystem::remove(path);
  }

  SECTION("BREP keeps the triangulation") {
    const auto path = Temp("plain.brep");
    REQUIRE(BRepTools::Write(face, path.string().c_str()));
    TopoDS_Shape back;
    BRep_Builder b;
    REQUIRE(BRepTools::Read(back, path.string().c_str(), b));
    std::filesystem::remove(path);
    CHECK(TrianglesOf(back) == n);
  }

#if OCC_VERSION_HEX >= 0x070700
  // Tessellated STEP is OpenCASCADE 7.7's. On 7.6.3 (Ubuntu 24.04's) the
  // writer CRASHED on this face, so a mesh body must never reach it there.
  SECTION("STEP: nothing by default, a tessellated face in AP242") {
    const auto write = [&](const std::filesystem::path& path) {
      std::scoped_lock exchange(DataExchangeMutex());
      XcafDocument doc;
      // A face on its own is not written tessellated even then; in a shell
      // it is, which is what a mesh body's shape would be.
      TopoDS_Shell shell;
      BRep_Builder b;
      b.MakeShell(shell);
      b.Add(shell, face);
      XCAFDoc_DocumentTool::ShapeTool(doc->Main())->AddShape(shell, false);
      STEPCAFControl_Writer writer;
      REQUIRE(writer.Transfer(doc, STEPControl_AsIs));
      return writer.Write(path.string().c_str()) == IFSelect_RetDone;
    };
    const auto readBack = [&](const std::filesystem::path& path) {
      std::scoped_lock exchange(DataExchangeMutex());
      StaticValue tess("read.step.tessellated", 1); // On
      STEPControl_Reader reader;
      REQUIRE(reader.ReadFile(path.string().c_str()) == IFSelect_RetDone);
      reader.TransferRoots();
      return TrianglesOf(reader.OneShape());
    };
    const auto plain = Temp("default.step");
    const bool wrotePlain = write(plain);
    const std::string plainText = Slurp(plain);
    // What the app's STEP export does today (it sets no schema): AP214,
    // which has no tessellated entities, so the face is dropped.
    CHECK(plainText.find("TESSELLATED") == std::string::npos);
    CHECK(readBack(plain) == 0);
    std::filesystem::remove(plain);
    (void)wrotePlain;

    const auto ap242 = Temp("ap242.step");
    {
      StaticValue schema("write.step.schema", 5);    // AP242DIS
      StaticValue tess("write.step.tessellated", 1); // On
      REQUIRE(write(ap242));
    }
    const std::string text = Slurp(ap242);
    CHECK(text.find("TRIANGULATED_FACE") != std::string::npos);
    CHECK(readBack(ap242) == n);
    std::filesystem::remove(ap242);
  }
#endif
}
