#include "core/Command.h"
#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/io/Serialization.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using Catch::Approx;

namespace {

// A Ø20 shaft, so the element the tolerances hang off is a real round face.
std::unique_ptr<Document> ShaftDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Shaft", glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 10.0, 40.0));
  return doc;
}

Tolerance G6(uint64_t featureId) {
  Tolerance t;
  t.ref = ElementRef{featureId, "1/face0"};
  t.isoClass = "g6";
  return t;
}

} // namespace

TEST_CASE("A tolerance is stored per element and replaces in place",
          "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();

  const Tolerance& first = doc->SetTolerance(G6(id));
  CHECK(first.id != 0);
  CHECK(doc->GetTolerances().size() == 1);

  // The same face cannot be two sizes at once, so a second set REPLACES the
  // first rather than stacking a contradiction the checker would have to
  // pick between.
  Tolerance tighter = G6(id);
  tighter.isoClass = "h6";
  const uint64_t replacedId = doc->SetTolerance(tighter).id;
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].isoClass == "h6");
  CHECK(replacedId != 0);

  // A different face is a different tolerance.
  Tolerance other = G6(id);
  other.ref.name = "1/cap0";
  doc->SetTolerance(other);
  CHECK(doc->GetTolerances().size() == 2);

  CHECK(doc->ToleranceFor(ElementRef{id, "1/face0"}) != nullptr);
  CHECK(doc->ToleranceFor(ElementRef{id, "nope"}) == nullptr);
  CHECK(doc->ToleranceFor(ElementRef{}) == nullptr);
}

// The claim the whole design rests on: a size tolerance moves no surface, so
// recording one must rebuild nothing. If this ever fails, tolerances have
// leaked into the regen chain and every tolerance edit costs a regeneration.
TEST_CASE("A tolerance edit rebuilds zero features", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  const uint64_t versionBefore = doc->Version();
  const double volumeBefore = doc->GetBodies().at(0).shape.Volume();

  const uint64_t tolId = doc->SetTolerance(G6(id)).id;
  CHECK(doc->Version() == versionBefore);

  doc->RemoveTolerance(tolId);
  CHECK(doc->Version() == versionBefore);
  CHECK(doc->GetBodies().at(0).shape.Volume() == Approx(volumeBefore));
}

TEST_CASE("Tolerance commands undo and redo to the same ids", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  CommandStack stack;

  auto set = std::make_unique<SetToleranceCommand>(G6(id));
  SetToleranceCommand* raw = set.get();
  stack.Execute(std::move(set), *doc);
  const uint64_t assigned = raw->ToleranceId();
  REQUIRE(assigned != 0);
  REQUIRE(doc->GetTolerances().size() == 1);

  stack.Undo(*doc);
  CHECK(doc->GetTolerances().empty());
  stack.Redo(*doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  // Redo must land the SAME id: anything holding one would otherwise be
  // pointing at a tolerance that no longer exists.
  CHECK(doc->GetTolerances()[0].id == assigned);

  // Replacing and undoing restores the tolerance that was there before,
  // not merely "no tolerance".
  Tolerance tighter = G6(id);
  tighter.isoClass = "h6";
  stack.Execute(std::make_unique<SetToleranceCommand>(tighter), *doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].isoClass == "h6");
  stack.Undo(*doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].isoClass == "g6");

  stack.Execute(std::make_unique<ClearToleranceCommand>(assigned), *doc);
  CHECK(doc->GetTolerances().empty());
  stack.Undo(*doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].id == assigned);
  CHECK(doc->GetTolerances()[0].isoClass == "g6");
}

TEST_CASE("Tolerances round-trip through a save", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  doc->SetTolerance(G6(id));
  Tolerance explicitPair;
  explicitPair.ref = ElementRef{id, "1/cap0"};
  explicitPair.lower = -0.02;
  explicitPair.upper = 0.05;
  explicitPair.comment = "off the drawing";
  doc->SetTolerance(explicitPair);

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "designer_tolerance_roundtrip.json";
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  Document& reloaded = **loaded;
  REQUIRE(reloaded.GetTolerances().size() == 2);

  const Tolerance* a = reloaded.ToleranceFor(ElementRef{id, "1/face0"});
  REQUIRE(a != nullptr);
  CHECK(a->isoClass == "g6");
  const Tolerance* b = reloaded.ToleranceFor(ElementRef{id, "1/cap0"});
  REQUIRE(b != nullptr);
  CHECK(b->isoClass.empty());
  CHECK(b->lower == Approx(-0.02));
  CHECK(b->upper == Approx(0.05));
  CHECK(b->comment == "off the drawing");
  // The id counter must clear the loaded tolerances, or the next feature
  // added collides with one.
  CHECK(reloaded.GetTolerances()[0].id != 0);
  std::filesystem::remove(path);
}

// A document without tolerances must serialize exactly as it did before v97,
// which is what keeps every existing golden valid.
TEST_CASE("No tolerances writes no key", "[tolerances]") {
  auto doc = ShaftDoc();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "designer_tolerance_absent.json";
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("tolerances") == std::string::npos);
  std::filesystem::remove(path);
}

TEST_CASE("A copied document carries its tolerances", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  doc->SetTolerance(G6(id));
  Document copy(*doc);
  REQUIRE(copy.GetTolerances().size() == 1);
  CHECK(copy.GetTolerances()[0].isoClass == "g6");
  CHECK(copy.ToleranceFor(ElementRef{id, "1/face0"}) != nullptr);
}
