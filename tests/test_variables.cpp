// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/VariableActions.h"

#include <filesystem>
#include <fstream>
#include <iterator>

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
  ext->distance.expr = "depth * 2";
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
    return std::get<sketch::Circle>(sk->entities[sk->IndexOfEntity(circleId)])
        .radius;
  };
  CHECK_THAT(circleRadius(), WithinRel(1.5, 1e-9));

  doc.SetParameter("r", "4");
  CHECK_THAT(circleRadius(), WithinRel(4.0, 1e-9));
}

// An ANGLE dimension through everything that carries it: the expression in
// degrees, VisitExprs' conversion to the radians the solver takes, the
// solve, the extrude, and the measure tool on the solid. The two faces a
// 30-degree corner makes stand at 180 - 30 between their outward normals,
// on the side of the base line the sketch was drawn on -- and at 130 when
// the variable says 50. The solver's own test checks |angle| only.
TEST_CASE("An angle dimension's variable is the angle the solid measures",
          "[variables][sketch]") {
  Document doc;
  doc.SetParameter("theta", "30");
  auto skOwned = std::make_unique<SketchFeature>("Sketch1", kXY);
  const uint32_t base = skOwned->AddEntity(sketch::Line{{0, 0}, {40, 0}});
  const uint32_t leg = skOwned->AddEntity(sketch::Line{{0, 0}, {30, 20}});
  const uint32_t close = skOwned->AddEntity(sketch::Line{{40, 0}, {30, 20}});
  using K = sketch::ConstraintKind;
  skOwned->constraints.push_back({K::Coincident, {leg, 0}, {base, 0}});
  skOwned->constraints.push_back({K::Coincident, {close, 0}, {base, 1}});
  skOwned->constraints.push_back({K::Coincident, {close, 1}, {leg, 1}});
  skOwned->constraints.push_back({K::Horizontal, {base, 0}});
  skOwned->constraints.push_back({K::Vertical, {close, 0}});
  sketch::Constraint angle{K::Angle, {base, 0}, {leg, 0}, 0.5};
  angle.valueExpr = "theta";
  skOwned->constraints.push_back(angle);
  doc.AddFeature(std::move(skOwned));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 5.0,
                                                  ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());

  const auto measured = [&] {
    REQUIRE(doc.GetBodies().size() == 1);
    const Body& b = doc.GetBodies()[0];
    ElementRef under, slope;
    for (const auto& [idx, name] : b.elementMap.names) {
      glm::dvec3 c, n, u;
      if (!b.shape.FacePlane(idx, c, n, u) || std::abs(n.z) > 0.5f)
        continue;
      if (n.y < -0.99f)
        under = ElementRef{b.createdBy, name}; // the base line's face
      else if (n.x < -0.01f)
        slope = ElementRef{b.createdBy, name}; // the leg's
    }
    REQUIRE(under.IsSet()); // still below: drawn above the base, kept there
    REQUIRE(slope.IsSet());
    const auto a = Measure::Angle(doc.GetBodies(), under, slope);
    REQUIRE(a.has_value());
    return a->degrees;
  };
  CHECK_THAT(measured(), Catch::Matchers::WithinAbs(150.0, 1e-4));
  doc.SetParameter("theta", "50");
  CHECK_THAT(measured(), Catch::Matchers::WithinAbs(130.0, 1e-4));
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
  const uint32_t circleId = skOwned->AddEntity(sketch::Circle{{0, 0}, 1.0});
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

