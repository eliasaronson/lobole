#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/features/ExtrudeFeature.h"
#include "core/io/Serialization.h"
#include "core/features/SketchFeature.h"

#include <filesystem>

using Catch::Matchers::WithinRel;
namespace fs = std::filesystem;

static const SketchPlane kXY(PlaneType::XY);

// Four lines at exact corners, chainable without a solve (same recipe as
// test_extrude.cpp).
static uint32_t AddRect(SketchFeature& sk, glm::vec2 c1, glm::vec2 c2) {
  glm::vec2 a{c1.x, c1.y}, b{c2.x, c1.y}, c{c2.x, c2.y}, d{c1.x, c2.y};
  uint32_t first = sk.AddEntity(sketch::Line{a, b});
  sk.AddEntity(sketch::Line{b, c});
  sk.AddEntity(sketch::Line{c, d});
  sk.AddEntity(sketch::Line{d, a});
  return first;
}

// A document with a 4x2 rectangle sketch and an extrude; returns the extrude.
static ExtrudeFeature* MakeExtrudedRect(Document& doc, double distance) {
  auto sk = std::make_unique<SketchFeature>("Sketch1", kXY);
  AddRect(*sk, {0, 0}, {4, 2});
  doc.AddFeature(std::move(sk));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Extrude1", sketchId, distance, ExtrudeMode::NewBody));
  return dynamic_cast<ExtrudeFeature*>(doc.GetFeature(1));
}

// --- Parameter table ---------------------------------------------------------

TEST_CASE("Parameters evaluate in dependency order", "[variables]") {
  Document doc;
  // Deliberately defined before its dependency: topo-sort must handle it.
  CHECK(doc.SetParameter("length", "width * 2"));
  CHECK(doc.SetParameter("width", "20"));

  CHECK_THAT(doc.FindParameter("width")->value, WithinRel(20.0));
  CHECK_THAT(doc.FindParameter("length")->value, WithinRel(40.0));
  CHECK(doc.FindParameter("length")->error.empty());

  // Editing the upstream parameter re-evaluates dependents.
  doc.SetParameter("width", "5");
  CHECK_THAT(doc.FindParameter("length")->value, WithinRel(10.0));
}

TEST_CASE("Invalid and reserved parameter names are refused", "[variables]") {
  Document doc;
  CHECK_FALSE(doc.SetParameter("2x", "1"));
  CHECK_FALSE(doc.SetParameter("a b", "1"));
  CHECK_FALSE(doc.SetParameter("pi", "1"));
  CHECK_FALSE(doc.SetParameter("sin", "1"));
  CHECK(doc.GetParameters().empty());
}

TEST_CASE("Parameter cycles are reported and keep last good values",
          "[variables]") {
  Document doc;
  doc.SetParameter("a", "2");
  doc.SetParameter("b", "a + 1");
  CHECK_THAT(doc.FindParameter("b")->value, WithinRel(3.0));

  // Introduce a cycle: a <-> b. Both flagged, values stay at last good.
  doc.SetParameter("a", "b * 2");
  CHECK_FALSE(doc.FindParameter("a")->error.empty());
  CHECK_FALSE(doc.FindParameter("b")->error.empty());
  CHECK_THAT(doc.FindParameter("a")->value, WithinRel(2.0));
  CHECK_THAT(doc.FindParameter("b")->value, WithinRel(3.0));

  // Break the cycle: errors clear, evaluation resumes.
  doc.SetParameter("a", "4");
  CHECK(doc.FindParameter("a")->error.empty());
  CHECK(doc.FindParameter("b")->error.empty());
  CHECK_THAT(doc.FindParameter("b")->value, WithinRel(5.0));
}

TEST_CASE("A parameter with an unknown reference keeps its last good value",
          "[variables]") {
  Document doc;
  doc.SetParameter("d", "7");
  doc.SetParameter("d", "nonsense + 1");
  CHECK_FALSE(doc.FindParameter("d")->error.empty());
  CHECK_THAT(doc.FindParameter("d")->value, WithinRel(7.0));
}

// --- Features follow parameter edits -----------------------------------------

