// Derived (docs/modeling/derived.md): a copy of another part's
// bodies inside this part, regenerated from its snapshot and updated on
// request. The properties pinned here are the ones the page's Tests list:
// the snapshot regenerates with the source ABSENT, an update replaces the
// geometry and clears the stale flag (and undoes), the names on the copy are
// the source's under the derived id and a sketch on a derived face survives
// an update, a cycle is refused, and a reference body is absent from mass
// (which is also the BOM's number) and from every Add/Cut.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "cad/CADKernel.h"
#include "core/AssemblySolve.h"
#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/MassProps.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

namespace {

static const SketchPlane kXY(PlaneType::XY);

// The "servo": a 20 x 10 x 5 box with a boss on top — two bodies, so the
// created_by filter has something to choose between.
std::unique_ptr<Document> MakeSource(double width = 20.0) {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Servo", glm::vec3(0.0f), width, 10.0, 5.0));
  doc->AddFeature(
      std::make_unique<CylinderFeature>("Boss", glm::vec3(5.0f, 5.0f, 5.0f),
                                        glm::vec3(0.0f, 0.0f, 1.0f), 2.0, 3.0));
  REQUIRE(doc->Regenerate());
  REQUIRE(doc->GetBodies().size() == 2);
  return doc;
}

uint64_t BoxId(const Document& source) {
  return source.GetFeatures()[0]->GetId();
}

// Adds a Derived of `source` (slot id 7) to `host` and returns its feature id.
uint64_t Derive(Document& host, const Document& source, bool reference = true,
                std::vector<uint64_t> bodies = {}) {
  auto d = std::make_unique<DerivedFeature>("Servo copy", 7);
  d->sourceName = "Servo";
  d->reference = reference;
  d->bodies = std::move(bodies);
  REQUIRE(derived::TakeSnapshot(*d, source, host).empty());
  host.AddFeature(std::move(d));
  return host.GetFeature(host.FeatureCount() - 1)->GetId();
}

double VolumeOf(const Document& doc, uint64_t createdBy) {
  double v = 0.0;
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == createdBy)
      v += b.shape.Volume();
  return v;
}

const Body* BodyNamed(const Document& doc, const std::string& name) {
  for (const Body& b : doc.GetBodies())
    if (b.name == name)
      return &b;
  return nullptr;
}

fs::path TempPath(const std::string& name) {
  const fs::path p = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  return p;
}

} // namespace

TEST_CASE("a derived part regenerates from its snapshot with the source absent",
          "[derived]") {
  Document host;
  uint64_t did = 0;
  {
    auto source = MakeSource();
    did = Derive(host, *source);
    REQUIRE(host.Regenerate());
    // The source document is destroyed here. Nothing below can reach it.
  }
  REQUIRE(host.GetBodies().size() == 2);
  for (const Body& b : host.GetBodies()) {
    CHECK(b.createdBy == did);
    CHECK(b.reference);
  }
  CHECK_THAT(VolumeOf(host, did),
             WithinRel(1000.0 + 3.14159265 * 4.0 * 3.0, 1e-6));
  CHECK(BodyNamed(host, "Servo") != nullptr);
  CHECK(BodyNamed(host, "Boss") != nullptr);

  // Both file forms carry the snapshot: the single file (base64 assets) and
  // the directory (assets/derived-<hash>.brep). Each reloads and regenerates
  // with no source part anywhere.
  for (const char* file :
       {"derived-roundtrip.json", "derived-roundtrip.dsgn"}) {
    INFO(file);
    const fs::path path = TempPath(file);
    REQUIRE(testio::Save(host, path.string()));
    auto reloaded = testio::Load(path.string());
    REQUIRE(reloaded);
    REQUIRE(reloaded->GetBodies().size() == 2);
    CHECK_THAT(VolumeOf(*reloaded, did), WithinRel(VolumeOf(host, did), 1e-9));
    CHECK(reloaded->GetBodies()[0].reference);
    const auto* d =
        dynamic_cast<const DerivedFeature*>(reloaded->FindFeatureById(did));
    REQUIRE(d != nullptr);
    CHECK(d->sourcePart == 7);
    CHECK(d->sourceName == "Servo");
    CHECK(d->snapshot.size() == 2);
    CHECK(d->sourceChainHash != 0);
    std::error_code ec;
    fs::remove_all(path, ec);
  }
}

