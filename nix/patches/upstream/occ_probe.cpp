// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The upstream reports' reproducer (nix/patches/upstream/*.md): plain
// OpenCASCADE 7.9, no Loböle code. Reads a .brep and makes one kernel call.
// Build against an OCC install, e.g.
//   g++ -std=c++17 -I$OCC/include/opencascade occ_probe.cpp -o occ_probe \
//     -L$OCC/lib -lTKernel -lTKMath -lTKBRep -lTKG2d -lTKG3d -lTKGeomBase \
//     -lTKGeomAlgo -lTKTopAlgo -lTKPrim -lTKBO -lTKBool -lTKFillet \
//     -lTKOffset -lTKShHealing -lTKDESTEP -lTKXSBase
//   occ_probe fillet  BODY.brep R EDGE...      (edge = TopExp::MapShapes index,
//   0-based) occ_probe chamfer BODY.brep D EDGE... occ_probe draft   BODY.brep
//   DEG NX NY NZ PX PY PZ FACE... (face = explorer index) occ_probe offset
//   BODY.brep DIST FACE...   (BRepOffset_MakeOffset skin, one face) occ_probe
//   shell   BODY.brep THICK FACE...
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Check.hxx>
#include <BRepAlgoAPI_Splitter.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepOffsetAPI_DraftAngle.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepOffset_MakeOffset.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>

#include <BRep_Tool.hxx>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double Vol(const TopoDS_Shape& s) {
  GProp_GProps g;
  if (getenv("EPS"))
    BRepGProp::VolumeProperties(s, g, atof(getenv("EPS")));
  else
    BRepGProp::VolumeProperties(s, g);
  return g.Mass();
}

static void Report(const char* what, bool done, const TopoDS_Shape& in,
                   const TopoDS_Shape& out) {
  std::printf("%s: done=%d", what, done ? 1 : 0);
  if (done && !out.IsNull()) {
    std::printf(" volume %.9g -> %.9g valid=%d", Vol(in), Vol(out),
                BRepCheck_Analyzer(out).IsValid() ? 1 : 0);
  }
  std::printf("\n");
}

