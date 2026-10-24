// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>

#include "TestIO.h"
#include "cad/CADKernel.h"
#include "core/Document.h"
#include "core/Feature.h" // BoxFeature
#include "ui/ProjectActions.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <unistd.h>

// Opening a project is a read that touches no AppState and an adopt that swaps
// it in (docs/ui/long-operations.md#opening-a-project). The read loads the
// project's parts side by side, so what it produces must not depend on how many
// threads did it -- the same rule tests/sweep_parallel_smoke.py holds the clash
// sweep to, because a race in a rebuild corrupts an ANSWER before it crashes.

namespace fs = std::filesystem;

namespace {

// Per process: worktrees run this suite side by side, and a fixed path let one
// run's remove_all empty the folder another was writing its STEP into.
fs::path TempProject(const char* name) {
  const fs::path p =
      fs::temp_directory_path() / (std::to_string(::getpid()) + "-" + name);
  fs::remove_all(p);
  fs::create_directories(p);
  return p;
}

// Pins parallel::Workers() for one scope, restoring what was there.
struct ScopedThreads {
  explicit ScopedThreads(const char* n) {
    if (const char* was = std::getenv("LOBOLE_SWEEP_THREADS"))
      m_was = was;
    if (n)
      setenv("LOBOLE_SWEEP_THREADS", n, 1);
    else
      unsetenv("LOBOLE_SWEEP_THREADS");
  }
  ~ScopedThreads() {
    if (m_was.empty())
      unsetenv("LOBOLE_SWEEP_THREADS");
    else
      setenv("LOBOLE_SWEEP_THREADS", m_was.c_str(), 1);
  }
  std::string m_was;
};

// A container project whose parts are every golden document that loads on
// its own: sketches, fillets, patterns, lofts, sweeps, sheet metal, tubes,
// fasteners, threads, derived parts. Flat golden files carry no baked
// geometry, so every one of them REBUILDS, which is the path that runs
// feature code on several threads at once.
fs::path GoldenProject(const char* name, size_t* parts) {
  const fs::path proj = TempProject(name);
  const fs::path golden = fs::path(TestSourceDir()) / "tests" / "golden";
  nlohmann::json docs = nlohmann::json::array();
  uint64_t id = 1;
  for (const auto& entry : fs::directory_iterator(golden)) {
    const fs::path p = entry.path();
    const std::string file = p.filename().string();
    if (p.extension() != ".json" || file.find(".expect.") != std::string::npos)
      continue;
    if (testio::Load(p.string()) == nullptr)
      continue; // not a part document on its own (it needs its project)
    // Copied in: a project's documents are inside it, or it is refused
    // (docs/versioning/format.md#paths-in-a-project).
    fs::create_directories(proj / "golden");
    fs::copy_file(p, proj / "golden" / file);
    docs.push_back({{"name", p.stem().string()},
                    {"dir", "golden/" + file},
                    {"type", "part"},
                    {"id", id++}});
  }
  // And parts that import a STEP file, rebuilt side by side: OpenCASCADE's
  // data exchange is process-wide and unlocked (cad/XcafDocument.h), so these
  // are what a missing lock would crash on. The goldens' own imports name
  // files that are not beside them and fail the same way on every thread.
  CADKernel& k = CADKernel::Instance();
  const fs::path step = proj / "shared.step";
  REQUIRE(k.SaveSTEP(
      step.string(),
      {k.CreateBox(20.0, 10.0, 5.0),
       k.CreateCylinder(glm::vec3(0), glm::vec3(0, 0, 1), 3.0, 12.0)}));
  for (int i = 0; i < 8; i++) {
    const fs::path dir =
        proj / "parts" / ("import" + std::to_string(i) + ".dsgn");
    fs::create_directories(dir.parent_path());
    Document doc;
    doc.AddFeature(std::make_unique<ImportFeature>("Imported", step.string()));
    REQUIRE(doc.GetBodies().size() >= 1);
    REQUIRE(testio::Save(doc, dir.string()));
    fs::remove_all(dir / "geometry"); // so it rebuilds, re-reading the STEP
    docs.push_back({{"name", "import" + std::to_string(i)},
                    {"dir", "parts/import" + std::to_string(i) + ".dsgn"},
                    {"type", "part"},
                    {"id", id++}});
  }
  std::ofstream(proj / "project.json")
      << nlohmann::json{{"documents", docs}, {"active", 0}}.dump(2);
  *parts = docs.size();
  return proj;
}

} // namespace

