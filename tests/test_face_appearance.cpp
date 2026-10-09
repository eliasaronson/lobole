// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A face's own appearance (v164,
// docs/rendering/materials.md#a-face-of-its-own): keyed by element name on the
// body's creating feature, resolved to a face index at every regeneration,
// reported when it stops resolving. These cases go through Document::Regenerate
// and the shared look resolver every renderer and exporter reads
// (ui/BodyAppearance.h); the face_appearance_smoke drives the RPC, and
// test_step_export the files.

#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "ui/BodyAppearance.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include <nlohmann/json.hpp>

#include <set>
#include <string>

namespace {

// The element name of the face of `body` whose centroid is at `at`.
std::string FaceAt(const Body& body, glm::dvec3 at) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), ++i) {
    GProp_GProps props;
    BRepGProp::SurfaceProperties(TopoDS::Face(ex.Current()), props);
    const gp_Pnt c = props.CentreOfMass();
    if (glm::length(glm::dvec3(c.X(), c.Y(), c.Z()) - at) < 1e-6) {
      const std::string* name = body.elementMap.NameOf(i);
      REQUIRE(name != nullptr);
      return *name;
    }
  }
  FAIL("no face centred at " << at.x << ", " << at.y << ", " << at.z);
  return {};
}

Appearance Red() {
  Appearance a;
  a.color = glm::vec3(0.8f, 0.02f, 0.02f);
  return a;
}

} // namespace

TEST_CASE("a face override resolves by name, survives an upstream edit and "
          "rebuilds nothing",
          "[appearance][toponaming]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Wall", glm::dvec3(0, 0, 0), 4, 4, 2));
  Feature* wall = doc.GetFeature(0);
  const std::string top = FaceAt(doc.GetBodies()[0], {2, 2, 2});

  wall->SetFaceAppearance(top, Red());
  REQUIRE(doc.Regenerate());
  CHECK(doc.LastRegenStats().executed == 0); // stripped from the chain hash
  const Body& body = doc.GetBodies()[0];
  REQUIRE(body.faceAppearances.size() == 1);
  const auto& [index, fa] = *body.faceAppearances.begin();
  CHECK(fa.element == top);
  CHECK(body.elementMap.Resolve(top) == index);
  CHECK_FALSE(body.appearance.Any()); // a face, not the body
  CHECK(wall->UnresolvedFaceLooks().empty());

  // Taller: the top face moves and keeps its name, and its paint.
  auto* box = dynamic_cast<BoxFeature*>(wall);
  REQUIRE(box != nullptr);
  box->depth = 5; // the Z extent (width, height, depth = X, Y, Z)
  doc.Regenerate();
  const Body& taller = doc.GetBodies()[0];
  REQUIRE(taller.faceAppearances.size() == 1);
  CHECK(taller.faceAppearances.begin()->second.element == top);
  CHECK(FaceAt(taller, {2, 2, 5}) == top);

  // Empty look removes it, and reports that it was there.
  CHECK(wall->SetFaceAppearance(top, Appearance{}));
  CHECK(wall->GetFaceAppearances().empty());
  doc.Regenerate();
  CHECK(doc.GetBodies()[0].faceAppearances.empty());
}

TEST_CASE("a face a later cut made is painted on the body's creating feature, "
          "and reported when the cut goes",
          "[appearance][toponaming]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("Wall", glm::dvec3(0, 0, 0), 4, 4, 2));
  const SketchPlane topPlane(glm::dvec3(0, 0, 2), glm::dvec3(0, 0, 1),
                             glm::dvec3(1, 0, 0));
  auto sk = std::make_unique<SketchFeature>("Pocket sketch", topPlane);
  sk->AddRectangle({1, 1}, {3, 3});
  doc.AddFeature(std::move(sk));
  const uint64_t sid = doc.GetFeature(1)->GetId();
  auto cut =
      std::make_unique<ExtrudeFeature>("Pocket", sid, 1.0, ExtrudeMode::Cut);
  cut->reversed = true; // into the block
  doc.AddFeature(std::move(cut));
  REQUIRE(doc.GetBodies().size() == 1);
  REQUIRE_THAT(doc.GetBodies()[0].shape.Volume(),
               Catch::Matchers::WithinRel(32.0 - 4.0, 1e-6));

  Feature* wall = doc.GetFeature(0);
  // The top was painted BEFORE the pocket: the pocket modifies it (renaming
  // it "…;M<tag>"), and the paint follows it.
  doc.GetFeature(2)->SetSuppressed(true);
  doc.Regenerate();
  const std::string top = FaceAt(doc.GetBodies()[0], {2, 2, 2});
  wall->SetFaceAppearance(top, Red());
  doc.GetFeature(2)->SetSuppressed(false);
  doc.Regenerate();
  REQUIRE(doc.GetBodies()[0].faceAppearances.size() == 1);
  CHECK(doc.GetBodies()[0].elementMap.Resolve(top) == std::nullopt);
  CHECK(wall->UnresolvedFaceLooks().empty());
  wall->SetFaceAppearance(top, Appearance{});

  const std::string floor = FaceAt(doc.GetBodies()[0], {2, 2, 1});
  wall->SetFaceAppearance(floor, Red());
  doc.Regenerate();
  REQUIRE(doc.GetBodies()[0].faceAppearances.size() == 1);
  CHECK(wall->UnresolvedFaceLooks().empty());

  // Suppress the cut: the floor is gone. Kept on the feature, and named.
  doc.GetFeature(2)->SetSuppressed(true);
  doc.Regenerate();
  CHECK(doc.GetBodies()[0].faceAppearances.empty());
  REQUIRE(wall->GetFaceAppearances().size() == 1);
  REQUIRE(wall->UnresolvedFaceLooks().size() == 1);
  CHECK(wall->UnresolvedFaceLooks()[0] == floor);

  // The stale name survives a save, and comes back when the cut does.
  const nlohmann::json j = DocumentIO::FeatureToJson(*wall);
  REQUIRE(j.contains("face_appearance"));
  CHECK(j.at("face_appearance").at(0).at("element") == floor);
  doc.GetFeature(2)->SetSuppressed(false);
  doc.Regenerate();
  CHECK(wall->UnresolvedFaceLooks().empty());
  CHECK(doc.GetBodies()[0].faceAppearances.size() == 1);
}

