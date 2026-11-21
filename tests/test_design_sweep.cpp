// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// The design sweep (docs/requirements/README.md#design-sweep), through the
// RPC handler on an AppState with no server: the model regenerated over a
// variable's range on copies, where it breaks bracketed and named, and the
// user's document left as it was.

#include "cad/CADMaterials.h"
#include "core/DesignSweep.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/FilletFeature.h"
#include "core/features/SketchFeature.h"
#include "core/fem/SimDocument.h"
#include "remote/RemoteRequirements.h"
#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/SimRunActions.h"
#include "ui/SimulationActions.h"
#include "ui/VariableActions.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>

using Catch::Matchers::ContainsSubstring;
using nlohmann::json;

namespace {

struct App {
  ui::AppState state;
  App() {
    state.document = std::make_unique<Document>();
    state.commands = std::make_unique<CommandStack>();
    ui::EnsureParts(state);
  }
  json Call(const std::string& method, const json& params) {
    remote::RpcParams p{params};
    auto r = remote::HandleRequirementsRpc(state, method, p);
    REQUIRE(r.has_value());
    return *r;
  }
  std::string Refusal(const std::string& method, const json& params) {
    try {
      Call(method, params);
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  }
  uint64_t PartId() const { return state.parts[state.activePart].id; }
};

// A 20 x 20 block `t` thick with every edge filleted at `r`: the fillet
// cannot round a wall thinner than twice its radius.
void FilletedBlock(App& app) {
  REQUIRE(ui::SetProjectVariable(app.state, "t", "10", "mm, the wall"));
  REQUIRE(ui::SetProjectVariable(app.state, "r", "2", "mm, its edges"));
  Document& doc = *app.state.document;
  auto box =
      std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 20.0, 10.0, 20.0);
  box->height.expr = "t";
  doc.AddFeature(std::move(box));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  std::vector<ElementRef> edges;
  const size_t n = body.shape.GenerateEdgePolylines().size();
  for (size_t e = 0; e < n; e++)
    if (const std::string* name =
            body.elementMap.EdgeNameOf(static_cast<int>(e)))
      edges.push_back({body.createdBy, *name});
  REQUIRE(edges.size() == 12);
  auto fillet = std::make_unique<FilletFeature>("Edges round", edges, 2.0);
  fillet->radius.expr = "r";
  doc.AddFeature(std::move(fillet));
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE(doc.GetFeature(1)->GetError().empty());
}

const json* PointAt(const json& out, const std::string& name, double v) {
  for (const json& p : out.at("points"))
    if (std::abs(p.at("values").at(name).get<double>() - v) < 1e-9)
      return &p;
  return nullptr;
}

} // namespace

TEST_CASE("a sweep breaks where the fillet cannot round the wall, and names "
          "it",
          "[requirements][design_sweep]") {
  App app;
  FilletedBlock(app);
  const json out =
      app.Call("design_sweep",
               {{"variables", {{{"name", "t"}, {"min", 2.0}, {"max", 20.0}}}}});
  REQUIRE(out.at("points").size() == 5); // 2, 6.5, 11, 15.5, 20
  CHECK(out.at("base").at("ok") == true);
  const json* thin = PointAt(out, "t", 2.0);
  REQUIRE(thin != nullptr);
  CHECK(thin->at("ok") == false);
  CHECK(thin->at("parts")[0].at("regen") == "broken");
  CHECK(thin->at("parts")[0].at("broken")[0].at("name") == "Edges round");
  CHECK(PointAt(out, "t", 6.5)->at("ok") == true);
  const json& below = out.at("first_failure").at("t").at("below");
  // Between the samples either side of 2 r = 4.
  CHECK(below.at("failed").get<double>() == 2.0);
  CHECK(below.at("last_ok").get<double>() == 6.5);
  CHECK_THAT(below.at("why").get<std::string>(),
             ContainsSubstring("Edges round"));
  CHECK_FALSE(out.at("first_failure").at("t").contains("above"));

  // refine narrows the bracket round 2 r.
  const json fine =
      app.Call("design_sweep",
               {{"variables", {{{"name", "t"}, {"min", 2.0}, {"max", 20.0}}}},
                {"refine", 5}});
  const json& b = fine.at("first_failure").at("t").at("below");
  const double failed = b.at("failed").get<double>();
  const double ok = b.at("last_ok").get<double>();
  CHECK(ok - failed <= (6.5 - 2.0) / 32.0 + 1e-9);
  CHECK(failed <= 4.0 + 1e-6);
  CHECK(ok >= 4.0 - (6.5 - 2.0) / 32.0);
  CHECK(fine.at("points").size() == 10);
}

