// The printing process allowance (docs/tolerances/README.md#printed-parts):
// the table, its refusals, where the process lives on a part, and the
// sentences the fit and thread reports are built from.
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/PrintAllowance.h"
#include "core/features/ThreadFeature.h"
#include "core/io/Serialization.h"
#include "ui/ToleranceActions.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using Catch::Approx;

namespace {

const printfit::Process kPetg{"fdm", "petg"};

bool Contains(const std::string& s, const std::string& what) {
  return s.find(what) != std::string::npos;
}

std::unique_ptr<Document> BossDoc() {
  auto doc = std::make_unique<Document>();
  // An M8-sized boss: r = 4, h = 20.
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), 4.0, 20.0));
  return doc;
}

std::string RoundFace(const Body& body) {
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder")
      return name;
  }
  return {};
}

} // namespace

TEST_CASE("The PETG table says what to model for each kind of diameter",
          "[tolerances][print]") {
  std::string why;
  // Holes print small: +0.2 on the diameter under Ø10, +0.3 from Ø10.
  auto a = printfit::ForBasic(kPetg, printfit::Printed::Hole, 8.0, &why);
  REQUIRE(a);
  CHECK(a->allowanceMm == Approx(0.2));
  CHECK(a->modelMm == Approx(8.2));
  a = printfit::ForBasic(kPetg, printfit::Printed::Hole, 10.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(10.3));
  // Shafts print large.
  a = printfit::ForBasic(kPetg, printfit::Printed::Shaft, 8.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(7.9));
  // Threads: an internal one opens by 0.4, an external one closes by 0.1.
  a = printfit::ForBasic(kPetg, printfit::Printed::InternalThread, 8.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(8.4));
  a = printfit::ForBasic(kPetg, printfit::Printed::ExternalThread, 8.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(7.9));

  // Every row names where its number comes from, and the kinds are covered
  // from zero with no hole in the size range.
  const printfit::Table* t = printfit::Find(kPetg);
  REQUIRE(t != nullptr);
  CHECK(!t->conditions.empty());
  for (printfit::Printed k :
       {printfit::Printed::Hole, printfit::Printed::Shaft,
        printfit::Printed::InternalThread, printfit::Printed::ExternalThread}) {
    double reach = 0.0;
    for (const printfit::Row& r : t->rows) {
      if (r.kind != k)
        continue;
      CHECK(!r.source.empty());
      CHECK(r.fromMm == Approx(reach));
      reach = r.toMm;
    }
    CHECK(reach > 500.0);
  }
  CHECK(Contains(printfit::ConfirmNote(), "coupon"));
}

TEST_CASE("A modelled diameter reads back to what it prints",
          "[tolerances][print]") {
  std::string why;
  auto a = printfit::ForModelled(kPetg, printfit::Printed::Hole, 8.2, &why);
  REQUIRE(a);
  CHECK(a->basicMm == Approx(8.0));
  a = printfit::ForModelled(kPetg, printfit::Printed::Hole, 10.3, &why);
  REQUIRE(a);
  CHECK(a->basicMm == Approx(10.0));
  // Ø10.2..Ø10.3 is claimed by neither row's model range: the table steps at
  // Ø10, so a model in the gap prints at the step.
  a = printfit::ForModelled(kPetg, printfit::Printed::Hole, 10.25, &why);
  REQUIRE(a);
  CHECK(a->basicMm == Approx(10.0));
  a = printfit::ForModelled(kPetg, printfit::Printed::Shaft, 7.9, &why);
  REQUIRE(a);
  CHECK(a->basicMm == Approx(8.0));
  // Round trip across the whole hole range.
  for (double d = 1.0; d < 40.0; d += 0.7) {
    const auto f = printfit::ForBasic(kPetg, printfit::Printed::Hole, d, &why);
    REQUIRE(f);
    const auto b =
        printfit::ForModelled(kPetg, printfit::Printed::Hole, f->modelMm, &why);
    REQUIRE(b);
    CHECK(b->basicMm == Approx(d));
  }
}

TEST_CASE("An unknown process or a material without data is refused by name",
          "[tolerances][print]") {
  std::string why = printfit::Validate({"sla", "petg"});
  CHECK(Contains(why, "unknown process 'sla'"));
  CHECK(Contains(why, "Accepted: fdm"));
  why = printfit::Validate({"fdm", "abs"});
  CHECK(Contains(why, "no data for 'abs'"));
  CHECK(Contains(why, "Accepted for fdm: petg"));
  CHECK(printfit::Validate(kPetg).empty());

  std::string err;
  CHECK_FALSE(
      printfit::ForBasic({"fdm", "pla"}, printfit::Printed::Hole, 8.0, &err));
  CHECK(Contains(err, "pla"));
  CHECK_FALSE(printfit::ForBasic(kPetg, printfit::Printed::Hole, 0.0, &err));
  CHECK_FALSE(printfit::PrintedFromName("bore"));
  CHECK(printfit::PrintedFromName("internal_thread") ==
        printfit::Printed::InternalThread);
}

