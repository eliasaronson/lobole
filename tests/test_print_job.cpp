// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Slicer settings, the printed mass and the print job's 3MF
// (docs/tolerances/README.md#slicer-settings, #printed-mass, #print-jobs).

#include "core/Feature.h"
#include "core/PrintAllowance.h"
#include "core/PrintCheck.h"
#include "core/PrintSettings.h"
#include "core/PrintedMass.h"
#include "core/io/PrintJob3mf.h"
#include "core/io/SerializePrintSettings.h"

#include <BRepPrimAPI_MakeBox.hxx>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstring>

using Catch::Approx;
using nlohmann::json;

namespace {

Body Box(double x, double y, double z, double dx, double dy, double dz,
         const char* name = "box") {
  Body b;
  b.name = name;
  b.shape = CADShape(BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape());
  return b;
}

std::string Refusal(const json& j) {
  try {
    (void)DocumentIO::PrintSettingsFromJson(j, "settings");
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

uint32_t Read32(const std::string& s, size_t at) {
  uint32_t v = 0;
  for (int k = 3; k >= 0; k--)
    v = (v << 8) | static_cast<unsigned char>(s[at + k]);
  return v;
}

} // namespace

TEST_CASE("Slicer settings are refused where the slicer would refuse them",
          "[print][settings]") {
  // Bambu refuses grid at 100 %: a pattern that cannot fill solid.
  CHECK(Refusal({{"infill_pct", 100}, {"infill_pattern", "grid"}})
            .find("cannot print 100 %") != std::string::npos);
  // ...and 100 % with no pattern leaves the preset's, which may be grid.
  CHECK(Refusal({{"infill_pct", 100}}).find("pattern stated") !=
        std::string::npos);
  CHECK(Refusal({{"infill_pct", 100}, {"infill_pattern", "rectilinear"}})
            .empty());
  CHECK(
      Refusal({{"infill_pct", 100}, {"infill_pattern", "concentric"}}).empty());
  CHECK(Refusal({{"infill_pct", 15}, {"infill_pattern", "grid"}}).empty());
  // An unknown pattern is refused naming the ones that exist.
  const std::string unknown = Refusal({{"infill_pattern", "zigzag"}});
  CHECK(unknown.find("gyroid") != std::string::npos);
  CHECK(unknown.find("rectilinear") != std::string::npos);
  CHECK(Refusal({{"brim", "mouse_ears"}}).find("ears") != std::string::npos);
  CHECK(!Refusal({{"walls", 0}}).empty());
  CHECK(!Refusal({{"top_layers", -1}}).empty());
  CHECK(!Refusal({{"walls", 2.5}}).empty());
  CHECK(!Refusal({{"infill_pct", 101}}).empty());
  CHECK(!Refusal({{"direction", {0, 0, 0}}}).empty());
  CHECK(Refusal({{"wals", 3}}).find("unknown key") != std::string::npos);

  // A body's override must hold over the part's.
  json part{{"part", {{"infill_pct", 100}, {"infill_pattern", "rectilinear"}}},
            {"bodies", {{"7", {{"infill_pattern", "grid"}}}}}};
  CHECK_THROWS(DocumentIO::PartPrintSettingsFromJson(part));
}

TEST_CASE("A body's override sits over its part's settings",
          "[print][settings]") {
  printset::PartSettings ps;
  ps.part.walls = 2;
  ps.part.infillPct = 15.0;
  printset::Settings pin;
  pin.walls = 3;
  ps.bodies[42] = pin;
  CHECK(*ps.For(42).walls == 3);
  CHECK(*ps.For(42).infillPct == 15.0);
  CHECK(*ps.For(1).walls == 2);
  // The round trip keeps exactly what is stated.
  const json j = DocumentIO::PartPrintSettingsToJson(ps);
  CHECK(DocumentIO::PartPrintSettingsFromJson(j) == ps);
  CHECK_FALSE(j.at("part").contains("top_layers"));
}

TEST_CASE("What the slicer gets for an object", "[print][settings]") {
  printset::Settings s;
  s.walls = 1;
  s.topLayers = 2;
  s.bottomLayers = 2;
  s.infillPct = 15.0;
  s.infill = printset::Infill::Gyroid;
  const auto kv = printset::BambuObjectSettings(s, 0.2);
  const auto get = [&](const char* k) {
    for (const auto& [key, v] : kv)
      if (key == k)
        return v;
    return std::string("<none>");
  };
  CHECK(get("layer_height") == "0.2");
  CHECK(get("wall_loops") == "1");
  CHECK(get("top_shell_layers") == "2");
  // The preset's thickness floor would raise two layers to five.
  CHECK(get("top_shell_thickness") == "0");
  CHECK(get("bottom_shell_thickness") == "0");
  CHECK(get("sparse_infill_density") == "15%");
  CHECK(get("sparse_infill_pattern") == "gyroid");
  // Supports off and no brim unless set.
  CHECK(get("enable_support") == "0");
  CHECK(get("brim_type") == "no_brim");
  s.infillPct = 100.0;
  s.infill = printset::Infill::Rectilinear;
  for (const auto& [key, v] : printset::BambuObjectSettings(s, 0.2))
    if (key == "sparse_infill_pattern")
      CHECK(v == "zig-zag");
}

TEST_CASE("A printed cube weighs its shell and its infill", "[print][mass]") {
  // 20 mm, layers of 0.2 and walls of 0.5: whole pixels (a quarter of the
  // line width) and whole layers, so the raster is exact.
  const Body cube = Box(0, 0, 0, 20, 20, 20);
  printmass::Input in;
  in.layerMm = 0.2;
  in.lineWidthMm = 0.5;
  in.walls = 2;
  in.topLayers = 5;
  in.bottomLayers = 3;
  in.infill = 0.15;
  std::string why;
  const auto r = printmass::Estimate(cube, in, &why);
  REQUIRE(r);
  // By hand: 0.6 mm of bottom and 1.0 of top solid; a 1 mm wall ring
  // between; the 18 x 18 core at 15 %.
  const double L = 20.0, t = 1.0, bottom = 0.6, top = 1.0;
  const double mid = L - bottom - top;
  const double ring = L * L - (L - 2 * t) * (L - 2 * t);
  const double core = (L - 2 * t) * (L - 2 * t) * mid;
  const double shell = L * L * (bottom + top) + ring * mid;
  CHECK(r->solidMm3 == Approx(L * L * L).epsilon(1e-9));
  CHECK(r->shellMm3 == Approx(shell).epsilon(1e-9));
  CHECK(r->coreMm3 == Approx(core).epsilon(1e-9));
  CHECK(r->effectiveMm3 == Approx(shell + 0.15 * core).epsilon(1e-9));
  CHECK(r->layers == 100);
  CHECK(r->pixelMm == Approx(0.125));
  const double zMoment =
      L * L * bottom * (bottom / 2) + L * L * top * (L - top / 2) +
      (ring + 0.15 * (L - 2 * t) * (L - 2 * t)) * mid * (bottom + mid / 2);
  CHECK(r->centerOfMass.z == Approx(zMoment / r->effectiveMm3).epsilon(1e-9));
  CHECK(r->centerOfMass.x == Approx(10.0).epsilon(1e-9));
  CHECK(r->centerOfMass.y == Approx(10.0).epsilon(1e-9));

  // Solid all through, it is the solid: its inertia the cube's, V L^2 / 6.
  in.infill = 1.0;
  const auto s = printmass::Estimate(cube, in, &why);
  REQUIRE(s);
  CHECK(s->effectiveMm3 == Approx(L * L * L).epsilon(1e-9));
  for (int k = 0; k < 3; k++)
    CHECK(s->inertia[k][k] == Approx(L * L * L * L * L / 6.0).epsilon(1e-9));
  CHECK(std::abs(s->inertia[0][1]) < 1e-9 * s->inertia[0][0]);

  // Printed lying on its side it is the same cube: the same weight.
  in.infill = 0.15;
  in.up = glm::dvec3(1.0, 0.0, 0.0);
  const auto side = printmass::Estimate(cube, in, &why);
  REQUIRE(side);
  CHECK(side->effectiveMm3 == Approx(r->effectiveMm3).epsilon(1e-9));
  CHECK(side->centerOfMass.x ==
        Approx(zMoment / r->effectiveMm3).epsilon(1e-9));
}

TEST_CASE("A printed mass names what it lacks", "[print][mass]") {
  printfit::Process proc{"fdm", "pla", std::nullopt};
  printset::Settings s;
  std::string why;
  const Body cube = Box(0, 0, 0, 10, 10, 10);
  CHECK_FALSE(printmass::EstimatePrinted(&proc, s, cube, &why));
  CHECK(why.find("profile") != std::string::npos);
  printfit::Profile prof;
  prof.name = "P";
  prof.table.process = "fdm";
  prof.table.material = "pla";
  prof.layerHeightMm = 0.2;
  proc.profile = prof;
  CHECK_FALSE(printmass::EstimatePrinted(&proc, s, cube, &why));
  CHECK(why.find("line_width_mm") != std::string::npos);
  CHECK(why.find("walls") != std::string::npos);
  CHECK(why.find("infill_pct") != std::string::npos);
  proc.profile->lineWidthMm = 0.5;
  s.walls = 2;
  s.topLayers = 3;
  s.bottomLayers = 3;
  s.infillPct = 20.0;
  CHECK(printmass::EstimatePrinted(&proc, s, cube, &why));
}

TEST_CASE("bed_fit with keep-out zones judges where each piece is",
          "[print][check]") {
  const std::vector<std::array<double, 4>> zones = {{0, 0, 18, 28}};
  const glm::dvec3 bed(256, 256, 256), up(0, 0, 1);
  const Body clear = Box(30, 30, 0, 10, 10, 10, "clear");
  const Body corner = Box(5, 5, 0, 10, 10, 10, "corner");
  const Body off = Box(-5, 100, 0, 10, 10, 10, "off");
  auto f = printcheck::FitOnBed({&clear}, up, bed, zones);
  CHECK(f.status == printcheck::Status::Pass);
  CHECK(f.positional);
  f = printcheck::FitOnBed({&clear, &corner}, up, bed, zones);
  CHECK(f.status == printcheck::Status::Fail);
  REQUIRE(f.intrusions.size() == 1);
  CHECK(f.intrusions[0].body == 1);
  CHECK(f.intrusions[0].zone == 0);
  CHECK(f.intrusions[0].overlapMm2 == Approx(100.0));
  f = printcheck::FitOnBed({&off}, up, bed, zones);
  REQUIRE(f.intrusions.size() == 1);
  CHECK(f.intrusions[0].zone == -1);
  // Without zones it is the extent alone, as before: a slicer places it.
  f = printcheck::FitOnBed({&corner}, up, bed, {});
  CHECK(f.status == printcheck::Status::Pass);
  CHECK_FALSE(f.positional);
  // Zones are judged in the bed's frame; another print direction has none.
  f = printcheck::FitOnBed({&clear}, glm::dvec3(1, 0, 0), bed, zones);
  CHECK(f.status == printcheck::Status::Fail);
}

TEST_CASE("A print job's 3MF carries its plates and per-object settings",
          "[print][job]") {
  printjob::Job job;
  job.bedMm = {256.0, 256.0};
  for (int k = 0; k < 2; k++) {
    printjob::Plate plate;
    plate.name = "plate " + std::to_string(k + 1);
    printjob::Object o;
    o.name = k == 0 ? "pendulum" : "gear";
    o.vertices = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
    o.triangles = {{0, 2, 1}, {0, 1, 3}, {1, 2, 3}, {0, 3, 2}};
    o.settings = {{"wall_loops", k == 0 ? "3" : "2"},
                  {"sparse_infill_density", k == 0 ? "100%" : "15%"}};
    plate.objects.push_back(o);
    job.plates.push_back(plate);
  }
  job.record = {{"name", "test"}};

  // Bambu's plate grid: two plates side by side, a bed and a fifth apart.
  CHECK(printjob::PlateOrigin(1, 2, job.bedMm).x == Approx(307.2));
  CHECK(printjob::PlateOrigin(2, 3, job.bedMm).y == Approx(-307.2));
  CHECK(printjob::PlateOrigin(2, 5, job.bedMm).x == Approx(614.4));

  auto find = [](const auto& entries, const std::string& name) {
    for (const auto& [n, c] : entries)
      if (n == name)
        return c;
    return std::string();
  };
  auto e = printjob::Entries(job);
  const std::string settings = find(e, "Metadata/model_settings.config");
  CHECK(settings.find("<metadata key=\"wall_loops\" value=\"3\"/>") !=
        std::string::npos);
  CHECK(settings.find("<metadata key=\"sparse_infill_density\" "
                      "value=\"15%\"/>") != std::string::npos);
  CHECK(settings.find("<metadata key=\"plater_id\" value=\"2\"/>") !=
        std::string::npos);
  const std::string model = find(e, "3D/3dmodel.model");
  CHECK(model.find("transform=\"1 0 0 0 1 0 0 0 1 307.2 0 0\"") !=
        std::string::npos);
  // The model form: no Application, no project config.
  CHECK(model.find("Application") == std::string::npos);
  CHECK(find(e, "Metadata/project_settings.config").empty());

  job.projectConfig = json{{"printer_settings_id", "Bambu Lab P1S 0.4 nozzle"}};
  e = printjob::Entries(job);
  CHECK(find(e, "3D/3dmodel.model").find("BambuStudio-") != std::string::npos);
  CHECK(find(e, "Metadata/project_settings.config")
            .find("Bambu Lab P1S 0.4 nozzle") != std::string::npos);
  CHECK(
      printjob::MissingProjectKeys(*job.projectConfig).find("printable_area") !=
      std::string::npos);

  // A zip: every entry's local header, deflated, its sizes and CRC.
  const std::string zip = printjob::Archive(job);
  REQUIRE(zip.size() > 30);
  CHECK(Read32(zip, 0) == 0x04034b50u);
  CHECK(Read32(zip, zip.size() - 22) == 0x06054b50u);
  size_t at = 0;
  int entries = 0;
  while (Read32(zip, at) == 0x04034b50u) {
    const uint32_t comp = Read32(zip, at + 18);
    const uint16_t nameLen = static_cast<unsigned char>(zip[at + 26]) |
                             (static_cast<unsigned char>(zip[at + 27]) << 8);
    at += 30 + nameLen + comp;
    entries++;
  }
  CHECK(entries == static_cast<int>(e.size()));
}
