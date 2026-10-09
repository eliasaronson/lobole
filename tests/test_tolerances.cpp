// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Command.h"
#include "core/Document.h"
#include "core/FitStandards.h"
#include "core/Measure.h"
#include "core/ToleranceStack.h"
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

// The two flat ends of the shaft, found by KIND rather than by a guessed
// toponame — a stack needs a nominal it can actually measure, so a name that
// does not resolve fails the arithmetic rather than the storage.
std::pair<std::string, std::string> EndFaces(const Document& doc) {
  std::vector<std::string> flat;
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "plane")
      flat.push_back(name);
  }
  REQUIRE(flat.size() >= 2);
  return {flat[0], flat[1]};
}

std::string RoundFace(const Document& doc) {
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder")
      return name;
  }
  return {};
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

// A document without tolerances must serialize exactly as it did before v98,
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

// A distance tolerance is a different SUBJECT from a size on the same face,
// so the two coexist — and the size lookup must not hand check_fits a
// statement about a gap when it asked how big a bore is.
TEST_CASE("A size and a distance from the same face coexist", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();

  doc->SetTolerance(G6(id));
  Tolerance span;
  span.ref = ElementRef{id, "1/face0"};
  span.ref2 = ElementRef{id, "1/cap0"};
  span.lower = -0.05;
  span.upper = 0.05;
  doc->SetTolerance(span);
  REQUIRE(doc->GetTolerances().size() == 2);

  // The size lookup answers with the SIZE, not the distance that happens to
  // start on the same face.
  const Tolerance* size = doc->ToleranceFor(ElementRef{id, "1/face0"});
  REQUIRE(size != nullptr);
  CHECK(size->isoClass == "g6");
  CHECK_FALSE(size->IsDistance());

  // A second distance between the SAME ordered pair replaces; the reverse
  // pair is a different subject and does not.
  Tolerance again = span;
  again.lower = -0.02;
  again.upper = 0.02;
  doc->SetTolerance(again);
  CHECK(doc->GetTolerances().size() == 2);
  Tolerance reversed;
  reversed.ref = ElementRef{id, "1/cap0"};
  reversed.ref2 = ElementRef{id, "1/face0"};
  reversed.lower = -0.05;
  reversed.upper = 0.05;
  doc->SetTolerance(reversed);
  CHECK(doc->GetTolerances().size() == 3);
}

TEST_CASE("A distance tolerance round-trips with both refs", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  Tolerance span;
  span.ref = ElementRef{id, "1/face0"};
  span.ref2 = ElementRef{id, "1/cap0"};
  span.lower = -0.05;
  span.upper = 0.05;
  doc->SetTolerance(span);

  const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     "designer_tolerance_distance.json";
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  Document& reloaded = **loaded;
  REQUIRE(reloaded.GetTolerances().size() == 1);
  const Tolerance& got = reloaded.GetTolerances()[0];
  CHECK(got.IsDistance());
  CHECK(got.ref.name == "1/face0");
  CHECK(got.ref2.name == "1/cap0");
  CHECK(got.lower == Approx(-0.05));
  // A distance must NOT come back as a size tolerance on its first face.
  CHECK(reloaded.ToleranceFor(ElementRef{id, "1/face0"}) == nullptr);
  std::filesystem::remove(path);
}

// Regression: SetToleranceCommand snapshotted its previous value with
// ToleranceFor, which answers about a face's SIZE. Replacing a DISTANCE
// tolerance then either found nothing (and undo deleted the band instead of
// restoring it) or found the face's unrelated size tolerance (and undo
// restored THAT, leaving the edit standing). Both look like a working undo
// until you check what came back.
TEST_CASE("Undo restores a replaced DISTANCE tolerance", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  CommandStack stack;

  Tolerance span;
  span.ref = ElementRef{id, "1/face0"};
  span.ref2 = ElementRef{id, "1/cap0"};
  span.lower = -0.05;
  span.upper = 0.05;
  stack.Execute(std::make_unique<SetToleranceCommand>(span), *doc);
  REQUIRE(doc->GetTolerances().size() == 1);

  Tolerance tighter = span;
  tighter.lower = -0.01;
  tighter.upper = 0.01;
  stack.Execute(std::make_unique<SetToleranceCommand>(tighter), *doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].upper == Approx(0.01));

  stack.Undo(*doc);
  REQUIRE(doc->GetTolerances().size() == 1);
  CHECK(doc->GetTolerances()[0].upper == Approx(0.05));
  CHECK(doc->GetTolerances()[0].IsDistance());
}