TEST_CASE("a snapshot body's asset is content-addressed and reused",
          "[derived]") {
  auto source = MakeSource();
  Document host;
  const uint64_t a = Derive(host, *source);
  const uint64_t b = Derive(host, *source);
  REQUIRE(host.Regenerate());
  const auto* da = dynamic_cast<const DerivedFeature*>(host.FindFeatureById(a));
  const auto* db = dynamic_cast<const DerivedFeature*>(host.FindFeatureById(b));
  REQUIRE(da != nullptr);
  REQUIRE(db != nullptr);
  CHECK(da->snapshot[0].asset == db->snapshot[0].asset);
  CHECK(host.GetAssets().size() == 2); // one blob per distinct body
  CHECK(host.GetBodies().size() == 4);
}

TEST_CASE("update after the source changes replaces the geometry, clears the "
          "stale flag, and undoes",
          "[derived]") {
  auto source = MakeSource();
  std::map<uint64_t, const Document*> parts{{7, source.get()}};
  const derived::PartResolver resolve = [&](uint64_t id) -> const Document* {
    const auto it = parts.find(id);
    return it == parts.end() ? nullptr : it->second;
  };

  Document host;
  CommandStack commands;
  const uint64_t did = Derive(host, *source);
  REQUIRE(host.Regenerate());
  const double before = VolumeOf(host, did);

  auto* feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  REQUIRE(derived::IsStale(*feature, resolve) == std::optional<bool>{false});
  CHECK(!derived::IsStale(*feature, [](uint64_t) -> const Document* {
           return nullptr;
         }).has_value()); // source not loaded: unknown, not stale

  // The source grows. Nothing happens to the copy until asked (the FEM rule).
  auto* box = dynamic_cast<BoxFeature*>(source->GetFeature(0));
  REQUIRE(box);
  box->width = 40.0;
  REQUIRE(source->Regenerate());
  CHECK(derived::IsStale(*feature, resolve) == std::optional<bool>{true});
  REQUIRE(host.Regenerate());
  CHECK_THAT(VolumeOf(host, did), WithinRel(before, 1e-9));

  // Update = a ModifyFeatureCommand with the retaken snapshot; the previous
  // snapshot's assets stay in the store, which is what makes undo a load.
  size_t index = 0;
  REQUIRE(host.FindFeatureById(did, &index) == feature);
  const nlohmann::json was = DocumentIO::FeatureToJson(*feature);
  DerivedFeature retaken = *feature;
  REQUIRE(derived::TakeSnapshot(retaken, *source, host).empty());
  REQUIRE(commands.Execute(std::make_unique<ModifyFeatureCommand>(
                               index, was, DocumentIO::FeatureToJson(retaken)),
                           host));
  CHECK_THAT(VolumeOf(host, did),
             WithinRel(before + 1000.0, 1e-6)); // 20 more mm of 10 x 5
  feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  CHECK(derived::IsStale(*feature, resolve) == std::optional<bool>{false});

  commands.Undo(host);
  CHECK_THAT(VolumeOf(host, did), WithinRel(before, 1e-9));
  feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  CHECK(derived::IsStale(*feature, resolve) == std::optional<bool>{true});
  commands.Redo(host);
  CHECK_THAT(VolumeOf(host, did), WithinRel(before + 1000.0, 1e-6));
}

