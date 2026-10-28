// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A mate on an imported file's own face carries the face's print, so a file
// that renumbers its faces moves the mate with the face rather than onto
// whatever face then holds the number
// (docs/assemblies/README.md#a-mate-on-an-imported-face).

#include "TestPaths.h"
#include "mesh_fixtures.h"

#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Compound.hxx>

#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string ReadBytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

void WriteBytes(const std::filesystem::path& p, const std::string& bytes) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The same binary STL with its triangles in reverse order: the same part,
// whose recognised faces come out in another order -- what a re-export from
// another program, or another version of the file, does to the names.
std::string Reversed(const std::string& stl) {
  std::string out = stl.substr(0, 84);
  const size_t n = (stl.size() - 84) / 50;
  for (size_t i = n; i-- > 0;)
    out.append(stl, 84 + i * 50, 50);
  return out;
}

std::unique_ptr<Document> ImportPart(const std::filesystem::path& stl) {
  auto doc = std::make_unique<Document>();
  auto imp = std::make_unique<ImportFeature>("plate", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Recognize;
  doc->AddFeature(std::move(imp));
  return doc;
}

// The name of the bore about the vertical line through (x, y).
std::string BoreName(const Document& part, double x, double y) {
  const Body& body = part.GetBodies().at(0);
  for (const auto& [i, name] : body.elementMap.names) {
    glm::dvec3 origin, axis, xh;
    const auto info = Measure::DescribeFaceAt(body, i);
    if (info && info->kind == "cylinder" &&
        body.shape.FaceAxis(i, origin, axis, xh) &&
        std::hypot(origin.x - x, origin.y - y) < 1e-4)
      return name;
  }
  return {};
}

Occurrence At(uint64_t partId, glm::dvec3 pos, bool fixed) {
  Occurrence o;
  o.partId = partId;
  o.position = pos;
  o.fixed = fixed;
  return o;
}

// A pin's origin fastened to the plate's bore at (-50, -30): where the pin
// lands says which face the mate is on.
Mate PinInBore(uint64_t importId, const std::string& bore) {
  Mate m;
  m.type = MateType::Fastened;
  m.a.occurrenceId = 1;
  m.a.anchor.face = ElementRef{importId, bore};
  m.b.occurrenceId = 2;
  m.b.anchor.origin = true;
  m.flipped = false;
  return m;
}

} // namespace

