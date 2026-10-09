// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifdef HAVE_CAM

// Surfacing passes kept on disk between runs
// (docs/ui/long-operations.md#passes-kept-on-disk).

#include "cam_fixtures.h"
#include "core/cam/CamSurface.h"
#include "core/cam/CamSurfaceStore.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

// A scratch directory of its own: the store must never be pointed at the
// user's cache from a test.
struct TempDir {
  explicit TempDir(const char* name)
      : path(fs::temp_directory_path() /
             (std::string(name) + "-" + std::to_string(::getpid()))) {
    fs::remove_all(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  fs::path path;
};

// A turned shaft and a rotary finish over it, coarse enough to stay quick.
struct Turning {
  std::unique_ptr<Document> doc = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  cam::Setup setup;
  cam::Operation op;
  Turning() {
    setup = BasicSetup();
    RotaryMachine(setup);
    setup.stock.margin = {0.0, 0.0, 0.0};
    setup.safeZ = 40.0;
    setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
    const cam::DerivedOp d = cam::DeriveOperation(
        setup, doc->GetBodies(), cam::OpType::SurfaceRotary, cam::Target{});
    REQUIRE(d.ok);
    op = d.op;
    op.id = 1;
    op.stepover = 0.25;
  }
  const cam::Tool& tool() const { return setup.tools[0]; }
};

bool SameMoves(const std::vector<cam::Move>& a,
               const std::vector<cam::Move>& b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
    if (a[i].kind != b[i].kind || a[i].to != b[i].to || a[i].a != b[i].a ||
        a[i].hasA != b[i].hasA || a[i].feed != b[i].feed)
      return false;
  return true;
}

size_t Files(const fs::path& dir) {
  size_t n = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    n += e.path().extension() == ".surf" ? 1 : 0;
  return n;
}

} // namespace

TEST_CASE("cam surface store: a pass is read back, and only for its own key",
          "[cam]") {
  Turning t;
  TempDir dir("designer-surface-store");
  const auto salted = [&](uint64_t salt) {
    return cam::SurfaceStore(dir.path, 1ull << 30, [salt] { return salt; });
  };
  const cam::SurfaceStore store = salted(7);

  // Cut once: the pass is written.
  const cam::SurfaceResult first = cam::SurfacePassStored(
      t.setup, t.op, t.tool(), t.doc->GetBodies(), &store);
  REQUIRE(first.error.empty());
  REQUIRE(first.moves.size() > 500);
  CHECK(Files(dir.path) == 1);

  // Read back whole, moves and words.
  const uint64_t key = cam::SurfaceKey(
      t.setup, t.op, t.tool(), cam::BodiesContentKey(t.doc->GetBodies()));
  cam::SurfaceResult back;
  REQUIRE(store.Load(key, back));
  CHECK(SameMoves(back.moves, first.moves));
  CHECK(back.strategy == first.strategy);

  // What a later call returns IS what the store holds: plant a marker under
  // the pass's key and the pass answers with it rather than cutting.
  cam::SurfaceResult marker;
  marker.strategy = "planted";
  marker.moves.resize(3);
  marker.moves[1].to = {1.0, 2.0, 3.0};
  marker.moves[1].hasA = true;
  marker.moves[1].a = 45.0;
  store.Save(key, marker);
  const cam::SurfaceResult read = cam::SurfacePassStored(
      t.setup, t.op, t.tool(), t.doc->GetBodies(), &store);
  CHECK(read.strategy == "planted");
  CHECK(SameMoves(read.moves, marker.moves));

  // One input changed is another key: cut again, never the marker.
  cam::Operation other = t.op;
  other.stepover = 0.3;
  const cam::SurfaceResult changed = cam::SurfacePassStored(
      t.setup, other, t.tool(), t.doc->GetBodies(), &store);
  CHECK(changed.strategy != "planted");
  CHECK(changed.moves.size() > 500);
  CHECK_FALSE(SameMoves(changed.moves, first.moves));
  CHECK(Files(dir.path) == 2);

  // ...and so is the geometry: a thicker journal, the same op.
  auto thicker = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 16.0);
  CHECK(cam::BodiesContentKey(thicker->GetBodies()) !=
        cam::BodiesContentKey(t.doc->GetBodies()));
  const cam::SurfaceResult moved = cam::SurfacePassStored(
      t.setup, t.op, t.tool(), thicker->GetBodies(), &store);
  CHECK(moved.strategy != "planted");

  // Another build's file is not an answer: a different salt misses.
  const cam::SurfaceStore otherBuild = salted(8);
  cam::SurfaceResult none;
  CHECK_FALSE(otherBuild.Load(key, none));
}

TEST_CASE("cam surface store: the geometry key is the shape, not a version",
          "[cam]") {
  // Two documents built apart, as two runs would open them: every version
  // counter differs in meaning, and the shapes are the same.
  auto a = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  auto b = NeckedShaftDoc(20.0, 60.0, 20.0, 20.0, 15.0);
  CHECK(cam::BodiesContentKey(a->GetBodies()) ==
        cam::BodiesContentKey(b->GetBodies()));
  auto c = NeckedShaftDoc(20.0, 60.0, 21.0, 20.0, 15.0); // the neck moved
  CHECK(cam::BodiesContentKey(a->GetBodies()) !=
        cam::BodiesContentKey(c->GetBodies()));
}

TEST_CASE("cam surface store: a torn or foreign file is a miss, and goes",
          "[cam]") {
  TempDir dir("designer-surface-torn");
  const cam::SurfaceStore store(dir.path, 1ull << 30, [] { return 1ull; });
  cam::SurfaceResult r;
  r.moves.resize(100);
  store.Save(42, r);
  REQUIRE(Files(dir.path) == 1);
  fs::path file;
  for (const auto& e : fs::directory_iterator(dir.path))
    file = e.path();
  // Cut short, as a crash mid-write would -- except that a write never
  // lands half-done (temporary, then rename); this is a disk that lost it.
  fs::resize_file(file, fs::file_size(file) - 10);
  cam::SurfaceResult back;
  CHECK_FALSE(store.Load(42, back));
  CHECK(Files(dir.path) == 0);
}

TEST_CASE("cam surface store: past its bound, the least recently read goes",
          "[cam]") {
  TempDir dir("designer-surface-evict");
  cam::SurfaceResult r;
  r.moves.resize(1000); // ~42 KB a file
  // Room for three.
  const cam::SurfaceStore store(dir.path, 3 * 1000 * 42 + 3 * 200,
                                [] { return 1ull; });
  const auto age = [&](uint64_t key, int secondsAgo) {
    // The order the store judges by, set rather than slept for.
    cam::SurfaceResult probe;
    REQUIRE(store.Load(key, probe));
    for (const auto& e : fs::directory_iterator(dir.path))
      if (fs::last_write_time(e.path()) >
          fs::file_time_type::clock::now() - std::chrono::seconds(1))
        fs::last_write_time(e.path(), fs::file_time_type::clock::now() -
                                          std::chrono::seconds(secondsAgo));
  };
  store.Save(1, r);
  age(1, 300);
  store.Save(2, r);
  age(2, 200);
  store.Save(3, r);
  age(3, 100);
  // Reading 1 makes it the newest...
  cam::SurfaceResult probe;
  REQUIRE(store.Load(1, probe));
  // ...so the fourth pushes out 2, the oldest nobody read since.
  store.Save(4, r);
  CHECK(Files(dir.path) == 3);
  CHECK(store.Load(1, probe));
  CHECK_FALSE(store.Load(2, probe));
  CHECK(store.Load(3, probe));
  CHECK(store.Load(4, probe));
}

#endif // HAVE_CAM