TEST_CASE("the names on the copy are the source's under the derived id, and a "
          "sketch on a derived face survives an update that keeps the face",
          "[derived][toponaming]") {
  auto source = MakeSource();
  const uint64_t boxId = BoxId(*source);
  Document host;
  const uint64_t did = Derive(host, *source, /*reference=*/true, {boxId});

  // Placed 10 mm along +X: the copy is where placement says, and the face
  // indices in its map are still the source's (a transform moves the
  // Location, not the explorer order).
  auto* feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  feature->position = glm::vec3(10.0f, 0.0f, 0.0f);
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 1);
  const Body& copy = host.GetBodies()[0];
  const std::string prefix =
      std::to_string(did) + "/" + std::to_string(boxId) + "/";
  REQUIRE(!copy.elementMap.names.empty());
  for (const auto& [index, name] : copy.elementMap.names)
    CHECK_THAT(name, Catch::Matchers::StartsWith(prefix));

  // The source box's top face (+Z) under the derived name: find it by
  // geometry on the source, then look the same name up on the copy.
  std::string topName;
  for (const Body& sb : source->GetBodies()) {
    if (sb.createdBy != boxId)
      continue;
    for (const auto& [index, name] : sb.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(sb, index);
      if (info && info->normal.z > 0.99f)
        topName = name;
    }
  }
  REQUIRE(!topName.empty());
  const auto topIndex = copy.elementMap.Resolve(prefix + topName);
  REQUIRE(topIndex.has_value());
  const auto top = Measure::DescribeFaceAt(copy, *topIndex);
  REQUIRE(top);
  CHECK(top->normal.z > 0.99f);
  CHECK_THAT(top->centroid.x, WithinAbs(20.0, 1e-4)); // 10 (box) + 10 placed
  CHECK_THAT(top->centroid.z, WithinAbs(5.0, 1e-4));

  // A sketch on that face, extruded in Add mode: the derived body is a
  // ghost, so the prism becomes this part's FIRST body rather than fusing
  // into the copy (which would make the servo part of the jaw).
  auto sk = std::make_unique<SketchFeature>("On servo", kXY);
  sk->faceRef = ElementRef{did, prefix + topName};
  sk->AddRectangle({-2, -2}, {2, 2});
  host.AddFeature(std::move(sk));
  const uint64_t skId = host.GetFeature(1)->GetId();
  host.AddFeature(
      std::make_unique<ExtrudeFeature>("Foot", skId, 3.0, ExtrudeMode::Add));
  const uint64_t footId = host.GetFeature(2)->GetId();
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 2);
  CHECK_THAT(VolumeOf(host, did), WithinRel(1000.0, 1e-9)); // untouched
  CHECK_THAT(VolumeOf(host, footId), WithinRel(48.0, 1e-6));
  CHECK(!BodyNamed(host, "Foot")->reference);
  glm::vec3 mn, mx;
  BodyNamed(host, "Foot")->shape.GetBoundingBox(mn, mx);
  CHECK_THAT(mn.z, WithinAbs(5.0, 1e-4)); // sits ON the servo's top

  // The source gets taller; the top face keeps its name, so the sketch and
  // the foot follow it after an update.
  auto* box = dynamic_cast<BoxFeature*>(source->GetFeature(0));
  box->depth = 8.0;
  REQUIRE(source->Regenerate());
  feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(derived::TakeSnapshot(*feature, *source, host).empty());
  REQUIRE(host.Regenerate());
  for (const auto& f : host.GetFeatures())
    CHECK(f->GetError().empty());
  BodyNamed(host, "Foot")->shape.GetBoundingBox(mn, mx);
  CHECK_THAT(mn.z, WithinAbs(8.0, 1e-4));
}

TEST_CASE("a cycle over derived edges is refused, naming the chain",
          "[derived]") {
  // Part 1 derives from 2, 2 from 3. Deriving 3 from 1 would close the loop.
  Document p1, p2, p3;
  p3.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0.0f), 1.0, 1.0, 1.0));
  REQUIRE(p3.Regenerate());
  {
    auto d = std::make_unique<DerivedFeature>("from 3", 3);
    d->reference = false; // a ghost is not derivable further (see below)
    REQUIRE(derived::TakeSnapshot(*d, p3, p2).empty());
    p2.AddFeature(std::move(d));
  }
  {
    auto d = std::make_unique<DerivedFeature>("from 2", 2);
    REQUIRE(derived::TakeSnapshot(*d, p2, p1).empty());
    p1.AddFeature(std::move(d));
  }
  std::map<uint64_t, const Document*> parts{{1, &p1}, {2, &p2}, {3, &p3}};
  const derived::PartResolver resolve = [&](uint64_t id) -> const Document* {
    const auto it = parts.find(id);
    return it == parts.end() ? nullptr : it->second;
  };
  std::string why;
  CHECK(derived::WouldCycle(3, 1, resolve, &why));
  CHECK_THAT(why, ContainsSubstring("1 -> 2 -> 3"));
  CHECK(derived::WouldCycle(2, 2, resolve, &why));
  CHECK_THAT(why, ContainsSubstring("itself"));
  CHECK(!derived::WouldCycle(3, 4, resolve, &why)); // 4 is not loaded
  CHECK(!derived::WouldCycle(1, 3, resolve, &why)); // deeper, same direction
}

TEST_CASE("a bodies filter naming a body the source lacks is refused, "
          "listing what it has",
          "[derived]") {
  auto source = MakeSource();
  Document host;
  auto d = std::make_unique<DerivedFeature>("copy", 7);
  d->bodies = {999};
  const std::string why = derived::TakeSnapshot(*d, *source, host);
  CHECK_THAT(why, ContainsSubstring("no body created by feature 999"));
  CHECK_THAT(why, ContainsSubstring(std::to_string(BoxId(*source))));
  Document empty;
  d->bodies.clear();
  CHECK_THAT(derived::TakeSnapshot(*d, empty, host),
             ContainsSubstring("no bodies"));
}

