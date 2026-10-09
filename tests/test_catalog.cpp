// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/catalog/Catalog.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
using catalog::Json;
namespace {
Json Options(std::string storage = "reference") {
  return {{"id", "synthetic"},    {"revision", "1"}, {"source", "test fixture"},
          {"license", "CC0-1.0"}, {"units", "mm"},   {"storage", storage}};
}
std::string Rows() {
  return R"([{"designation":"B-25","bore_diameter":25,"outer_diameter":52,"width":15,"shaft_min":24.99,"shaft_max":25.01}])";
}
} // namespace
TEST_CASE("Catalog SHA256 matches published test vectors", "[catalog]") {
  CHECK(catalog::Sha256("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(catalog::Sha256("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(catalog::Sha256(std::string(1000000, 'a')) ==
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}
TEST_CASE("Bearing catalog normalizes mapped quoted CSV and explicit units",
          "[catalog]") {
  auto o = Options();
  o["units"] = "in";
  o["mapping"] = {{"designation", "part"},
                  {"bore_diameter", "d"},
                  {"outer_diameter", "D"},
                  {"width", "B"}};
  auto c = catalog::Preview(
      "part,d,D,B,extra\r\n\"Bearing, A\",1,2,0.5,uncertified\r\n", "csv", o);
  auto r = catalog::Record(c, "Bearing, A");
  CHECK_THAT(r["bore_diameter"].get<double>(),
             Catch::Matchers::WithinAbs(25.4, 1e-12));
  CHECK(r["unsupported_fields"] == Json::array({"extra"}));
  CHECK(c["storage"] == "reference");
  CHECK_THROWS(catalog::Preview("a,a\nx,y", "csv", o));
  CHECK_THROWS(catalog::Preview("a,b\n\"broken", "csv", o));
  CHECK_THROWS(catalog::Preview("a,b\nx,y,z", "csv", o));
}
TEST_CASE("Catalog rejects incorrect geometry and ambiguous limits",
          "[catalog]") {
  auto r = Json::parse(Rows());
  for (const auto* field : {"bore_diameter", "outer_diameter", "width"}) {
    auto bad = r;
    bad[0].erase(field);
    CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  }
  auto bad = r;
  bad[0]["outer_diameter"] = 20;
  CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  bad = r;
  bad[0]["width"] = "NaN";
  CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  bad = r;
  bad[0]["shaft_min"] = 26;
  CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  bad = r;
  bad[0].erase("shaft_max");
  CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  bad = r;
  bad.push_back(r[0]);
  CHECK_THROWS(catalog::Preview(bad.dump(), "json", Options()));
  auto o = Options();
  o["units"] = "m";
  CHECK_THROWS(catalog::Preview(Rows(), "json", o));
  o = Options();
  o["typo"] = 12;
  CHECK_THROWS(catalog::Preview(Rows(), "json", o));
}
TEST_CASE(
    "Catalog references omit private records and require explicit resolution",
    "[catalog]") {
  auto c = catalog::Preview(Rows(), "json", Options());
  Json all = Json::array();
  REQUIRE(catalog::Install(all, c));
  auto stored = catalog::Serialize(all);
  CHECK_FALSE(stored[0].contains("records"));
  auto loaded = catalog::Deserialize(stored);
  CHECK_THROWS(catalog::Record(loaded[0], "B-25"));
  REQUIRE(catalog::Install(loaded, c));
  CHECK(catalog::Record(loaded[0], "B-25")["width"] == 15);
  CHECK_FALSE(catalog::Install(loaded, c));
  auto changed = Json::parse(Rows());
  changed[0]["width"] = 16;
  CHECK_THROWS(catalog::Install(
      loaded, catalog::Preview(changed.dump(), "json", Options())));
  CHECK(loaded[0] == c);
}
TEST_CASE("Embedded catalog validates content on reload", "[catalog]") {
  auto c = catalog::Preview(Rows(), "json", Options("embedded"));
  auto stored = catalog::Serialize(Json::array({c}));
  CHECK(catalog::Deserialize(stored) == Json::array({c}));
  stored[0]["records"][0]["width"] = 100;
  CHECK_THROWS(catalog::Deserialize(stored));
}
TEST_CASE(
    "Bearing requirement preview distinguishes missing evidence from passing",
    "[catalog]") {
  auto c = catalog::Preview(Rows(), "json", Options());
  auto r = catalog::SeatRequirements(c, "B-25", {{"shaft_diameter", 25}});
  CHECK(r["status"] == "unverified");
  CHECK(r["checks"][0]["status"] == "pass");
  CHECK(r["checks"][1]["status"] == "unverified");
  r = catalog::SeatRequirements(c, "B-25", {{"shaft_diameter", 26}});
  CHECK(r["status"] == "fail");
  CHECK_THROWS(catalog::SeatRequirements(c, "B-25", {{"shaft_radius", 12.5}}));
}

#include "ui/AppState.h"
#include "ui/CatalogActions.h"
TEST_CASE("Catalog installation and undo preserve nominal geometry version",
          "[catalog]") {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  auto before = state.document->Version();
  auto c = catalog::Preview(Rows(), "json", Options());
  CHECK(ui::InstallCatalog(state, c));
  CHECK(state.document->Version() == before);
  CHECK(state.projectMetaDirty);
  CHECK(ui::UndoCatalog(state));
  CHECK(state.catalogs.empty());
  CHECK(state.document->Version() == before);
  CHECK(ui::RedoCatalog(state));
  CHECK(state.catalogs.size() == 1);
  CHECK(state.document->Version() == before);
}

TEST_CASE(
    "Catalog packages default to reference storage and retain pinned identity",
    "[catalog]") {
  const auto entry = catalog::Preview(Rows(), "json", Options("embedded"));
  auto package = Json{{"format", "designer-catalog"},
                      {"version", 1},
                      {"catalogs", Json::array({entry})}};
  auto preview = catalog::Preview(package.dump(), "json", Json::object());
  CHECK(preview["storage"] == "reference");
  CHECK(preview["hash"] == entry["hash"]);
  CHECK(preview.contains("records"));
  CHECK_FALSE(
      catalog::Serialize(Json::array({preview}))[0].contains("records"));
  package["catalogs"] = catalog::Serialize(Json::array({preview}));
  preview = catalog::Preview(package.dump(), "json", Json::object());
  CHECK_FALSE(preview.contains("records"));
  Json entries = Json::array();
  CHECK(catalog::Install(entries, preview));
  CHECK_FALSE(catalog::Install(entries, preview));
  CHECK_THROWS(
      catalog::Preview(package.dump(), "json", {{"storage", "embedded"}}));
}

TEST_CASE("Bearing limits accept a complete seat and zero fillet radius",
          "[catalog]") {
  auto rows = Json::parse(Rows());
  rows[0]["housing_min"] = 51.99;
  rows[0]["housing_max"] = 52.01;
  rows[0]["shoulder_min"] = 30;
  rows[0]["shoulder_max"] = 35;
  rows[0]["fillet_radius_max"] = 0;
  auto c = catalog::Preview(rows.dump(), "json", Options());
  Json observed = {{"shaft_diameter", 25},
                   {"housing_diameter", 52},
                   {"shoulder_diameter", 32},
                   {"fillet_radius", 0}};
  CHECK(catalog::SeatRequirements(c, "B-25", observed)["status"] == "pass");
  observed["fillet_radius"] = 0.01;
  CHECK(catalog::SeatRequirements(c, "B-25", observed)["status"] == "fail");
  observed["fillet_radius"] = -1;
  CHECK_THROWS(catalog::SeatRequirements(c, "B-25", observed));
}
