// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/SheetMetal.h"
#include "core/features/BaseFlangeFeature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/EdgeFlangeFeature.h"
#include "core/features/FrameFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/GeometryCache.h"
#include "core/sheet/Unfold.h"

#include <BRepAdaptor_Curve.hxx>
#include <BinTools.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using Catch::Matchers::WithinRel;

namespace {

// A model with enough features that "did it execute?" is unambiguous.
std::unique_ptr<Document> MakePart() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0), 40, 10, 30));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Boss", glm::vec3(5, 10, 5), 8, 6, 8));
  doc->AddFeature(
      std::make_unique<BoxFeature>("Rail", glm::vec3(20, 10, 5), 6, 4, 20));
  return doc;
}

fs::path TempProject(const char* name) {
  const fs::path p = fs::temp_directory_path() / name;
  fs::remove_all(p);
  return p;
}

} // namespace

// Opening a project used to re-execute its whole feature list: 259 s for a
// 148-feature part against 0.4 s to save it
// (docs/versioning/README.md#baked-geometry). The built bodies are stored
// beside the recipe, and a load that still matches executes nothing.
TEST_CASE("Baked geometry loads instead of rebuilding", "[baked]") {
  const fs::path proj = TempProject("baked-basic.dsgn");
  auto doc = MakePart();
  const double volume = doc->GetBodies()[0].shape.Volume();
  const size_t faces = doc->GetBodies()[0].elementMap.names.size();
  REQUIRE(testio::Save(*doc, proj.string()));
  REQUIRE(fs::exists(proj / "geometry" / "index.json"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // the whole point
  REQUIRE(loaded->GetBodies().size() == doc->GetBodies().size());
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));
  // The element map rides along: face and edge names are transient in memory,
  // and a dimension or mate that references one must still resolve.
  CHECK(loaded->GetBodies()[0].elementMap.names.size() == faces);
  CHECK(loaded->GetBodies()[0].elementMap.names ==
        doc->GetBodies()[0].elementMap.names);
  CHECK(loaded->GetBodies()[0].name == doc->GetBodies()[0].name);

  // Regenerating again is still free — the blob seeds the regen cache, so an
  // unrelated document-version bump cannot silently rebuild the model.
  loaded->Regenerate();
  CHECK(loaded->LastRegenStats().executed == 0);

  // The first EDIT rebuilds from the start: only the tip is stored, so there
  // is no intermediate state to resume from. Documented, not accidental.
  dynamic_cast<BoxFeature*>(loaded->GetFeature(0))->width = 44.0;
  loaded->Regenerate();
  CHECK(loaded->LastRegenStats().executed == 3);
  fs::remove_all(proj);
}

// A cosmetic edit changes the chain hash but not the shape. Git would ignore
// the rewrite anyway (identical bytes are the same object), but rsync, backups
// and file watchers should still see only what actually changed — the same
// discipline WriteTextAtomic keeps for the recipe.
TEST_CASE("Renaming a feature does not touch its baked shape", "[baked]") {
  const fs::path proj = TempProject("baked-rename.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));
  const fs::path brep = proj / "geometry" / "0.brep";
  REQUIRE(fs::exists(brep));
  const auto before = fs::last_write_time(brep);

  doc->GetFeature(0)->SetName("Renamed plate");
  doc->Regenerate();
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK(fs::last_write_time(brep) == before); // same shape, same file
  // The index DOES change: it carries the new chain hash, which is what makes
  // the blob still match the recipe on the next open.
  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0);
  fs::remove_all(proj);
}

