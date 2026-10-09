// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Feature templates (docs/modeling/templates.md): a composite saved once as
// data and placed as one `Template` feature that expands, in its own
// namespace, into the features it was saved from. Pinned here: a seat saved
// from one part places twice on another with different values and distinct
// face names; an upstream edit rebuilds both and a later feature on one's
// face follows it; a changed template changes nothing until the instance is
// updated, and an update keeps the names; unlink hands over the same
// features with the same geometry; a bad parameter is refused naming what is
// accepted; a save and reload rebuilds the same; a lost reference is an
// error; and a template stands on its own floor (a host reference) and takes
// a string.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/Template.h"
#include "core/features/TemplateFeature.h"
#include "core/io/Serialization.h"

#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinRel;
using nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr double kPi = std::numbers::pi;

json Plane() {
  return {{"origin", {0.0, 0.0, 0.0}},
          {"normal", {0.0, 0.0, 1.0}},
          {"uAxis", {1.0, 0.0, 0.0}}};
}

json Line(double ax, double ay, double bx, double by) {
  return {{"type", "line"}, {"a", {ax, ay}}, {"b", {bx, by}}};
}

uint64_t Add(Document& doc, const json& j) {
  doc.AddFeature(DocumentIO::FeatureFromJson(j));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// A w x d block, `t` high (an expression), on the Top plane: returns the
// extrude's id, which is the body's created_by.
uint64_t Block(Document& doc, double w, double d, const std::string& t) {
  const uint64_t sk = Add(doc, {{"type", "Sketch"},
                                {"name", "Block"},
                                {"plane", Plane()},
                                {"entities",
                                 {Line(-w / 2, -d / 2, w / 2, -d / 2),
                                  Line(w / 2, -d / 2, w / 2, d / 2),
                                  Line(w / 2, d / 2, -w / 2, d / 2),
                                  Line(-w / 2, d / 2, -w / 2, -d / 2)}}});
  return Add(doc, {{"type", "Extrude"},
                   {"name", "Block"},
                   {"sketch_id", sk},
                   {"distance", 1.0},
                   {"distance_expr", t},
                   {"mode", "new_body"}});
}

// The seat: a circle of diameter D on `face`, cut B deep into the body.
std::pair<uint64_t, uint64_t> Seat(Document& doc, uint64_t body,
                                   const std::string& face) {
  const uint64_t sk =
      Add(doc, {{"type", "Sketch"},
                {"name", "Seat"},
                {"plane", Plane()},
                {"face_ref", {{"feature_id", body}, {"element", face}}},
                {"entities",
                 {{{"type", "circle"},
                   {"center", {0.0, 0.0}},
                   {"radius", 1.0},
                   {"r_expr", "D / 2"}}}}});
  const uint64_t cut = Add(doc, {{"type", "Extrude"},
                                 {"name", "Seat"},
                                 {"sketch_id", sk},
                                 {"distance", 1.0},
                                 {"distance_expr", "B"},
                                 {"mode", "cut"},
                                 {"reversed", true}});
  return {sk, cut};
}

double Volume(const Document& doc) {
  double v = 0.0;
  for (const Body& b : doc.GetBodies())
    v += b.shape.Volume();
  return v;
}

std::string Cap(uint64_t extrude, int k) {
  return std::to_string(extrude) + "/cap" + std::to_string(k);
}

// The source part: a 40 x 20 x 10 plate with a seat on its top.
struct Source {
  Document doc;
  uint64_t plate = 0, sketch = 0, cut = 0;
};
std::unique_ptr<Source> MakeSource() {
  auto s = std::make_unique<Source>();
  REQUIRE(s->doc.SetParameter("D", "10"));
  REQUIRE(s->doc.SetParameter("B", "4"));
  s->plate = Block(s->doc, 40.0, 20.0, "10");
  std::tie(s->sketch, s->cut) = Seat(s->doc, s->plate, Cap(s->plate, 1));
  REQUIRE(s->doc.Regenerate());
  return s;
}

tmpl::Template SaveSeat(const Source& s) {
  tmpl::SaveSpec spec;
  spec.name = "bearing seat";
  spec.featureIds = {s.cut, s.sketch}; // any order: kept in the document's
  spec.literals.push_back({s.sketch, "entities/0/cx", "u"});
  return tmpl::FromFeatures(s.doc, spec);
}

// Places `t` on `on` with the values given; returns the instance's id.
uint64_t Place(Document& doc, const tmpl::Template& t, const json& params,
               const json& on) {
  auto tf = std::make_unique<TemplateFeature>(t.name);
  tf->templateName = t.name;
  tf->definition = t;
  tf->values = tmpl::ValuesFromRequest(t, params, &on);
  tf->AssignIds([&] { return doc.AllocateId(); });
  doc.AddFeature(std::move(tf));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

TemplateFeature& Instance(Document& doc, uint64_t id) {
  auto* tf = dynamic_cast<TemplateFeature*>(doc.FindFeatureById(id));
  REQUIRE(tf != nullptr);
  return *tf;
}

// A face of `doc`'s one body whose name starts with `prefix`.
std::string FaceStarting(const Document& doc, const std::string& prefix) {
  for (const Body& b : doc.GetBodies())
    for (const auto& [i, n] : b.elementMap.names)
      if (n.rfind(prefix, 0) == 0)
        return n;
  return {};
}

// The seat's floor: the cut's cap that is still a face of the body.
std::string Floor(const Document& doc, uint64_t cut) {
  for (int k : {0, 1}) {
    const std::string n = FaceStarting(doc, Cap(cut, k));
    if (!n.empty())
      return n;
  }
  return {};
}

fs::path TempPath(const std::string& name) {
  const fs::path p = fs::temp_directory_path() / name;
  std::error_code ec;
  fs::remove_all(p, ec);
  return p;
}

} // namespace

TEST_CASE("RewriteIds maps every id a name mentions and nothing else",
          "[templates]") {
  const auto plus = [](uint64_t id) { return id + 100; };
  CHECK(topo::RewriteIds("4/g7", plus) == "104/g7");
  CHECK(topo::RewriteIds("4/g7;M6;G9", plus) == "104/g7;M106;G109");
  CHECK(topo::RewriteIds("4/g7#2;S3", plus) == "104/g7#2;S103");
  CHECK(topo::RewriteIds("4/cap1;U2,5/g3;U0;L", plus) ==
        "104/cap1;U2,105/g3;U0;L");
  CHECK(topo::RewriteIds("4/p12/bore", plus) == "104/p12/bore");
}

TEST_CASE("a saved template is data: local ids, parameters, its reference",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);
  CHECK(tmpl::Validate(t).empty());
  REQUIRE(t.features.size() == 2);
  CHECK(t.features[0]["type"] == "Sketch");
  CHECK(t.features[0]["id"] == 1);
  CHECK(t.features[1]["sketch_id"] == 1); // the extrude names ITS sketch
  CHECK(t.features[1]["distance_expr"] == "B");
  // The variables the features read, the literal chosen, and what the
  // sketch stood on.
  REQUIRE(t.FindParam("D") != nullptr);
  CHECK(t.FindParam("D")->kind == tmpl::ParamKind::Length);
  CHECK(t.FindParam("D")->example == 10.0);
  REQUIRE(t.FindParam("B") != nullptr);
  REQUIRE(t.FindParam("u") != nullptr);
  REQUIRE(t.FindParam("on") != nullptr);
  CHECK(t.FindParam("on")->kind == tmpl::ParamKind::Reference);
  CHECK(t.FindParam("on")->example["element"] == Cap(src->plate, 1));
  REQUIRE(t.bindings.size() == 1);
  CHECK(t.bindings[0].path == tmpl::kAttach);
  CHECK_FALSE(t.features[0].contains("face_ref"));
  // It round-trips as JSON.
  CHECK(tmpl::FromJson(tmpl::ToJson(t)) == t);
}

