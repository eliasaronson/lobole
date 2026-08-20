#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "core/AssemblySolve.h"
#include "core/Configuration.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"

#include <glm/gtc/matrix_transform.hpp>

#include <map>
#include <memory>

using Catch::Matchers::WithinAbs;

// A5: an assembly may place another assembly. Flatten is the ONE walk that
// turns that tree into leaf parts with world transforms, so that render, BOM,
// mass, contact, export and drawings cannot each grow their own recursion and
// drift apart. These tests pin the contract they all depend on.
namespace {

std::unique_ptr<Document> BoxPart() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<BoxFeature>("B", glm::vec3(0), 2, 2, 2));
  return doc;
}

// Slot ids are the app's; here a small registry stands in for AppState.
struct World {
  std::map<uint64_t, std::unique_ptr<Document>> parts;
  std::map<uint64_t, std::unique_ptr<AssemblyDocument>> assemblies;

  assembly::PartResolver PartById() {
    return [this](uint64_t id) -> const Document* {
      const auto it = parts.find(id);
      return it == parts.end() ? nullptr : it->second.get();
    };
  }
  assembly::AssemblyResolver AsmById() {
    return [this](uint64_t id) -> const AssemblyDocument* {
      const auto it = assemblies.find(id);
      return it == assemblies.end() ? nullptr : it->second.get();
    };
  }
};

Occurrence Place(const std::string& name, uint64_t refId, OccurrenceRef kind,
                 glm::vec3 at) {
  Occurrence o;
  o.name = name;
  o.partId = refId;
  o.refKind = kind;
  o.position = at;
  return o;
}

} // namespace

TEST_CASE("Flatten composes transforms down the tree", "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();

  auto sub = std::make_unique<AssemblyDocument>();
  sub->AddOccurrence(Place("Board A", 1, OccurrenceRef::Part, {0, 0, 0}));
  sub->AddOccurrence(Place("Board B", 1, OccurrenceRef::Part, {10, 0, 0}));
  w.assemblies[2] = std::move(sub);

  AssemblyDocument top;
  top.AddOccurrence(Place("Gangway", 2, OccurrenceRef::Assembly, {100, 0, 0}));
  top.AddOccurrence(Place("Ladder", 1, OccurrenceRef::Part, {0, 50, 0}));

  const auto flat = assembly::Flatten(top, w.PartById(), w.AsmById());
  REQUIRE(flat);
  REQUIRE(flat->size() == 3); // two boards + the ladder

  // The sub-assembly's placement composes onto its contents.
  CHECK_THAT((*flat)[0].world[3].x, WithinAbs(100.0, 1e-6));
  CHECK_THAT((*flat)[1].world[3].x, WithinAbs(110.0, 1e-6));
  CHECK_THAT((*flat)[2].world[3].y, WithinAbs(50.0, 1e-6));

  // Leaves are always parts, and every one knows which TOP-LEVEL occurrence
  // owns it — that is what makes a sub-assembly one contact node, one
  // explode step, one drawing reference.
  const uint64_t gangwayId = top.GetOccurrences()[0].id;
  CHECK((*flat)[0].partId == 1);
  CHECK((*flat)[0].rootOccurrenceId == gangwayId);
  CHECK((*flat)[1].rootOccurrenceId == gangwayId);
  CHECK((*flat)[2].rootOccurrenceId == top.GetOccurrences()[1].id);

  // Paths read the way the tree does.
  CHECK((*flat)[0].path == "Gangway/Board A");
  CHECK((*flat)[2].path == "Ladder");
}

TEST_CASE("Flatten hides a whole subtree with its occurrence",
          "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();
  auto sub = std::make_unique<AssemblyDocument>();
  sub->AddOccurrence(Place("Board", 1, OccurrenceRef::Part, {0, 0, 0}));
  w.assemblies[2] = std::move(sub);

  AssemblyDocument top;
  Occurrence gang = Place("Gangway", 2, OccurrenceRef::Assembly, {0, 0, 0});
  gang.visible = false;
  top.AddOccurrence(gang);

  const auto flat = assembly::Flatten(top, w.PartById(), w.AsmById());
  REQUIRE(flat);
  REQUIRE(flat->size() == 1);
  CHECK_FALSE((*flat)[0].visible); // hiding the unit hides its contents
}

TEST_CASE("Flatten refuses a reference cycle, naming it",
          "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();
  // A contains B, and B contains A: renders forever if nobody checks.
  auto a = std::make_unique<AssemblyDocument>();
  auto b = std::make_unique<AssemblyDocument>();
  a->AddOccurrence(Place("B in A", 3, OccurrenceRef::Assembly, {0, 0, 0}));
  b->AddOccurrence(Place("A in B", 2, OccurrenceRef::Assembly, {0, 0, 0}));
  w.assemblies[2] = std::move(a);
  w.assemblies[3] = std::move(b);

  AssemblyDocument top;
  top.AddOccurrence(Place("A", 2, OccurrenceRef::Assembly, {0, 0, 0}));
  const auto flat = assembly::Flatten(top, w.PartById(), w.AsmById());
  REQUIRE_FALSE(flat);
  INFO(flat.error());
  CHECK(flat.error().find("cycle") != std::string::npos);
  CHECK(flat.error().find("A in B") != std::string::npos); // the closing ref

  // ...and the same cycle is refused at ADD time, before anything renders.
  std::string why;
  CHECK(assembly::WouldCycle(*w.assemblies[3], 3, 2, OccurrenceRef::Assembly,
                             w.AsmById(), &why));
  INFO(why);
  CHECK(!why.empty());
  CHECK_FALSE(assembly::WouldCycle(top, 9, 2, OccurrenceRef::Assembly,
                                   w.AsmById(), nullptr));
  // A part reference can never close a cycle.
  CHECK_FALSE(
      assembly::WouldCycle(top, 9, 1, OccurrenceRef::Part, w.AsmById()));
}