// The chain hash names the recipe, not the code that ran it. A build that
// changes what a recipe produces (a kernel fix) regenerates a part to new
// bodies under the same hash, and a save that trusted the hash alone left the
// previous build's bodies on disk -- to come back on every open.
TEST_CASE("A regenerated part is baked even when the blob's hash matches",
          "[baked]") {
  const fs::path proj = TempProject("baked-rebuilt.dsgn");
  auto doc = MakePart();
  const double volume = doc->GetBodies()[0].shape.Volume();
  REQUIRE(testio::Save(*doc, proj.string()));
  // What an older build left: the same hash over different bodies.
  {
    auto other = std::make_unique<Document>();
    other->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0), 1, 1, 1));
    const fs::path brep = proj / "geometry" / "0.brep";
    REQUIRE(BinTools::Write(other->GetBodies()[0].shape.GetShape(),
                            brep.string().c_str()));
  }
  // This build regenerated the part (no blob installed) and saves it.
  auto rebuilt = MakePart();
  REQUIRE(testio::Save(*rebuilt, proj.string()));
  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));

  // A part opened from its blob and saved unchanged still writes nothing.
  const fs::path brep = proj / "geometry" / "0.brep";
  const auto before = fs::last_write_time(brep);
  REQUIRE(testio::Save(*loaded, proj.string()));
  CHECK(fs::last_write_time(brep) == before);

  // Opened from one project and saved as another whose blob an older build
  // left under the same hash: the second project gets these bodies.
  const fs::path other = TempProject("baked-rebuilt-copy.dsgn");
  REQUIRE(testio::Save(*rebuilt, other.string()));
  {
    auto tiny = std::make_unique<Document>();
    tiny->AddFeature(
        std::make_unique<BoxFeature>("Plate", glm::vec3(0), 1, 1, 1));
    REQUIRE(BinTools::Write(tiny->GetBodies()[0].shape.GetShape(),
                            (other / "geometry" / "0.brep").string().c_str()));
  }
  REQUIRE(testio::Save(*loaded, other.string()));
  auto copy = testio::Load(other.string());
  REQUIRE(copy != nullptr);
  CHECK_THAT(copy->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));
  fs::remove_all(proj);
  fs::remove_all(other);
}

// A body's threads are stamped by the features that cut them, like its bends:
// a baked open executes nothing, so the blob carries them. Without them every
// screw in a reopened tapped hole read as an exact clash of its thread's depth,
// and a thread mill found no thread to cut.
TEST_CASE("Threads survive a baked open", "[baked][hole]") {
  const fs::path proj = TempProject("baked-threads.dsgn");
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0), 30, 30, 12));
  SketchPlane top(glm::vec3(0, 0, 12), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centre", top);
  sk->AddEntity(sketch::Point{{15.0f, 15.0f}});
  doc->AddFeature(std::move(sk));
  auto hole = std::make_unique<HoleFeature>(
      "Tapped", doc->GetFeature(1)->GetId(), HoleKind::Simple, 5.0, 10.0);
  hole->fitThread = "M6";
  hole->fitKind = "tapped";
  doc->AddFeature(std::move(hole));
  REQUIRE(doc->Regenerate());
  const Body& built = doc->GetBodies().front();
  REQUIRE(built.threadRegions.size() == 1);
  REQUIRE(built.threads.size() == 1);
  REQUIRE(testio::Save(*doc, proj.string()));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0);
  const Body& body = loaded->GetBodies().front();
  REQUIRE(body.threadRegions.size() == 1);
  CHECK(body.threadRegions[0].face.name == built.threadRegions[0].face.name);
  CHECK(body.threadRegions[0].designation ==
        built.threadRegions[0].designation);
  CHECK(body.threadRegions[0].length == built.threadRegions[0].length);
  REQUIRE(body.threads.size() == 1);
  CHECK(body.threads[0].face.name == built.threads[0].face.name);
  CHECK(body.threads[0].featureId == built.threads[0].featureId);
  CHECK(body.threads[0].designation == built.threads[0].designation);
  CHECK(body.threads[0].pitch == built.threads[0].pitch);
  CHECK(body.threads[0].majorDia == built.threads[0].majorDia);
  fs::remove_all(proj);
}