// The nastier half of the same bug: a face carrying BOTH a size and a
// distance. Replacing the distance must not snapshot the size.
TEST_CASE("Undo of a distance edit leaves a coexisting size alone",
          "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  CommandStack stack;
  doc->SetTolerance(G6(id)); // a SIZE on 1/face0

  Tolerance span;
  span.ref = ElementRef{id, "1/face0"};
  span.ref2 = ElementRef{id, "1/cap0"};
  span.lower = -0.05;
  span.upper = 0.05;
  stack.Execute(std::make_unique<SetToleranceCommand>(span), *doc);
  Tolerance tighter = span;
  tighter.upper = 0.01;
  stack.Execute(std::make_unique<SetToleranceCommand>(tighter), *doc);
  REQUIRE(doc->GetTolerances().size() == 2);

  stack.Undo(*doc);
  REQUIRE(doc->GetTolerances().size() == 2);
  // The distance came back to its old band...
  const Tolerance* dist = doc->ToleranceForSubject(span.ref, span.ref2);
  REQUIRE(dist != nullptr);
  CHECK(dist->upper == Approx(0.05));
  // ...and the size on the same face was never touched.
  const Tolerance* size = doc->ToleranceFor(ElementRef{id, "1/face0"});
  REQUIRE(size != nullptr);
  CHECK(size->isoClass == "g6");
}

// The stack-up arithmetic. Signs are the whole of it: a term that SUBTRACTS
// contributes its UPPER deviation to the minimum, and getting that backwards
// produces a stack wrong by twice the band that looks entirely reasonable.
TEST_CASE("A stack sums worst case, with signs", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();

  // Two explicit bands on the same body, so the nominals are known exactly:
  // the Ø20 face, and its 40 mm length between the two caps.
  const auto ends = EndFaces(*doc);
  Tolerance dia;
  dia.ref = ElementRef{id, RoundFace(*doc)};
  dia.lower = -0.10;
  dia.upper = 0.20;
  const uint64_t diaId = doc->SetTolerance(dia).id;

  Tolerance len;
  len.ref = ElementRef{id, ends.first};
  len.ref2 = ElementRef{id, ends.second};
  len.lower = -0.01;
  len.upper = 0.05;
  const uint64_t lenId = doc->SetTolerance(len).id;

  std::string why;
  auto sum = tolstack::Sum(*doc, {{diaId, false}, {lenId, false}}, &why);
  INFO(why);
  REQUIRE(sum.has_value());
  REQUIRE(sum->terms.size() == 2);
  CHECK(sum->nominal == Approx(60.0));           // 20 + 40
  CHECK(sum->min == Approx(60.0 - 0.10 - 0.01)); // both at their lowest
  CHECK(sum->max == Approx(60.0 + 0.20 + 0.05));

  // Subtracting flips which deviation lands on which end.
  sum = tolstack::Sum(*doc, {{lenId, false}, {diaId, true}}, &why);
  INFO(why);
  REQUIRE(sum.has_value());
  CHECK(sum->nominal == Approx(20.0)); // 40 - 20
  // min = (40 - 0.01) - (20 + 0.20); max = (40 + 0.05) - (20 - 0.10)
  CHECK(sum->min == Approx(19.79));
  CHECK(sum->max == Approx(20.15));
}

TEST_CASE("A stack refuses rather than guessing", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  const uint64_t only = doc->SetTolerance(G6(id)).id;
  std::string why;

  // One term is a tolerance, not a stack.
  CHECK_FALSE(tolstack::Sum(*doc, {{only, false}}, &why).has_value());
  CHECK(why.find("at least two") != std::string::npos);
  CHECK_FALSE(tolstack::Sum(*doc, {}, &why).has_value());

  // An id nobody set.
  CHECK_FALSE(
      tolstack::Sum(*doc, {{only, false}, {9999, false}}, &why).has_value());
  CHECK(why.find("9999") != std::string::npos);

  // A term whose geometry is gone has no nominal to add, and is refused
  // rather than counted as zero — which would silently shorten the stack.
  Tolerance dangling;
  dangling.ref = ElementRef{id, "no-such-face"};
  dangling.lower = -0.1;
  dangling.upper = 0.1;
  const uint64_t bad = doc->SetTolerance(dangling).id;
  CHECK_FALSE(
      tolstack::Sum(*doc, {{only, false}, {bad, false}}, &why).has_value());
  CHECK(why.find("no longer resolves") != std::string::npos);
}

// A class-based term contributes the band its class implies AT ITS OWN SIZE,
// not a band copied from somewhere else in the chain.
TEST_CASE("A stack reads each class at its own size", "[tolerances]") {
  auto doc = ShaftDoc();
  const uint64_t id = doc->GetFeature(0)->GetId();
  Tolerance round;
  round.ref = ElementRef{id, RoundFace(*doc)};
  round.isoClass = "g6"; // Ø20 g6: -7/-20 um
  const uint64_t g6 = doc->SetTolerance(round).id;
  const auto ends = EndFaces(*doc);
  Tolerance len;
  len.ref = ElementRef{id, ends.first};
  len.ref2 = ElementRef{id, ends.second};
  len.lower = 0.0;
  len.upper = 0.0;
  const uint64_t lenId = doc->SetTolerance(len).id;

  std::string why;
  const auto sum = tolstack::Sum(*doc, {{g6, false}, {lenId, false}}, &why);
  INFO(why);
  REQUIRE(sum.has_value());
  CHECK(sum->terms[0].upper == Approx(-0.007));
  CHECK(sum->terms[0].lower == Approx(-0.020));
  CHECK(sum->nominal == Approx(60.0));
  CHECK(sum->max == Approx(60.0 - 0.007));
  CHECK(sum->min == Approx(60.0 - 0.020));
}
