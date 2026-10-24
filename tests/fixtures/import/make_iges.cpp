// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Writes this directory's IGES fixtures with OpenCASCADE's own IGES writer:
// the box of make_fixtures.py (10 x 30 x 20 mm from the origin) as a solid
// (MSBO, entity 186), as trimmed surfaces (entity 144) the way IGES usually
// arrives, and as surfaces under a SolidWorks global section (native system
// ID and preprocessor version). Run from the repository root:
//
//   O=$(dirname $(dirname $(grep -m1 OpenCASCADE_DIR build/CMakeCache.txt \
//        | cut -d= -f2)))/..
//   tools/nix-run "g++ -std=c++17 -I$O/include/opencascade \
//     tests/fixtures/import/make_iges.cpp -L$O/lib -lTKDEIGES -lTKXSBase \
//     -lTKBRep -lTKPrim -lTKTopAlgo -lTKG3d -lTKMath -lTKernel \
//     -Wl,-rpath,$O/lib -o /tmp/make_iges" && /tmp/make_iges
//     tests/fixtures/import
#include <BRepPrimAPI_MakeBox.hxx>
#include <IGESControl_Controller.hxx>
#include <IGESControl_Writer.hxx>
#include <IGESData_GlobalSection.hxx>
#include <IGESData_IGESModel.hxx>
#include <Interface_Static.hxx>
#include <TCollection_HAsciiString.hxx>
#include <TopoDS_Shape.hxx>

#include <string>

namespace {

bool Write(const TopoDS_Shape& box, const std::string& path, int brepMode,
           const char* system) {
  IGESControl_Controller::Init();
  Interface_Static::SetCVal("write.iges.unit", "MM");
  IGESControl_Writer writer("MM", brepMode); // 0 faces, 1 BRep (MSBO)
  writer.AddShape(box);
  writer.ComputeModel();
  IGESData_GlobalSection g = writer.Model()->GlobalSection();
  // Not whoever ran this.
  g.SetAuthorName(new TCollection_HAsciiString("fixture"));
  g.SetCompanyName(new TCollection_HAsciiString(""));
  if (system != nullptr) {
    g.SetSystemId(new TCollection_HAsciiString(system));
    g.SetInterfaceVersion(new TCollection_HAsciiString(system));
  }
  writer.Model()->SetGlobalSection(g);
  return writer.Write(path.c_str());
}

} // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 30.0, 20.0).Shape();
  const bool ok = Write(box, dir + "/box-solid.igs", 1, nullptr) &&
                  Write(box, dir + "/box-surfaces.igs", 0, nullptr) &&
                  Write(box, dir + "/box-solidworks.igs", 0, "SolidWorks 2024");
  return ok ? 0 : 1;
}