TEST_CASE("two instances on two faces build their own seats, each named "
          "under its own ids, and follow an upstream edit",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);

  Document doc;
  REQUIRE(doc.SetParameter("h", "20"));
  const uint64_t block = Block(doc, 60.0, 30.0, "h");
  const uint64_t a = Place(doc, t, {{"D", 12.0}, {"B", 5.0}, {"u", -8.0}},
                           {{"feature_id", block}, {"element", Cap(block, 1)}});
  const uint64_t b = Place(doc, t, {{"D", "h / 2.5"}, {"B", 3.0}, {"u", 8.0}},
                           {{"feature_id", block}, {"element", Cap(block, 0)}});
  REQUIRE(doc.Regenerate());
  REQUIRE(doc.GetBodies().size() == 1);
  const auto expect = [](double h) {
    return 60.0 * 30.0 * h - kPi * 36.0 * 5.0 -
           kPi * std::pow(h / 5.0, 2) * 3.0;
  };
  CHECK_THAT(Volume(doc), WithinRel(expect(20.0), 1e-9));

  TemplateFeature& A = Instance(doc, a);
  TemplateFeature& B = Instance(doc, b);
  REQUIRE(A.Expanded().size() == 2);
  // Each instance's features have ids of their own, and the faces they cut
  // are named under them: distinct, so a reference to one is unambiguous.
  const uint64_t cutA = A.IdOf(2), cutB = B.IdOf(2);
  CHECK(cutA != 0);
  CHECK(cutB != 0);
  CHECK(cutA != cutB);
  CHECK(A.Expanded()[1]->GetId() == cutA);
  CHECK(doc.FindFeatureById(cutA) == nullptr); // owned, not in the tree
  CHECK(doc.OwnerIndex(cutA) == doc.OwnerIndex(a));
  const std::string floorA = Floor(doc, cutA), floorB = Floor(doc, cutB);
  CHECK_FALSE(floorA.empty());
  CHECK_FALSE(floorB.empty());
  CHECK(floorA != floorB);

  // A later feature stands on instance A's floor: a relief cut 1 deep.
  const uint64_t relief = Add(
      doc, {{"type", "Sketch"},
            {"name", "Relief"},
            {"plane", Plane()},
            {"face_ref", {{"feature_id", block}, {"element", floorA}}},
            {"entities",
             {{{"type", "circle"}, {"center", {0.0, 0.0}}, {"radius", 2.0}}}}});
  Add(doc, {{"type", "Extrude"},
            {"name", "Relief"},
            {"sketch_id", relief},
            {"distance", 1.0},
            {"mode", "cut"},
            {"reversed", true}});
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Volume(doc), WithinRel(expect(20.0) - kPi * 4.0, 1e-9));

  // Upstream: the block 5 higher. Both instances rebuild (B's diameter is
  // an expression of h), and the relief follows A's floor.
  REQUIRE(doc.SetParameter("h", "25"));
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Volume(doc), WithinRel(expect(25.0) - kPi * 4.0, 1e-9));
  for (const auto& f : doc.GetFeatures())
    CHECK(f->GetError().empty());
}