TEST_CASE("A project's parts load the same on one thread and on many",
          "[open][parallel]") {
  size_t n = 0;
  const fs::path proj = GoldenProject("open-parallel.dsgn", &n);
  REQUIRE(n >= 40); // the corpus, not a token handful

  std::expected<ui::LoadedProject, std::string> serial =
      std::unexpected(std::string("unread"));
  std::expected<ui::LoadedProject, std::string> again =
      std::unexpected(std::string("unread"));
  {
    ScopedThreads one("1");
    serial = ui::ReadProject(proj.string());
    again = ui::ReadProject(proj.string());
  }
  INFO((serial ? std::string() : serial.error()));
  REQUIRE(serial.has_value());
  REQUIRE(again.has_value());
  ui::OpenProgress progress;
  std::expected<ui::LoadedProject, std::string> parallel =
      std::unexpected(std::string("unread"));
  {
    ScopedThreads all(nullptr);
    parallel = ui::ReadProject(proj.string(), &progress);
  }
  REQUIRE(parallel.has_value());

  CHECK(progress.total.load() == static_cast<int>(n));
  CHECK(progress.done.load() == static_cast<int>(n));
  CHECK(progress.building.empty());
  REQUIRE(serial->parts.size() == n);
  REQUIRE(parallel->parts.size() == n);
  CHECK(serial->rebuiltParts == parallel->rebuiltParts);
  CHECK(serial->rebuildReason == parallel->rebuildReason);

  for (size_t i = 0; i < n; i++) {
    const ui::PartSlot& a = serial->parts[i];
    const ui::PartSlot& b = parallel->parts[i];
    INFO(a.name);
    REQUIRE(a.name == b.name); // in the project's order, not finishing order
    REQUIRE(a.doc != nullptr);
    REQUIRE(b.doc != nullptr);
    // The chain hash, where it is reproducible at all: a sketch that
    // projects external geometry mints fresh random entity ids when it is
    // rebuilt, so its hash differs between two serial loads as well.
    if (a.doc->TipChainHash() == again->parts[i].doc->TipChainHash())
      CHECK(a.doc->TipChainHash() == b.doc->TipChainHash());
    REQUIRE(a.doc->FeatureCount() == b.doc->FeatureCount());
    for (size_t f = 0; f < a.doc->FeatureCount(); f++)
      CHECK(a.doc->GetFeature(f)->GetError() ==
            b.doc->GetFeature(f)->GetError());
    const auto& ba = a.doc->GetBodies();
    const auto& bb = b.doc->GetBodies();
    REQUIRE(ba.size() == bb.size());
    for (size_t k = 0; k < ba.size(); k++) {
      // Bit for bit: the same recipe on the same kernel is deterministic,
      // and a tolerance here would hide exactly the race this is for.
      CHECK(ba[k].shape.Volume() == bb[k].shape.Volume());
      CHECK(ba[k].elementMap.names == bb[k].elementMap.names);
    }
  }
  fs::remove_all(proj);
}

TEST_CASE("An open that is cancelled reads nothing", "[open]") {
  size_t n = 0;
  const fs::path proj = GoldenProject("open-cancel.dsgn", &n);
  std::atomic<bool> cancel{true};
  ui::OpenProgress progress;
  progress.cancel = &cancel;
  const auto r = ui::ReadProject(proj.string(), &progress);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error() == "cancelled");
  CHECK(progress.done.load() == 0);
  fs::remove_all(proj);
}