TEST_CASE("A mate on an imported face follows it when the file renumbers",
          "[assembly][mesh_recognition]") {
  const fixtures::Part plate = fixtures::Plate();
  const auto stl = fixtures::WriteStl(plate, 0.0214, 2 * kPi / 24, "mate");
  const std::string original = ReadBytes(stl);

  auto part = ImportPart(stl);
  REQUIRE(part->GetBodies().size() == 1);
  const uint64_t importId = part->GetFeature(0)->GetId();
  const std::string bore = BoreName(*part, -50, -30);
  REQUIRE(!bore.empty());
  Document pin; // an empty part: only its origin is used

  const assembly::PartResolver resolve = [&](uint64_t id,
                                             uint64_t) -> const Document* {
    return id == 1 ? part.get() : &pin;
  };
  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(At(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(At(2, {100, 100, 100}, false));
  asmDoc.AddMate(PinInBore(importId, bore));

  REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
  const auto where = [&] { return asmDoc.FindById(2)->position; };
  CHECK_THAT(where().x, WithinAbs(-50.0, 1e-4));
  CHECK_THAT(where().y, WithinAbs(-30.0, 1e-4));
  // The first solve took the print.
  const auto& printed = asmDoc.GetMates().at(0).a.anchor.facePrint;
  REQUIRE(printed.has_value());
  CHECK(printed->shape == "cylinder");
  CHECK(printed->element == bore);

  // The file renumbers: the name now holds another face.
  WriteBytes(stl, Reversed(original));
  part->Regenerate();
  REQUIRE(part->GetBodies().size() == 1);
  REQUIRE(BoreName(*part, -50, -30) != bore);

  SECTION("followed, with the print") {
    asmDoc.FindById(2)->position = {100, 100, 100};
    REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
    CHECK_THAT(where().x, WithinAbs(-50.0, 1e-4));
    CHECK_THAT(where().y, WithinAbs(-30.0, 1e-4));
  }
  SECTION("without one (a file older than prints) it lands elsewhere") {
    Mate& m = asmDoc.GetMates().at(0);
    m.a.anchor.facePrint.reset();
    m.a.anchor.takePrints = false;
    asmDoc.FindById(2)->position = {100, 100, 100};
    assembly::Solve(asmDoc, resolve);
    CHECK(std::hypot(where().x + 50.0, where().y + 30.0) > 1.0);
    CHECK_FALSE(m.a.anchor.facePrint.has_value()); // and blesses nothing
  }
  SECTION("a face the file no longer has refuses the mate, naming it") {
    const fixtures::Part bracket = fixtures::Bracket();
    const auto other = fixtures::WriteStl(bracket, 0.01, 0.2, "other");
    WriteBytes(stl, ReadBytes(other));
    std::filesystem::remove(other);
    part->Regenerate();
    REQUIRE(part->GetBodies().size() == 1);
    CHECK(assembly::Solve(asmDoc, resolve).erroredMates == 1);
    const std::string& why = asmDoc.GetMates().at(0).error;
    INFO(why);
    CHECK(why.find("not the one it was picked on") != std::string::npos);
    CHECK(why.find("a cylinder face") != std::string::npos);
    CHECK(why.find("re-pick") != std::string::npos);
  }
  std::filesystem::remove(stl);
}

TEST_CASE("A mate whose imported face has a twin is refused as ambiguous",
          "[assembly][mesh_recognition]") {
  // Two copies of a part, one on the other, in one file (a multi-body
  // export that placed an instance twice): every face has a twin of the
  // same print, and which one the mate meant cannot be known. A box first
  // in the file renumbers the faces, so the name no longer holds the face.
  const fixtures::Part plate = fixtures::Plate();
  const auto brep =
      std::filesystem::temp_directory_path() /
      ("lobole_mate_twin_" + std::to_string(::getpid()) + ".brep");
  CADKernel& k = CADKernel::Instance();
  REQUIRE(k.SaveBREP(brep.string(), plate.shape));
  auto part = std::make_unique<Document>();
  {
    auto imp = std::make_unique<ImportFeature>("plate", brep.string());
    imp->keepOrigin = true;
    imp->upAxis = interchange::UpAxis::None;
    part->AddFeature(std::move(imp));
  }
  REQUIRE(part->GetBodies().size() == 1);
  const uint64_t importId = part->GetFeature(0)->GetId();
  const std::string bore = BoreName(*part, -50, -30);
  REQUIRE(!bore.empty());
  Document pin;
  const assembly::PartResolver resolve = [&](uint64_t id,
                                             uint64_t) -> const Document* {
    return id == 1 ? part.get() : &pin;
  };
  AssemblyDocument asmDoc;
  asmDoc.AddOccurrence(At(1, {0, 0, 0}, true));
  asmDoc.AddOccurrence(At(2, {100, 100, 100}, false));
  asmDoc.AddMate(PinInBore(importId, bore));
  REQUIRE(assembly::Solve(asmDoc, resolve).erroredMates == 0);
  REQUIRE(asmDoc.GetMates().at(0).a.anchor.facePrint.has_value());

  BRep_Builder bb;
  TopoDS_Compound twins;
  bb.MakeCompound(twins);
  bb.Add(twins, BRepPrimAPI_MakeBox(gp_Pnt(200, 0, 0), 5, 5, 5).Shape());
  bb.Add(twins, plate.shape.GetShape());
  bb.Add(twins, BRepBuilderAPI_Copy(plate.shape.GetShape()).Shape());
  REQUIRE(k.SaveBREP(brep.string(), CADShape(twins)));
  part->Regenerate();
  REQUIRE(!part->GetBodies().empty());
  CHECK(assembly::Solve(asmDoc, resolve).erroredMates == 1);
  const std::string& why = asmDoc.GetMates().at(0).error;
  INFO(why);
  CHECK(why.find("2 alike") != std::string::npos);
  CHECK(why.find("re-pick") != std::string::npos);
  std::filesystem::remove(brep);
}

TEST_CASE("Checking a mate's prints costs a solve nothing once taken",
          "[assembly][mesh_recognition]") {
  // A COST check (docs/ui/testing.md#tests-that-time-things), as a ratio of
  // two CPU times taken in the same run: a chain of mates on imported faces
  // solved with prints against the same chain without, each solved many
  // times as a drag would. A print is an OCC analysis; cached per part
  // version, the solves after the first pay a map lookup. Shown to fail
  // against a spin planted in the cached path.
  const fixtures::Part plate = fixtures::Plate();
  const auto stl = fixtures::WriteStl(plate, 0.0214, 2 * kPi / 24, "cost");
  auto part = ImportPart(stl);
  REQUIRE(part->GetBodies().size() == 1);
  const uint64_t importId = part->GetFeature(0)->GetId();
  std::vector<std::string> bores;
  for (int i = 0; i < 6; i++)
    for (int j = 0; j < 4; j++)
      bores.push_back(BoreName(*part, -50 + 20 * i, -30 + 20 * j));
  const assembly::PartResolver resolve =
      [&](uint64_t, uint64_t) -> const Document* { return part.get(); };
  const auto build = [&](bool prints) {
    auto a = std::make_unique<AssemblyDocument>();
    a->AddOccurrence(At(1, {0, 0, 0}, true));
    for (size_t k = 0; k < bores.size(); k++) {
      a->AddOccurrence(At(1, {500.0 + k, 0, 0}, false));
      Mate m = PinInBore(importId, bores[k]);
      m.b.occurrenceId = a->GetOccurrences().back().id;
      m.b.anchor.origin = false;
      m.b.anchor.face = ElementRef{importId, bores[(k + 1) % bores.size()]};
      m.a.anchor.takePrints = m.b.anchor.takePrints = prints;
      a->AddMate(m);
    }
    return a;
  };
  auto with = build(true), without = build(false);
  REQUIRE(assembly::Solve(*with, resolve).erroredMates == 0);
  REQUIRE(assembly::Solve(*without, resolve).erroredMates == 0);
  REQUIRE(with->GetMates().front().a.anchor.facePrint.has_value());
  REQUIRE_FALSE(without->GetMates().front().a.anchor.facePrint.has_value());
  // The least of three runs of 20 solves each: what the work costs, with
  // as little of the machine's other work in it as one run can leave.
  const auto cpu = [&](AssemblyDocument& a) {
    double least = 1e9;
    for (int run = 0; run < 3; run++) {
      const std::clock_t t0 = std::clock();
      for (int i = 0; i < 20; i++)
        assembly::Solve(a, resolve, nullptr, {}, false);
      least = std::min(least, double(std::clock() - t0) / CLOCKS_PER_SEC);
    }
    return least;
  };
  const double plain = cpu(*without);
  const double printed = cpu(*with);
  INFO("without prints " << plain << " s, with " << printed << " s");
  CHECK(printed <= 1.25 * plain + 0.02);
  std::filesystem::remove(stl);
}

TEST_CASE("A mate's prints are saved, and an older file's connectors take "
          "none",
          "[assembly][serialization]") {
  const auto root = std::filesystem::path(TestSourceDir()) / "tests/golden";
  auto loaded = DocumentIO::LoadAssembly(
      (root / "assembly/vNEXT2-mate-import-print.json").string());
  REQUIRE(loaded.has_value());
  const Mate& m = (*loaded)->GetMates().at(0);
  REQUIRE(m.a.anchor.facePrint.has_value());
  CHECK(m.a.anchor.facePrint->shape == "cylinder");
  CHECK(m.a.anchor.facePrint->element == m.a.anchor.face.name);
  CHECK_THAT(m.a.anchor.facePrint->size, WithinAbs(47.12388980384689, 1e-12));
  REQUIRE(m.a.anchor.snapPrint.has_value());
  CHECK(m.a.anchor.snapPrint->shape == "circle");
  CHECK(m.a.anchor.takePrints);

  // Saved and read again, the prints are as they were.
  const auto saved =
      std::filesystem::temp_directory_path() /
      ("lobole_mate_print_" + std::to_string(::getpid()) + ".json");
  REQUIRE(DocumentIO::SaveAssembly(**loaded, saved.string()).has_value());
  auto again = DocumentIO::LoadAssembly(saved.string());
  REQUIRE(again.has_value());
  const Mate& n = (*again)->GetMates().at(0);
  REQUIRE(n.a.anchor.facePrint.has_value());
  CHECK(n.a.anchor.facePrint->centroid == m.a.anchor.facePrint->centroid);
  CHECK(n.a.anchor.snapPrint->size == m.a.anchor.snapPrint->size);

  // A print of another element than the one beside it is refused.
  {
    std::string text = ReadBytes(saved);
    const auto at =
        text.find("\"element\": \"1/b1/face7\"", text.find("fingerprint"));
    REQUIRE(at != std::string::npos);
    text.replace(at, std::strlen("\"element\": \"1/b1/face7\""),
                 "\"element\": \"1/b1/face8\"");
    WriteBytes(saved, text);
    auto refused = DocumentIO::LoadAssembly(saved.string());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().find("not of the element it names") !=
          std::string::npos);
  }
  std::filesystem::remove(saved);

  // Older than prints: none to take.
  auto old = DocumentIO::LoadAssembly(
      (root / "assembly/v65-corner-mate.json").string());
  REQUIRE(old.has_value());
  CHECK_FALSE((*old)->GetMates().at(0).a.anchor.takePrints);
}