TEST_CASE("a changed template changes nothing until the instance is updated, "
          "and the update keeps its names",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template rev1 = SaveSeat(*src);

  Document doc;
  const uint64_t block = Block(doc, 60.0, 30.0, "20");
  const uint64_t a = Place(doc, rev1, {{"D", 12.0}, {"B", 5.0}, {"u", 0.0}},
                           {{"feature_id", block}, {"element", Cap(block, 1)}});
  REQUIRE(doc.Regenerate());
  const double before = Volume(doc);
  const uint64_t cut = Instance(doc, a).IdOf(2);
  const std::string floor = Floor(doc, cut);
  const uint64_t relief = Add(
      doc, {{"type", "Sketch"},
            {"name", "Relief"},
            {"plane", Plane()},
            {"face_ref", {{"feature_id", block}, {"element", floor}}},
            {"entities",
             {{{"type", "circle"}, {"center", {0.0, 0.0}}, {"radius", 2.0}}}}});
  Add(doc, {{"type", "Extrude"},
            {"name", "Relief"},
            {"sketch_id", relief},
            {"distance", 1.0},
            {"mode", "cut"},
            {"reversed", true}});
  REQUIRE(doc.Regenerate());
  const double withRelief = Volume(doc);

  // The template changes: the seat one millimetre deeper than B.
  src->doc.FindFeatureById(src->cut);
  {
    json fj = DocumentIO::FeatureToJson(*src->doc.FindFeatureById(src->cut));
    fj["distance_expr"] = "B + 1";
    size_t index = 0;
    src->doc.FindFeatureById(src->cut, &index);
    REQUIRE(src->doc.ReplaceFeature(index, DocumentIO::FeatureFromJson(fj)));
    REQUIRE(src->doc.Regenerate());
  }
  tmpl::Template rev2 = SaveSeat(*src);
  rev2.revision = rev1.revision + 1;
  CHECK_FALSE(rev2 == rev1);

  // The library moved on; the instance did not. It still expands rev 1.
  doc.ClearRegenCache();
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Volume(doc), WithinRel(withRelief, 1e-12));
  CHECK(Instance(doc, a).definition == rev1);

  // Updated: the deeper seat, its features the same ids, so the relief on
  // its floor still finds it.
  REQUIRE(Instance(doc, a)
              .UpdateTo(rev2, {}, [&] { return doc.AllocateId(); })
              .empty());
  REQUIRE(doc.Regenerate());
  CHECK(Instance(doc, a).IdOf(2) == cut);
  CHECK_THAT(Volume(doc), WithinRel(withRelief - kPi * 36.0, 1e-9));
  for (const auto& f : doc.GetFeatures())
    CHECK(f->GetError().empty());
  (void)before;
}