// A spline's poles take expressions like a line's endpoints do, so an outline
// drawn as a spline scales with the variable that sizes the rest of the part
// instead of staying the shape it was drawn at.
TEST_CASE("Expression-driven spline poles follow a parameter",
          "[variables][exprs]") {
  Document doc;
  doc.SetParameter("s", "1");
  auto skOwned = std::make_unique<SketchFeature>("Outline", kXY);
  sketch::BSpline bs;
  bs.poles = {{0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}};
  const uint32_t id = skOwned->AddEntity(bs);
  auto& ex = skOwned->entityExprs[id];
  const std::vector<std::pair<float, float>> unit = {
      {0, 0}, {10, 0}, {10, 10}, {0, 10}, {0, 0}};
  for (size_t k = 0; k < unit.size(); k++) {
    ex["p" + std::to_string(k) + "x"] = std::to_string(unit[k].first) + " * s";
    ex["p" + std::to_string(k) + "y"] = std::to_string(unit[k].second) + " * s";
  }
  doc.AddFeature(std::move(skOwned));
  const uint64_t sketchId = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ExtrudeFeature>("Pad", sketchId, 2.0,
                                                  ExtrudeMode::NewBody));
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  const double v1 = doc.GetBodies()[0].shape.Volume();

  doc.SetParameter("s", "2");
  auto* sk = dynamic_cast<SketchFeature*>(doc.GetFeature(0));
  REQUIRE(sk != nullptr);
  const auto& moved =
      std::get<sketch::BSpline>(sk->entities[sk->IndexOfEntity(id)]);
  CHECK_THAT(moved.poles[2].x, WithinRel(20.0f, 1e-6f));
  CHECK_THAT(moved.poles[3].y, WithinRel(20.0f, 1e-6f));
  // Twice the size in the plane, the same depth: four times the volume.
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(4.0 * v1, 1e-4));

  CHECK_FALSE(sketch::PoleField("px").has_value());
  CHECK_FALSE(sketch::PoleField("r").has_value());
  CHECK(sketch::PoleField("p12y") == std::pair<size_t, bool>{12, false});
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
  axleExt->distance.expr = "bore"; // resolves from the project scope
  axle.Regenerate();
  CHECK_THAT(axleExt->distance, WithinRel(6.0));
  CHECK(axleExt->GetError().empty());

  Document wheel;
  wheel.SetProjectSymbols(&project);
  ExtrudeFeature* wheelExt = MakeExtrudedRect(wheel, 1.0);
  wheelExt->distance.expr = "bore * 2";
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
  ext->distance.expr = "bore"; // never defined anywhere
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
  ext->distance.expr = "h";
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

  std::vector<Parameter> local = {
      {"c", "a + b", "", 0.0, {}},
      {"a", "10", "", 0.0, {}}}; // shadows project a
  SymbolTable out;
  EvaluateParameterTable(local, out, &project, {});

  CHECK_THAT(out.at("a"), WithinRel(10.0)); // local shadow wins
  CHECK_THAT(out.at("b"), WithinRel(4.0));  // inherited from project
  CHECK_THAT(out.at("c"), WithinRel(14.0)); // 10 (local a) + 4 (project b)
}

// --- Commands
// -----------------------------------------------------------------

TEST_CASE("Set/delete/rename variable commands undo and redo", "[variables]") {
  Document doc;
  CommandStack stack;

  REQUIRE(
      stack.Execute(std::make_unique<SetVariableCommand>("width", "20"), doc));
  CHECK_THAT(doc.FindParameter("width")->value, WithinRel(20.0));

  // Redefine, then undo back to the first definition.
  REQUIRE(
      stack.Execute(std::make_unique<SetVariableCommand>("width", "30"), doc));
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
  REQUIRE(stack.Execute(std::make_unique<RenameVariableCommand>("width", "w"),
                        doc));
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
  REQUIRE(
      stack.Execute(std::make_unique<DeleteVariableCommand>("orphan"), doc));
  CHECK(doc.FindParameter("orphan") == nullptr);
  stack.Undo(doc);
  CHECK(doc.FindParameter("orphan") != nullptr);
}

TEST_CASE("Rename rewrites feature expressions", "[variables]") {
  Document doc;
  doc.SetParameter("depth", "3");
  ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
  ext->distance.expr = "depth + 1";
  doc.Regenerate();
  CHECK_THAT(ext->distance, WithinRel(4.0));

  REQUIRE(doc.RenameParameter("depth", "h"));
  CHECK(ext->distance.expr == "h + 1");
  CHECK_THAT(ext->distance, WithinRel(4.0));

  // And deletion is refused while the feature references it.
  std::vector<std::string> refs;
  CHECK_FALSE(doc.RemoveParameter("h", &refs));
  REQUIRE(refs.size() == 1);
  CHECK(refs[0] == "feature 'Extrude1'");
}

