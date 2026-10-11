// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The printing process allowance (docs/tolerances/README.md#printed-parts):
// the table, its refusals, where the process lives on a part, and the
// sentences the fit and thread reports are built from.
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/PrintAllowance.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/ThreadFeature.h"
#include "core/io/Serialization.h"
#include "ui/ToleranceActions.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include <unistd.h> // getpid

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

  // Every table names where each number comes from, and covers every kind
  // from zero with no hole in the size range.
  for (const printfit::Table& t : printfit::Tables()) {
    INFO(t.process << " " << t.material);
    CHECK(!t.conditions.empty());
    for (printfit::Printed k :
         {printfit::Printed::Hole, printfit::Printed::Shaft,
          printfit::Printed::InternalThread, printfit::Printed::ExternalThread,
          printfit::Printed::Gap}) {
      INFO(printfit::PrintedName(k));
      double reach = 0.0;
      for (const printfit::Row& r : t.rows) {
        if (r.kind != k)
          continue;
        CHECK(!r.source.empty());
        CHECK(r.fromMm == Approx(reach));
        reach = r.toMm;
      }
      CHECK(reach > 500.0);
    }
  }
  CHECK(Contains(printfit::ConfirmNote(), "coupon"));
}

TEST_CASE("A print-in-place gap opens the outer surface by the whole "
          "clearance",
          "[tolerances][print]") {
  // A Ø10 pin printed inside a ring: the ring is modelled 0.5 larger in PLA
  // (the community's 0.25 a side, a default to calibrate) and 0.6 in PETG.
  std::string why;
  auto a =
      printfit::ForBasic({"fdm", "pla"}, printfit::Printed::Gap, 10.0, &why);
  REQUIRE(a);
  CHECK(a->allowanceMm == Approx(0.5));
  CHECK(a->modelMm == Approx(10.5));
  CHECK(Contains(a->row->source, "DEFAULT to calibrate"));
  a = printfit::ForBasic(kPetg, printfit::Printed::Gap, 10.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(10.6));
  // PLA holes carry no step at Ø10: that was PETG's shrinkage.
  a = printfit::ForBasic({"fdm", "pla"}, printfit::Printed::Hole, 12.0, &why);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(12.2));
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
  CHECK(Contains(why, "Accepted for fdm: petg, pla"));
  CHECK(printfit::Validate(kPetg).empty());

  std::string err;
  CHECK_FALSE(
      printfit::ForBasic({"fdm", "abs"}, printfit::Printed::Hole, 8.0, &err));
  CHECK(Contains(err, "abs"));
  CHECK_FALSE(printfit::ForBasic(kPetg, printfit::Printed::Hole, 0.0, &err));
  CHECK_FALSE(printfit::PrintedFromName("bore"));
  CHECK(printfit::PrintedFromName("internal_thread") ==
        printfit::Printed::InternalThread);
  CHECK(printfit::PrintedFromName("gap") == printfit::Printed::Gap);
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

// A custom thread takes a bore's wall as its basic minor, so its major is
// the wall plus twice the basic depth AT ITS FLANK ANGLE: a 45-degree jar
// thread read with ISO's 1.0825 P would report the wrong printed major.
TEST_CASE("A custom thread's printed major follows its flank angle",
          "[tolerances][print][thread]") {
  auto doc = std::make_unique<Document>();
  auto pad =
      std::make_unique<SketchFeature>("Block", SketchPlane(PlaneType::XY));
  pad->AddRectangle({-20, -20}, {20, 20});
  doc->AddFeature(std::move(pad));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc->GetFeature(0)->GetId(), 10.0, ExtrudeMode::NewBody));
  auto centre = std::make_unique<SketchFeature>(
      "Centre",
      SketchPlane(glm::vec3(0, 0, 10), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  centre->AddEntity(sketch::Point{{0, 0}, 1});
  doc->AddFeature(std::move(centre));
  auto hole = std::make_unique<HoleFeature>("Bore", doc->GetFeature(2)->GetId(),
                                            HoleKind::Simple, 20.0, 10.0);
  hole->through = true;
  doc->AddFeature(std::move(hole));
  REQUIRE(doc->Regenerate());
  REQUIRE(doc->GetBodies().size() == 1);
  const Body& body = doc->GetBodies().at(0);
  std::string face;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto f = Measure::DescribeFaceAt(body, index);
    if (f && f->kind == "cylinder" && std::abs(f->radius - 10.0) < 1e-6)
      face = name;
  }
  REQUIRE(!face.empty());
  auto th = std::make_unique<ThreadFeature>(
      "Jar", ElementRef{body.createdBy, face}, "jar thread", 4.0);
  th->flankDeg = 45.0;
  doc->AddFeature(std::move(th));
  const auto* t = dynamic_cast<const ThreadFeature*>(doc->GetFeature(4));
  INFO(t->GetError());
  REQUIRE(t->GetError().empty());
  REQUIRE_FALSE(t->overlay.external);
  doc->SetPrintProcess(kPetg);
  const auto r = ui::PrintedThreadFor(*doc, *t);
  REQUIRE(r);
  CHECK_FALSE(r->known);
  CHECK(r->modelledMajor == Approx(20.0 + 2.0 * 1.25));
  CHECK(r->printsMajor == Approx(22.5 - 0.4));
}