TEST_CASE("unlink hands the tree the same features: same ids, names and "
          "geometry, reading the document's variables",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);
  Document doc;
  REQUIRE(doc.SetParameter("bore", "14"));
  const uint64_t block = Block(doc, 60.0, 30.0, "20");
  const uint64_t a = Place(doc, t, {{"D", "bore"}, {"B", 5.0}, {"u", 0.0}},
                           {{"feature_id", block}, {"element", Cap(block, 1)}});
  REQUIRE(doc.Regenerate());
  const double v0 = Volume(doc);
  const uint64_t cut = Instance(doc, a).IdOf(2);
  const std::string floor = Floor(doc, cut);

  std::string why;
  std::vector<std::unique_ptr<Feature>> plain = Instance(doc, a).Unlinked(&why);
  REQUIRE(why.empty());
  REQUIRE(plain.size() == 2);
  size_t index = 0;
  doc.FindFeatureById(a, &index);
  {
    Document::BulkEdit bulk(doc);
    doc.RemoveFeature(index);
    for (size_t k = 0; k < plain.size(); k++)
      doc.InsertFeature(std::move(plain[k]), index + k);
  }
  REQUIRE(doc.Regenerate());
  CHECK(doc.FindFeatureById(cut) != nullptr);
  CHECK_THAT(Volume(doc), WithinRel(v0, 1e-12));
  CHECK(Floor(doc, cut) == floor);
  // The diameter reads the document's variable now, not a parameter.
  const json sk = DocumentIO::FeatureToJson(*doc.GetFeature(index));
  CHECK(sk["entities"][0]["r_expr"] == "(bore) / 2");
  REQUIRE(doc.SetParameter("bore", "16"));
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Volume(doc), WithinRel(v0 - kPi * (64.0 - 49.0) * 5.0, 1e-9));
}

