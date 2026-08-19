#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "cad/CADKernel.h"

#include <cstdio>
#include <filesystem>
#include <unistd.h>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

TEST_CASE("CreateBox produces a valid solid with correct volume",
          "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 3.0, 4.0);
  REQUIRE(box.IsValid());
  CHECK_THAT(box.Volume(), WithinRel(24.0, 1e-6));

  glm::vec3 min, max;
  box.GetBoundingBox(min, max);
  CHECK_THAT(max.x - min.x, WithinRel(2.0f, 1e-4f));
  CHECK_THAT(max.y - min.y, WithinRel(3.0f, 1e-4f));
  CHECK_THAT(max.z - min.z, WithinRel(4.0f, 1e-4f));
}

TEST_CASE("CreateBox from two corners", "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(glm::vec3(-1, -1, -1),
                                                 glm::vec3(1, 1, 1));
  REQUIRE(box.IsValid());
  CHECK_THAT(box.Volume(), WithinRel(8.0, 1e-6));
}

TEST_CASE("CreateBox rejects degenerate dimensions", "[kernel][primitives]") {
  CADShape box = CADKernel::Instance().CreateBox(0.0, 1.0, 1.0);
  CHECK_FALSE(box.IsValid());
}

TEST_CASE("CreateSphere volume", "[kernel][primitives]") {
  const double r = 1.5;
  CADShape sphere = CADKernel::Instance().CreateSphere(glm::vec3(0), r);
  REQUIRE(sphere.IsValid());
  const double expected = 4.0 / 3.0 * 3.14159265358979 * r * r * r;
  CHECK_THAT(sphere.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("CreateCylinder volume", "[kernel][primitives]") {
  const double r = 1.0, h = 2.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  REQUIRE(cyl.IsValid());
  const double expected = 3.14159265358979 * r * r * h;
  CHECK_THAT(cyl.Volume(), WithinRel(expected, 1e-4));
}

TEST_CASE("FaceCircleCenters + EdgeSnapPoint find a cylinder's circle",
          "[kernel]") {
  // A cylinder about +Z at the origin: its flat cap faces each have one
  // circular edge, centered on the axis (A2.5 connector inference).
  const double r = 1.0, h = 2.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  REQUIRE(cyl.IsValid());

  // Find a planar cap face (the flat end), and confirm it reports a circle
  // center on the Z axis.
  bool foundCap = false;
  for (int f = 0; f < 8 && !foundCap; f++) {
    glm::vec3 c, n, u;
    if (!cyl.FacePlane(f, c, n, u))
      continue; // skip the curved side
    std::vector<glm::vec3> centers;
    std::vector<int> edges;
    cyl.FaceCircleCenters(f, centers, edges);
    if (centers.empty())
      continue;
    foundCap = true;
    CHECK_THAT(centers[0].x, WithinAbs(0.0, 1e-6)); // on the axis
    CHECK_THAT(centers[0].y, WithinAbs(0.0, 1e-6));
    // The named-edge snap point matches the circle center.
    glm::vec3 snap;
    REQUIRE(cyl.EdgeSnapPoint(edges[0], snap));
    CHECK_THAT(snap.x, WithinAbs(centers[0].x, 1e-6));
    CHECK_THAT(snap.y, WithinAbs(centers[0].y, 1e-6));
    CHECK_THAT(snap.z, WithinAbs(centers[0].z, 1e-6));
  }
  CHECK(foundCap);
}

TEST_CASE("FaceEdgeIndices exposes box-edge midpoint snaps", "[kernel]") {
  CADShape box = CADKernel::Instance().CreateBox(2.0, 2.0, 2.0);
  REQUIRE(box.IsValid());

  bool foundFace = false;
  for (int face = 0; face < 8 && !foundFace; ++face) {
    glm::vec3 center, normal, uAxis;
    if (!box.FacePlane(face, center, normal, uAxis))
      continue;
    std::vector<int> edges;
    box.FaceEdgeIndices(face, edges);
    if (edges.empty())
      continue;
    foundFace = true;
    CHECK(edges.size() == 4);
    for (const int edge : edges) {
      glm::vec3 midpoint;
      REQUIRE(box.EdgeSnapPoint(edge, midpoint));
      CHECK(std::isfinite(midpoint.x));
      CHECK(std::isfinite(midpoint.y));
      CHECK(std::isfinite(midpoint.z));
    }
  }
  CHECK(foundFace);
}

TEST_CASE("Boolean union of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape u = a.Union(b);
  REQUIRE(u.IsValid());
  // Two 8-unit boxes overlapping in a 1x2x2 = 4 unit region
  CHECK_THAT(u.Volume(), WithinRel(12.0, 1e-6));
}

TEST_CASE("Boolean intersection of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape i = a.Intersection(b);
  REQUIRE(i.IsValid());
  CHECK_THAT(i.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Boolean difference of overlapping boxes", "[kernel][booleans]") {
  CADKernel& k = CADKernel::Instance();
  CADShape a = k.CreateBox(glm::vec3(0, 0, 0), glm::vec3(2, 2, 2));
  CADShape b = k.CreateBox(glm::vec3(1, 0, 0), glm::vec3(3, 2, 2));

  CADShape d = a.Difference(b);
  REQUIRE(d.IsValid());
  CHECK_THAT(d.Volume(), WithinRel(4.0, 1e-6));
}

TEST_CASE("Translate moves the bounding box", "[kernel][transform]") {
  CADShape box =
      CADKernel::Instance().CreateBox(glm::vec3(0), glm::vec3(1, 1, 1));
  box.Translate(glm::vec3(10, 0, 0));

  glm::vec3 min, max;
  box.GetBoundingBox(min, max);
  CHECK_THAT(min.x, WithinRel(10.0f, 1e-3f));
  CHECK_THAT(max.x, WithinRel(11.0f, 1e-3f));
  CHECK_THAT(box.Volume(), WithinRel(1.0, 1e-6));
}

TEST_CASE("GenerateMesh produces triangles for a box", "[kernel][mesh]") {
  CADShape box = CADKernel::Instance().CreateBox(1.0, 1.0, 1.0);
  CADShape::Mesh mesh = box.GenerateMesh();

  REQUIRE_FALSE(mesh.vertices.empty());
  REQUIRE_FALSE(mesh.indices.empty());
  CHECK(mesh.indices.size() % 3 == 0);
  // A tessellated box is exactly 2 triangles per face
  CHECK(mesh.indices.size() / 3 == 12);
  for (uint32_t idx : mesh.indices) {
    REQUIRE(idx < mesh.vertices.size());
  }
}

TEST_CASE("STEP save/load roundtrip preserves volume", "[kernel][io]") {
  namespace fs = std::filesystem;
  CADKernel& k = CADKernel::Instance();

  std::vector<CADShape> out;
  out.push_back(k.CreateBox(2.0, 3.0, 4.0));

  fs::path path = fs::temp_directory_path() / "designer_test_roundtrip.step";
  REQUIRE(k.SaveSTEP(path.string(), out));

  std::vector<CADShape> in;
  REQUIRE(k.LoadSTEP(path.string(), in));
  REQUIRE(in.size() == 1);
  CHECK_THAT(in[0].Volume(), WithinRel(24.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("STL save/load roundtrip yields a SOLID with the box volume",
          "[kernel][io]") {
  namespace fs = std::filesystem;
  CADKernel& k = CADKernel::Instance();

  CADShape box = k.CreateBox(2.0, 3.0, 4.0);
  fs::path path = fs::temp_directory_path() / "designer_test_roundtrip.stl";
  REQUIRE(k.SaveSTL(path.string(), box, 0.1));

  // A watertight mesh must come back as a closed solid (LoadSTL promotes the
  // sewn shell), so volume/mass work on it — a planar box tessellates
  // exactly, so the volume is exact too.
  CADShape in;
  REQUIRE(k.LoadSTL(path.string(), in));
  REQUIRE(in.IsValid());
  CHECK_THAT(in.Volume(), WithinRel(24.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("Mass properties of a box match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double a = 2.0, b = 3.0, c = 4.0;
  CADShape box =
      CADKernel::Instance().CreateBox(glm::vec3(0, 0, 0), glm::vec3(a, b, c));
  CADShape::MassProps mp;
  REQUIRE(box.ComputeMassProps(mp));
  CHECK_THAT(mp.volume, WithinRel(a * b * c, 1e-9));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * (a * b + b * c + c * a), 1e-9));
  CHECK_THAT(mp.centerOfMass.x, WithinAbs(a / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.y, WithinAbs(b / 2, 1e-9));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(c / 2, 1e-9));
  // Unit density: I_xx = m/12 (b^2 + c^2) about the CoM, and no products of
  // inertia for an axis-aligned box.
  const double m = a * b * c;
  CHECK_THAT(mp.inertia[0][0], WithinRel(m / 12 * (b * b + c * c), 1e-9));
  CHECK_THAT(mp.inertia[1][1], WithinRel(m / 12 * (a * a + c * c), 1e-9));
  CHECK_THAT(mp.inertia[2][2], WithinRel(m / 12 * (a * a + b * b), 1e-9));
  CHECK_THAT(mp.inertia[0][1], WithinAbs(0.0, 1e-9));
  CHECK_THAT(mp.inertia[0][2], WithinAbs(0.0, 1e-9));
}

TEST_CASE("Mass properties of a cylinder match the analytic values",
          "[kernel][massprops]") {
  using Catch::Matchers::WithinAbs;
  const double r = 1.5, h = 4.0;
  CADShape cyl = CADKernel::Instance().CreateCylinder(glm::vec3(0),
                                                      glm::vec3(0, 0, 1), r, h);
  CADShape::MassProps mp;
  REQUIRE(cyl.ComputeMassProps(mp));
  const double v = M_PI * r * r * h;
  CHECK_THAT(mp.volume, WithinRel(v, 1e-6));
  CHECK_THAT(mp.surfaceArea, WithinRel(2 * M_PI * r * (r + h), 1e-6));
  CHECK_THAT(mp.centerOfMass.z, WithinAbs(h / 2, 1e-6));
  // About the CoM: I_zz = m r^2 / 2, I_xx = I_yy = m (3r^2 + h^2) / 12.
  CHECK_THAT(mp.inertia[2][2], WithinRel(v * r * r / 2, 1e-6));
  CHECK_THAT(mp.inertia[0][0], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
  CHECK_THAT(mp.inertia[1][1], WithinRel(v * (3 * r * r + h * h) / 12, 1e-6));
}

TEST_CASE("Mass properties of an invalid shape fail cleanly",
          "[kernel][massprops]") {
  CADShape::MassProps mp;
  CHECK_FALSE(CADShape().ComputeMassProps(mp));
}

// ---------------------------------------------------------------------------
// Structured STEP import (I1): names, colors, and the flattened instance tree
// read through XCAF. The fixture is authored with XCAF + STEPCAFControl so the
// test needs no binary files: an assembly "Rig" holding a red "Widget" box
// twice (second instance rotated 90° about Z then moved to x=10) and an
// unnamed-color "Pin" cylinder at y=10.
// ---------------------------------------------------------------------------

#include <BRep_Builder.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <TDF_Label.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Compound.hxx>
#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Trsf.hxx>

#include "core/Document.h"
#include "core/Feature.h"

namespace {
std::filesystem::path WriteRigStep() {
  CADKernel& k = CADKernel::Instance();
  CADShape box = k.CreateBox(2.0, 2.0, 2.0);
  CADShape cyl =
      k.CreateCylinder(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 0.5, 3.0);

  Handle(TDocStd_Document) doc;
  XCAFApp_Application::GetApplication()->NewDocument("BinXCAF", doc);
  Handle(XCAFDoc_ShapeTool) st = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) ct = XCAFDoc_DocumentTool::ColorTool(doc->Main());

  gp_Trsf rot;
  rot.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), 1.5707963267949);
  gp_Trsf toX10;
  toX10.SetTranslation(gp_Vec(10, 0, 0));
  gp_Trsf toY10;
  toY10.SetTranslation(gp_Vec(0, 10, 0));

  TopoDS_Compound comp;
  BRep_Builder builder;
  builder.MakeCompound(comp);
  builder.Add(comp, box.GetShape());
  builder.Add(comp, box.GetShape().Moved(TopLoc_Location(toX10 * rot)));
  builder.Add(comp, cyl.GetShape().Moved(TopLoc_Location(toY10)));
  TDF_Label asmLabel = st->AddShape(comp, Standard_True);
  TDataStd_Name::Set(asmLabel, "Rig");

  TDF_Label boxLabel, cylLabel;
  REQUIRE(st->FindShape(box.GetShape(), boxLabel));
  REQUIRE(st->FindShape(cyl.GetShape(), cylLabel));
  TDataStd_Name::Set(boxLabel, "Widget");
  ct->SetColor(boxLabel, Quantity_Color(0.9, 0.1, 0.1, Quantity_TOC_RGB),
               XCAFDoc_ColorSurf);
  TDataStd_Name::Set(cylLabel, "Pin");

  std::filesystem::path path =
      // Per-process: catch_discover_tests runs each TEST_CASE in its own
      // process and ctest runs those in parallel, so a fixed filename is two
      // tests writing and reading one file at once (an intermittent import
      // failure that only appears under -j).
      std::filesystem::temp_directory_path() /
      ("designer_test_rig_" + std::to_string(::getpid()) + ".step");
  STEPCAFControl_Writer writer;
  REQUIRE(writer.Transfer(doc, STEPControl_AsIs));
  REQUIRE(writer.Write(path.string().c_str()) == IFSelect_RetDone);
  return path;
}
} // namespace

TEST_CASE("Structured STEP import reads names, colors, and instances",
          "[kernel][import]") {
  const auto path = WriteRigStep();

  CADKernel::ImportedModel model;
  REQUIRE(CADKernel::Instance().LoadSTEPStructured(path.string(), model));
  REQUIRE(model.products.size() == 2);
  REQUIRE(model.instances.size() == 3);

  const auto widget =
      std::find_if(model.products.begin(), model.products.end(),
                   [](const auto& p) { return p.name == "Widget"; });
  const auto pin = std::find_if(model.products.begin(), model.products.end(),
                                [](const auto& p) { return p.name == "Pin"; });
  REQUIRE(widget != model.products.end());
  REQUIRE(pin != model.products.end());
  CHECK_THAT(widget->shape.Volume(), WithinRel(8.0, 1e-4));
  CHECK(widget->hasColor);
  CHECK_THAT(widget->color.r, WithinAbs(0.9f, 0.05f));
  CHECK_THAT(widget->color.g, WithinAbs(0.1f, 0.05f));

  // Two Widget instances: one at identity, one rotated 90° about Z at
  // x=10 (rotation maps +X to +Y — read the transform's first column).
  const size_t widgetIdx = (size_t)(widget - model.products.begin());
  std::vector<glm::mat4> widgetXf;
  glm::mat4 pinXf(0.0f);
  for (const auto& inst : model.instances) {
    if (inst.product == widgetIdx)
      widgetXf.push_back(inst.transform);
    else
      pinXf = inst.transform;
  }
  REQUIRE(widgetXf.size() == 2);
  if (glm::length(glm::vec3(widgetXf[0][3])) >
      glm::length(glm::vec3(widgetXf[1][3])))
    std::swap(widgetXf[0], widgetXf[1]);
  CHECK_THAT(glm::length(glm::vec3(widgetXf[0][3])), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(widgetXf[1][3].x, WithinAbs(10.0f, 1e-4f));
  CHECK_THAT(widgetXf[1][0].y, WithinAbs(1.0f, 1e-4f)); // +X -> +Y
  CHECK_THAT(pinXf[3].y, WithinAbs(10.0f, 1e-4f));

  std::filesystem::remove(path);
}

TEST_CASE("ImportFeature carries names and colors onto bodies",
          "[feature][import]") {
  const auto path = WriteRigStep();

  Document doc;
  doc.AddFeature(std::make_unique<ImportFeature>("rig", path.string()));
  const auto& bodies = doc.GetBodies();
  REQUIRE(bodies.size() == 3);

  int widgets = 0, pins = 0;
  for (const auto& b : bodies) {
    if (b.name == "Widget") {
      widgets++;
      CHECK(b.hasDisplayColor);
      CHECK_THAT(b.displayColor.r, WithinAbs(0.9f, 0.05f));
    } else if (b.name == "Pin") {
      pins++;
      CHECK_FALSE(b.hasDisplayColor);
    }
  }
  CHECK(widgets == 2);
  CHECK(pins == 1);

  // Instances keep their relative placement, centered as one unit. The box
  // is corner-at-origin, so the 90°-rotation puts instance B's center at
  // (-1,1,1)+x10 = (9,1,1) vs A's (1,1,1): centers 8 apart.
  glm::vec3 minA, maxA, minB, maxB;
  const Body* w[2] = {nullptr, nullptr};
  for (const auto& b : bodies)
    if (b.name == "Widget")
      (w[0] ? w[1] : w[0]) = &b;
  REQUIRE(w[1] != nullptr);
  w[0]->shape.GetBoundingBox(minA, maxA);
  w[1]->shape.GetBoundingBox(minB, maxB);
  const glm::vec3 cA = (minA + maxA) * 0.5f, cB = (minB + maxB) * 0.5f;
  CHECK_THAT(glm::length(cB - cA), WithinAbs(8.0f, 0.01f));

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// Assembly import plan (I2): SaveSTEPStructured → BuildAssemblyImportPlan
// round-trip. Parts come out centered with the offset moved into the
// occurrence, so materialized occurrences land exactly where the file put
// them.
// ---------------------------------------------------------------------------

#include "core/io/ImportAssembly.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

TEST_CASE("Assembly import plan extracts centered parts + placed occurrences",
          "[kernel][import][assembly]") {
  CADKernel& k = CADKernel::Instance();

  // Author with the write half: a red "Widget" box (corner at origin, so
  // centering is non-trivial) twice — identity and rotated-90°-at-x10 —
  // plus a "Pin" cylinder at y=10.
  CADKernel::ImportedModel model;
  model.products.push_back({k.CreateBox(2.0, 2.0, 2.0), "Widget", true,
                            glm::vec3(0.9f, 0.1f, 0.1f)});
  model.products.push_back(
      {k.CreateCylinder(glm::vec3(0), glm::vec3(0, 0, 1), 0.5, 3.0), "Pin",
       false, glm::vec3(0.0f)});
  const glm::mat4 xfB =
      glm::translate(glm::mat4(1.0f), glm::vec3(10, 0, 0)) *
      glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(0, 0, 1));
  model.instances.push_back({0, glm::mat4(1.0f), "W1"});
  model.instances.push_back({0, xfB, "W2"});
  model.instances.push_back(
      {1, glm::translate(glm::mat4(1.0f), glm::vec3(0, 10, 0)), "P1"});

  const auto stepPath =
      std::filesystem::temp_directory_path() / "designer_test_asm.step";
  REQUIRE(k.SaveSTEPStructured(stepPath.string(), model, "Rig"));

  const auto destDir =
      std::filesystem::temp_directory_path() / "designer_test_asm_out";
  std::filesystem::remove_all(destDir);
  const auto plan =
      interchange::BuildAssemblyImportPlan(stepPath.string(), destDir.string());
  REQUIRE(plan.error.empty());
  REQUIRE(plan.parts.size() == 2);
  REQUIRE(plan.occurrences.size() == 3);

  // Parts: named, colored, geometry centered at the part origin.
  const auto widget =
      std::find_if(plan.parts.begin(), plan.parts.end(),
                   [](const auto& p) { return p.name == "Widget"; });
  REQUIRE(widget != plan.parts.end());
  CHECK(widget->hasColor);
  CHECK_THAT(widget->color.r, WithinAbs(0.9f, 0.05f));
  CADShape widgetShape;
  REQUIRE(k.LoadBREP(widget->brepPath, widgetShape));
  glm::vec3 min, max;
  widgetShape.GetBoundingBox(min, max);
  CHECK_THAT(glm::length((min + max) * 0.5f), WithinAbs(0.0f, 1e-4f));
  CHECK_THAT(widgetShape.Volume(), WithinRel(8.0, 1e-4));

  // Occurrences: centered part + placement composes back to the file's
  // world positions. Box corner at origin → center (1,1,1); W2 is that
  // center pushed through xfB.
  const size_t wi = (size_t)(widget - plan.parts.begin());
  std::vector<const interchange::PlannedOccurrence*> ws;
  const interchange::PlannedOccurrence* pin = nullptr;
  for (const auto& o : plan.occurrences)
    (o.part == wi ? (void)ws.push_back(&o) : (void)(pin = &o));
  REQUIRE(ws.size() == 2);
  REQUIRE(pin != nullptr);
  if (glm::length(ws[0]->position) > glm::length(ws[1]->position))
    std::swap(ws[0], ws[1]);
  CHECK_THAT(glm::length(ws[0]->position - glm::vec3(1, 1, 1)),
             WithinAbs(0.0f, 1e-3f));
  const glm::vec3 expectB = glm::vec3(xfB * glm::vec4(1, 1, 1, 1));
  CHECK_THAT(glm::length(ws[1]->position - expectB), WithinAbs(0.0f, 1e-3f));
  // W2's orientation is the 90°-about-Z rotation.
  const glm::vec3 xAxis = ws[1]->orientation * glm::vec3(1, 0, 0);
  CHECK_THAT(xAxis.y, WithinAbs(1.0f, 1e-3f));
  // Pin: cylinder is axis-centered in XY, z spans [0,3] → center (0,0,1.5).
  CHECK_THAT(glm::length(pin->position - glm::vec3(0, 10, 1.5f)),
             WithinAbs(0.0f, 1e-3f));

  std::filesystem::remove(stepPath);
  std::filesystem::remove_all(destDir);
}