// Warnings are a product of EXECUTION, and a baked open executes nothing —
// without carrying them the model would silently lose its advice on reopen,
// which is the same silence the warnings exist to break.
TEST_CASE("Advice survives a baked open", "[baked]") {
  const fs::path proj = TempProject("baked-warning.dsgn");
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("A", glm::vec3(0), 2, 2, 2));
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(10, 0, 0), 2, 2, 2));
  // Subtracting a body that does not touch the target: valid, and warned.
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Cut", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc.Regenerate();
  const std::string warning = doc.GetFeature(2)->GetWarning();
  REQUIRE(!warning.empty());
  REQUIRE(testio::Save(doc, proj.string()));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // it really was baked
  CHECK(loaded->GetFeature(2)->GetWarning() == warning);
  fs::remove_all(proj);
}

// A DATUM's output is not a body, so the blob does not carry it. Executing
// nothing therefore left every datum at its DEFAULT — a plane at the origin,
// a frame at the identity — on a document that had just been saved correct.
// Nothing downstream could tell: a mate anchored to that frame solved
// cleanly and put the part at the origin. The bake installs bodies and then
// runs the datums (Feature::IsDatum), which is what this pins.
TEST_CASE("Datums are resolved on a baked open", "[baked][frame][datum]") {
  const fs::path proj = TempProject("baked-datums.dsgn");
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0), 40, 10, 30));
  const uint64_t plate = doc.GetFeature(0)->GetId();

  auto dp = std::make_unique<DatumPlaneFeature>("Deck", DatumPlaneKind::Offset);
  dp->offset = 50.0;
  doc.AddFeature(std::move(dp));

  auto da =
      std::make_unique<DatumAxisFeature>("Spine", DatumAxisKind::TwoPoints);
  da->p0 = glm::vec3(1.0f, 2.0f, 3.0f);
  da->p1 = glm::vec3(1.0f, 2.0f, 9.0f);
  doc.AddFeature(std::move(da));

  auto seat = std::make_unique<FrameFeature>("seat", FrameKind::Explicit);
  seat->origin[0] = 7.0;
  seat->origin[2] = 11.0;
  doc.AddFeature(std::move(seat));
  const uint64_t seatId = doc.GetFeature(3)->GetId();

  // The one that needs the BODIES the bake installs, not just arithmetic.
  std::string top;
  for (const auto& [index, name] : doc.GetBodies()[0].elementMap.names)
    if (const auto info = Measure::DescribeFaceAt(doc.GetBodies()[0], index);
        info && info->kind == "plane" && info->normal.y > 0.99f)
      top = name;
  REQUIRE(!top.empty());
  auto face = std::make_unique<FrameFeature>("lid", FrameKind::FromElement);
  face->elements = {ElementRef{plate, top}};
  doc.AddFeature(std::move(face));
  const uint64_t lidId = doc.GetFeature(4)->GetId();
  REQUIRE(doc.Regenerate());
  REQUIRE(testio::Save(doc, proj.string()));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  // The BODY still came from the blob: one box, no B-Rep rebuilt.
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK(loaded->LastRegenStats().cached == 1);   // the Box was not re-executed
  CHECK(loaded->LastRegenStats().executed == 4); // the four datums were

  CHECK_THAT(dynamic_cast<DatumPlaneFeature*>(loaded->GetFeature(1))
                 ->OutputPlane()
                 .GetOrigin()
                 .z,
             WithinRel(50.0f, 1e-5f));
  CHECK_THAT(dynamic_cast<DatumAxisFeature*>(loaded->GetFeature(2))->Origin().x,
             WithinRel(1.0f, 1e-5f));
  auto* fr = dynamic_cast<FrameFeature*>(loaded->FindFeatureById(seatId));
  REQUIRE(fr != nullptr);
  CHECK(fr->GetError().empty());
  CHECK_THAT(fr->Origin().x, WithinRel(7.0f, 1e-5f));
  CHECK_THAT(fr->Origin().z, WithinRel(11.0f, 1e-5f));
  auto* lid = dynamic_cast<FrameFeature*>(loaded->FindFeatureById(lidId));
  REQUIRE(lid != nullptr);
  INFO(lid->GetError());
  CHECK(lid->GetError().empty());
  CHECK_THAT(lid->Origin().y, WithinRel(10.0f, 1e-4f)); // the box's top
  CHECK_THAT(lid->ZAxis().y, WithinRel(1.0f, 1e-5f));
  fs::remove_all(proj);
}