TEST_CASE("a reference body weighs nothing, is not cut, is not fused into and "
          "cannot be a boolean operand; reference:false makes it matter",
          "[derived]") {
  auto source = MakeSource();
  Document host;
  const uint64_t did = Derive(host, *source);
  // This part's own body, overlapping the copy.
  host.AddFeature(std::make_unique<BoxFeature>(
      "Jaw", glm::vec3(0.0f, 0.0f, -4.0f), 20.0, 10.0, 4.0));
  const uint64_t jawId = host.GetFeature(1)->GetId();
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 3);

  const auto mass = Measure::MassOfBodies(host.GetBodies());
  REQUIRE(mass);
  CHECK(mass->bodies.size() == 1);
  CHECK_THAT(mass->volume, WithinRel(800.0, 1e-9)); // the jaw alone

  // A cut through everything: the jaw loses material, the ghost does not.
  auto sk = std::make_unique<SketchFeature>("Slot", kXY);
  sk->AddRectangle({8, 4}, {12, 6});
  host.AddFeature(std::move(sk));
  const uint64_t skId = host.GetFeature(2)->GetId();
  auto cut =
      std::make_unique<ExtrudeFeature>("Cut", skId, 20.0, ExtrudeMode::Cut);
  cut->reversed = true; // down through the jaw
  host.AddFeature(std::move(cut));
  auto through =
      std::make_unique<ExtrudeFeature>("Cut up", skId, 20.0, ExtrudeMode::Cut);
  host.AddFeature(std::move(through)); // up through the ghost: no-op + warning
  REQUIRE(host.Regenerate());
  CHECK_THAT(VolumeOf(host, jawId), WithinRel(800.0 - 4.0 * 2.0 * 4.0, 1e-6));
  CHECK_THAT(VolumeOf(host, did),
             WithinRel(1000.0 + 3.14159265 * 4.0 * 3.0, 1e-6));
  CHECK_THAT(host.GetFeature(4)->GetWarning(),
             ContainsSubstring("removed no material"));

  // A boolean naming the ghost is refused in words that say the fix.
  host.AddFeature(
      std::make_unique<BooleanFeature>("Fuse", jawId, did, BooleanOp::Union));
  CHECK(!host.Regenerate());
  CHECK_THAT(host.GetFeature(5)->GetError(),
             ContainsSubstring("reference: false"));

  // reference:false — the copy is this part's matter now: the fuse works
  // and the mass counts it.
  auto* feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  feature->reference = false;
  REQUIRE(host.Regenerate());
  for (const Body& b : host.GetBodies())
    CHECK(!b.reference);
  const auto all = Measure::MassOfBodies(host.GetBodies());
  REQUIRE(all);
  CHECK(all->volume > 1700.0);
}

TEST_CASE("a part's own ghosts are not derived further", "[derived]") {
  auto source = MakeSource();
  Document mid;
  Derive(mid, *source); // a reference copy
  REQUIRE(mid.Regenerate());
  Document host;
  auto d = std::make_unique<DerivedFeature>("copy of a copy", 8);
  CHECK_THAT(derived::TakeSnapshot(*d, mid, host),
             ContainsSubstring("only reference bodies"));
}

TEST_CASE("a derived copy picks source bodies by name", "[derived]") {
  // One Import feature creates every body of a STEP, so a created_by
  // filter cannot pick one of them: the name is the only handle (v85).
  auto source = MakeSource();
  Document host;
  auto d = std::make_unique<DerivedFeature>("Boss only", 7);
  d->sourceName = "Servo";
  d->bodyNames = {"Boss"};
  REQUIRE(derived::TakeSnapshot(*d, *source, host).empty());
  host.AddFeature(std::move(d));
  const uint64_t did = host.GetFeatures()[0]->GetId();
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 1);
  CHECK_THAT(VolumeOf(host, did), WithinRel(3.14159265 * 4.0 * 3.0, 1e-4));

  // A name the source does not have is refused, listing what it has.
  DerivedFeature bad("x", 7);
  bad.bodyNames = {"Flange"};
  const std::string why = derived::TakeSnapshot(bad, *source, host);
  CHECK_THAT(why, ContainsSubstring("no body named 'Flange'"));
  CHECK_THAT(why, ContainsSubstring("Boss"));

  // Both filters narrow TOGETHER, and selecting nothing says so.
  DerivedFeature both("y", 7);
  both.bodies = {BoxId(*source)};
  both.bodyNames = {"Boss"};
  CHECK_THAT(derived::TakeSnapshot(both, *source, host),
             ContainsSubstring("narrow TOGETHER"));
}