int main(int argc, char** argv) {
  if (argc < 4)
    return 2;
  const char* op = argv[1];
  TopoDS_Shape s;
  BRep_Builder b;
  const std::string path = argv[2];
  if (path.size() > 5 && path.substr(path.size() - 5) == ".step") {
    STEPControl_Reader r;
    r.ReadFile(argv[2]);
    r.TransferRoots();
    s = r.OneShape();
  } else if (!BRepTools::Read(s, argv[2], b)) {
    std::printf("cannot read %s\n", argv[2]);
    return 2;
  }
  {
    double mt = 0;
    for (TopExp_Explorer e(s, TopAbs_EDGE); e.More(); e.Next())
      mt = std::max(mt, BRep_Tool::Tolerance(TopoDS::Edge(e.Current())));
    std::printf("input max edge tol %.3g\n", mt);
  }
  std::printf("read: volume %.9g valid=%d\n", Vol(s),
              BRepCheck_Analyzer(s).IsValid() ? 1 : 0);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);
  std::vector<TopoDS_Face> faces;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
    faces.push_back(TopoDS::Face(ex.Current()));
  if (!std::strcmp(op, "faces")) {
    int i = 0;
    for (const TopoDS_Face& f : faces) {
      BRepAdaptor_Surface a(f);
      GProp_GProps g;
      BRepGProp::SurfaceProperties(f, g);
      gp_Pnt c = g.CentreOfMass();
      std::printf("face %d type %d area %.4g centroid (%.3f %.3f %.3f)\n", i++,
                  (int)a.GetType(), g.Mass(), c.X(), c.Y(), c.Z());
    }
    return 0;
  }
  if (!std::strcmp(op, "save")) {
    BRepTools::Write(s, argv[3]);
    return 0;
  }
  try {
    if (!std::strcmp(op, "fillet") || !std::strcmp(op, "chamfer")) {
      const double r = std::atof(argv[3]);
      if (!std::strcmp(op, "fillet")) {
        BRepFilletAPI_MakeFillet mk(s);
        for (int i = 4; i < argc; i++) {
          std::printf("Add edge %s...\n", argv[i]);
          std::fflush(stdout);
          mk.Add(r, TopoDS::Edge(edges(std::atoi(argv[i]) + 1)));
        }
        std::printf("Build...\n");
        std::fflush(stdout);
        mk.Build();
        Report("fillet", mk.IsDone(), s,
               mk.IsDone() ? mk.Shape() : TopoDS_Shape());
      } else {
        BRepFilletAPI_MakeChamfer mk(s);
        for (int i = 4; i < argc; i++) {
          std::printf("Add edge %s...\n", argv[i]);
          std::fflush(stdout);
          mk.Add(r, TopoDS::Edge(edges(std::atoi(argv[i]) + 1)));
        }
        std::printf("Build...\n");
        std::fflush(stdout);
        mk.Build();
        Report("chamfer", mk.IsDone(), s,
               mk.IsDone() ? mk.Shape() : TopoDS_Shape());
      }
    } else if (!std::strcmp(op, "draft")) {
      const double deg = std::atof(argv[3]);
      const gp_Dir pull(std::atof(argv[4]), std::atof(argv[5]),
                        std::atof(argv[6]));
      const gp_Pln neutral(
          gp_Pnt(std::atof(argv[7]), std::atof(argv[8]), std::atof(argv[9])),
          pull);
      BRepOffsetAPI_DraftAngle d(s);
      for (int i = 10; i < argc; i++) {
        d.Add(faces[std::atoi(argv[i])], pull, deg * M_PI / 180.0, neutral);
        std::printf("Add face %s: AddDone=%d\n", argv[i], d.AddDone() ? 1 : 0);
      }
      std::printf("Build...\n");
      std::fflush(stdout);
      d.Build();
      Report("draft", d.IsDone(), s, d.IsDone() ? d.Shape() : TopoDS_Shape());
    } else if (!std::strcmp(op, "offset")) {
      const double dist = std::atof(argv[3]);
      BRepOffset_MakeOffset mk;
      mk.Initialize(s, 0.0, Precision::Confusion(), BRepOffset_Skin,
                    Standard_True, Standard_False, GeomAbs_Intersection,
                    Standard_False, Standard_False);
      for (int i = 4; i < argc; i++)
        mk.SetOffsetOnFace(faces[std::atoi(argv[i])], dist);
      std::printf("MakeOffsetShape...\n");
      std::fflush(stdout);
      mk.MakeOffsetShape();
      Report("offset", mk.IsDone(), s,
             mk.IsDone() ? mk.Shape() : TopoDS_Shape());
    } else if (!std::strcmp(op, "split")) {
      // split BODY NX NY NZ OX OY OZ [nd] [twice]
      const gp_Dir n(std::atof(argv[3]), std::atof(argv[4]),
                     std::atof(argv[5]));
      gp_Pnt o(std::atof(argv[6]), std::atof(argv[7]), std::atof(argv[8]));
      bool nd = false;
      int runs = 1;
      for (int i = 9; i < argc; i++) {
        if (!std::strcmp(argv[i], "nd"))
          nd = true;
        if (!std::strcmp(argv[i], "twice"))
          runs = 2;
      }
      Bnd_Box bb;
      BRepBndLib::Add(s, bb);
      double x0, y0, z0, x1, y1, z1;
      bb.Get(x0, y0, z0, x1, y1, z1);
      gp_Pnt c((x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2);
      gp_Vec cv(o, c);
      double d = cv.Dot(gp_Vec(n));
      gp_Pnt proj = c.Translated(-d * gp_Vec(n));
      double h = gp_Pnt(x0, y0, z0).Distance(gp_Pnt(x1, y1, z1));
      for (int r = 0; r < runs; r++) {
        BRepBuilderAPI_MakeFace mf(gp_Pln(proj, n), -h, h, -h, h);
        BRepAlgoAPI_Splitter sp;
        TopTools_ListOfShape a, t;
        a.Append(s);
        t.Append(mf.Face());
        sp.SetArguments(a);
        sp.SetTools(t);
        sp.SetNonDestructive(nd);
        sp.Build();
        double tot = 0;
        int k = 0;
        for (TopExp_Explorer ex(sp.Shape(), TopAbs_SOLID); ex.More();
             ex.Next(), k++) {
          double v = Vol(ex.Current());
          tot += v;
          GProp_GProps g2;
          BRepGProp::VolumeProperties(ex.Current(), g2, 1e-7);
          double mt = 0;
          for (TopExp_Explorer e(ex.Current(), TopAbs_EDGE); e.More(); e.Next())
            mt = std::max(mt, BRep_Tool::Tolerance(TopoDS::Edge(e.Current())));
          std::printf(
              "  piece %d volume %.9g (eps %.9g) maxtol %.3g valid=%d\n", k, v,
              g2.Mass(), mt,
              BRepCheck_Analyzer(ex.Current()).IsValid() ? 1 : 0);
        }
        std::printf("split run %d: %d pieces, sum %.9g, delta %.6g\n", r, k,
                    tot, tot - Vol(s));
      }
    } else if (!std::strcmp(op, "shell")) {
      const double t = std::atof(argv[3]);
      TopTools_ListOfShape rm;
      for (int i = 4; i < argc; i++)
        rm.Append(faces[std::atoi(argv[i])]);
      BRepOffsetAPI_MakeThickSolid mk;
      mk.MakeThickSolidByJoin(s, rm, -t, 1.0e-3);
      Report("shell", mk.IsDone(), s,
             mk.IsDone() ? mk.Shape() : TopoDS_Shape());
    } else {
      return 2;
    }
  } catch (const Standard_Failure& e) {
    std::printf("threw %s: %s\n", e.DynamicType()->Name(),
                e.GetMessageString() ? e.GetMessageString() : "");
    return 1;
  }
  return 0;
}