TEST_CASE("Bends survive a baked open", "[baked][sheetmetal]") {
  // A bend record is derived, like the element map — and a baked open
  // executes nothing to derive it again. Without it in the blob, a reopened
  // project exported a DXF with no bend lines (a blank with nothing saying
  // where to fold) and check_sheet_metal checked nothing while reporting ok.
  const fs::path proj = TempProject("baked-bends.dsgn");
  Document doc;
  sheet::SheetMetalRules rules;
  rules.material = "aluminum";
  rules.gauge = "1.5mm";
  rules.thickness = 1.5;
  doc.SetSheetMetal(rules);
  auto sk = std::make_unique<SketchFeature>(
      "Blank",
      SketchPlane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  sk->AddEntity(sketch::Line{{0, 0}, {100, 0}, 1});
  sk->AddEntity(sketch::Line{{100, 0}, {100, 60}, 2});
  sk->AddEntity(sketch::Line{{100, 60}, {0, 60}, 3});
  sk->AddEntity(sketch::Line{{0, 60}, {0, 0}, 4});
  doc.AddFeature(std::move(sk));
  doc.AddFeature(
      std::make_unique<BaseFlangeFeature>("Base", doc.GetFeature(0)->GetId()));
  const uint64_t base = doc.GetFeature(1)->GetId();
  std::string edge;
  {
    const Body& body = doc.GetBodies()[0];
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(body.shape.GetShape(), TopAbs_EDGE, edges);
    for (int i = 1; i <= edges.Extent() && edge.empty(); ++i) {
      BRepAdaptor_Curve c(TopoDS::Edge(edges(i)));
      if (c.GetType() != GeomAbs_Line)
        continue;
      const gp_Pnt a = c.Value(c.FirstParameter());
      const gp_Pnt b = c.Value(c.LastParameter());
      if (std::abs(a.Y()) < 1e-6 && std::abs(b.Y()) < 1e-6 &&
          std::abs(a.Z() - 1.5) < 1e-6 && std::abs(b.Z() - 1.5) < 1e-6)
        if (const std::string* n = body.elementMap.EdgeNameOf(i - 1))
          edge = *n;
    }
  }
  REQUIRE_FALSE(edge.empty());
  auto flange = std::make_unique<EdgeFlangeFeature>(
      "Wall", std::vector<ElementRef>{ElementRef{base, edge}}, 20.0, 90.0);
  flange->position = FlangePosition::BendOutside;
  doc.AddFeature(std::move(flange));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies()[0].bends.size() == 1);
  const BendRegion before = doc.GetBodies()[0].bends[0];
  REQUIRE(testio::Save(doc, proj.string()));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 0); // nothing rebuilt: the blob
  REQUIRE(loaded->GetBodies().size() == 1);
  REQUIRE(loaded->GetBodies()[0].bends.size() == 1);
  const BendRegion& after = loaded->GetBodies()[0].bends[0];
  CHECK(after.featureId == before.featureId);
  CHECK_THAT(after.radius, WithinRel(before.radius, 1e-12));
  CHECK_THAT(after.angle, WithinRel(before.angle, 1e-12));
  CHECK_THAT(after.kFactor, WithinRel(before.kFactor, 1e-12));
  CHECK(after.kSource == before.kSource);
  CHECK_THAT(after.width, WithinRel(before.width, 1e-12));
  CHECK(after.parentFace.name == before.parentFace.name);
  CHECK(glm::length(after.axisOrigin - before.axisOrigin) < 1e-12);
  CHECK(glm::length(after.upDir - before.upDir) < 1e-12);
  CHECK(glm::length(after.parentDir - before.parentDir) < 1e-12);
  // Which is what the flat pattern is computed from: it must still flatten to
  // the same blank, and the DXF must still know which way the wall folds.
  CHECK(sheet::FoldsToward(after, {0, 0, 1}) ==
        sheet::FoldsToward(before, {0, 0, 1}));
  fs::remove_all(proj);
}

