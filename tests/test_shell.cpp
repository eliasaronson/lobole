// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementHistory.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SplitFeature.h"
#include "core/io/Serialization.h"

#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>

#include <cmath>
#include <filesystem>
#include <set>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
// Name of the box face whose outward normal is +Z (the "top"): the box spans
// [0,s] on each axis, so the +Z face is the one whose centroid has max z.
// We just enumerate names and pick by resolving — but the test only needs
// *a* face to open, so grab the first.
std::string AnyFace(const Body& b) {
  return b.elementMap.names.begin()->second;
}
} // namespace

TEST_CASE("Shell with an open face hollows a box into a bin", "[shell]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 10.0, 10.0, 10.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);

  // Open one face of a 10^3 cube, wall 1: cavity is 8x8x9, so the solid is
  // 1000 - 576 = 424 (the same for any single face of a cube, by symmetry).
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bin", id, std::vector<ElementRef>{ElementRef{id, face}}, 1.0));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(424.0, 1e-2));
}

TEST_CASE("Shell without an open face errors", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Sealed", id, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(1)->GetError().empty());
}

// OCC reports success and hands back the input solid when the wall is too
// thick for the geometry; the feature used to commit that as a silent no-op.
// (Found by the 2026-08-13 clicks-only dogfood, where a shell of a slab with
// a through hole "succeeded" and changed nothing.)
// U10: inputs that are not there yet are refused at COMMIT time, on both
// surfaces — the GUI disables Accept, the RPC throws. Regeneration itself
// stays permissive so an empty tool can still open.
// A draft that reshapes half the body is valid geometry and almost always a
// mistake: 25 degrees sounds modest, but on a 2-unit wall it travels 0.93 —
// four times the 0.25 wall it is tapering, so it cuts through into the shell
// cavity. Found in a real model (scratch/shell_draft.dsgn), which looked like
// a loft passing through the cut-out.
TEST_CASE("A draft that reshapes the body warns with the numbers",
          "[draft][warning]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 2, 2));
  const uint64_t boxId = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);
  doc.AddFeature(std::make_unique<ShellFeature>(
      "S", boxId, std::vector<ElementRef>{ElementRef{boxId, face}}, 0.25));

  // A side face of the shelled body, tapered hard.
  std::string side;
  for (const auto& [index, name] : doc.GetBodies()[0].elementMap.names) {
    glm::dvec3 c, n, u;
    if (doc.GetBodies()[0].shape.FacePlane(index, c, n, u) &&
        std::abs(n.z) < 0.1f) {
      side = name;
      break;
    }
  }
  REQUIRE(!side.empty());
  auto steep = std::make_unique<DraftFeature>(
      "Steep", boxId, std::vector<ElementRef>{ElementRef{boxId, side}}, 25.0);
  DraftFeature* draft = steep.get();
  doc.AddFeature(std::move(steep));
  REQUIRE(doc.GetFeature(2)->GetError().empty()); // valid, just drastic
  CHECK(draft->GetWarning().find("check the angle") != std::string::npos);
  CHECK(draft->GetWarning().find("25") != std::string::npos);

  // A sane angle on the same face says nothing.
  draft->angleDeg = 3.0;
  doc.Regenerate();
  CHECK(draft->GetWarning().empty());
}

TEST_CASE("WhyIncomplete names the missing input", "[u10][incomplete]") {
  FilletFeature blend("F", {}, 0.5);
  CHECK(blend.WhyIncomplete().find("edge") != std::string::npos);
  FilletFeature ready("F", {ElementRef{1, "1/face0;U0"}}, 0.5);
  CHECK(ready.WhyIncomplete().empty());

  DraftFeature draft("D", 1, {}, 5.0);
  CHECK(draft.WhyIncomplete().find("face") != std::string::npos);

  SplitFeature split("S", 0);
  CHECK(split.WhyIncomplete().find("body") != std::string::npos);
  SplitFeature knife("S", 7);
  knife.knife = SplitKnifeKind::Face;
  CHECK(knife.WhyIncomplete().find("face") != std::string::npos);

  BooleanFeature boo("B", 0, 0, BooleanOp::Union);
  CHECK(!boo.WhyIncomplete().empty());
  PatternFeature pat("P", PatternKind::Linear, 0);
  CHECK(!pat.WhyIncomplete().empty());

  // A healthy feature says nothing, and the base class default is silence.
  BoxFeature box("B", glm::vec3(0), 1, 1, 1);
  CHECK(box.WhyIncomplete().empty());
}

TEST_CASE("Shell that removes no material errors", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 4, 4, 4));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);
  doc.AddFeature(std::make_unique<ShellFeature>(
      "TooThick", id, std::vector<ElementRef>{ElementRef{id, face}}, 2.5));
  CHECK_FALSE(doc.Regenerate());
  // Refused before the offset builder runs: a wall of half the thinnest
  // extent or more leaves no cavity.
  CHECK(doc.GetFeature(1)->GetError().find("no cavity") != std::string::npos);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(64.0, 1e-9));
}