TEST_CASE("the user's document is as it was after a sweep",
          "[requirements][design_sweep]") {
  App app;
  FilletedBlock(app);
  Document& doc = *app.state.document;
  const uint64_t version = doc.Version();
  const double volume = doc.GetBodies()[0].shape.Volume();
  const auto params = app.state.projectParameters;
  app.Call("design_sweep",
           {{"variables", {{{"name", "t"}, {"min", 2.0}, {"max", 20.0}}}},
            {"samples", 3}});
  CHECK(app.state.document.get() == &doc);
  CHECK(doc.Version() == version);
  CHECK(doc.GetBodies().size() == 1);
  CHECK(doc.GetBodies()[0].shape.Volume() == volume);
  REQUIRE(app.state.projectParameters.size() == params.size());
  for (size_t i = 0; i < params.size(); i++) {
    CHECK(app.state.projectParameters[i].expression == params[i].expression);
    CHECK(app.state.projectParameters[i].value == params[i].value);
  }
  CHECK(app.state.projectSymbols.at("t") == 10.0);
}

#ifdef HAVE_PLANEGCS
TEST_CASE("a triangle swept over a side stops solving past the other two",
          "[requirements][design_sweep][sketch]") {
  App app;
  REQUIRE(ui::SetProjectVariable(app.state, "a", "10", ""));
  REQUIRE(ui::SetProjectVariable(app.state, "b", "10", ""));
  REQUIRE(ui::SetProjectVariable(app.state, "c", "12", ""));
  auto sk =
      std::make_unique<SketchFeature>("Triangle", SketchPlane(PlaneType::XY));
  const uint32_t l1 = sk->AddEntity(sketch::Line{{0, 0}, {12, 0}});
  const uint32_t l2 = sk->AddEntity(sketch::Line{{12, 0}, {6, 8}});
  const uint32_t l3 = sk->AddEntity(sketch::Line{{6, 8}, {0, 0}});
  using K = sketch::ConstraintKind;
  sk->constraints.push_back({K::Coincident, {l1, 1}, {l2, 0}});
  sk->constraints.push_back({K::Coincident, {l2, 1}, {l3, 0}});
  sk->constraints.push_back({K::Coincident, {l3, 1}, {l1, 0}});
  const auto side = [&](uint32_t line, const char* expr) {
    sketch::Constraint d{K::Distance, {line, 0}, {line, 1}, 10.0};
    d.valueExpr = expr;
    sk->constraints.push_back(d);
  };
  side(l1, "c");
  side(l2, "a");
  side(l3, "b");
  app.state.document->AddFeature(std::move(sk));
  const json out =
      app.Call("design_sweep",
               {{"variables", {{{"name", "c"}, {"min", 5.0}, {"max", 30.0}}}},
                {"samples", 6}}); // 5, 10, 15, 20, 25, 30
  const auto sketchOk = [&](double c) {
    const json* p = PointAt(out, "c", c);
    REQUIRE(p != nullptr);
    for (const json& s : p->at("parts")[0].value("sketches", json::array()))
      if (!s.at("ok").get<bool>())
        return false;
    return true;
  };
  CHECK(sketchOk(15.0));
  CHECK_FALSE(sketchOk(25.0));
  CHECK_FALSE(sketchOk(30.0));
  CHECK(PointAt(out, "c", 25.0)->at("ok") == false);
  const json& above = out.at("first_failure").at("c").at("above");
  CHECK(above.at("failed").get<double>() > 20.0 - 1e-9);
  CHECK(above.at("last_ok").get<double>() <= 20.0 + 1e-9);
  CHECK_THAT(above.at("why").get<std::string>(), ContainsSubstring("Triangle"));
}
#endif