TEST_CASE("A recipe edited behind the app's back is rebuilt", "[baked]") {
  const fs::path proj = TempProject("baked-stale.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));

  // Edit a feature file directly — the case that makes a naive cache lie.
  fs::path featureFile;
  for (const auto& e : fs::directory_iterator(proj / "features"))
    if (e.path().filename().string().starts_with("1-"))
      featureFile = e.path();
  REQUIRE(!featureFile.empty());
  nlohmann::json fj;
  {
    std::ifstream in(featureFile);
    fj = nlohmann::json::parse(in);
  }
  fj["width"] = 60.0;
  {
    std::ofstream out(featureFile);
    out << fj.dump(2);
  }

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 3); // hash differs → rebuild
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(60.0 * 10.0 * 30.0, 1e-6)); // the widened plate
  fs::remove_all(proj);
}

// A blob can only ever make an open faster, never wrong: every way it can be
// broken degrades to the rebuild the app did before it existed.
TEST_CASE("A broken blob is ignored, not fatal", "[baked]") {
  const fs::path proj = TempProject("baked-broken.dsgn");
  auto doc = MakePart();
  const double volume = doc->GetBodies()[0].shape.Volume();
  REQUIRE(testio::Save(*doc, proj.string()));

  SECTION("truncated B-Rep") {
    std::ofstream(proj / "geometry" / "0.brep",
                  std::ios::binary | std::ios::trunc)
        << "not a shape";
  }
  SECTION("unknown blob format") {
    const fs::path index = proj / "geometry" / "index.json";
    nlohmann::json j;
    {
      std::ifstream in(index);
      j = nlohmann::json::parse(in);
    }
    j["geometry_format"] = 99;
    std::ofstream(index, std::ios::trunc) << j.dump(2);
  }
  SECTION("hash from another model") {
    const fs::path index = proj / "geometry" / "index.json";
    nlohmann::json j;
    {
      std::ifstream in(index);
      j = nlohmann::json::parse(in);
    }
    j["chain_hash"] = 12345;
    std::ofstream(index, std::ios::trunc) << j.dump(2);
  }
  SECTION("no blob at all") { fs::remove_all(proj / "geometry"); }

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed == 3); // rebuilt, no complaint
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(volume, 1e-9));
  fs::remove_all(proj);
}

TEST_CASE("A failed model is not baked", "[baked]") {
  const fs::path proj = TempProject("baked-broken-model.dsgn");
  auto doc = MakePart();
  // A fillet with no resolvable body: the document regenerates with an error,
  // and a blob would then promise geometry the recipe does not produce.
  auto bad = std::make_unique<ShellFeature>("Bad", 999,
                                            std::vector<ElementRef>{}, 1.0);
  doc->AddFeature(std::move(bad));
  REQUIRE_FALSE(doc->Regenerate());
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK_FALSE(fs::exists(proj / "geometry" / "index.json"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->LastRegenStats().executed > 0);
  fs::remove_all(proj);
}

TEST_CASE("commit_geometry off removes the blob and persists", "[baked]") {
  const fs::path proj = TempProject("baked-optout.dsgn");
  auto doc = MakePart();
  REQUIRE(testio::Save(*doc, proj.string()));
  REQUIRE(fs::exists(proj / "geometry" / "index.json"));

  doc->SetCommitGeometry(false);
  REQUIRE(testio::Save(*doc, proj.string()));
  CHECK_FALSE(fs::exists(proj / "geometry"));

  auto loaded = testio::Load(proj.string());
  REQUIRE(loaded != nullptr);
  CHECK_FALSE(loaded->CommitGeometry()); // the choice survives a round trip
  CHECK(loaded->LastRegenStats().executed == 3);
  fs::remove_all(proj);
}