TEST_CASE("a bad parameter is refused naming what is accepted", "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);
  const json on = {{"feature_id", 1}, {"element", "1/cap1"}};
  const json ok = {{"D", 12.0}, {"B", 5.0}, {"u", 0.0}};
  CHECK_NOTHROW(tmpl::ValuesFromRequest(t, ok, &on));

  json unknown = ok;
  unknown["Q"] = 1.0;
  CHECK_THROWS_WITH(tmpl::ValuesFromRequest(t, unknown, &on),
                    ContainsSubstring("no parameter 'Q'") &&
                        ContainsSubstring("D (length)"));
  json missing = ok;
  missing.erase("B");
  CHECK_THROWS_WITH(tmpl::ValuesFromRequest(t, missing, &on),
                    ContainsSubstring("missing: B (length, saved as 4)"));
  json wrong = ok;
  wrong["D"] = true;
  CHECK_THROWS_WITH(tmpl::ValuesFromRequest(t, wrong, &on),
                    ContainsSubstring("is a length"));
  const json diagonal = "Diagonal";
  CHECK_THROWS_WITH(tmpl::ValuesFromRequest(t, ok, &diagonal),
                    ContainsSubstring("\"Top\""));
  CHECK_THROWS_WITH(tmpl::ValuesFromRequest(t, ok, nullptr),
                    ContainsSubstring("missing: on (reference"));
}

TEST_CASE("a save, reload and regenerate gives the same geometry and names",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);
  Document doc;
  doc.SetTemplatesRaw({t});
  const uint64_t block = Block(doc, 60.0, 30.0, "20");
  const uint64_t a = Place(doc, t, {{"D", 12.0}, {"B", 5.0}, {"u", -8.0}},
                           {{"feature_id", block}, {"element", Cap(block, 1)}});
  Place(doc, t, {{"D", 8.0}, {"B", 3.0}, {"u", 8.0}},
        {{"feature_id", block}, {"element", Cap(block, 0)}});
  REQUIRE(doc.Regenerate());
  const double v = Volume(doc);
  const ElementMap names = doc.GetBodies()[0].elementMap;

  for (const char* form : {"templates.json", "templates.dsgn"}) {
    const fs::path path = TempPath(form);
    REQUIRE(testio::Save(doc, path.string()));
    auto back = testio::Load(path.string());
    REQUIRE(back != nullptr);
    REQUIRE(back->Regenerate());
    CHECK_THAT(Volume(*back), WithinRel(v, 1e-12));
    CHECK(back->GetBodies()[0].elementMap.names == names.names);
    CHECK(back->GetTemplates().size() == 1);
    CHECK(Instance(*back, a).definition == t);
    // What it is made of, whether or not it executed (the directory form
    // opens from baked geometry and executes nothing).
    const json rows = Instance(*back, a).ExpandedRows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[1]["id"] == Instance(*back, a).IdOf(2));
    CHECK(rows[1]["type"] == "Extrude");
    // Its id counter is past every id an instance owns.
    CHECK(back->AllocateId() > Instance(*back, a).IdOf(2));
    std::error_code ec;
    fs::remove_all(path, ec);
  }
}

TEST_CASE("a lost reference is the instance's error, never a fall-back",
          "[templates]") {
  auto src = MakeSource();
  const tmpl::Template t = SaveSeat(*src);
  Document doc;
  const uint64_t block = Block(doc, 60.0, 30.0, "20");
  const uint64_t a = Place(doc, t, {{"D", 12.0}, {"B", 5.0}, {"u", 0.0}},
                           {{"feature_id", block}, {"element", "99/cap1"}});
  CHECK_FALSE(doc.Regenerate());
  CHECK_THAT(Instance(doc, a).GetError(), ContainsSubstring("is gone"));
  CHECK_THAT(Volume(doc), WithinRel(60.0 * 30.0 * 20.0, 1e-12));
}