TEST_CASE("corners add the box's 2^n corners and its centre",
          "[requirements][design_sweep]") {
  for (size_t n = 1; n <= 3; n++) {
    sweep::Spec spec;
    spec.samples = 2;
    spec.corners = true;
    for (size_t i = 0; i < n; i++)
      spec.variables.push_back({"v" + std::to_string(i), 1.0, 2.0, 0, 1.5});
    const auto chunks = sweep::PlanChunks(spec);
    REQUIRE(chunks.size() == n + 1);
    CHECK(chunks.back().variable == -1);
    CHECK(chunks.back().points.size() == (size_t{1} << n) + 1);
    CHECK(sweep::PlannedCount(spec) == 2 * n + (size_t{1} << n) + 1);
  }
}

TEST_CASE("from_requirements takes a within range and refuses a one-sided "
          "one",
          "[requirements][design_sweep]") {
  App app;
  REQUIRE(ui::SetProjectVariable(app.state, "t", "10", ""));
  REQUIRE(ui::SetProjectVariable(app.state, "od", "18", ""));
  auto box =
      std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 20.0, 10.0, 20.0);
  box->height.expr = "t";
  app.state.document->AddFeature(std::move(box));
  app.Call("add_requirement",
           {{"name", "wall"},
            {"measure", {{"kind", "expression"}, {"expression", "t"}}},
            {"bound", {{"kind", "within"}, {"min", 5}, {"max", "od - 3"}}}});
  const json plan =
      app.Call("design_sweep", {{"from_requirements", true}, {"samples", 2}});
  REQUIRE(plan.at("variables").size() == 1);
  CHECK(plan.at("variables")[0].at("name") == "t");
  CHECK(plan.at("variables")[0].at("min") == 5.0);
  CHECK(plan.at("variables")[0].at("max") == 15.0);

  App one;
  REQUIRE(ui::SetProjectVariable(one.state, "t", "10", ""));
  auto box2 =
      std::make_unique<BoxFeature>("Block", glm::dvec3(0.0), 20.0, 10.0, 20.0);
  box2->height.expr = "t";
  one.state.document->AddFeature(std::move(box2));
  one.Call("add_requirement",
           {{"name", "thick enough"},
            {"measure", {{"kind", "expression"}, {"expression", "t"}}},
            {"bound", {{"kind", "at_least"}, {"value", 4}}}});
  CHECK_THAT(one.Refusal("design_sweep", {{"from_requirements", true}}),
             ContainsSubstring("one side only; give its max"));
  // Its other end in variables completes it.
  const json both =
      one.Call("design_sweep", {{"from_requirements", true},
                                {"samples", 2},
                                {"variables", {{{"name", "t"}, {"max", 12}}}}});
  CHECK(both.at("variables")[0].at("min") == 4.0);
  CHECK(both.at("variables")[0].at("max") == 12.0);
}

TEST_CASE("a design sweep is refused before anything starts",
          "[requirements][design_sweep]") {
  App app;
  FilletedBlock(app);
  REQUIRE(ui::SetProjectVariable(app.state, "unused", "3", ""));
  const json t{{"name", "t"}, {"min", 2.0}, {"max", 20.0}};
  CHECK_THAT(
      app.Refusal("design_sweep",
                  {{"variables", {{{"name", "tt"}, {"min", 1}, {"max", 2}}}}}),
      ContainsSubstring("variables in scope: t, r, unused"));
  CHECK_THAT(app.Refusal("design_sweep",
                         {{"variables",
                           {{{"name", "unused"}, {"min", 1}, {"max", 2}}}}}),
             ContainsSubstring("no part reads 'unused'"));
  CHECK_THAT(
      app.Refusal("design_sweep",
                  {{"variables", {{{"name", "t"}, {"min", 5}, {"max", 2}}}}}),
      ContainsSubstring("is above max"));
  CHECK_THAT(app.Refusal("design_sweep", {{"variables", {t}}, {"samples", 40}}),
             ContainsSubstring("`samples` is a whole number from 2 to 33"));
  CHECK_THAT(app.Refusal("design_sweep", {{"variables", {t}}, {"refine", 9}}),
             ContainsSubstring("`refine` is a whole number from 0 to 8"));
  CHECK_THAT(
      app.Refusal("design_sweep",
                  {{"variables", {t}}, {"samples", 33}, {"max_points", 20}}),
      ContainsSubstring("would run 33 points, over the cap of 20"));
  CHECK_THAT(
      app.Refusal("design_sweep", {{"variables", {t}}, {"documents", {99}}}),
      ContainsSubstring("document 99 is gone"));
  CHECK_THAT(
      app.Refusal("design_sweep",
                  {{"variables",
                    {{{"name", "t"}, {"min", 1}, {"max", 2}, {"step", 1}}}}}),
      ContainsSubstring(
          "unknown key 'step' (accepted: name, min, max, scope)"));
  CHECK_THAT(app.Refusal("design_sweep", {{"from_requirements", true}}),
             ContainsSubstring("no expression requirement bounds one bare"));
  // A configuration that holds the variable: the sweep would change nothing.
  Document& doc = *app.state.document;
  Configuration& c = doc.AddConfiguration("Thick");
  c.parameterOverrides.push_back({"t", "12"});
  const uint64_t cid = c.id;
  REQUIRE(doc.SetActiveConfiguration(cid));
  CHECK_THAT(app.Refusal("design_sweep", {{"variables", {t}}}),
             ContainsSubstring("configuration 'Thick', which overrides 't'"));
}