TEST_CASE("Flatten skips a dangling reference rather than failing",
          "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();
  AssemblyDocument top;
  top.AddOccurrence(Place("Gone", 99, OccurrenceRef::Assembly, {0, 0, 0}));
  top.AddOccurrence(Place("Here", 1, OccurrenceRef::Part, {0, 0, 0}));
  const auto flat = assembly::Flatten(top, w.PartById(), w.AsmById());
  REQUIRE(flat); // a deleted reference already behaves this way for parts
  CHECK(flat->size() == 1);
}

// The bug this prevents: AssemblyPartsStale resolved references through
// PartDocById, which returns null for an assembly — so a sub never read as
// stale, and the top tab kept rendering yesterday's geometry after an edit
// inside it. Correct-looking output, quietly wrong.
TEST_CASE("Subtree versions see through a sub-assembly",
          "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();
  auto sub = std::make_unique<AssemblyDocument>();
  sub->AddOccurrence(Place("Board", 1, OccurrenceRef::Part, {0, 0, 0}));
  w.assemblies[2] = std::move(sub);

  AssemblyDocument top;
  top.AddOccurrence(Place("Gangway", 2, OccurrenceRef::Assembly, {0, 0, 0}));

  const auto before = assembly::SubtreeVersions(top, w.PartById(), w.AsmById());
  // Both the sub's own version and its part's are stamped.
  REQUIRE(before.size() == 2);

  // Edit the part two levels down.
  w.parts[1]->AddFeature(
      std::make_unique<BoxFeature>("B2", glm::vec3(9, 0, 0), 1, 1, 1));
  const auto after = assembly::SubtreeVersions(top, w.PartById(), w.AsmById());
  REQUIRE(after.size() == 2);
  CHECK(after != before); // the top can tell that something below changed

  // ...and so does moving an occurrence INSIDE the sub, which changes no
  // part at all.
  const auto beforeMove = after;
  w.assemblies[2]->BumpVersion();
  CHECK(assembly::SubtreeVersions(top, w.PartById(), w.AsmById()) !=
        beforeMove);
}

// v56: which configuration a PLACEMENT uses. Two occurrences of one
// sub-assembly, one showing all its parts and one with a configuration that
// suppresses some — the thing that makes a sub-assembly reusable rather than
// merely tidy.
TEST_CASE("A placement chooses the sub-assembly's configuration",
          "[assembly][flatten]") {
  World w;
  w.parts[1] = BoxPart();
  auto sub = std::make_unique<AssemblyDocument>();
  const uint64_t keep =
      sub->AddOccurrence(Place("Keep", 1, OccurrenceRef::Part, {0, 0, 0}));
  const uint64_t drop =
      sub->AddOccurrence(Place("Drop", 1, OccurrenceRef::Part, {10, 0, 0}));
  Configuration shortCfg;
  shortCfg.id = 7;
  shortCfg.name = "short";
  shortCfg.suppressedFeatures = {drop}; // assembly configs suppress occurrences
  sub->GetConfigurations().push_back(shortCfg);
  w.assemblies[2] = std::move(sub);
  (void)keep;

  AssemblyDocument top;
  Occurrence full = Place("Full", 2, OccurrenceRef::Assembly, {0, 0, 0});
  Occurrence lean = Place("Lean", 2, OccurrenceRef::Assembly, {100, 0, 0});
  lean.configurationId = 7;
  top.AddOccurrence(full);
  top.AddOccurrence(lean);

  const auto flat = assembly::Flatten(top, w.PartById(), w.AsmById());
  REQUIRE(flat);
  int visibleFull = 0, visibleLean = 0;
  for (const assembly::FlatInstance& inst : *flat) {
    if (!inst.visible)
      continue;
    (inst.path.starts_with("Full/") ? visibleFull : visibleLean)++;
  }
  CHECK(visibleFull == 2); // no configuration named: everything
  CHECK(visibleLean == 1); // "short" drops one, in THIS placement only

  // A configuration that no longer exists falls back to the document's
  // active one rather than breaking the model.
  AssemblyDocument top2;
  Occurrence stale = Place("Stale", 2, OccurrenceRef::Assembly, {0, 0, 0});
  stale.configurationId = 999;
  top2.AddOccurrence(stale);
  const auto flat2 = assembly::Flatten(top2, w.PartById(), w.AsmById());
  REQUIRE(flat2);
  int visible2 = 0;
  for (const assembly::FlatInstance& inst : *flat2)
    if (inst.visible)
      visible2++;
  CHECK(visible2 == 2);
}
