#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Command.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include <cmath>

#include <filesystem>

namespace fs = std::filesystem;
using Catch::Matchers::WithinRel;

namespace {

const SketchPlane kXY{{0, 0, 0}, {0, 0, 1}, {1, 0, 0}};

// Appends a w x h rectangle at the origin to a sketch.
void AddRect(SketchFeature& sk, double w, double h) {
  glm::vec2 a{0, 0}, b{w, 0}, c{w, h}, d{0, h};
  sk.AddEntity(sketch::Line{a, b});
  sk.AddEntity(sketch::Line{b, c});
  sk.AddEntity(sketch::Line{c, d});
  sk.AddEntity(sketch::Line{d, a});
}

// A document with a 4x2 rect extruded by `distanceExpr` (a NewBody prism);
// volume = 8 * distance. Returns the extrude for further tweaks.
ExtrudeFeature* MakeBar(Document& doc, const std::string& distanceExpr,
                        double literal) {
  auto sk = std::make_unique<SketchFeature>("Sketch1", kXY);
  AddRect(*sk, 4, 2);
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  auto ext = std::make_unique<ExtrudeFeature>("Extrude1", sketchId, literal,
                                              ExtrudeMode::NewBody);
  ext->distanceExpr = distanceExpr;
  auto* raw = ext.get();
  doc.AddFeature(std::move(ext));
  return raw;
}

double Vol(const Document& doc) {
  REQUIRE(!doc.GetBodies().empty());
  return doc.GetBodies()[0].shape.Volume();
}

} // namespace

// ---- C0 gate: the override-regeneration mechanic --------------------------

TEST_CASE("A configuration overrides a parameter during regeneration",
          "[configuration]") {
  Document doc;
  doc.SetParameter("depth", "3");
  MakeBar(doc, "depth", 3);
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Vol(doc), WithinRel(8.0 * 3.0, 1e-6)); // base

  Configuration& tall = doc.AddConfiguration("Tall");
  tall.parameterOverrides = {{"depth", "10"}};
  const uint64_t tallId = tall.id;

  // Adding a config changes nothing until it is activated.
  CHECK(doc.ActiveConfiguration() == 0);
  CHECK_THAT(Vol(doc), WithinRel(24.0, 1e-6));

  REQUIRE(doc.SetActiveConfiguration(tallId));
  CHECK_THAT(Vol(doc), WithinRel(8.0 * 10.0, 1e-6)); // override applied

  // Switching back to base restores the authored value — no stored state was
  // mutated by the override.
  REQUIRE(doc.SetActiveConfiguration(0));
  CHECK_THAT(Vol(doc), WithinRel(24.0, 1e-6));
  CHECK(doc.FindParameter("depth")->expression == "3"); // base expr intact
}

// Primitives were the one feature kind that carried no expression fields, so
// a variable could not drive a box and a part built from primitives could not
// be configured at all — the Configurations panel would offer overrides that
// changed nothing. Fixed in v61; this is the case that was impossible.
TEST_CASE("A configuration drives a primitive through a variable",
          "[configuration]") {
  Document doc;
  doc.SetParameter("len", "40");
  doc.SetParameter("wall", "6");
  auto box =
      std::make_unique<BoxFeature>("Plate", glm::vec3(0.0f), 40.0, 6.0, 30.0);
  box->widthExpr = "len";
  box->heightExpr = "wall";
  doc.AddFeature(std::move(box));
  auto cyl = std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0.0f, 6.0f, 0.0f), glm::vec3(0, 1, 0), 6.0, 12.0);
  cyl->radiusExpr = "wall";
  doc.AddFeature(std::move(cyl));
  REQUIRE(doc.Regenerate());
  // Two separate bodies — Vol() reads the first, so check each by hand.
  const auto vols = [&] {
    std::vector<double> v;
    for (const Body& b : doc.GetBodies())
      v.push_back(b.shape.Volume());
    return v;
  };
  REQUIRE(vols().size() == 2);
  CHECK_THAT(vols()[0], WithinRel(40.0 * 6.0 * 30.0, 1e-3));
  CHECK_THAT(vols()[1], WithinRel(M_PI * 36.0 * 12.0, 1e-3));
  const double base = vols()[0];

  Configuration& small = doc.AddConfiguration("Small");
  small.parameterOverrides = {{"len", "20"}, {"wall", "3"}};
  REQUIRE(doc.SetActiveConfiguration(small.id));
  // Both primitives follow: the box in two dimensions, the cylinder in its
  // radius. A literal-only primitive would have come back unchanged.
  REQUIRE(vols().size() == 2);
  CHECK_THAT(vols()[0], WithinRel(20.0 * 3.0 * 30.0, 1e-3));
  CHECK_THAT(vols()[1], WithinRel(M_PI * 9.0 * 12.0, 1e-3));

  REQUIRE(doc.SetActiveConfiguration(0));
  CHECK_THAT(vols()[0], WithinRel(base, 1e-6));
}