// A Hole's clearance fit names the size the hole must COME OUT at, so on a
// printed part it is modelled at that size plus the hole allowance
// (docs/tolerances/README.md#printed-parts): M6 normal is Ø6.6 in steel and
// a Ø6.8 model in PETG. A tapped hole stays at its tap drill. Setting the
// process rebuilds that hole; a part without one rebuilds nothing.
TEST_CASE("A printed clearance hole is modelled at its fit plus the allowance",
          "[tolerances][print][hole]") {
  const auto block = [](const std::string& kind) {
    auto doc = std::make_unique<Document>();
    auto pad =
        std::make_unique<SketchFeature>("Block", SketchPlane(PlaneType::XY));
    pad->AddRectangle({-20, -20}, {20, 20});
    doc->AddFeature(std::move(pad));
    doc->AddFeature(std::make_unique<ExtrudeFeature>(
        "Pad", doc->GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
    auto centre = std::make_unique<SketchFeature>(
        "Centre", SketchPlane(glm::vec3(0, 0, 20), glm::vec3(0, 0, 1),
                              glm::vec3(1, 0, 0)));
    centre->AddEntity(sketch::Point{{0, 0}, 1});
    doc->AddFeature(std::move(centre));
    const uint64_t at = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
    auto hole =
        std::make_unique<HoleFeature>("Hole", at, HoleKind::Simple, 1.0, 12.0);
    hole->fitThread = "M6";
    hole->fitKind = kind;
    doc->AddFeature(std::move(hole));
    REQUIRE(doc->Regenerate());
    return doc;
  };
  const auto holeOf = [](const Document& doc) {
    return dynamic_cast<const HoleFeature*>(
        doc.GetFeature(doc.FeatureCount() - 1));
  };

  auto doc = block("normal");
  CHECK(holeOf(*doc)->diameter == Approx(6.6));
  const auto a =
      printfit::ForBasic(kPetg, printfit::Printed::Hole, 6.6, nullptr);
  REQUIRE(a);
  CommandStack stack;
  stack.Execute(std::make_unique<SetPrintProcessCommand>(kPetg), *doc);
  INFO(holeOf(*doc)->GetError());
  CHECK(holeOf(*doc)->diameter == Approx(a->modelMm));
  CHECK(holeOf(*doc)->diameter == Approx(6.8));
  CHECK(doc->LastRegenStats().executed >= 1); // the hole rebuilt
  // The bore really is that size.
  bool found = false;
  for (const auto& [index, name] : doc->GetBodies().at(0).elementMap.names) {
    const auto face = Measure::DescribeFaceAt(doc->GetBodies().at(0), index);
    if (face && face->kind == "cylinder") {
      CHECK(face->radius * 2.0 == Approx(6.8));
      found = true;
    }
  }
  CHECK(found);
  // Undone, it is the steel size again.
  stack.Undo(*doc);
  CHECK(holeOf(*doc)->diameter == Approx(6.6));

  // A tapped hole stays at its tap drill.
  auto tapped = block("tapped");
  const double drill = holeOf(*tapped)->diameter;
  const uint64_t version = tapped->Version();
  tapped->SetPrintProcess(kPetg);
  CHECK(holeOf(*tapped)->diameter == Approx(drill));
  CHECK(tapped->Version() == version); // nothing on it moved: no regen
}

// ---- Print profiles (docs/tolerances/README.md#print-profiles) ----------

namespace {

// K5 PLA on a P1S: the gap measured at 0.1 a side.
printfit::Profile K5() {
  auto p = printfit::ProfileFromBuiltin("K5 PLA on P1S", "fdm", "pla");
  REQUIRE(p);
  for (printfit::Row& r : p->table.rows)
    if (r.kind == printfit::Printed::Gap) {
      r.allowanceMm = 0.2;
      r.source = "coupon 2026-10-09";
    }
  p->stlDeflectionMm = 0.01;
  return *p;
}

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(const char* tag)
      : path(std::filesystem::temp_directory_path() /
             (std::string("designer-") + tag + "-" +
              std::to_string(::getpid()))) {
    std::filesystem::remove_all(path);
  }
  ~TempDir() { std::filesystem::remove_all(path); }
};

} // namespace

TEST_CASE("A print profile round-trips through its file",
          "[tolerances][print][profile]") {
  TempDir dir("print-profiles");
  std::string problems = "x";
  CHECK(DocumentIO::LoadPrintProfiles(dir.path.string(), &problems).empty());
  CHECK(problems.empty()); // no store yet is not an error
  const printfit::Profile k5 = K5();
  std::string error;
  REQUIRE(DocumentIO::SavePrintProfile(dir.path.string(), k5, &error));
  CHECK(std::filesystem::exists(dir.path / "k5-pla-on-p1s.json"));
  // A broken neighbour is reported, and does not hide the good one.
  std::ofstream(dir.path / "broken.json") << "{ not json";
  const auto loaded =
      DocumentIO::LoadPrintProfiles(dir.path.string(), &problems);
  REQUIRE(loaded.size() == 1);
  CHECK(loaded[0] == k5);
  CHECK(Contains(problems, "broken.json"));
  // The open end survives as "and up", not as a number.
  const auto j = DocumentIO::PrintProfileToJson(k5);
  CHECK_FALSE(j.at("rows").at(0).contains("to_mm"));
  // Two names that fold to one file: the second is refused.
  printfit::Profile clash = k5;
  clash.name = "k5 pla on p1s";
  CHECK_FALSE(DocumentIO::SavePrintProfile(dir.path.string(), clash, &error));
  CHECK(Contains(error, "would share a file"));
  REQUIRE(DocumentIO::DeletePrintProfile(dir.path.string(), k5.name, &error));
  CHECK_FALSE(
      DocumentIO::DeletePrintProfile(dir.path.string(), k5.name, &error));
  CHECK(Contains(error, "no print profile called"));
}

TEST_CASE("A print profile refuses what it cannot be, saying what",
          "[tolerances][print][profile]") {
  const auto refusal = [](const printfit::Profile& p) {
    return printfit::ValidateProfile(p);
  };
  CHECK(refusal(K5()).empty());
  auto p = K5();
  p.name = "  ";
  CHECK(Contains(refusal(p), "needs a name"));
  p = K5();
  p.table.process = "sla";
  CHECK(Contains(refusal(p), "Accepted: fdm"));
  p = K5();
  p.table.material = "abs";
  CHECK(Contains(refusal(p), "Accepted for fdm: petg, pla"));
  p = K5();
  p.table.rows.clear();
  CHECK(Contains(refusal(p), "at least one row"));
  p = K5();
  p.stlDeflectionMm = -0.01;
  CHECK(Contains(refusal(p), "stl_deflection_mm"));
  // The wrong sign for the kind.
  p = K5();
  p.table.rows.push_back({printfit::Printed::Gap, 0.0, 1.0, 0.0, ""});
  CHECK(Contains(refusal(p), "gap's allowance must be positive"));
  p = K5();
  p.table.rows = {{printfit::Printed::Shaft, 0.0, printfit::kAboveMm, 0.1, ""}};
  CHECK(Contains(refusal(p), "not positive"));
  p = K5();
  p.table.rows = {{printfit::Printed::Hole, 0.0, printfit::kAboveMm, -0.1, ""}};
  CHECK(Contains(refusal(p), "not negative"));
  // Overlapping and out-of-order ranges within a kind.
  p = K5();
  p.table.rows = {{printfit::Printed::Hole, 0.0, 10.0, 0.2, ""},
                  {printfit::Printed::Hole, 8.0, 20.0, 0.3, ""}};
  CHECK(Contains(refusal(p), "overlaps"));
  p.table.rows = {{printfit::Printed::Hole, 10.0, 20.0, 0.2, ""},
                  {printfit::Printed::Hole, 0.0, 10.0, 0.3, ""}};
  CHECK(Contains(refusal(p), "out of order"));
  p.table.rows = {{printfit::Printed::Hole, 5.0, 5.0, 0.2, ""}};
  CHECK(Contains(refusal(p), "size range"));
  // A refusal writes nothing.
  TempDir dir("print-profiles-refused");
  std::string error;
  CHECK_FALSE(DocumentIO::SavePrintProfile(dir.path.string(), p, &error));
  CHECK_FALSE(std::filesystem::exists(dir.path));
}

TEST_CASE("A part reads its embedded profile, and says how it stands",
          "[tolerances][print][profile]") {
  const printfit::Profile k5 = K5();
  auto doc = BossDoc();
  doc->SetPrintProcess(printfit::FromProfile(k5));
  // The copy is what the lookups read: 0.1 a side, not the default 0.25.
  auto a = printfit::ForBasic(*doc->PrintProcess(), printfit::Printed::Gap,
                              10.0, nullptr);
  REQUIRE(a);
  CHECK(a->modelMm == Approx(10.2));
  CHECK(printfit::StlDeflectionMm(*doc->PrintProcess()) == Approx(0.01));
  CHECK(printfit::Label(*doc->PrintProcess()) == "'K5 PLA on P1S' (fdm pla)");

  // Saved and loaded, it carries the rows.
  const auto path =
      std::filesystem::temp_directory_path() /
      ("designer_print_profile_" + std::to_string(::getpid()) + ".json");
  REQUIRE(DocumentIO::Save(*doc, path.string()).has_value());
  auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->PrintProcess());
  CHECK(*(*loaded)->PrintProcess() == *doc->PrintProcess());
  std::filesystem::remove(path);

  // Against the store: same, differs (the user re-measured), missing.
  std::vector<printfit::Profile> store{k5};
  CHECK(printfit::MatchProfile(store, *doc->PrintProcess()) ==
        printfit::ProfileMatch::Same);
  store[0].table.rows.back().allowanceMm = 0.3;
  CHECK(printfit::MatchProfile(store, *doc->PrintProcess()) ==
        printfit::ProfileMatch::Differs);
  // ...and the part still reads ITS copy, not the store's.
  a = printfit::ForBasic(*doc->PrintProcess(), printfit::Printed::Gap, 10.0,
                         nullptr);
  CHECK(a->modelMm == Approx(10.2));
  store.clear();
  CHECK(printfit::MatchProfile(store, *doc->PrintProcess()) ==
        printfit::ProfileMatch::Missing);
  CHECK(printfit::MatchProfile(store, kPetg) ==
        printfit::ProfileMatch::Builtin);
}