TEST_CASE("Expression-driven extrude follows a parameter edit", "[variables]") {
  Document doc;
  doc.SetParameter("depth", "3");
  ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
  ext->distanceExpr = "depth * 2";
  doc.Regenerate();

  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(ext->distance, WithinRel(6.0));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 6.0, 1e-6));

  // The one-line pitch of the whole feature: change depth, the solid updates.
  doc.SetParameter("depth", "5");
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 10.0, 1e-6));
}

#ifdef HAVE_PLANEGCS
TEST_CASE("Expression-driven sketch dimension re-solves during regeneration",
          "[variables]") {
  Document doc;
  doc.SetParameter("r", "1.5");

  auto skOwned = std::make_unique<SketchFeature>("Sketch1", kXY);
  const uint32_t circleId = skOwned->AddEntity(sketch::Circle{{0, 0}, 1.0});
  sketch::Constraint radius;
  radius.kind = sketch::ConstraintKind::Radius;
  radius.a = {circleId, 0};
  radius.value = 1.0;
  radius.valueExpr = "r";
  skOwned->constraints.push_back(radius);
  doc.AddFeature(std::move(skOwned));

  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  REQUIRE(sk != nullptr);
  const auto circleRadius = [&] {
    return std::get<sketch::Circle>(
               sk->entities[sk->IndexOfEntity(circleId)])
        .radius;
  };
  CHECK_THAT(circleRadius(), WithinRel(1.5, 1e-9));

  doc.SetParameter("r", "4");
  CHECK_THAT(circleRadius(), WithinRel(4.0, 1e-9));
}
#endif

TEST_CASE("Expression-driven entity coordinates follow a parameter through "
          "regeneration",
          "[variables][exprs]") {
  // Decision 2A: a circle whose radius (and center) are *entity* expressions,
  // consumed by an extrude — the parameter edit must ripple through the
  // sketch geometry into the downstream solid, which requires the exprs to
  // be hash-visible to the regen cache (a cache hit on the stale sketch
  // would freeze the volume).
  Document doc;
  doc.SetParameter("bore", "6");
  doc.SetParameter("off", "bore + 4");

  auto skOwned = std::make_unique<SketchFeature>("Sketch1", kXY);
  const uint32_t circleId =
      skOwned->AddEntity(sketch::Circle{{0, 0}, 1.0});
  skOwned->entityExprs[circleId] = {{"r", "bore / 2"}, {"cx", "off"}};
  doc.AddFeature(std::move(skOwned));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 2.0,
                                                  ExtrudeMode::NewBody));
  doc.Regenerate();

  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  REQUIRE(sk != nullptr);
  const auto circle = [&] {
    return std::get<sketch::Circle>(sk->entities[sk->IndexOfEntity(circleId)]);
  };
  CHECK_THAT(circle().radius, WithinRel(3.0, 1e-9));
  CHECK_THAT(static_cast<double>(circle().center.x), WithinRel(10.0, 1e-6));
  REQUIRE(doc.GetBodies().size() == 1);
  const double pi = 3.14159265358979323846;
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(pi * 9.0 * 2.0, 1e-4));

  // The ripple: edit the parameter, the coordinate AND the solid follow —
  // through the incremental-regeneration cache.
  doc.SetParameter("bore", "10");
  CHECK_THAT(circle().radius, WithinRel(5.0, 1e-9));
  CHECK_THAT(circle().center.x, WithinRel(14.0f, 1e-6f));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(pi * 25.0 * 2.0, 1e-4));

  // A no-change regeneration is stable (cache hit keeps the same geometry).
  doc.Regenerate();
  CHECK_THAT(circle().radius, WithinRel(5.0, 1e-9));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
             WithinRel(pi * 25.0 * 2.0, 1e-4));

  // Entity exprs are visible to reference tracking (also via VisitExprs):
  // deleting a referenced parameter is refused, renaming rewrites the expr.
  std::vector<std::string> refs;
  CHECK_FALSE(doc.RemoveParameter("off", &refs));
  CHECK_FALSE(refs.empty());
  REQUIRE(doc.RenameParameter("off", "lobe_off"));
  CHECK(sk->entityExprs[circleId]["cx"] == "lobe_off");
  CHECK_THAT(circle().center.x, WithinRel(14.0f, 1e-6f));
}