TEST_CASE("A configuration override propagates to dependent parameters",
          "[configuration]") {
  Document doc;
  doc.SetParameter("width", "5");
  doc.SetParameter("depth", "width * 2"); // = 10
  MakeBar(doc, "depth", 10);
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Vol(doc), WithinRel(8.0 * 10.0, 1e-6));

  // Overriding `width` must re-evaluate `depth = width * 2` under the config,
  // not leave depth at its base value.
  Configuration& narrow = doc.AddConfiguration("Narrow");
  narrow.parameterOverrides = {{"width", "1"}};
  REQUIRE(doc.SetActiveConfiguration(narrow.id));
  CHECK_THAT(Vol(doc), WithinRel(8.0 * 2.0, 1e-6)); // depth followed width -> 2
}

TEST_CASE("A configuration suppresses a feature, composed with authored flags",
          "[configuration]") {
  Document doc;
  // Two independent prisms -> two bodies.
  auto sk1 = std::make_unique<SketchFeature>("S1", kXY);
  AddRect(*sk1, 4, 2);
  doc.AddFeature(std::move(sk1));
  const uint64_t s1 = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("E1", s1, 3.0, ExtrudeMode::NewBody));
  const uint64_t e1 = doc.GetFeature(1)->GetId();

  auto sk2 = std::make_unique<SketchFeature>("S2", kXY);
  AddRect(*sk2, 2, 2);
  doc.AddFeature(std::move(sk2));
  const uint64_t s2 = doc.GetFeature(2)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("E2", s2, 3.0, ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 2);

  Configuration& lite = doc.AddConfiguration("NoFirst");
  lite.suppressedFeatures = {e1}; // suppress the first prism
  REQUIRE(doc.SetActiveConfiguration(lite.id));
  CHECK(doc.GetBodies().size() == 1); // one prism gone

  // Compose with an authored suppression: also suppress the sketch of the
  // remaining prism -> its extrude has no profile, so no bodies at all.
  doc.GetFeature(2)->SetSuppressed(true);
  REQUIRE_FALSE(doc.Regenerate()); // E2 fails without its sketch
  CHECK(doc.GetBodies().empty());

  // Base config: authored suppression still applies (E2 broken), but E1 is back
  // -> exactly one body.
  doc.GetFeature(1)->ClearError();
  REQUIRE(doc.SetActiveConfiguration(0)); // regenerates
  CHECK(doc.EffectiveSuppressed(2));      // still authored-suppressed
  CHECK(doc.GetBodies().size() == 1);
}