#ifdef HAVE_NETGEN
namespace {

// A steel plate 60 long, 20 wide and `t` thick (a project variable), a
// cantilever: clamped at x = 0, pushed across its thickness at x = 60 by
// 40 N, in a simulation whose study states two requirements, its factor of
// safety and its deflection. Returns the study's id.
uint64_t WallCantilever(App& app, double t) {
  REQUIRE(ui::SetProjectVariable(app.state, "t", std::to_string(t),
                                 "mm, the wall"));
  Document& doc = *app.state.document;
  auto box =
      std::make_unique<BoxFeature>("Wall", glm::dvec3(0.0), 60.0, t, 20.0);
  box->height.expr = "t";
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies().at(0);
  ElementRef x0, x1;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && std::abs(info->centroid.x) < 1e-6)
      x0 = ElementRef{body.createdBy, name};
    if (info && std::abs(info->centroid.x - 60.0) < 1e-6)
      x1 = ElementRef{body.createdBy, name};
  }
  REQUIRE_FALSE(x0.name.empty());
  REQUIRE_FALSE(x1.name.empty());
  fem::SimDocument sim;
  sim.SetSubject({fem::SubjectKind::Part, app.PartId(), 0, "Wall"});
  sim.SetMesh({ExprValue{1.5}, {}});
  fem::Study& st = sim.AddStudy("Bend");
  st.converge = fem::ConvergeMode::Never;
  fem::Load fixed;
  fixed.id = sim.AllocateId();
  fixed.type = fem::LoadType::Fixed;
  fixed.face = x0;
  st.loads.push_back(fixed);
  fem::Load force;
  force.id = sim.AllocateId();
  force.type = fem::LoadType::Force;
  force.face = x1;
  force.magnitude = 40.0;
  force.direction = glm::dvec3(0, -1, 0);
  st.loads.push_back(force);
  const uint64_t sid = st.id;
  REQUIRE(ui::AddSimulation(app.state, "Wall simulation", std::move(sim)) == 1);
  ui::SetActivePart(app.state, 0);
  app.Call("add_requirement",
           {{"name", "wall holds"},
            {"measure",
             {{"kind", "study_result"},
              {"document", app.PartId()},
              {"study", sid},
              {"quantity", "factor_of_safety"}}},
            {"bound", {{"kind", "at_least"}, {"value", 2.0}}}});
  app.Call("add_requirement",
           {{"name", "tip deflection"},
            {"measure",
             {{"kind", "study_result"},
              {"document", app.PartId()},
              {"study", sid},
              {"quantity", "max_displacement"}}},
            {"bound", {{"kind", "at_most"}, {"value", 5.0}}}});
  return sid;
}

double Reading(const json& point, const std::string& name) {
  for (const json& r : point.at("requirements"))
    if (r.at("name") == name) {
      INFO(r.dump());
      REQUIRE(r.contains("value"));
      return r.at("value").get<double>();
    }
  FAIL("no requirement " << name);
  return 0.0;
}

} // namespace