TEST_CASE("Editing a profile's allowance rebuilds what was modelled from it",
          "[tolerances][print][profile][hole]") {
  auto doc = std::make_unique<Document>();
  auto pad =
      std::make_unique<SketchFeature>("Block", SketchPlane(PlaneType::XY));
  pad->AddRectangle({-20, -20}, {20, 20});
  doc->AddFeature(std::move(pad));
  doc->AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc->GetFeature(0)->GetId(), 20.0, ExtrudeMode::NewBody));
  auto centre = std::make_unique<SketchFeature>(
      "Centre",
      SketchPlane(glm::vec3(0, 0, 20), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0)));
  centre->AddEntity(sketch::Point{{0, 0}, 1});
  doc->AddFeature(std::move(centre));
  const uint64_t at = doc->GetFeature(doc->FeatureCount() - 1)->GetId();
  auto hole =
      std::make_unique<HoleFeature>("Hole", at, HoleKind::Simple, 1.0, 12.0);
  hole->fitThread = "M6";
  hole->fitKind = "normal";
  doc->AddFeature(std::move(hole));
  REQUIRE(doc->Regenerate());
  const auto holeOf = [&] {
    return dynamic_cast<const HoleFeature*>(
        doc->GetFeature(doc->FeatureCount() - 1));
  };

  printfit::Profile mine = K5();
  CommandStack stack;
  stack.Execute(
      std::make_unique<SetPrintProcessCommand>(printfit::FromProfile(mine)),
      *doc);
  CHECK(holeOf()->diameter == Approx(6.6 + 0.2)); // the pla hole row
  const std::string before = printfit::Fingerprint(*doc->PrintProcess());

  // The same name, a different hole row: "use mine" after a coupon.
  for (printfit::Row& r : mine.table.rows)
    if (r.kind == printfit::Printed::Hole)
      r.allowanceMm = 0.35;
  stack.Execute(
      std::make_unique<SetPrintProcessCommand>(printfit::FromProfile(mine)),
      *doc);
  INFO(holeOf()->GetError());
  CHECK(holeOf()->diameter == Approx(6.95));
  CHECK(doc->LastRegenStats().executed >= 1);
  // The requirements hash folds the same fingerprint.
  CHECK(printfit::Fingerprint(*doc->PrintProcess()) != before);
  stack.Undo(*doc);
  CHECK(holeOf()->diameter == Approx(6.8));
}
