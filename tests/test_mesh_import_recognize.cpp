// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// An Import of an STL with `representation: recognize`, through the real
// pipeline: what was recognised is measured, mated, projected and edited as
// the CAD part it was exported from
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include "mesh_fixtures.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/features/OffsetFaceFeature.h"
#include "core/io/Serialization.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::unique_ptr<ImportFeature> Recognize(const std::filesystem::path& stl,
                                         int minSides = 12) {
  auto imp = std::make_unique<ImportFeature>("download", stl.string());
  imp->keepOrigin = true;
  imp->upAxis = interchange::UpAxis::None;
  imp->representation = mesh::Representation::Recognize;
  imp->minSides = minSides;
  return imp;
}

int FaceCount(const Body& b) {
  int n = 0;
  while (Measure::DescribeFaceAt(b, n))
    n++;
  return n;
}

// The face of `body` that is a cylinder of radius `r` about the vertical
// line through (x, y).
std::optional<int> Bore(const Body& body, double x, double y, double r) {
  for (int i = 0;; i++) {
    const auto info = Measure::DescribeFaceAt(body, i);
    if (!info)
      return std::nullopt;
    if (info->kind != "cylinder" || std::abs(info->radius - r) > 1e-4)
      continue;
    glm::dvec3 origin, axis, x0;
    if (!body.shape.FaceAxis(i, origin, axis, x0))
      continue;
    if (std::abs(std::abs(axis.z) - 1.0) < 1e-9 &&
        std::hypot(origin.x - x, origin.y - y) < 1e-4)
      return i;
  }
}

} // namespace

TEST_CASE("A recognised STL is the CAD part again", "[mesh_recognition]") {
  const fixtures::Part plate = fixtures::Plate();
  const auto stl = fixtures::WriteStl(plate, 0.0214, 2 * kPi / 24, "import");

  Document doc;
  doc.AddFeature(Recognize(stl));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  CHECK(FaceCount(body) == 30);
  CHECK_THAT(body.shape.Volume(), WithinRel(plate.volume, 1e-6));

  // Every hole measures its diameter and gives its axis, where the mate and
  // the measure tools read them.
  for (const fixtures::Round& c : plate.cylinders) {
    INFO("hole at " << c.point.x << ", " << c.point.y);
    CHECK(Bore(body, c.point.x, c.point.y, c.radius).has_value());
  }

  const auto* imp = dynamic_cast<const ImportFeature*>(doc.GetFeature(0));
  REQUIRE(imp != nullptr);
  REQUIRE(imp->RecognizedBodies().size() == 1);
  const mesh::Recognition& r = imp->RecognizedBodies()[0].recognition;
  CHECK(r.fellBack.empty());
  CHECK(r.Count(mesh::Kind::Cylinder) == 24);
  CHECK(r.planes == 6);
  for (const mesh::RecognizedRound& round : r.rounds)
    CHECK_THAT(round.centre.z, WithinAbs(3.0, 1e-6)); // mid-thickness

  SECTION("facets is a face per triangle, as before") {
    Document facets;
    auto f = Recognize(stl);
    f->representation = mesh::Representation::Facets;
    facets.AddFeature(std::move(f));
    REQUIRE(facets.GetBodies().size() == 1);
    CHECK(FaceCount(facets.GetBodies()[0]) > 1000);
  }
  SECTION("the same file names the same faces") {
    Document again;
    again.AddFeature(Recognize(stl));
    REQUIRE(again.GetBodies().size() == 1);
    const Body& b = again.GetBodies()[0];
    CHECK(b.elementMap.names == body.elementMap.names);
    for (int i = 0; i < 30; i++)
      CHECK(Measure::DescribeFaceAt(b, i)->kind ==
            Measure::DescribeFaceAt(body, i)->kind);
  }
  SECTION("a hole's rim is a circle edge") {
    int circles = 0;
    for (int i = 0;; i++) {
      const auto e = Measure::DescribeEdgeAt(body, i);
      if (!e)
        break;
      circles += e->kind == "circle";
    }
    CHECK(circles == 48); // two rims a hole
  }
  SECTION("Offset Face moves a recognised flat, and opens a recognised bore") {
    // The top: the plane facing +z at z = 6.
    std::string top, bore;
    for (const auto& [i, name] : body.elementMap.names) {
      const auto info = Measure::DescribeFaceAt(body, i);
      if (info && info->kind == "plane" && info->normal.z > 0.99 &&
          std::abs(info->centroid.z - 6.0) < 1e-6)
        top = name;
    }
    REQUIRE(!top.empty());
    const double before = body.shape.Volume();
    const auto topInfo =
        Measure::DescribeFaceAt(body, *body.elementMap.Resolve(top));
    doc.AddFeature(std::make_unique<OffsetFaceFeature>(
        "raise", imp->GetId(), std::vector<ElementRef>{{imp->GetId(), top}},
        1.0));
    REQUIRE(doc.GetBodies().size() == 1);
    CHECK_THAT(doc.GetBodies()[0].shape.Volume(),
               WithinRel(before + topInfo->area * 1.0, 1e-6));
    // The bore, outward by 0.5 (adds material: the hole closes in), named
    // as the raised part names it.
    const std::optional<int> b = Bore(doc.GetBodies()[0], -50, -30, 2.5);
    REQUIRE(b.has_value());
    bore = doc.GetBodies()[0].elementMap.names.at(*b);
    doc.AddFeature(std::make_unique<OffsetFaceFeature>(
        "bush", imp->GetId(), std::vector<ElementRef>{{imp->GetId(), bore}},
        0.5));
    REQUIRE(doc.GetBodies().size() == 1);
    INFO("bush: " << doc.GetFeature(doc.FeatureCount() - 1)->GetError());
    CHECK(Bore(doc.GetBodies()[0], -50, -30, 2.0).has_value());
  }
  std::filesystem::remove(stl);
}