namespace {
// A study whose load and mesh size read variables: the two kinds of field
// Study::VisitExprs walks.
fem::Study StudyReading(Document& doc, const std::string& load,
                        const std::string& mesh) {
  fem::Study study = doc.AddStudy("Bracket");
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = fem::LoadType::Force;
  l.magnitude.expr = load;
  study.loads.push_back(l);
  study.meshSize.expr = mesh;
  REQUIRE(doc.ReplaceStudy(study));
  return study;
}
} // namespace

TEST_CASE("Rename reaches a study and a configuration's overrides; delete "
          "names the study",
          "[variables]") {
  Document doc;
  doc.SetParameter("pull", "100");
  doc.SetParameter("depth", "3");
  const uint64_t id = StudyReading(doc, "pull * 2", "depth").id;
  Configuration& added = doc.AddConfiguration("Heavy");
  Configuration cfg = added;
  cfg.parameterOverrides = {{"depth", "pull / 10"}};
  REQUIRE(doc.ReplaceConfiguration(cfg));

  CommandStack stack;
  REQUIRE(stack.Execute(
      std::make_unique<RenameVariableCommand>("pull", "force"), doc));
  REQUIRE(doc.RenameParameter("depth", "h"));
  const fem::Study* study = doc.FindStudy(id);
  REQUIRE(study != nullptr);
  CHECK(study->loads.at(0).magnitude.expr == "force * 2");
  CHECK(study->meshSize.expr == "h");
  // The override follows the variable it overrides, and the text it reads.
  const Configuration* heavy = doc.FindConfiguration(cfg.id);
  REQUIRE(heavy != nullptr);
  REQUIRE(heavy->parameterOverrides.size() == 1);
  CHECK(heavy->parameterOverrides[0].first == "h");
  CHECK(heavy->parameterOverrides[0].second == "force / 10");
  // ...and still applies: h is 100 / 10 under it.
  REQUIRE(doc.SetActiveConfiguration(cfg.id));
  CHECK_THAT(doc.Symbols().at("h"), WithinRel(10.0));
  REQUIRE(doc.SetActiveConfiguration(0));

  fem::Study evaluated = *study;
  std::string err;
  REQUIRE(fem::EvaluateStudyExprs(evaluated, doc.Symbols(), &err));
  CHECK_THAT(evaluated.loads.at(0).magnitude, WithinRel(200.0));

  // A variable only the study reads cannot be deleted, and says who reads it.
  std::vector<std::string> refs;
  CHECK_FALSE(doc.RemoveParameter("force", &refs));
  CHECK(std::find(refs.begin(), refs.end(), "study 'Bracket'") != refs.end());

  // Undoing the rename puts the study's text back too.
  stack.Undo(doc);
  CHECK(doc.FindStudy(id)->loads.at(0).magnitude.expr == "pull * 2");
}

TEST_CASE("A project variable's rename and delete reach a part's study and an "
          "assembly's",
          "[variables]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  REQUIRE(ui::SetProjectVariable(state, "pull", "100", ""));
  StudyReading(*state.document, "pull", "");
  ui::AddAssembly(state, "Rig");
  fem::Study rig;
  rig.id = 1;
  rig.name = "Rig study";
  fem::Load l;
  l.id = 2;
  l.magnitude.expr = "pull / 2";
  rig.loads.push_back(l);
  state.assembly->GetStudies().push_back(rig);

  std::vector<std::string> refs;
  CHECK_FALSE(ui::RemoveProjectVariable(state, "pull", &refs));
  CHECK(std::find(refs.begin(), refs.end(), "Part 1: study 'Bracket'") !=
        refs.end());
  CHECK(std::find(refs.begin(), refs.end(), "Rig: study 'Rig study'") !=
        refs.end());

  const uint64_t asmStamp = state.assembly->SaveStamp();
  REQUIRE(ui::RenameProjectVariable(state, "pull", "force"));
  CHECK(state.assembly->GetStudies().at(0).loads.at(0).magnitude.expr ==
        "force / 2");
  CHECK(state.assembly->SaveStamp() != asmStamp); // saved, so dirty
  CHECK(state.parts.at(0).doc->GetStudies().at(0).loads.at(0).magnitude.expr ==
        "force");
  // Undo reverses it in both.
  REQUIRE(ui::UndoProjectVars(state));
  CHECK(state.assembly->GetStudies().at(0).loads.at(0).magnitude.expr ==
        "pull / 2");
  CHECK(state.parts.at(0).doc->GetStudies().at(0).loads.at(0).magnitude.expr ==
        "pull");
}

