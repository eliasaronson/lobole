#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <limits>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

namespace {

// Arbitrary pixel bytes — core never decodes them, so any blob round-trips.
const std::vector<unsigned char> kBytes = {0x89, 'P', 'N', 'G', 1, 2,
                                           3,    0,   255, 128, 64};

sketch::ReferenceImage MakeImage() {
  sketch::ReferenceImage img;
  img.asset = "underlay.png";
  img.origin = glm::vec2(1.0f, 2.0f);
  img.rotation = 0.0;
  img.unitsPerPixel = 0.25;
  img.opacity = 0.7;
  img.widthPx = 100;
  img.heightPx = 40;
  return img;
}

// A sketch (rectangle) + extrude so the doc has a body, plus one underlay whose
// pixels live in the document asset store.
std::unique_ptr<Document> MakeDocWithImage(uint32_t* imageIdOut) {
  auto doc = std::make_unique<Document>();
  auto sketch =
      std::make_unique<SketchFeature>("Base", SketchPlane(PlaneType::XY));
  sketch->AddRectangle(glm::vec2(0, 0), glm::vec2(4, 2));
  const uint32_t imgId = sketch->AddImage(MakeImage());
  if (imageIdOut)
    *imageIdOut = imgId;
  doc->AddFeature(std::move(sketch));
  doc->AddFeature(
      std::make_unique<ExtrudeFeature>("Pad", 1, 3.0, ExtrudeMode::NewBody));
  doc->SetAsset("underlay.png", kBytes);
  return doc;
}

SketchFeature* FirstSketch(Document& doc) {
  for (size_t i = 0; i < doc.FeatureCount(); i++)
    if (auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(i)))
      return sk;
  return nullptr;
}

} // namespace

TEST_CASE("Reference image add/index/delete", "[reference-image]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t a = sk.AddImage(MakeImage());
  const uint32_t b = sk.AddImage(MakeImage());
  CHECK(a == 1);
  CHECK(b == 2);
  CHECK(sk.images.size() == 2);
  CHECK(sk.IndexOfImage(a) == 0);
  CHECK(sk.IndexOfImage(b) == 1);
  CHECK(sk.IndexOfImage(999) == std::numeric_limits<size_t>::max());

  CHECK(sk.DeleteImage(a));
  CHECK(sk.images.size() == 1);
  CHECK(sk.images[0].id == b);
  CHECK_FALSE(sk.DeleteImage(a)); // already gone
  // Ids are never reused: the next image gets 3, not the freed 1.
  CHECK(sk.AddImage(MakeImage()) == 3);
}

TEST_CASE("Reference image empty asset gets a default name",
          "[reference-image]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  sketch::ReferenceImage img; // asset left empty
  const uint32_t id = sk.AddImage(img);
  CHECK_FALSE(sk.images[sk.IndexOfImage(id)].asset.empty());
}

TEST_CASE("Reference image calibrate: scale only", "[reference-image]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  sketch::ReferenceImage img = MakeImage();
  img.origin = glm::vec2(0, 0);
  img.rotation = 0.0;
  img.unitsPerPixel = 1.0;
  const uint32_t id = sk.AddImage(img);

  // Two points 40 plane-units apart today; declare them to be 10 units.
  const glm::vec2 p1(10, 5), p2(50, 5); // |p2-p1| = 40
  REQUIRE(sk.CalibrateImage(id, p1, p2, 10.0, /*alignAxis=*/-1));
  const auto& out = sk.images[sk.IndexOfImage(id)];

  CHECK_THAT(out.unitsPerPixel, WithinRel(1.0 * (10.0 / 40.0), 1e-9)); // 0.25
  CHECK_THAT(out.rotation, WithinAbs(0.0, 1e-12)); // unchanged
  // p1 is held fixed: its pixel maps back to (10,5). Pixel of p1 under old
  // placement was (10,5); under new scale 0.25 with new origin it must still
  // land at (10,5). Equivalently the image-space distance between the picks is
  // now exactly the requested 10 units.
  const double c = std::cos(out.rotation), s = std::sin(out.rotation);
  auto pixToPlane = [&](glm::vec2 px) {
    return glm::dvec2(out.origin.x + out.unitsPerPixel * (c * px.x - s * px.y),
                      out.origin.y + out.unitsPerPixel * (s * px.x + c * px.y));
  };
  // Old pixel coords of the two picks (old placement: origin 0, scale 1, rot
  // 0).
  const glm::vec2 pix1(10, 5), pix2(50, 5);
  const glm::dvec2 q1 = pixToPlane(pix1), q2 = pixToPlane(pix2);
  CHECK_THAT(q1.x, WithinAbs(10.0, 1e-6)); // p1 fixed
  CHECK_THAT(q1.y, WithinAbs(5.0, 1e-6));
  CHECK_THAT(std::hypot(q2.x - q1.x, q2.y - q1.y), WithinAbs(10.0, 1e-6));
}

TEST_CASE("Reference image calibrate: align to horizontal",
          "[reference-image]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  sketch::ReferenceImage img = MakeImage();
  img.origin = glm::vec2(0, 0);
  img.rotation = 0.0;
  img.unitsPerPixel = 1.0;
  const uint32_t id = sk.AddImage(img);

  // A diagonal pick pair (45 degrees); ask for horizontal alignment.
  const glm::vec2 p1(0, 0), p2(3, 3);
  REQUIRE(sk.CalibrateImage(id, p1, p2, 10.0, /*alignAxis=*/0));
  const auto& out = sk.images[sk.IndexOfImage(id)];
  // Rotation rotates the -45 deg pick direction back onto +U.
  CHECK_THAT(out.rotation, WithinAbs(-M_PI / 4.0, 1e-9));
}