// A configuration that turns a variant ON turns on its sketch as well as the
// feature built on it. References resolved against the AUTHORED flag only, so
// the revolve or extrude of a family member failed with "profile sketch not
// found earlier in history" in exactly the configuration that asked for it --
// and a sketch a configuration suppressed stayed referable.
TEST_CASE("A reference resolves through the configuration's suppression",
          "[configuration]") {
  Document doc;
  auto sk1 = std::make_unique<SketchFeature>("Wide", kXY);
  AddRect(*sk1, 4, 2);
  doc.AddFeature(std::move(sk1));
  const uint64_t s1 = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("E1", s1, 3.0, ExtrudeMode::NewBody));
  const uint64_t e1 = doc.GetFeature(1)->GetId();
  auto sk2 = std::make_unique<SketchFeature>("Narrow", kXY);
  AddRect(*sk2, 2, 2);
  doc.AddFeature(std::move(sk2));
  const uint64_t s2 = doc.GetFeature(2)->GetId();
  doc.AddFeature(
      std::make_unique<ExtrudeFeature>("E2", s2, 3.0, ExtrudeMode::NewBody));
  const uint64_t e2 = doc.GetFeature(3)->GetId();
  // The authored model is the wide variant; the narrow one is off.
  doc.GetFeature(2)->SetSuppressed(true);
  doc.GetFeature(3)->SetSuppressed(true);
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Vol(doc), WithinRel(4.0 * 2.0 * 3.0, 1e-6));

  Configuration& narrow = doc.AddConfiguration("Narrow");
  narrow.suppressedFeatures = {s1, e1};
  narrow.unsuppressedFeatures = {s2, e2};
  const uint64_t narrowId = narrow.id;
  REQUIRE(doc.SetActiveConfiguration(narrowId));
  CHECK(doc.GetFeature(3)->GetError().empty());
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(Vol(doc), WithinRel(2.0 * 2.0 * 3.0, 1e-6));

  // Suppressing only a SKETCH in a configuration breaks the feature on it,
  // the same as suppressing it in the authored model does.
  Configuration& noProfile = doc.AddConfiguration("No profile");
  noProfile.suppressedFeatures = {s1};
  doc.SetActiveConfiguration(noProfile.id);
  CHECK_FALSE(doc.GetFeature(1)->GetError().empty());
  CHECK(doc.GetBodies().empty());
}

TEST_CASE("Configurations round-trip through save/load (v31)",
          "[configuration][serialization]") {
  Document doc;
  doc.SetParameter("depth", "3");
  MakeBar(doc, "depth", 3);
  Configuration& tall = doc.AddConfiguration("Tall");
  tall.parameterOverrides = {{"depth", "10"}};
  tall.suppressedFeatures = {doc.GetFeature(1)->GetId()};
  const uint64_t tallId = tall.id;
  REQUIRE(doc.SetActiveConfiguration(tallId)); // active + suppresses -> empty

  const auto check = [&](Document& reloaded) {
    REQUIRE(reloaded.GetConfigurations().size() == 1);
    const Configuration& c = reloaded.GetConfigurations()[0];
    CHECK(c.id == tallId);
    CHECK(c.name == "Tall");
    REQUIRE(c.parameterOverrides.size() == 1);
    CHECK(c.parameterOverrides[0].first == "depth");
    CHECK(c.parameterOverrides[0].second == "10");
    REQUIRE(c.suppressedFeatures.size() == 1);
    CHECK(reloaded.ActiveConfiguration() == tallId);
    // The active config suppressed the extrude, so the reload has no bodies.
    CHECK(reloaded.GetBodies().empty());
  };

  SECTION("single JSON file") {
    const fs::path tmp = fs::temp_directory_path() / "cfg_rt.json";
    std::string err;
    REQUIRE(testio::Save(doc, tmp.string()));
    auto reloaded = testio::Load(tmp.string());
    INFO(err);
    REQUIRE(reloaded != nullptr);
    check(*reloaded);
    fs::remove(tmp);
  }

  SECTION("directory format, one file per configuration") {
    const fs::path proj = fs::temp_directory_path() / "cfg_rt.dsgn";
    fs::remove_all(proj);
    std::string err;
    REQUIRE(testio::Save(doc, proj.string()));
    CHECK(fs::exists(proj / "configurations" / "3-tall.json"));
    auto reloaded = testio::Load(proj.string());
    INFO(err);
    REQUIRE(reloaded != nullptr);
    check(*reloaded);
    fs::remove_all(proj);
  }
}

