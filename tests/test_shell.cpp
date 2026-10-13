// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestIO.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/features/BooleanFeature.h"
#include "core/features/DraftFeature.h"
#include "core/features/FilletFeature.h"
#include "core/features/PatternFeature.h"
#include "core/features/ShellFeature.h"
#include "core/features/SplitFeature.h"
#include "core/io/Serialization.h"

#include <filesystem>
#include <vector>

using Catch::Matchers::WithinRel;

namespace {
// Name of the box face whose outward normal is +Z (the "top"): the box spans
// [0,s] on each axis, so the +Z face is the one whose centroid has max z.
// We just enumerate names and pick by resolving — but the test only needs
// *a* face to open, so grab the first.
std::string AnyFace(const Body& b) {
  return b.elementMap.names.begin()->second;
}
} // namespace

TEST_CASE("Shell with an open face hollows a box into a bin", "[shell]") {
  Document doc;
  doc.AddFeature(
      std::make_unique<BoxFeature>("B", glm::vec3(0), 10.0, 10.0, 10.0));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);

  // Open one face of a 10^3 cube, wall 1: cavity is 8x8x9, so the solid is
  // 1000 - 576 = 424 (the same for any single face of a cube, by symmetry).
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bin", id, std::vector<ElementRef>{ElementRef{id, face}}, 1.0));
  REQUIRE(doc.GetBodies().size() == 1);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(424.0, 1e-2));
}

TEST_CASE("Shell without an open face errors", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Sealed", id, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(1)->GetError().empty());
}

// OCC reports success and hands back the input solid when the wall is too
// thick for the geometry; the feature used to commit that as a silent no-op.
// (Found by the 2026-08-13 clicks-only dogfood, where a shell of a slab with
// a through hole "succeeded" and changed nothing.)
// U10: inputs that are not there yet are refused at COMMIT time, on both
// surfaces — the GUI disables Accept, the RPC throws. Regeneration itself
// stays permissive so an empty tool can still open.
// A draft that reshapes half the body is valid geometry and almost always a
// mistake: 25 degrees sounds modest, but on a 2-unit wall it travels 0.93 —
// four times the 0.25 wall it is tapering, so it cuts through into the shell
// cavity. Found in a real model (scratch/shell_draft.dsgn), which looked like
// a loft passing through the cut-out.
TEST_CASE("A draft that reshapes the body warns with the numbers",
          "[draft][warning]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 2, 2));
  const uint64_t boxId = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);
  doc.AddFeature(std::make_unique<ShellFeature>(
      "S", boxId, std::vector<ElementRef>{ElementRef{boxId, face}}, 0.25));

  // A side face of the shelled body, tapered hard.
  std::string side;
  for (const auto& [index, name] : doc.GetBodies()[0].elementMap.names) {
    glm::dvec3 c, n, u;
    if (doc.GetBodies()[0].shape.FacePlane(index, c, n, u) &&
        std::abs(n.z) < 0.1f) {
      side = name;
      break;
    }
  }
  REQUIRE(!side.empty());
  auto steep = std::make_unique<DraftFeature>(
      "Steep", boxId, std::vector<ElementRef>{ElementRef{boxId, side}}, 25.0);
  DraftFeature* draft = steep.get();
  doc.AddFeature(std::move(steep));
  REQUIRE(doc.GetFeature(2)->GetError().empty()); // valid, just drastic
  CHECK(draft->GetWarning().find("check the angle") != std::string::npos);
  CHECK(draft->GetWarning().find("25") != std::string::npos);

  // A sane angle on the same face says nothing.
  draft->angleDeg = 3.0;
  doc.Regenerate();
  CHECK(draft->GetWarning().empty());
}

TEST_CASE("WhyIncomplete names the missing input", "[u10][incomplete]") {
  FilletFeature blend("F", {}, 0.5);
  CHECK(blend.WhyIncomplete().find("edge") != std::string::npos);
  FilletFeature ready("F", {ElementRef{1, "1/face0;U0"}}, 0.5);
  CHECK(ready.WhyIncomplete().empty());

  DraftFeature draft("D", 1, {}, 5.0);
  CHECK(draft.WhyIncomplete().find("face") != std::string::npos);

  SplitFeature split("S", 0);
  CHECK(split.WhyIncomplete().find("body") != std::string::npos);
  SplitFeature knife("S", 7);
  knife.knife = SplitKnifeKind::Face;
  CHECK(knife.WhyIncomplete().find("face") != std::string::npos);

  BooleanFeature boo("B", 0, 0, BooleanOp::Union);
  CHECK(!boo.WhyIncomplete().empty());
  PatternFeature pat("P", PatternKind::Linear, 0);
  CHECK(!pat.WhyIncomplete().empty());

  // A healthy feature says nothing, and the base class default is silence.
  BoxFeature box("B", glm::vec3(0), 1, 1, 1);
  CHECK(box.WhyIncomplete().empty());
}

TEST_CASE("Shell that removes no material errors", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 4, 4, 4));
  const uint64_t id = doc.GetFeature(0)->GetId();
  const std::string face = AnyFace(doc.GetBodies()[0]);
  doc.AddFeature(std::make_unique<ShellFeature>(
      "TooThick", id, std::vector<ElementRef>{ElementRef{id, face}}, 2.5));
  CHECK_FALSE(doc.Regenerate());
  // Refused before the offset builder runs: a wall of half the thinnest
  // extent or more leaves no cavity.
  CHECK(doc.GetFeature(1)->GetError().find("no cavity") != std::string::npos);
  CHECK_THAT(doc.GetBodies()[0].shape.Volume(), WithinRel(64.0, 1e-9));
}

TEST_CASE("Shell errors on a missing body and round-trips (v20)", "[shell]") {
  Document doc;
  doc.AddFeature(std::make_unique<ShellFeature>(
      "Bad", 999, std::vector<ElementRef>{}, 1.0));
  CHECK_FALSE(doc.Regenerate());
  CHECK(!doc.GetFeature(0)->GetError().empty());

  Document doc2;
  doc2.AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 6, 6, 6));
  const uint64_t id = doc2.GetFeature(0)->GetId();
  const std::string f = AnyFace(doc2.GetBodies()[0]);
  auto sh = std::make_unique<ShellFeature>(
      "H", id, std::vector<ElementRef>{ElementRef{id, f}}, 1.0);
  doc2.AddFeature(std::move(sh));
  const double v = doc2.GetBodies()[0].shape.Volume();

  const auto path = std::filesystem::temp_directory_path() / "shell_rt.json";
  std::string error;
  REQUIRE(testio::Save(doc2, path.string()));
  auto loaded = testio::Load(path.string());
  REQUIRE(loaded != nullptr);
  auto* rt = dynamic_cast<ShellFeature*>(loaded->GetFeature(1));
  REQUIRE(rt != nullptr);
  CHECK(rt->bodyId == id);
  CHECK_THAT(loaded->GetBodies()[0].shape.Volume(), WithinRel(v, 1e-9));
  std::filesystem::remove(path);
}