// The bike took 20 s to open because its baked geometry was from an older
// build, and nothing said so. The read reports how many parts it rebuilt and
// why, which is what the menu bar's notice and open_project's answer show.
TEST_CASE("An open says which parts it rebuilt and why", "[open][baked]") {
  const fs::path proj = TempProject("open-rebuilt.dsgn");
  fs::remove_all(proj);
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Plate", glm::vec3(0), 40, 10, 30));
  doc.AddFeature(
      std::make_unique<BoxFeature>("Boss", glm::vec3(5, 10, 5), 8, 6, 8));
  REQUIRE(testio::Save(doc, proj.string()));

  {
    const auto r = ui::ReadProject(proj.string());
    REQUIRE(r.has_value());
    CHECK(r->rebuiltParts == 0); // a baked open is a load
    CHECK(r->rebuildReason.empty());
  }

  // What an older build's blob looks like to this one.
  const fs::path index = proj / "geometry" / "index.json";
  nlohmann::json j = nlohmann::json::parse(std::ifstream(index));
  j["geometry_format"] = 1;
  std::ofstream(index) << j.dump();
  {
    const auto r = ui::ReadProject(proj.string());
    REQUIRE(r.has_value());
    CHECK(r->rebuiltParts == 1);
    CHECK(r->rebuildReason == "the saved geometry is from an older build");
  }

  fs::remove_all(proj / "geometry");
  {
    const auto r = ui::ReadProject(proj.string());
    REQUIRE(r.has_value());
    CHECK(r->rebuiltParts == 1);
    CHECK(r->rebuildReason == "no saved geometry");
  }
  fs::remove_all(proj);
}

// A part that uses a PROJECT variable regenerates against it on the read, not
// only once the adopt wires the project scope in. Read without it, the chess
// set's piece part came up with king_h unknown: every feature on it failed,
// the baked geometry did not match, and each open rebuilt it from scratch
// before the adopt put it right.
TEST_CASE("A part using a project variable opens from its baked geometry",
          "[open][baked][variables]") {
  const fs::path proj = TempProject("open-project-var.dsgn");
  const fs::path dir = proj / "parts" / "plate.dsgn";
  fs::create_directories(dir.parent_path());
  std::vector<Parameter> params{{"plate_w", "40", "", 0.0, {}}};
  SymbolTable symbols;
  EvaluateParameterTable(params, symbols, nullptr, {});
  {
    Document doc;
    doc.SetProjectSymbols(&symbols);
    doc.SetProjectParameters(&params);
    // Through a part variable, as the piece's base_r reads king_h: a
    // feature naming the project variable directly keeps its saved number
    // when the name is unknown, a part variable has none to keep.
    doc.SetParameter("half_w", "plate_w / 2");
    auto box = std::make_unique<BoxFeature>("Plate", glm::vec3(0), 1, 10, 30);
    box->widthExpr = "half_w * 2";
    doc.AddFeature(std::move(box));
    REQUIRE(doc.Regenerate());
    REQUIRE(doc.GetBodies().size() == 1);
    REQUIRE(testio::Save(doc, dir.string()));
  }
  std::ofstream(proj / "project.json")
      << nlohmann::json{{"documents",
                         {{{"name", "plate"},
                           {"dir", "parts/plate.dsgn"},
                           {"type", "part"},
                           {"id", 1}}}},
                        {"parameters",
                         {{{"name", "plate_w"}, {"expression", "40"}}}},
                        {"active", 0}}
             .dump(2);
  const auto r = ui::ReadProject(proj.string());
  REQUIRE(r.has_value());
  CHECK(r->rebuiltParts == 0);
  REQUIRE(r->parts.size() == 1);
  const Document& plate = *r->parts[0].doc;
  REQUIRE(plate.GetBodies().size() == 1);
  CHECK(plate.GetFeatures()[0]->GetError().empty());
  // The read leaves no pointer into its own, now gone, scope.
  CHECK(plate.ProjectSymbols() == nullptr);
  fs::remove_all(proj);
}