TEST_CASE("Config commands undo and redo", "[configuration][command]") {
  Document doc;
  doc.SetParameter("depth", "3");
  MakeBar(doc, "depth", 3);
  CommandStack stack;

  auto add = std::make_unique<AddConfigurationCommand>("Tall");
  const uint64_t id = add->ConfigurationId(); // 0 until executed
  REQUIRE(stack.Execute(std::move(add), doc));
  REQUIRE(doc.GetConfigurations().size() == 1);
  const uint64_t cid = doc.GetConfigurations()[0].id;

  // Modify: give it an override.
  Configuration edited = doc.GetConfigurations()[0];
  edited.parameterOverrides = {{"depth", "10"}};
  const nlohmann::json before =
      DocumentIO::ConfigurationToJson(doc.GetConfigurations()[0]);
  const nlohmann::json after = DocumentIO::ConfigurationToJson(edited);
  REQUIRE(stack.Execute(
      std::make_unique<ModifyConfigurationCommand>("edit", before, after),
      doc));
  CHECK(doc.FindConfiguration(cid)->parameterOverrides.size() == 1);

  // Activate, and check geometry follows.
  REQUIRE(
      stack.Execute(std::make_unique<SetActiveConfigurationCommand>(cid), doc));
  CHECK_THAT(Vol(doc), WithinRel(80.0, 1e-6));
  stack.Undo(doc); // deactivate
  CHECK_THAT(Vol(doc), WithinRel(24.0, 1e-6));
  stack.Undo(doc); // un-modify
  CHECK(doc.FindConfiguration(cid)->parameterOverrides.empty());
  stack.Undo(doc); // un-add
  CHECK(doc.GetConfigurations().empty());
  (void)id;
}

TEST_CASE("Config switches invalidate exactly the affected cache suffix",
          "[configuration]") {
  Document doc;
  doc.SetParameter("depth", "3");
  MakeBar(doc, "depth", 3); // features: [Sketch(0), Extrude(1)]
  REQUIRE(doc.Regenerate());

  Configuration& tall = doc.AddConfiguration("Tall");
  tall.parameterOverrides = {{"depth", "10"}};

  // Activating a config that changes `depth` (which only feeds the extrude)
  // must re-execute the extrude — but the sketch prefix stays cached.
  REQUIRE(doc.SetActiveConfiguration(tall.id));
  CHECK(doc.LastRegenStats().cached >= 1);   // the sketch was reused
  CHECK(doc.LastRegenStats().executed >= 1); // the extrude re-ran
  CHECK_THAT(Vol(doc), WithinRel(80.0, 1e-6));

  // Re-activating the same config is a no-op geometry-wise and fully cached.
  REQUIRE(doc.SetActiveConfiguration(tall.id));
  CHECK(doc.LastRegenStats().executed == 0);

  // A suppression-only config (no field values change) must still invalidate:
  // the folded effective-suppressed bit moves the divergence.
  Configuration& off = doc.AddConfiguration("Off");
  off.suppressedFeatures = {doc.GetFeature(1)->GetId()};
  REQUIRE(doc.SetActiveConfiguration(off.id));
  CHECK(doc.GetBodies().empty());
  CHECK(doc.LastRegenStats().executed == 0); // suppressed = nothing runs...
  // ...but the cache did re-diverge (bodies changed from the Tall prism to
  // empty), proving the hash folded suppression in.
  REQUIRE(doc.SetActiveConfiguration(0));
  CHECK_THAT(Vol(doc), WithinRel(24.0, 1e-6));
}