TEST_CASE("face_appearance round-trips and an empty entry is refused",
          "[appearance][serialization]") {
  BoxFeature box("Wall", glm::dvec3(0, 0, 0), 4, 4, 2);
  Appearance a = Red();
  a.roughness = 0.9f;
  box.SetFaceAppearance("1/cap1", a);
  nlohmann::json j = DocumentIO::FeatureToJson(box);
  const auto back = DocumentIO::FeatureFromJson(j);
  REQUIRE(back->GetFaceAppearances().size() == 1);
  const FaceAppearance& fa = back->GetFaceAppearances()[0];
  CHECK(fa.element == "1/cap1");
  CHECK(fa.look.color == a.color);
  CHECK(fa.look.roughness == a.roughness);
  CHECK_FALSE(fa.look.metallic.has_value());

  j["face_appearance"] = nlohmann::json::array({{{"element", "1/cap1"}}});
  CHECK_THROWS(DocumentIO::FeatureFromJson(j));
}

TEST_CASE("a face's look inherits from its body's, and its runs tile the "
          "tessellation",
          "[appearance][render]") {
  Body body;
  body.material = "steel";
  body.appearance.roughness = 0.2f;
  size_t palette = 0;
  const ui::BodyLook bodyLook = ui::ResolveBodyLook(body, palette);
  REQUIRE(bodyLook.hasMaterial);

  Appearance face;
  face.color = glm::vec3(1, 0, 0);
  const ui::BodyLook faceLook = ui::ResolveFaceLook(bodyLook, face);
  CHECK(faceLook.Color() == glm::vec3(1, 0, 0));
  CHECK(faceLook.mat.roughness == 0.2f);                 // the body's override
  CHECK(faceLook.mat.metallic == bodyLook.mat.metallic); // the material's

  // An unassigned body: the face is a neutral dielectric, the body still
  // the palette colour it was.
  Body plain;
  palette = 0;
  const ui::BodyLook plainLook = ui::ResolveBodyLook(plain, palette);
  CHECK_FALSE(plainLook.hasMaterial);
  const ui::BodyLook plainFace = ui::ResolveFaceLook(plainLook, face);
  CHECK(plainFace.hasMaterial);
  CHECK(plainFace.mat.metallic == 0.0f);
  CHECK(plainFace.Color() == glm::vec3(1, 0, 0));
  Appearance roughOnly;
  roughOnly.roughness = 0.1f;
  CHECK(ui::ResolveFaceLook(plainLook, roughOnly).Color() == plainLook.palette);

  // Faces 0..3, two triangles each; face 2 painted.
  const std::vector<uint32_t> triangleFaces{0, 0, 1, 1, 2, 2, 3, 3};
  CHECK(ui::FaceLookRuns(body, triangleFaces).empty()); // nothing painted
  body.faceAppearances[2] = {"x", face};
  const std::vector<ui::FaceLookRun> runs =
      ui::FaceLookRuns(body, triangleFaces);
  REQUIRE(runs.size() == 3);
  CHECK((runs[0].firstTriangle == 0 && runs[0].triangleCount == 4 &&
         runs[0].face == nullptr));
  CHECK((runs[1].firstTriangle == 4 && runs[1].triangleCount == 2 &&
         runs[1].face == &body.faceAppearances[2]));
  CHECK((runs[2].firstTriangle == 6 && runs[2].triangleCount == 2 &&
         runs[2].face == nullptr));
  // A painted face this tessellation has no triangles of: one draw.
  Body elsewhere;
  elsewhere.faceAppearances[9] = {"y", face};
  CHECK(ui::FaceLookRuns(elsewhere, triangleFaces).empty());
}