// How tests/golden/v133-derived-configuration.json was made: the source
// below with a configuration that suppresses its boss, derived IN that
// configuration while the source shows its authored model. Set
// DESIGNER_WRITE_GOLDEN=<path> to write it again.
TEST_CASE("the v133 derived-configuration golden builds from this recipe",
          "[derived][configuration]") {
  auto source = MakeSource();
  Configuration& bare = source->AddConfiguration("Bare");
  bare.suppressedFeatures = {source->GetFeatures()[1]->GetId()};
  const uint64_t bareId = bare.id;
  // What ui::ConfiguredPartDocument hands TakeSnapshot: a copy regenerated
  // under the configuration, the source itself untouched.
  Document configured(*source);
  REQUIRE(configured.SetActiveConfiguration(bareId));
  REQUIRE(configured.GetBodies().size() == 1);
  REQUIRE(source->GetBodies().size() == 2);

  Document host;
  auto d = std::make_unique<DerivedFeature>("Bare servo", 7);
  d->sourceName = "Servo";
  d->sourceConfiguration = bareId;
  d->reference = false;
  REQUIRE(derived::TakeSnapshot(*d, configured, host).empty());
  host.AddFeature(std::move(d));
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 1);
  CHECK_THAT(host.GetBodies()[0].shape.Volume(),
             WithinRel(20.0 * 10.0 * 5.0, 1e-6));
  // Stale is judged in the copy's configuration: the configured source has
  // not moved, while its authored model's tip is a different hash.
  const auto* taken = dynamic_cast<const DerivedFeature*>(host.GetFeature(0));
  REQUIRE(taken);
  CHECK(derived::IsStale(*taken, [&](uint64_t) { return &configured; }) ==
        std::optional<bool>{false});
  CHECK(derived::IsStale(*taken, [&](uint64_t) { return source.get(); }) ==
        std::optional<bool>{true});

  const char* out = std::getenv("DESIGNER_WRITE_GOLDEN");
  const fs::path path = out ? fs::path(out) : TempPath("derived-v133.json");
  REQUIRE(testio::Save(host, path.string()));
  auto reloaded = testio::Load(path.string());
  REQUIRE(reloaded);
  CHECK(reloaded->GetBodies().size() == 1);
  const auto* back =
      dynamic_cast<const DerivedFeature*>(reloaded->GetFeature(0));
  REQUIRE(back);
  CHECK(back->sourceConfiguration == bareId);
  if (!out) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}

// How tests/golden/v85-derived-by-name.json was made: the source below,
// with ONE of its two bodies derived BY NAME into a host that draws on the
// copy. Set DESIGNER_WRITE_GOLDEN=<path> to write it again.
TEST_CASE("the v85 derived-by-name golden builds from this recipe",
          "[derived]") {
  auto source = MakeSource();
  Document host;
  auto d = std::make_unique<DerivedFeature>("Boss copy", 7);
  d->sourceName = "Servo";
  d->reference = true;
  d->bodyNames = {"Boss"};
  REQUIRE(derived::TakeSnapshot(*d, *source, host).empty());
  host.AddFeature(std::move(d));
  host.AddFeature(std::make_unique<BoxFeature>("Plate", glm::vec3(0, 0, -4.0f),
                                               20.0, 10.0, 4.0));
  REQUIRE(host.Regenerate());
  REQUIRE(host.GetBodies().size() == 2);
  const char* out = std::getenv("DESIGNER_WRITE_GOLDEN");
  const fs::path path = out ? fs::path(out) : TempPath("derived-v85.json");
  REQUIRE(testio::Save(host, path.string()));
  auto reloaded = testio::Load(path.string());
  REQUIRE(reloaded);
  CHECK(reloaded->GetBodies().size() == 2);
  if (!out) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}