// --- Project-wide variables (Phase V) ----------------------------------------

TEST_CASE("A project symbol table drives features in two documents",
          "[variables][project]") {
  // Simulates AppState's project scope: one SymbolTable injected into two
  // Documents. A part-local variable of the same name shadows the project one.
  SymbolTable project;
  project["bore"] = 6.0;

  Document axle;
  axle.SetProjectSymbols(&project);
  ExtrudeFeature* axleExt = MakeExtrudedRect(axle, 1.0);
  axleExt->distanceExpr = "bore"; // resolves from the project scope
  axle.Regenerate();
  CHECK_THAT(axleExt->distance, WithinRel(6.0));
  CHECK(axleExt->GetError().empty());

  Document wheel;
  wheel.SetProjectSymbols(&project);
  ExtrudeFeature* wheelExt = MakeExtrudedRect(wheel, 1.0);
  wheelExt->distanceExpr = "bore * 2";
  wheel.Regenerate();
  CHECK_THAT(wheelExt->distance, WithinRel(12.0));

  // A part-local variable named `bore` shadows the project value in THAT doc.
  wheel.SetParameter("bore", "100");
  CHECK_THAT(wheelExt->distance, WithinRel(200.0));
  // The other document, sharing only the project scope, is unaffected.
  axle.Regenerate();
  CHECK_THAT(axleExt->distance, WithinRel(6.0));

  // Editing the project value ripples into every non-shadowing document on its
  // next regenerate (the regen cache invalidates: the baked value changed).
  project["bore"] = 10.0;
  axle.Regenerate();
  CHECK_THAT(axleExt->distance, WithinRel(10.0));
  wheel.Regenerate();
  CHECK_THAT(wheelExt->distance, WithinRel(200.0)); // still shadowed
}

TEST_CASE("An unknown variable is a loud feature error, not a silent literal",
          "[variables][project]") {
  // Fail-loud: a feature expression whose variable resolves in NEITHER scope
  // must error, not silently keep drilling the literal twin (the 1 mm hole).
  Document doc;
  ExtrudeFeature* ext = MakeExtrudedRect(doc, 5.0);
  ext->distanceExpr = "bore"; // never defined anywhere
  doc.Regenerate();

  // The literal is kept as the RENDERED fallback (5.0), but the error is set.
  CHECK_THAT(ext->distance, WithinRel(5.0));
  REQUIRE_FALSE(ext->GetError().empty());
  CHECK(ext->GetError().find("unknown variable") != std::string::npos);
  CHECK(ext->GetError().find("bore") != std::string::npos);

  // Defining it (as a project var) clears the error and drives the geometry.
  SymbolTable project;
  project["bore"] = 3.0;
  doc.SetProjectSymbols(&project);
  doc.Regenerate();
  CHECK(ext->GetError().empty());
  CHECK_THAT(ext->distance, WithinRel(3.0));
}

TEST_CASE("A project-value change invalidates the regen cache across a part",
          "[variables][project]") {
  SymbolTable project;
  project["h"] = 2.0;
  Document doc;
  doc.SetProjectSymbols(&project);
  ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
  ext->distanceExpr = "h";
  doc.Regenerate();
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 2.0, 1e-6));

  // No local edit, only the shared project value moved: the extrude must
  // re-execute (its baked distance changed), not serve a stale cached body.
  project["h"] = 7.0;
  doc.Regenerate();
  CHECK_THAT(ext->distance, WithinRel(7.0));
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 7.0, 1e-6));
}

TEST_CASE("EvaluateParameterTable overlays a base scope, shadowed by locals",
          "[variables][project]") {
  SymbolTable project;
  project["a"] = 3.0;
  project["b"] = 4.0;

  std::vector<Parameter> local = {{"c", "a + b", "", 0.0, {}},
                                  {"a", "10", "", 0.0, {}}}; // shadows project a
  SymbolTable out;
  EvaluateParameterTable(local, out, &project, {});

  CHECK_THAT(out.at("a"), WithinRel(10.0));      // local shadow wins
  CHECK_THAT(out.at("b"), WithinRel(4.0));       // inherited from project
  CHECK_THAT(out.at("c"), WithinRel(14.0));      // 10 (local a) + 4 (project b)
}