TEST_CASE("a template stands on a face it made itself, and takes a string",
          "[templates]") {
  // Seat plus relief: the relief is drawn on the seat's floor, which is a
  // face of the plate the template did not create -- found by its name
  // when the instance expands -- and how far it goes is a string.
  auto src = MakeSource();
  const std::string floor = Floor(src->doc, src->cut);
  REQUIRE_FALSE(floor.empty());
  const uint64_t rs =
      Add(src->doc,
          {{"type", "Sketch"},
           {"name", "Relief"},
           {"plane", Plane()},
           {"face_ref", {{"feature_id", src->plate}, {"element", floor}}},
           {"entities",
            {{{"type", "circle"}, {"center", {0.0, 0.0}}, {"radius", 2.0}}}}});
  const uint64_t rc = Add(src->doc, {{"type", "Extrude"},
                                     {"name", "Relief"},
                                     {"sketch_id", rs},
                                     {"distance", 2.0},
                                     {"end_mode", "through_all"},
                                     {"mode", "cut"},
                                     {"reversed", true}});
  REQUIRE(src->doc.Regenerate());
  tmpl::SaveSpec spec;
  spec.name = "seat and relief";
  spec.featureIds = {src->sketch, src->cut, rs, rc};
  spec.literals.push_back({src->sketch, "entities/0/cx", "u"});
  spec.strings.push_back({rc, "/end_mode", "relief", {"blind", "through_all"}});
  const tmpl::Template t = tmpl::FromFeatures(src->doc, spec);
  // The relief sketch stands on the template's own floor: no parameter.
  CHECK(t.FindParam("on2") == nullptr);
  CHECK(t.features[2]["face_ref"]["feature_id"] == 0);
  REQUIRE(t.FindParam("relief") != nullptr);
  CHECK(t.FindParam("relief")->kind == tmpl::ParamKind::String);

  Document doc;
  const uint64_t block = Block(doc, 60.0, 30.0, "20");
  const json on = {{"feature_id", block}, {"element", Cap(block, 1)}};
  Place(doc, t, {{"D", 12.0}, {"B", 5.0}, {"u", -8.0}, {"relief", "blind"}},
        on);
  REQUIRE(doc.Regenerate());
  // The first seat modified the top face, so it is named for that now
  // (docs/modeling/toponaming.md): the second stands on it by that name.
  const json top = {{"feature_id", block},
                    {"element", FaceStarting(doc, Cap(block, 1))}};
  Place(doc, t,
        {{"D", 12.0}, {"B", 5.0}, {"u", 8.0}, {"relief", "through_all"}}, top);
  REQUIRE(doc.Regenerate());
  CHECK_THAT(Volume(doc), WithinRel(60.0 * 30.0 * 20.0 - 2 * kPi * 36.0 * 5.0 -
                                        kPi * 4.0 * 2.0 - kPi * 4.0 * 15.0,
                                    1e-9));
  CHECK_THROWS_WITH(
      tmpl::ValuesFromRequest(
          t, {{"D", 12.0}, {"B", 5.0}, {"u", 0.0}, {"relief", "deep"}}, &on),
      ContainsSubstring("one of: blind, through_all"));
}

TEST_CASE("save refuses what a template cannot carry", "[templates]") {
  auto src = MakeSource();
  tmpl::SaveSpec spec;
  spec.name = "seat";
  // The cut alone: it names its sketch, outside the selection.
  spec.featureIds = {src->cut};
  CHECK_THROWS_WITH(tmpl::FromFeatures(src->doc, spec),
                    ContainsSubstring("outside the selection"));
  spec.featureIds = {12345};
  CHECK_THROWS_WITH(tmpl::FromFeatures(src->doc, spec),
                    ContainsSubstring("no feature with id 12345"));
  // A literal that is an expression already.
  spec.featureIds = {src->sketch, src->cut};
  spec.literals = {{src->cut, "distance", "depth"}};
  CHECK_THROWS_WITH(tmpl::FromFeatures(src->doc, spec),
                    ContainsSubstring("not a literal"));
}