// How tests/golden/v84-derived-servo.json was made: the source part below,
// derived (placed, one body filtered out) into a host with a foot drawn on
// the copy's top face. Set DESIGNER_WRITE_GOLDEN=<path> to write it again;
// unset, this only checks that the document builds and round-trips.
TEST_CASE("the v84 derived golden builds from this recipe", "[derived]") {
  auto source = MakeSource();
  const uint64_t boxId = BoxId(*source);
  Document host;
  const uint64_t did = Derive(host, *source, /*reference=*/true, {boxId});
  auto* feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  feature->position = glm::vec3(10.0f, 0.0f, 0.0f);
  const std::string prefix =
      std::to_string(did) + "/" + std::to_string(boxId) + "/";
  auto sk = std::make_unique<SketchFeature>("On servo", kXY);
  std::string topName;
  for (const Body& sb : source->GetBodies())
    if (sb.createdBy == boxId)
      for (const auto& [index, name] : sb.elementMap.names)
        if (const auto info = Measure::DescribeFaceAt(sb, index);
            info && info->normal.z > 0.99f)
          topName = name;
  REQUIRE(!topName.empty());
  sk->faceRef = ElementRef{did, prefix + topName};
  sk->AddRectangle({-2, -2}, {2, 2});
  host.AddFeature(std::move(sk));
  host.AddFeature(std::make_unique<ExtrudeFeature>(
      "Foot", host.GetFeature(1)->GetId(), 3.0, ExtrudeMode::Add));
  REQUIRE(host.Regenerate());
  for (const auto& f : host.GetFeatures()) {
    INFO(f->GetName() << ": " << f->GetError());
    CHECK(f->GetError().empty());
  }
  REQUIRE(host.GetBodies().size() == 2);
  const char* out = std::getenv("DESIGNER_WRITE_GOLDEN");
  const fs::path path = out ? fs::path(out) : TempPath("derived-golden.json");
  REQUIRE(testio::Save(host, path.string()));
  auto reloaded = testio::Load(path.string());
  REQUIRE(reloaded);
  CHECK(reloaded->GetBodies().size() == 2);
  if (!out) {
    std::error_code ec;
    fs::remove(path, ec);
  }
}

// ---- D3: the proofs that a derived body is geometry like any other, with
// no code of its own: external geometry, hole centres, a mate connector.

namespace {
// The name of the source box's +Z face on the copy, and the copy's index.
std::string DerivedTopName(const Document& source, uint64_t boxId,
                           uint64_t did) {
  for (const Body& sb : source.GetBodies())
    if (sb.createdBy == boxId)
      for (const auto& [index, name] : sb.elementMap.names)
        if (const auto info = Measure::DescribeFaceAt(sb, index);
            info && info->normal.z > 0.99f)
          return std::to_string(did) + "/" + std::to_string(boxId) + "/" + name;
  return {};
}
} // namespace

TEST_CASE("external geometry projects a derived face and a derived edge",
          "[derived][external]") {
  auto source = MakeSource();
  const uint64_t boxId = BoxId(*source);
  Document host;
  const uint64_t did = Derive(host, *source, /*reference=*/true, {boxId});
  auto* feature = dynamic_cast<DerivedFeature*>(host.FindFeatureById(did));
  REQUIRE(feature);
  feature->position = glm::vec3(10.0f, 0.0f, 0.0f);
  REQUIRE(host.Regenerate());
  const std::string top = DerivedTopName(*source, boxId, did);
  REQUIRE(!top.empty());
  // An edge of the top face (midpoint at z = 5), so its projection is a
  // line and not the point a vertical edge collapses to.
  const Body& copy = host.GetBodies()[0];
  std::string anEdge;
  for (const auto& [index, name] : copy.elementMap.edgeNames)
    if (const auto info = Measure::DescribeEdgeAt(copy, index);
        info && std::abs(info->midpoint.z - 5.0f) < 1e-3f)
      anEdge = name;
  REQUIRE(!anEdge.empty());

  auto sk = std::make_unique<SketchFeature>("Ref", kXY);
  sk->AddExternalGeometry(ElementRef{did, top}, sketch::ExternalKind::Face);
  sk->AddExternalGeometry(ElementRef{did, anEdge}, sketch::ExternalKind::Edge);
  host.AddFeature(std::move(sk));
  auto* s2 = dynamic_cast<SketchFeature*>(host.GetFeature(1));
  REQUIRE(s2 != nullptr);
  CHECK(s2->GetError().empty());
  REQUIRE(s2->external.size() == 2);
  REQUIRE(s2->external[0].genIds.size() == 4); // the top face's 4 edges
  float minX = 1e9f, maxX = -1e9f;
  for (uint32_t id : s2->external[0].genIds) {
    const size_t idx = s2->IndexOfEntity(id);
    REQUIRE(idx != SIZE_MAX);
    auto* l = std::get_if<sketch::Line>(&s2->entities[idx]);
    REQUIRE(l != nullptr);
    for (glm::vec2 p : {l->a, l->b}) {
      minX = std::min(minX, p.x);
      maxX = std::max(maxX, p.x);
    }
  }
  CHECK_THAT(minX, WithinAbs(10.0f, 1e-3f)); // placed 10 along +X
  CHECK_THAT(maxX, WithinAbs(30.0f, 1e-3f));
  CHECK(s2->external[1].genIds.size() == 1);
}