TEST_CASE("Shell errors on a missing body and round-trips (v20)", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bad", 999, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc2.GetFeature(0)->GetId();
  const std::string f = AnyFace(doc2.GetBodies()[0]);
  auto sh = std::make_unique<ShellFeature>(
      "H", id, std::vector<ElementRef>{ElementRef{id, f}}, 1.0);
  doc2.AddFeature(std::move(sh));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "shell_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ShellFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->bodyId == id);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}

// The offset builder's answer depends on WHERE its input lives in memory: it
// walks maps hashed by shape address, so the same solid at other addresses (a
// copy, or the same document built in another process) can come back a valid
// cavity or an invalid solid of five times the volume. A drilled box shelled
// through one bore's floor (tests/fuzz/cases/rebuild-139.jsonl) did the second
// about one build in seven, which made that case fail at random in a fresh
// instance (fuzz_corpus_smoke). Every copy must shell alike.
TEST_CASE("A shell builds the same wherever its input lives in memory",
          "[shell][determinism]") {
  // 57.5 x 40 x 27.5 on the XY plane, two d1.5 blind bores 40 deep from the
  // -X face, the second's flat floor opened.
  TopoDS_Shape box =
      BRepPrimAPI_MakeBox(gp_Pnt(-28.75, -20.0, 0.0), 57.5, 40.0, 27.5).Shape();
  for (const auto& [y, z] : {std::pair{5.0, 16.25}, std::pair{10.0, 21.25}})
    box =
        BRepAlgoAPI_Cut(
            box, BRepPrimAPI_MakeCylinder(
                     gp_Ax2(gp_Pnt(-28.75, y, z), gp_Dir(1, 0, 0)), 0.75, 40.0)
                     .Shape())
            .Shape();
  Body base;
  base.name = "Base";
  base.createdBy = 1;
  base.shape = CADShape(box);
  topo::NamePrimitiveElements(base.shape, "1", base.elementMap);
  // The floor: the planar face 40 in, about the second bore's axis.
  std::string floor;
  int k = 0;
  for (TopExp_Explorer ex(box, TopAbs_FACE); ex.More(); ex.Next(), k++) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(ex.Current(), p);
    if (p.CentreOfMass().Distance(gp_Pnt(11.25, 10.0, 21.25)) < 1e-6)
      floor = base.elementMap.names.at(k);
  }
  REQUIRE(!floor.empty());

  std::vector<std::unique_ptr<Feature>> none;
  std::vector<double> volumes;
  std::vector<std::set<std::string>> names;
  int failed = 0;
  std::string why;
  for (int i = 0; i < 60; i++) {
    // The same solid at new addresses; the explorer order, and so every
    // index-keyed name, is the copy's too.
    std::vector<Body> bodies{base};
    bodies[0].shape = CADShape(BRepBuilderAPI_Copy(box, false).Shape());
    ShellFeature shell("Sh", 1, {ElementRef{1, floor}}, 1.95);
    RegenContext ctx{bodies, none, 0};
    if (!shell.Execute(ctx)) {
      failed++;
      why = shell.GetError();
      continue;
    }
    volumes.push_back(bodies[0].shape.Volume());
    // Named through the history, however many draws it took: a retry on a
    // copy names the result through the copy as well.
    std::set<std::string> n;
    for (const auto& [idx, name] : bodies[0].elementMap.names)
      n.insert(name);
    names.push_back(std::move(n));
  }
  INFO(failed << " of 60 copies failed: " << why);
  CHECK(failed == 0);
  REQUIRE(!volumes.empty());
  for (double v : volumes)
    CHECK_THAT(v, WithinRel(volumes.front(), 1e-9));
  for (const auto& n : names)
    CHECK(n == names.front());
  CHECK(names.front().count(floor) == 0); // the opened floor is gone
}

// The offset builder writes to its input: apt's OpenCASCADE 7.6 raised every
// edge of a revolve to the builder's own 1e-3 tolerance, on a shell it then
// refused. The input is the previous feature's cached body, so the part no
// longer matched a rebuild from nothing (fuzz_seeds_smoke, seed 27, on CI's
// Ubuntu 24.04 job). Whatever the shell answers, the body it was handed is
// left as it was.
TEST_CASE("A shell leaves the body it was handed as it found it", "[shell]") {
  const auto tolerances = [](const TopoDS_Shape& s) {
    std::vector<double> t;
    for (TopExp_Explorer ex(s, TopAbs_EDGE); ex.More(); ex.Next())
      t.push_back(BRep_Tool::Tolerance(TopoDS::Edge(ex.Current())));
    for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next())
      t.push_back(BRep_Tool::Tolerance(TopoDS::Vertex(ex.Current())));
    return t;
  };
  // A 2.5..25 x 15 section revolved 270 degrees about Z, opened at the
  // bottom: seed 27's base and its shell.
  BRepBuilderAPI_MakePolygon outline(gp_Pnt(2.5, 0, 0), gp_Pnt(25, 0, 0),
                                     gp_Pnt(25, 0, 15), gp_Pnt(2.5, 0, 15),
                                     /*Close=*/true);
  const TopoDS_Face section = BRepBuilderAPI_MakeFace(outline.Wire()).Face();
  const TopoDS_Shape revolve =
      BRepPrimAPI_MakeRevol(section, gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)),
                            270.0 * M_PI / 180.0)
          .Shape();
  Body base;
  base.name = "Base";
  base.createdBy = 1;
  base.shape = CADShape(revolve);
  topo::NamePrimitiveElements(base.shape, "1", base.elementMap);
  std::string bottom;
  int k = 0;
  for (TopExp_Explorer ex(revolve, TopAbs_FACE); ex.More(); ex.Next(), k++) {
    GProp_GProps p;
    BRepGProp::SurfaceProperties(ex.Current(), p);
    if (std::abs(p.CentreOfMass().Z()) < 1e-9)
      bottom = base.elementMap.names.at(k);
  }
  REQUIRE(!bottom.empty());
  const std::vector<double> before = tolerances(revolve);

  std::vector<std::unique_ptr<Feature>> none;
  for (const double wall : {1.0, 2.12, 4.0, 6.0, 7.4}) {
    INFO("wall " << wall);
    std::vector<Body> bodies{base};
    ShellFeature shell("Sh", 1, {ElementRef{1, bottom}}, wall);
    RegenContext ctx{bodies, none, 0};
    shell.Execute(ctx);
    CHECK(tolerances(revolve) == before);
  }
}