// A sweep of a wall's thickness solves its study at each point
// (docs/requirements/README.md#design-sweep): each point's deflection is
// the cantilever's F L^3 / (3 E I), its factor of safety grows with the
// wall, a stored result answers the point it was solved at, and the same
// sweep again solves nothing.
TEST_CASE("a sweep of wall thickness solves its study at every point, "
          "reused on a rerun",
          "[requirements][design_sweep][fem][slow]") {
  App app;
  const uint64_t sid = WallCantilever(app, 4.0);
  // The base solved in the app first: its stored result answers that point.
  std::string err;
  const uint64_t simSlot = app.state.parts.at(1).id;
  REQUIRE(ui::RunSimStudyAndCache(app.state, simSlot, sid, &err));
  INFO(err);
  ui::SetActivePart(app.state, 0);
  const json req = {
      {"variables", {{{"name", "t"}, {"min", 2.0}, {"max", 6.0}}}},
      {"samples", 3},
      {"study_budget_s", 600}};
  const json out = app.Call("design_sweep", req);
  INFO(out.dump(1));
  REQUIRE(out.contains("studies"));
  CHECK(out.at("not_swept").empty());
  // 2, 4, 6 and the base (4, from the stored result): two solves.
  CHECK(out.at("studies").at("solved") == 2);
  CHECK(out.at("studies").at("reused").get<int>() >= 2);
  CHECK(out.at("studies").at("budget_spent") == false);
  const double E = 1000.0 * CADMaterials::MaterialLibrary::GetMaterial(
                                CADMaterials::MaterialType::STEEL)
                                .youngGPa;
  const double F = 40.0, L = 60.0, b = 20.0;
  double lastFos = 0.0;
  for (double t : {2.0, 4.0, 6.0}) {
    const json* p = PointAt(out, "t", t);
    REQUIRE(p != nullptr);
    const double I = b * t * t * t / 12.0;
    const double closed = F * L * L * L / (3.0 * E * I);
    const double got = Reading(*p, "tip deflection");
    INFO("t " << t << ": deflection " << got << " mm, F L^3/3EI " << closed);
    // A wall 20 wide is a little stiffer than Euler-Bernoulli's beam (the
    // clamp's Poisson hold, a plate's anticlastic bending): 1-3 % here.
    CHECK(std::abs(got - closed) < 0.05 * closed);
    const double fos = Reading(*p, "wall holds");
    CHECK(fos > lastFos);
    lastFos = fos;
  }
  // The thin wall fails its factor of safety (yield over a peak that grows
  // as 1/t^2), the thick one holds.
  CHECK(PointAt(out, "t", 2.0)->at("ok") == false);
  CHECK(PointAt(out, "t", 6.0)->at("ok") == true);

  // Again: nothing solved.
  const json again = app.Call("design_sweep", req);
  CHECK(again.at("studies").at("solved") == 0);
  for (double t : {2.0, 4.0, 6.0})
    CHECK(Reading(*PointAt(again, "t", t), "tip deflection") ==
          Reading(*PointAt(out, "t", t), "tip deflection"));
}

// What would make a sweep that solves studies unbounded is refused before
// anything starts: no budget stated, and a study that refines until it
// settles however long that takes. A budget on a sweep that solves nothing
// is refused too.
TEST_CASE("a sweep that solves studies states its budget and refuses "
          "converge always",
          "[requirements][design_sweep][fem]") {
  App app;
  const uint64_t sid = WallCantilever(app, 4.0);
  const json vars = {{{"name", "t"}, {"min", 2.0}, {"max", 6.0}}};
  CHECK_THAT(app.Refusal("design_sweep", {{"variables", vars}}),
             ContainsSubstring("study_budget_s"));
  CHECK_THAT(app.Refusal("design_sweep",
                         {{"variables", vars}, {"study_budget_s", -1}}),
             ContainsSubstring("above 0"));
  fem::SimDocument* sim = ui::SimBySlotId(app.state, app.state.parts[1].id);
  REQUIRE(sim != nullptr);
  fem::Study st = *sim->FindStudy(sid);
  st.converge = fem::ConvergeMode::Always;
  REQUIRE(sim->ReplaceStudy(st));
  CHECK_THAT(app.Refusal("design_sweep",
                         {{"variables", vars}, {"study_budget_s", 60}}),
             ContainsSubstring("converges always"));
  CHECK(ui::FindBackgroundJob(app.state, "design_sweep") == nullptr);
}
#endif // HAVE_NETGEN