TEST_CASE("The print process lives on the part and rebuilds nothing",
          "[tolerances][print]") {
  auto doc = BossDoc();
  const uint64_t version = doc->Version();
  CommandStack stack;
  stack.Execute(std::make_unique<SetPrintProcessCommand>(kPetg), *doc);
  REQUIRE(doc->PrintProcess());
  CHECK(*doc->PrintProcess() == kPetg);
  CHECK(doc->Version() == version); // drives no geometry
  stack.Undo(*doc);
  CHECK_FALSE(doc->PrintProcess());
  stack.Redo(*doc);
  CHECK(doc->PrintProcess());

  // A copy carries it (a duplicated tab is printed the same way).
  Document copy(*doc);
  CHECK(copy.PrintProcess() == doc->PrintProcess());

  const auto path =
      std::filesystem::temp_directory_path() / "designer_print_process.json";
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->PrintProcess());
  CHECK(*(*loaded)->PrintProcess() == kPetg);

  // A file that names a pair this build has no data for is refused, not read
  // as unprinted or as PETG.
  {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    const auto at = text.find("\"petg\"");
    REQUIRE(at != std::string::npos);
    text.replace(at, 6, "\"abs\"");
    std::ofstream(path) << text;
  }
  CHECK_FALSE(DocumentIO::Load(path.string()).has_value());

  // An unprinted part writes no key, so every older golden re-saves as it
  // was.
  auto plain = BossDoc();
  REQUIRE(DocumentIO::Save(*plain, path.string()).has_value());
  std::ifstream in(path);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK_FALSE(Contains(text, "\"print\""));
  std::filesystem::remove(path);
}

TEST_CASE("A printed fit says what to model", "[tolerances][print]") {
  std::string why;
  auto d = ui::PrintedForBasic(kPetg, true, 8.0, std::nullopt, "H7", &why);
  REQUIRE(d);
  CHECK(d->Sentence("H7") == "H7 on a printed Ø8 bore (fdm petg): model Ø8.2");
  // H7 at Ø8 is 15 µm wide; a desktop FDM print holds about ±0.2 mm.
  CHECK(Contains(d->bandNote, "finer than fdm holds"));

  // Modelled at the bare size, it says what that prints and what to model.
  d = ui::PrintedForBasic(kPetg, true, 8.0, 8.0, "H7", &why);
  REQUIRE(d);
  CHECK_FALSE(d->Compensated());
  CHECK(d->Prints() == Approx(7.8));
  CHECK(Contains(d->Sentence("H7"), "it is modelled Ø8, which prints Ø7.8"));

  // From the model alone: Ø7.9 of shaft prints Ø8.
  d = ui::PrintedForModelled(kPetg, false, 7.9, "g6", &why);
  REQUIRE(d);
  CHECK(d->basic == Approx(8.0));
  CHECK(d->Compensated());
  CHECK(d->Sentence("g6") == "g6 on a printed Ø8 shaft (fdm petg): model Ø7.9");
}

TEST_CASE("A Thread on a printed part reports its printed allowance",
          "[tolerances][print][thread]") {
  auto doc = BossDoc();
  const Body& body = doc->GetBodies().at(0);
  const uint64_t createdBy = body.createdBy;
  const std::string face = RoundFace(body);
  REQUIRE(!face.empty());
  doc->AddFeature(std::make_unique<ThreadFeature>(
      "Thread", ElementRef{createdBy, face}, "M8", 1.25));
  const auto* th = dynamic_cast<const ThreadFeature*>(doc->GetFeature(1));
  REQUIRE(th != nullptr);
  REQUIRE(th->GetError().empty());

  // Not printed: nothing to say.
  CHECK_FALSE(ui::PrintedThreadFor(*doc, *th));

  doc->SetPrintProcess(kPetg);
  const auto r = ui::PrintedThreadFor(*doc, *th);
  REQUIRE(r);
  CHECK(r->external);
  CHECK(r->known);
  CHECK(r->modelledMajor == Approx(8.0));
  CHECK(r->allowance == Approx(-0.1));
  CHECK(r->printsMajor == Approx(8.1)); // the basic profile prints tight
  CHECK(r->modelMajor == Approx(7.9));
  CHECK(r->modelFace == Approx(7.9));
  CHECK(Contains(r->Sentence(), "model its major Ø7.9"));
  // A cosmetic thread prints nothing, and says so.
  bool cosmetic = false;
  for (const std::string& n : r->notes)
    cosmetic = cosmetic || Contains(n, "cosmetic");
  CHECK(cosmetic);
}