TEST_CASE("Recognition lists the polygons it left flat, and min_sides "
          "rounds them",
          "[mesh_recognition]") {
  const fixtures::Part bracket = fixtures::Bracket();
  const auto stl = fixtures::WriteStl(bracket, 0.01, 0.2, "polygons");
  for (const int minSides : {12, 8}) {
    INFO("min_sides " << minSides);
    Document doc;
    doc.AddFeature(Recognize(stl, minSides));
    REQUIRE(doc.GetBodies().size() == 1);
    const auto* imp = dynamic_cast<const ImportFeature*>(doc.GetFeature(0));
    const mesh::Recognition& r = imp->RecognizedBodies().at(0).recognition;
    CHECK(r.fellBack.empty());
    CHECK(r.polygonal.size() == (minSides == 12 ? 3u : 1u));
    CHECK(r.Count(mesh::Kind::Cone) == 1);
  }
  std::filesystem::remove(stl);
}

TEST_CASE("A mesh recognition cannot rebuild is kept as facets, and says why",
          "[mesh_recognition]") {
  // An open mesh: the rebuild needs a closed solid, so the import sews the
  // facets as it always did, and the feature's warning names the check.
  const fixtures::Part plate = fixtures::Plate();
  const auto stl = fixtures::WriteStl(plate, 0.05, 0.5, "open");
  {
    std::string bytes;
    {
      std::ifstream in(stl, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    uint32_t count = 0;
    std::memcpy(&count, bytes.data() + 80, 4);
    count -= 1;
    std::memcpy(bytes.data() + 80, &count, 4);
    bytes.resize(bytes.size() - 50);
    std::ofstream out(stl, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  }
  CADKernel::ImportedModel model;
  std::string why;
  ImportFeature::ReadOptions o;
  o.representation = mesh::Representation::Recognize;
  REQUIRE(ImportFeature::ReadModel(stl.string(), o, model, why));
  REQUIRE(model.products.size() == 1);
  REQUIRE(model.products[0].recognition.has_value());
  CHECK(model.products[0].recognition->fellBack.find("not a closed solid") !=
        std::string::npos);
  REQUIRE(!model.notes.empty());
  CHECK(model.notes[0].find("kept it as facets") != std::string::npos);
  std::filesystem::remove(stl);
}

TEST_CASE("A representation is refused on a file that is not a mesh",
          "[mesh_recognition]") {
  CADKernel::ImportedModel model;
  std::string why;
  ImportFeature::ReadOptions o;
  o.representation = mesh::Representation::Recognize;
  CHECK_FALSE(ImportFeature::ReadModel("part.step", o, model, why));
  CHECK(why.find("not a mesh") != std::string::npos);
  o.minSides = 4;
  CHECK_FALSE(ImportFeature::ReadModel("part.stl", o, model, why));
  CHECK(why.find("min_sides 4") != std::string::npos);
}