// --- Serialization
// -------------------------------------------------------------

TEST_CASE("Parameters and expressions round-trip through save/load",
          "[variables]") {
  const fs::path path = fs::temp_directory_path() / "designer_vars_test.json";

  {
    Document doc;
    doc.SetParameter("width", "20", "overall width");
    doc.SetParameter("depth", "width / 4");
    ExtrudeFeature* ext = MakeExtrudedRect(doc, 1.0);
    ext->distance.expr = "depth";
    doc.Regenerate();
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(8.0 * 5.0, 1e-6));
    REQUIRE(testio::Save(doc, path.string()));
  }

  std::string error;
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->GetParameters().size() == 2);
  CHECK(loaded->FindParameter("width")->comment == "overall width");
  CHECK_THAT(loaded->FindParameter("depth")->value, WithinRel(5.0));

  const auto* ext = dynamic_cast<ExtrudeFeature*>(loaded->GetFeature(1));
  REQUIRE(ext != nullptr);
  CHECK(ext->distance.expr == "depth");
  CHECK_THAT(ext->distance, WithinRel(5.0));
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(8.0 * 5.0, 1e-6));

  fs::remove(path);
}

TEST_CASE("Documents without parameters load unchanged", "[variables]") {
  const fs::path path = fs::temp_directory_path() / "designer_novars_test.json";
  {
    Document doc;
    MakeExtrudedRect(doc, 2.0);
    REQUIRE(testio::Save(doc, path.string()));
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  CHECK(loaded->GetParameters().empty());
  REQUIRE(loaded->GetBodies().size() == 1);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(16.0, 1e-6));
  fs::remove(path);
}

// A document may hold an expression that does not parse; the RPC refuses
// one (FeatureFromJsonNamed), but a FILE with one must still open, the
// feature that holds it marked broken with the parse error and the rest of
// the part built (docs/variables/README.md#numbers-that-are-expressions).
TEST_CASE("A file with an expression that does not parse opens, that feature "
          "broken",
          "[variables]") {
  const fs::path path =
      fs::temp_directory_path() / "lobole_unreadable_expr.json";
  {
    Document doc;
    doc.SetParameter("len", "40");
    auto bad =
        std::make_unique<BoxFeature>("Bad", glm::vec3(0.0f), 10.0, 10.0, 10.0);
    bad->width.expr = "len";
    doc.AddFeature(std::move(bad));
    doc.AddFeature(std::make_unique<BoxFeature>(
        "Good", glm::vec3(50.0f, 0.0f, 0.0f), 5.0, 5.0, 5.0));
    REQUIRE(testio::Save(doc, path.string()));
  }
  std::string text;
  {
    std::ifstream in(path);
    text.assign(std::istreambuf_iterator<char>(in), {});
  }
  const std::string from = "\"width_expr\": \"len\"";
  const size_t at = text.find(from);
  REQUIRE(at != std::string::npos);
  text.replace(at, from.size(), "\"width_expr\": \"len *\"");
  {
    std::ofstream out(path);
    out << text;
  }
  const auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  REQUIRE(loaded->FeatureCount() == 2);
  const Feature* bad = loaded->GetFeature(0);
  CHECK(!bad->GetError().empty());
  CHECK(static_cast<const BoxFeature*>(bad)->width.expr == "len *");
  CHECK(loaded->GetFeature(1)->GetError().empty());
  CHECK(!loaded->GetBodies().empty()); // the good box built
  // And it writes back what it read: the text is kept, not dropped.
  REQUIRE(testio::Save(*loaded, path.string()));
  const auto again = testio::Load(path.string());
  REQUIRE(again != nullptr);
  CHECK(static_cast<const BoxFeature*>(again->GetFeature(0))->width.expr ==
        "len *");
  fs::remove(path);
}
