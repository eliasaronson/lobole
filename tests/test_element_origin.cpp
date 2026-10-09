// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TestPaths.h"
#include <catch2/catch_test_macros.hpp>

#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/io/Serialization.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using topo::DecodeElementName;
using topo::ElementRole;

// The names below are real: a block with a pocket, a hole and a fillet, and a
// chess blank's Derived copies (docs/modeling/toponaming.md#naming-grammar).
TEST_CASE("A name says which feature made it and how", "[toponaming]") {
  const auto floor = DecodeElementName("4/cap1");
  REQUIRE(floor.ok);
  CHECK(floor.madeBy == 4);
  CHECK(floor.role == ElementRole::EndCap);

  const auto wall = DecodeElementName("4/g3");
  REQUIRE(wall.ok);
  CHECK(wall.role == ElementRole::Side);
  CHECK(wall.entity == 3);

  // cut by 4, 6 and 7, still extrude 2's end
  const auto top = DecodeElementName("2/cap1;M4;M6;M7");
  REQUIRE(top.ok);
  CHECK(top.madeBy == 2);
  CHECK(top.role == ElementRole::EndCap);
  CHECK(top.modifiedBy == std::vector<uint64_t>{4, 6, 7});

  // a fillet's face is the fillet's, not the edge's it was generated from
  const auto blend = DecodeElementName("2/g5;U3;G7");
  REQUIRE(blend.ok);
  CHECK(blend.madeBy == 7);
  CHECK(blend.seedTag == 2);
  CHECK(blend.role == ElementRole::Generated);

  const auto split = DecodeElementName("2/cap1;M4.1");
  REQUIRE(split.ok);
  CHECK(split.modifiedBy == std::vector<uint64_t>{4});

  const auto bore = DecodeElementName("6/face0");
  REQUIRE(bore.ok);
  CHECK(bore.role == ElementRole::Positional);
  CHECK(bore.madeBy == 6);

  const auto hole = DecodeElementName("6/p3/counterbore_floor");
  REQUIRE(hole.ok);
  CHECK(hole.role == ElementRole::CounterboreFloor);
  CHECK(hole.entity == 3);

  const auto piece = DecodeElementName("7/b1/face4;M9");
  REQUIRE(piece.ok);
  CHECK(piece.role == ElementRole::Positional);

  const auto group = DecodeElementName("5/cap0@g12");
  REQUIRE(group.ok);
  CHECK(group.role == ElementRole::StartCap);
}

TEST_CASE("A name decodes through Derived copies and pattern instances",
          "[toponaming][derived]") {
  // the third copy of the piece part's cut 31's floor, come in through
  // Derived feature 9 (a body the source part's feature 12 created)
  const auto crenel = DecodeElementName("9/12/31/cap1#2;M15");
  REQUIRE(crenel.ok);
  REQUIRE(crenel.derived.size() == 1);
  CHECK(crenel.derived[0].derivedId == 9);
  CHECK(crenel.derived[0].sourceCreatedBy == 12);
  CHECK(crenel.madeBy == 31);
  CHECK(crenel.role == ElementRole::EndCap);
  CHECK(crenel.instances == std::vector<std::string>{"2"});
  CHECK(crenel.modifiedBy == std::vector<uint64_t>{15});

  const auto nested = DecodeElementName("5/9/9/12/4/g14;M4;M8");
  REQUIRE(nested.ok);
  CHECK(nested.derived.size() == 2);
  CHECK(nested.madeBy == 4);
  CHECK(nested.modifiedBy == std::vector<uint64_t>{4, 8}); // its own M too

  const auto mirror = DecodeElementName("4/g7#m");
  REQUIRE(mirror.ok);
  CHECK(mirror.instances == std::vector<std::string>{"m"});
}

TEST_CASE("A face named from its edges says it has no role", "[toponaming]") {
  const auto l = DecodeElementName("12/g2;U0,12/g3;U1;L;M28");
  REQUIRE(l.ok);
  CHECK(l.role == ElementRole::Boundary);
  CHECK(l.madeBy == 0);
  CHECK(l.modifiedBy == std::vector<uint64_t>{28});

  // an edge is its face's origin
  const auto edge = DecodeElementName("2/g5;M4;M7;U1");
  REQUIRE(edge.ok);
  CHECK(edge.role == ElementRole::Side);
  CHECK(edge.madeBy == 2);
}

TEST_CASE("What is not a name does not decode", "[toponaming]") {
  CHECK_FALSE(DecodeElementName("").ok);
  CHECK_FALSE(DecodeElementName("face0").ok);
  CHECK_FALSE(DecodeElementName("4/g3;X9").ok);
  CHECK_FALSE(DecodeElementName("9/12/cap1").ok); // two leading ids: no seed
  topo::ElementRole r;
  CHECK(topo::ElementRoleFromString("end_cap", r));
  CHECK(r == ElementRole::EndCap);
  CHECK_FALSE(topo::ElementRoleFromString("floor", r));
  CHECK(topo::ElementRoleList().find("counterbore_floor") != std::string::npos);
}

TEST_CASE("Every element name in the golden corpus decodes",
          "[toponaming][golden]") {
  const fs::path dir = fs::path(TestSourceDir()) / "tests" / "golden";
  std::vector<fs::path> files;
  for (const auto& entry : fs::directory_iterator(dir))
    if (entry.path().extension() == ".json" &&
        entry.path().filename().string().find(".expect.") == std::string::npos)
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());
  REQUIRE(!files.empty());
  size_t names = 0;
  for (const fs::path& file : files) {
    auto loaded = DocumentIO::Load(file.string());
    REQUIRE(loaded.has_value());
    Document& doc = **loaded;
    for (const Body& b : doc.GetBodies()) {
      std::vector<std::string> all;
      for (const auto& [i, n] : b.elementMap.names)
        all.push_back(n);
      for (const auto& [i, n] : b.elementMap.edgeNames)
        all.push_back(n);
      for (const std::string& n : all) {
        INFO(file.filename().string() << ": " << n);
        const auto o = DecodeElementName(n);
        names++;
        CHECK(o.ok);
        CHECK(o.role != ElementRole::Unknown);
        // The maker is a feature of this document, or one a Template
        // instance owns (docs/modeling/templates.md#names) -- unless the name
        // came in through a Derived copy, where it is the SOURCE's, and then
        // the Derived feature is this document's.
        if (!o.derived.empty())
          CHECK(doc.FindFeatureById(o.derived.front().derivedId) != nullptr);
        else if (o.madeBy != 0)
          CHECK(doc.OwnerIndex(o.madeBy) >= 0);
      }
    }
  }
  CHECK(names > 1000);
}