TEST_CASE("hole centres read off a derived boss drill the part's own body",
          "[derived][hole]") {
  auto source = MakeSource();
  Document host;
  const uint64_t did = Derive(host, *source);
  host.AddFeature(std::make_unique<BoxFeature>("Jaw", glm::vec3(0, 0, -4.0f),
                                               20.0, 10.0, 4.0));
  const uint64_t jawId = host.GetFeature(1)->GetId();
  REQUIRE(host.Regenerate());
  // Where the boss is: its +Z cap on the derived copy.
  glm::vec2 centre(0.0f);
  bool found = false;
  for (const Body& b : host.GetBodies()) {
    if (b.name != "Boss")
      continue;
    for (const auto& [index, name] : b.elementMap.names)
      if (const auto info = Measure::DescribeFaceAt(b, index);
          info && info->normal.z > 0.99f) {
        centre = glm::vec2(info->centroid);
        found = true;
      }
  }
  REQUIRE(found);
  CHECK_THAT(centre.x, WithinAbs(5.0f, 1e-4f));
  CHECK_THAT(centre.y, WithinAbs(5.0f, 1e-4f));
  // A hole from the jaw's top (z = 0) down, at that centre.
  SketchPlane plane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centres", plane);
  sk->AddEntity(sketch::Point{centre});
  host.AddFeature(std::move(sk));
  const uint64_t skId = host.GetFeature(2)->GetId();
  host.AddFeature(
      std::make_unique<HoleFeature>("Drill", skId, HoleKind::Simple, 2.0, 4.0));
  REQUIRE(host.Regenerate());
  CHECK(host.GetFeature(3)->GetError().empty());
  CHECK_THAT(VolumeOf(host, jawId),
             WithinRel(800.0 - 3.14159265 * 1.0 * 4.0, 1e-4));
  CHECK_THAT(VolumeOf(host, did), // the ghost is not drilled
             WithinRel(1000.0 + 3.14159265 * 4.0 * 3.0, 1e-6));
}

TEST_CASE("a hole placed on a derived hole edge, no coordinates copied",
          "[derived][hole][external]") {
  // The boss's top cap, projected into the hole sketch as external geometry,
  // is a construction circle; the Hole takes its centre. No number from the
  // ghost crosses into this part by hand.
  auto source = MakeSource();
  Document host;
  const uint64_t did = Derive(host, *source);
  host.AddFeature(std::make_unique<BoxFeature>("Jaw", glm::vec3(0, 0, -4.0f),
                                               20.0, 10.0, 4.0));
  const uint64_t jawId = host.GetFeature(1)->GetId();
  REQUIRE(host.Regenerate());
  std::string cap;
  for (const Body& b : host.GetBodies()) {
    if (b.name != "Boss")
      continue;
    for (const auto& [index, name] : b.elementMap.names)
      if (const auto info = Measure::DescribeFaceAt(b, index);
          info && info->normal.z > 0.99f)
        cap = name;
  }
  REQUIRE(!cap.empty());
  SketchPlane plane(glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), glm::vec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Centres", plane);
  sk->AddExternalGeometry(ElementRef{did, cap}, sketch::ExternalKind::Face);
  host.AddFeature(std::move(sk));
  const uint64_t skId = host.GetFeature(2)->GetId();
  host.AddFeature(
      std::make_unique<HoleFeature>("Drill", skId, HoleKind::Simple, 2.0, 4.0));
  const bool ok = host.Regenerate();
  INFO("sketch: " << host.GetFeature(2)->GetError()
                  << " hole: " << host.GetFeature(3)->GetError());
  REQUIRE(ok);
  const auto* s2 = dynamic_cast<const SketchFeature*>(host.GetFeature(2));
  REQUIRE(s2 != nullptr);
  REQUIRE(s2->external.size() == 1);
  REQUIRE(s2->external[0].genIds.size() == 1); // the cap's one circle
  CHECK(host.GetFeature(3)->GetError().empty());
  CHECK_THAT(VolumeOf(host, jawId),
             WithinRel(800.0 - 3.14159265 * 1.0 * 4.0, 1e-4));
  CHECK_THAT(VolumeOf(host, did), // the ghost is not drilled
             WithinRel(1000.0 + 3.14159265 * 4.0 * 3.0, 1e-6));
}

