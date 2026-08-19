#include <catch2/catch_test_macros.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/io/Autosaver.h"
#include "core/io/Serialization.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// One directory per case, per process. `catch_discover_tests` gives every
// TEST_CASE its own process and ctest -j runs them at once, so a shared fixed
// path means one case's remove_all() deletes the directory another case is
// mid-save into — a failure that looks like an autosave bug and is a test bug.
fs::path TempDir(const char* name) {
  auto dir =
      fs::temp_directory_path() / ("designer_autosave_" + std::string(name) +
                                   "_" + std::to_string(::getpid()));
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

Document MakeDoc() {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 1.0, 1.0, 1.0));
  return doc;
}

} // namespace

TEST_CASE("Sidecar paths", "[autosave]") {
  CHECK(Autosaver::SidecarPath("/tmp/part.json") == "/tmp/part.json.autosave");
  // The unsaved slot lives under the state dir, not next to anything.
  CHECK(Autosaver::SidecarPath("").ends_with("unsaved.autosave.json"));
}

TEST_CASE("MaybeSave fires only when dirty, changed, and due", "[autosave]") {
  const fs::path dir = TempDir("maybesave");
  const std::string project = (dir / "part.json").string();
  Document doc = MakeDoc();
  const uint64_t v = doc.Version();

  Autosaver saver(10.0);
  saver.Prime(0.0);
  // Not dirty: version == savedVersion
  CHECK_FALSE(saver.MaybeSave(doc, project, v, 100.0));
  // Dirty but interval not elapsed since Prime
  CHECK_FALSE(saver.MaybeSave(doc, project, v - 1, 5.0));
  // Dirty + due -> saves
  std::string error;
  REQUIRE(saver.MaybeSave(doc, project, v - 1, 11.0, &error));
  CHECK(fs::exists(Autosaver::SidecarPath(project)));
  CHECK_FALSE(fs::exists(Autosaver::SidecarPath(project) + ".tmp"));
  // Unchanged since the last autosave -> suppressed even when due
  CHECK_FALSE(saver.MaybeSave(doc, project, v - 1, 100.0));
  // Change the document -> saves again once due
  doc.AddFeature(
      std::make_unique<BoxFeature>("B2", glm::vec3(5, 0, 0), 1.0, 1.0, 1.0));
  CHECK_FALSE(saver.MaybeSave(doc, project, v - 1, 12.0)); // not due yet
  CHECK(saver.MaybeSave(doc, project, v - 1, 25.0));

  // The sidecar is a loadable document
  auto loaded = testio::Load(Autosaver::SidecarPath(project));
  REQUIRE(loaded != nullptr);
  CHECK(loaded->FeatureCount() == 2);

  // Interval 0 = disabled
  Autosaver off(0.0);
  CHECK_FALSE(off.MaybeSave(doc, project, 0, 1e9));
}

// The headless smoke suite's one known flake, and a way to lose real work:
// open a project, edit, let an autosave fire, then replace the document (new
// project, open, history restore). Versions restart at 0, so the new
// document's version eventually EQUALS the version the autosaver last saved
// from — and it concluded "unchanged since the last autosave" and stopped
// saving. Under parallel load the earlier autosave lands at a different
// version each run, which is why it looked like a timing flake rather than
// what it is: a version number used as an identity.
TEST_CASE("A different document at the same version still autosaves",
          "[autosave]") {
  const fs::path dir = TempDir("newdoc");
  const std::string first = (dir / "first.json").string();
  const std::string second = (dir / "second.json").string();

  Autosaver saver(1.0);
  saver.Prime(0.0);
  Document a = MakeDoc();
  REQUIRE(saver.MaybeSave(a, first, a.Version() - 1, 10.0));
  const uint64_t collidingVersion = a.Version();

  // A different document that happens to reach the same version.
  Document b = MakeDoc();
  while (b.Version() < collidingVersion)
    b.AddFeature(
        std::make_unique<BoxFeature>("Filler", glm::vec3(0), 1.0, 1.0, 1.0));
  REQUIRE(b.Version() == collidingVersion);
  REQUIRE(b.InstanceId() != a.InstanceId());

  CHECK(saver.MaybeSave(b, second, b.Version() - 1, 20.0));
  CHECK(fs::exists(Autosaver::SidecarPath(second)));
}

TEST_CASE("HasNewerAutosave and Clear", "[autosave]") {
  const fs::path dir = TempDir("hasnewer");
  const std::string project = (dir / "part.json").string();
  Document doc = MakeDoc();

  CHECK_FALSE(Autosaver::HasNewerAutosave(project));

  Autosaver saver(0.001);
  saver.Prime(0.0);
  REQUIRE(saver.MaybeSave(doc, project, doc.Version() - 1, 1.0));
  // No project file at all: any sidecar counts as newer.
  CHECK(Autosaver::HasNewerAutosave(project));

  // A project file saved after the sidecar wins (not newer anymore).
  std::string error;
  REQUIRE(testio::Save(doc, project));
  fs::last_write_time(Autosaver::SidecarPath(project),
                      fs::last_write_time(project) - std::chrono::hours(1));
  CHECK_FALSE(Autosaver::HasNewerAutosave(project));
  // ...and an older project file loses (sidecar newer again).
  fs::last_write_time(Autosaver::SidecarPath(project),
                      fs::last_write_time(project) + std::chrono::hours(1));
  CHECK(Autosaver::HasNewerAutosave(project));

  Autosaver::Clear(project);
  CHECK_FALSE(Autosaver::HasNewerAutosave(project));
  CHECK_FALSE(fs::exists(Autosaver::SidecarPath(project)));
}