TEST_CASE("Reference image calibrate: degenerate inputs are no-ops",
          "[reference-image]") {
  SketchFeature sk("S", SketchPlane(PlaneType::XY));
  const uint32_t id = sk.AddImage(MakeImage());
  CHECK_FALSE(sk.CalibrateImage(id, glm::vec2(1, 1), glm::vec2(1, 1), 5.0));
  CHECK_FALSE(sk.CalibrateImage(id, glm::vec2(0, 0), glm::vec2(1, 0), 0.0));
  CHECK_FALSE(sk.CalibrateImage(999, glm::vec2(0, 0), glm::vec2(1, 0), 5.0));
}

TEST_CASE("Reference image single-file round-trip preserves bytes and fields",
          "[reference-image][serialization]") {
  uint32_t imgId = 0;
  auto doc = MakeDocWithImage(&imgId);
  const fs::path path = fs::temp_directory_path() / "refimg_roundtrip.json";
  std::string error;
  REQUIRE(testio::Save(*doc, path.string()));

  auto loaded = testio::Load(path.string());
  REQUIRE((loaded != nullptr && error.empty()));
  SketchFeature* sk = FirstSketch(*loaded);
  REQUIRE(sk != nullptr);
  REQUIRE(sk->images.size() == 1);
  const auto& img = sk->images[0];
  CHECK(img.id == imgId);
  CHECK(img.asset == MakeImage().asset);
  CHECK_THAT(img.origin.x, WithinAbs(1.0, 1e-6));
  CHECK_THAT(img.origin.y, WithinAbs(2.0, 1e-6));
  CHECK_THAT(img.unitsPerPixel, WithinRel(0.25, 1e-9));
  CHECK_THAT(img.opacity, WithinRel(0.7, 1e-9));
  CHECK(img.widthPx == 100);
  CHECK(img.heightPx == 40);
  const auto* bytes = loaded->GetAsset("underlay.png");
  REQUIRE(bytes != nullptr);
  CHECK(*bytes == kBytes); // base64 round-trips byte-exact
  fs::remove(path);
}

TEST_CASE("Reference image directory-format round-trip stores an assets file",
          "[reference-image][serialization][dirformat]") {
  auto doc = MakeDocWithImage(nullptr);
  const fs::path proj = fs::temp_directory_path() / "refimg_roundtrip.dsgn";
  fs::remove_all(proj);
  std::string error;
  REQUIRE(testio::Save(*doc, proj.string()));

  // The pixels live in assets/, not inline in the feature JSON.
  CHECK(fs::exists(proj / "assets" / MakeImage().asset));

  auto loaded = testio::Load(proj.string());
  REQUIRE((loaded != nullptr && error.empty()));
  SketchFeature* sk = FirstSketch(*loaded);
  REQUIRE(sk != nullptr);
  REQUIRE(sk->images.size() == 1);
  const auto* bytes = loaded->GetAsset("underlay.png");
  REQUIRE(bytes != nullptr);
  CHECK(*bytes == kBytes);
  fs::remove_all(proj);
}

TEST_CASE("Reference image bytes survive a feature rebuild (undo/redo)",
          "[reference-image]") {
  // ModifyFeatureCommand undo/redo replaces the feature via FeatureFromJson,
  // which never carries pixel bytes. Because the bytes live on the Document,
  // they must survive that rebuild and the image metadata must reload.
  auto doc = MakeDocWithImage(nullptr);
  SketchFeature* sk = FirstSketch(*doc);
  REQUIRE(sk != nullptr);
  const size_t idx = [&] {
    for (size_t i = 0; i < doc->FeatureCount(); i++)
      if (doc->GetFeature(i) == sk)
        return i;
    return size_t{0};
  }();

  nlohmann::json snapshot = DocumentIO::FeatureToJson(*sk);
  REQUIRE(doc->ReplaceFeature(idx, DocumentIO::FeatureFromJson(snapshot)));

  SketchFeature* rebuilt = FirstSketch(*doc);
  REQUIRE(rebuilt != nullptr);
  REQUIRE(rebuilt->images.size() == 1);
  CHECK(rebuilt->images[0].asset == "underlay.png");
  const auto* bytes = doc->GetAsset("underlay.png"); // untouched by the rebuild
  REQUIRE(bytes != nullptr);
  CHECK(*bytes == kBytes);
}

TEST_CASE("Reference image edits do not invalidate the regen cache",
          "[reference-image][regen]") {
  auto doc = MakeDocWithImage(nullptr);
  doc->Regenerate(); // populate the cache
  REQUIRE(doc->FeatureCount() == 2);

  // A cosmetic underlay edit must reuse every cached feature.
  SketchFeature* sk = FirstSketch(*doc);
  REQUIRE(sk != nullptr);
  sk->images[0].opacity = 0.1;
  sk->images[0].origin = glm::vec2(9, 9);
  doc->Regenerate();
  CHECK(doc->LastRegenStats().cached == 2); // nothing downstream rebuilt

  // Sanity: a real geometry edit DOES diverge, so the check above is
  // meaningful.
  sk->AddRectangle(glm::vec2(10, 10), glm::vec2(11, 11));
  doc->Regenerate();
  CHECK(doc->LastRegenStats().cached < 2);
}