// --- Commands -----------------------------------------------------------------

TEST_CASE("Set/delete/rename variable commands undo and redo", "[variables]") {
  Document doc;
  CommandStack stack;

  REQUIRE(stack.Execute(std::make_unique<SetVariableCommand>("width", "20"),
                        doc));
  CHECK_THAT(doc.FindParameter("width")->value, WithinRel(20.0));

  // Redefine, then undo back to the first definition.
  REQUIRE(stack.Execute(std::make_unique<SetVariableCommand>("width", "30"),
                        doc));
  stack.Undo(doc);
  CHECK_THAT(doc.FindParameter("width")->value, WithinRel(20.0));
  stack.Redo(doc);
  CHECK_THAT(doc.FindParameter("width")->value, WithinRel(30.0));

  // Undo of the original add removes the parameter entirely.
  stack.Undo(doc);
  stack.Undo(doc);
  CHECK(doc.FindParameter("width") == nullptr);

  // Rename rewrites referencing expressions (and undoes cleanly).
  doc.SetParameter("width", "20");
  doc.SetParameter("length", "width * 2");
  REQUIRE(stack.Execute(
      std::make_unique<RenameVariableCommand>("width", "w"), doc));
  CHECK(doc.FindParameter("width") == nullptr);
  CHECK(doc.FindParameter("length")->expression == "w * 2");
  stack.Undo(doc);
  CHECK(doc.FindParameter("length")->expression == "width * 2");

  // Deleting a referenced parameter is refused.
  CHECK_FALSE(
      stack.Execute(std::make_unique<DeleteVariableCommand>("width"), doc));
  CHECK(doc.FindParameter("width") != nullptr);

  // Unreferenced parameters delete fine and undo restores them.
  doc.SetParameter("orphan", "1");
  REQUIRE(stack.Execute(std::make_unique<DeleteVariableCommand>("orphan"),
                        doc));
  CHECK(doc.FindParameter("orphan") == nullptr);
  stack.Undo(doc);
  CHECK(doc.FindParameter("orphan") != nullptr);
}

TEST_CASE("Rename rewrites feature expressions", "[variables]") {
  Document doc;
  doc.SetParameter("depth", "3");
  ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
  ext->distanceExpr = "depth + 1";
  doc.Regenerate();
  CHECK_THAT(ext->distance, WithinRel(4.0));

  REQUIRE(doc.RenameParameter("depth", "h"));
  CHECK(ext->distanceExpr == "h + 1");
  CHECK_THAT(ext->distance, WithinRel(4.0));

  // And deletion is refused while the feature references it.
  std::vector<std::string> refs;
  CHECK_FALSE(doc.RemoveParameter("h", &refs));
  REQUIRE(refs.size() == 1);
  CHECK(refs[0] == "feature 'Extrude1'");
}

// --- Serialization -------------------------------------------------------------

TEST_CASE("Parameters and expressions round-trip through save/load",
          "[variables]") {
  const fs::path path = fs::temp_directory_path() / "designer_vars_test.json";

  {
    Document doc;
    doc.SetParameter("width", "20", "overall width");
    doc.SetParameter("depth", "width / 4");
    ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
    ext->distanceExpr = "depth";
    doc.Regenerate();
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 5.0, 1e-6));
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }

  std::string error;
  const auto loaded = DocumentIO::Load(path.string(), &error);
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->GetParameters().size() == 2);
  CHECK(loaded->FindParameter("width")->comment == "overall width");
  CHECK_THAT(loaded->FindParameter("depth")->value, WithinRel(5.0));

  const auto* ext = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(ext != nullptr);
  CHECK(ext->distanceExpr == "depth");
  CHECK_THAT(ext->distance, WithinRel(5.0));
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(),
             WithinRel(8.0 * 5.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("Documents without parameters load unchanged", "[variables]") {
  const fs::path path = fs::temp_directory_path() / "designer_novars_test.json";
  {
    Document doc;
    MakeExtrudedRect(doc, 2.0);
    REQUIRE(DocumentIO::Save(doc, path.string()));
  }
  const auto loaded = DocumentIO::Load(path.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->GetParameters().empty());
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-6));
  fs::remove(path);
}