TEST_CASE("a moved import file is not a changed source; a changed file is",
          "[derived][import]") {
  // Saving a project adopts staged import files into the project directory
  // and rewrites each Import's path. That is not a change to the part, so
  // it must not read as one (every copy of an imported part went stale
  // after the first save). A different file at the path is.
  const fs::path dir = TempPath("derived-import-hash");
  fs::create_directories(dir);
  Document box;
  box.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0.0f), 10.0, 10.0, 10.0));
  REQUIRE(box.Regenerate());
  const fs::path a = dir / "a.brep", b = dir / "b.brep", c = dir / "c.brep";
  REQUIRE(CADKernel::Instance().SaveBREP(a.string(), box.GetBodies()[0].shape));
  fs::copy_file(a, b);

  Document source;
  source.AddFeature(std::make_unique<ImportFeature>("servo", a.string()));
  REQUIRE(source.Regenerate());
  REQUIRE(source.GetBodies().size() == 1);
  Document host;
  const uint64_t did = Derive(host, source);
  REQUIRE(host.Regenerate());
  const auto resolver = [&](uint64_t) -> const Document* { return &source; };
  const auto* d = dynamic_cast<const DerivedFeature*>(host.GetFeature(0));
  REQUIRE(d != nullptr);
  CHECK(derived::IsStale(*d, resolver) == std::optional<bool>{false});

  // The same bytes at another path: the adoption case.
  auto* imp = dynamic_cast<ImportFeature*>(source.GetFeature(0));
  REQUIRE(imp != nullptr);
  imp->path = b.string();
  REQUIRE(source.Regenerate());
  CHECK(derived::IsStale(*d, resolver) == std::optional<bool>{false});

  // Different bytes: a bigger box at the path is a changed source.
  Document big;
  big.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0.0f), 20.0, 10.0, 10.0));
  REQUIRE(big.Regenerate());
  REQUIRE(CADKernel::Instance().SaveBREP(c.string(), big.GetBodies()[0].shape));
  imp->path = c.string();
  REQUIRE(source.Regenerate());
  CHECK(derived::IsStale(*d, resolver) == std::optional<bool>{true});
  (void)did;
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST_CASE("a mate connector on a derived face resolves in the assembly",
          "[derived][assembly]") {
  auto source = MakeSource();
  const uint64_t boxId = BoxId(*source);
  auto host = std::make_unique<Document>();
  const uint64_t did = Derive(*host, *source, /*reference=*/true, {boxId});
  REQUIRE(host->Regenerate());
  const std::string top = DerivedTopName(*source, boxId, did);
  REQUIRE(!top.empty());

  AssemblyDocument asmDoc;
  Occurrence jaw;
  jaw.name = "jaw";
  jaw.partId = 2;
  jaw.position = glm::vec3(0.0f, 0.0f, 100.0f);
  const uint64_t jawOcc = asmDoc.AddOccurrence(jaw);
  const assembly::PartResolver partById = [&](uint64_t id,
                                              uint64_t) -> const Document* {
    return id == 2 ? host.get() : nullptr;
  };
  const assembly::AssemblyResolver asmById =
      [](uint64_t) -> const AssemblyDocument* { return nullptr; };
  Connector c;
  c.occurrenceId = jawOcc;
  c.anchor.face = ElementRef{did, top};
  std::string why;
  const auto frame =
      assembly::ConnectorLocalFrame(c, asmDoc, partById, &why, asmById);
  INFO(why);
  REQUIRE(frame.has_value());
  // Local to the occurrence: the servo copy's top, at z = 5, facing +Z.
  CHECK_THAT((*frame)[3].z, WithinAbs(5.0f, 1e-3f));
  CHECK_THAT((*frame)[3].x, WithinAbs(10.0f, 1e-3f));
  CHECK_THAT(std::abs((*frame)[2].z), WithinAbs(1.0f, 1e-4f));
}

// A material given to the COPY reaches its bodies. Material is out of the
// chain hash, so the edit is a cache hit and Execute -- where a Derived
// stamps its own -- never ran: a walnut chess piece derived from the piece
// part kept its source's empty material and weighed nothing.
TEST_CASE("a material given to a derived copy reaches its bodies",
          "[derived]") {
  auto source = MakeSource();
  source->GetFeatures()[0]->SetMaterial("steel");
  REQUIRE(source->Regenerate());
  Document host;
  const uint64_t did = Derive(host, *source, /*reference=*/false);
  REQUIRE(host.Regenerate());
  const auto materialOf = [&](const std::string& name) {
    const Body* b = BodyNamed(host, name);
    REQUIRE(b != nullptr);
    return b->material;
  };
  CHECK(materialOf("Servo") == "steel"); // the source's, inherited
  Feature* copy = host.FindFeatureById(did);
  REQUIRE(copy != nullptr);
  copy->SetMaterial("wood_walnut");
  REQUIRE(host.Regenerate());
  CHECK(materialOf("Servo") == "wood_walnut");
  CHECK(materialOf("Boss") == "wood_walnut");
  copy->SetMaterial("");
  REQUIRE(host.Regenerate());
  CHECK(materialOf("Servo") == "steel"); // back to the source's
}
