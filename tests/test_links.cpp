// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Cross-part links (docs/modeling/derived.md#sketches and
// #copies-that-keep-up): a Derived that copies another part's SKETCH, a
// sketch here drawn on it, and the gates that keep every following copy up
// to date -- through the real actions layer, on a headless AppState (the
// pattern tests/test_actions.cpp sets out).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/Command.h"
#include "core/Document.h"
#include "core/features/DerivedFeature.h"
#include "core/features/ExtrudeFeature.h"
#include "core/features/SketchFeature.h"
#include "core/io/Serialization.h"
#include "core/sketch/SketchProfile.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/DerivedActions.h"
#include "ui/DocumentSlots.h"
#include "ui/SketchInteraction.h"

#include <cmath>
#include <memory>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ui::AppState MakeState() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state, "Plate");
  return state;
}

// A sketch on XY holding one circle; returns its id (the document's).
uint64_t AddCircleSketch(Document& doc, const std::string& name, double r,
                         glm::dvec2 at = {0.0, 0.0}) {
  auto sk = std::make_unique<SketchFeature>(name, SketchPlane(PlaneType::XY));
  sk->AddEntity(sketch::Circle{at, r});
  doc.AddFeature(std::move(sk));
  REQUIRE(doc.Regenerate());
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

// Sets the circle of sketch `id` in the ACTIVE part, as an edit does: one
// command on its stack, and no gate.
void SetRadius(ui::AppState& state, uint64_t id, double r) {
  size_t index = 0;
  auto* sk =
      dynamic_cast<SketchFeature*>(state.document->FindFeatureById(id, &index));
  REQUIRE(sk != nullptr);
  const nlohmann::json before = DocumentIO::FeatureToJson(*sk);
  SketchFeature changed = *sk;
  std::get<sketch::Circle>(changed.entities.at(0)).radius = r;
  REQUIRE(state.commands->Execute(
      std::make_unique<ModifyFeatureCommand>(
          index, before, DocumentIO::FeatureToJson(changed)),
      *state.document));
}

double Matter(const Document& doc) {
  double v = 0.0;
  for (const Body& b : doc.GetBodies())
    if (!b.reference)
      v += b.shape.Volume();
  return v;
}

const DerivedFeature* DerivedIn(const Document& doc, uint64_t id) {
  return dynamic_cast<const DerivedFeature*>(doc.FindFeatureById(id));
}

// In the active part: a Derived copying sketch `sketchId` of part `source`,
// a sketch on it projecting all of it, and that sketch extruded 5 mm.
// Returns the Derived's id.
uint64_t LinkAndExtrude(ui::AppState& state, uint64_t source,
                        uint64_t sketchId) {
  auto d = std::make_unique<DerivedFeature>("Outline", 0);
  d->sketches = {sketchId};
  ui::DerivedSource from;
  from.slotId = source;
  REQUIRE(ui::PointDerivedAt(state, *d, from).empty());
  REQUIRE(state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(d)), *state.document));
  const uint64_t did =
      state.document->GetFeature(state.document->FeatureCount() - 1)->GetId();
  REQUIRE(ui::SketchOnDerived(state, did, sketchId).empty());
  const uint64_t onIt = state.sketchEdit.activeSketchId;
  REQUIRE(onIt != 0);
  state.sketchEdit = ui::SketchEditState{};
  REQUIRE(state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::make_unique<ExtrudeFeature>(
          "Gasket", onIt, 5.0, ExtrudeMode::NewBody)),
      *state.document));
  return did;
}

} // namespace

TEST_CASE("a linked paste from another part links to THAT part's sketch",
          "[links][sketch]") {
  // Feature ids are per-part counters, so part B's first sketch has the id
  // of part A's, and the same circle has the same entity id. The clipboard
  // recorded the sketch id alone, and a linked paste into B bound to B's own
  // sketch: the paste never followed A.
  ui::AppState state = MakeState();
  const uint64_t plate = state.parts[0].id;
  const uint64_t sa = AddCircleSketch(*state.document, "Outline", 5.0);
  auto* source =
      dynamic_cast<SketchFeature*>(state.document->FindFeatureById(sa));
  REQUIRE(source != nullptr);
  const uint32_t circle = sketch::EntityId(source->entities[0]);
  state.sketchEdit.activeSketchId = sa;
  state.sketchEdit.selected = {{circle, 0}};
  ui::CopySketchSelection(state, *source);
  state.sketchEdit = ui::SketchEditState{};

  ui::AddPart(state, "Lid");
  const uint64_t own = AddCircleSketch(*state.document, "Own", 5.0);
  REQUIRE(own == sa); // the collision this is about
  const uint64_t target =
      AddCircleSketch(*state.document, "Target", 1.0, {50.0, 0.0});
  size_t index = 0;
  auto* t = dynamic_cast<SketchFeature*>(
      state.document->FindFeatureById(target, &index));
  REQUIRE(t != nullptr);
  state.sketchEdit.activeSketchId = target;
  ui::PasteSketchClipboardLinked(state, *t, index);
  state.sketchEdit = ui::SketchEditState{};

  // A changes; the gate when the Lid is shown again carries it over.
  ui::SetActivePart(state, 0);
  REQUIRE(state.parts[state.activePart].id == plate);
  SetRadius(state, sa, 7.0);
  ui::SetActivePart(state, 1);
  t = dynamic_cast<SketchFeature*>(state.document->FindFeatureById(target));
  REQUIRE(t != nullptr);
  bool followed = false;
  for (const sketch::Entity& e : t->entities)
    if (const auto* c = std::get_if<sketch::Circle>(&e))
      followed = followed || std::abs(c->radius - 7.0) < 1e-9;
  CHECK(followed);
  // ...through a copy of A's sketch, placed before the sketch it serves.
  size_t derivedAt = SIZE_MAX, targetAt = SIZE_MAX;
  for (size_t i = 0; i < state.document->FeatureCount(); i++) {
    if (const auto* d =
            dynamic_cast<const DerivedFeature*>(state.document->GetFeature(i)))
      if (d->sourcePart == plate)
        derivedAt = i;
    if (state.document->GetFeature(i)->GetId() == target)
      targetAt = i;
  }
  CHECK(derivedAt < targetAt);
}

TEST_CASE("a sketch copied from another part keeps the part drawn on it up "
          "to date at the gates",
          "[links][sketch]") {
  ui::AppState state = MakeState();
  const uint64_t plate = state.parts[0].id;
  const uint64_t sa = AddCircleSketch(*state.document, "Outline", 5.0);
  ui::AddPart(state, "Gasket");
  const uint64_t did = LinkAndExtrude(state, plate, sa);
  const double r5 = M_PI * 25.0 * 5.0;
  CHECK_THAT(Matter(*state.document), WithinRel(r5, 1e-6));
  CHECK(ui::DerivedStale(state, *DerivedIn(*state.document, did)) ==
        std::optional<bool>{false});

  // Edit A's sketch; show B's tab: B's extrude follows.
  ui::SetActivePart(state, 0);
  SetRadius(state, sa, 6.0);
  ui::SetActivePart(state, 1);
  const double r6 = M_PI * 36.0 * 5.0;
  CHECK_THAT(Matter(*state.document), WithinRel(r6, 1e-6));
  CHECK_THAT(state.linksNotice, ContainsSubstring("Gasket updated from Plate"));

  // The update is ONE step on B's own history: undone, the copy is stale
  // again, and the next gate takes it again.
  state.commands->Undo(*state.document);
  CHECK_THAT(Matter(*state.document), WithinRel(r5, 1e-6));
  CHECK(ui::DerivedStale(state, *DerivedIn(*state.document, did)) ==
        std::optional<bool>{true});
  const std::vector<ui::LinkUpdate> took = ui::RefreshLinks(state);
  REQUIRE(took.size() == 1);
  CHECK(took[0].features == std::vector<uint64_t>{did});
  CHECK_THAT(Matter(*state.document), WithinRel(r6, 1e-6));

  // With follow off it stays as taken, and says it is behind.
  REQUIRE(ui::SetDerivedFollow(state, did, false).empty());
  ui::SetActivePart(state, 0);
  SetRadius(state, sa, 8.0);
  ui::SetActivePart(state, 1);
  CHECK_THAT(Matter(*state.document), WithinRel(r6, 1e-6));
  std::string why;
  CHECK(ui::DerivedStale(state, *DerivedIn(*state.document, did),
                         state.document.get(),
                         &why) == std::optional<bool>{true});
  CHECK_THAT(why, ContainsSubstring("Plate"));
}

TEST_CASE("an edit reaches the end of a chain through one gate, in order",
          "[links]") {
  // C copies B's body, B is drawn on A's sketch. C must see B AFTER B has
  // taken A, in the same gate; and before the gate C reads stale although
  // its own source has not moved yet -- stale is transitive.
  ui::AppState state = MakeState();
  const uint64_t a = state.parts[0].id;
  const uint64_t sa = AddCircleSketch(*state.document, "Outline", 5.0);
  ui::AddPart(state, "B");
  const uint64_t b = state.parts[1].id;
  LinkAndExtrude(state, a, sa);
  ui::AddPart(state, "C");
  auto d = std::make_unique<DerivedFeature>("B copy", 0);
  d->reference = false;
  ui::DerivedSource from;
  from.slotId = b;
  REQUIRE(ui::PointDerivedAt(state, *d, from).empty());
  REQUIRE(state.commands->Execute(
      std::make_unique<AddFeatureCommand>(std::move(d)), *state.document));
  const uint64_t cd = state.document->GetFeature(0)->GetId();
  CHECK_THAT(Matter(*state.document), WithinRel(M_PI * 25.0 * 5.0, 1e-6));

  ui::SetActivePart(state, 0);
  SetRadius(state, sa, 6.0);
  // No gate yet (an edit is not one). C's copy of B has not moved, but B is
  // behind A, so C reads stale and says why.
  const Document& cDoc = *state.parts[2].doc;
  std::string why;
  CHECK(ui::DerivedStale(state, *DerivedIn(cDoc, cd), &cDoc, &why) ==
        std::optional<bool>{true});
  CHECK_THAT(why, ContainsSubstring("itself behind"));

  const std::vector<ui::LinkUpdate> took = ui::RefreshLinks(state);
  REQUIRE(took.size() == 2);
  CHECK(took[0].partId == b);
  CHECK(took[1].partName == "C");
  CHECK_THAT(Matter(*state.parts[2].doc), WithinRel(M_PI * 36.0 * 5.0, 1e-6));
  CHECK(ui::RefreshLinks(state).empty()); // nothing left to take
}

TEST_CASE("a cycle through a sketch link is refused at creation and at "
          "update",
          "[links]") {
  ui::AppState state = MakeState();
  const uint64_t a = state.parts[0].id;
  const uint64_t sa = AddCircleSketch(*state.document, "Outline", 5.0);
  ui::AddPart(state, "B");
  const uint64_t b = state.parts[1].id;
  LinkAndExtrude(state, a, sa); // B copies A's sketch

  // At creation: A copying B's body would close A -> B -> A.
  ui::SetActivePart(state, 0);
  DerivedFeature back("Back", 0);
  ui::DerivedSource from;
  from.slotId = b;
  CHECK_THAT(ui::PointDerivedAt(state, back, from),
             ContainsSubstring("would derive from"));

  // At update: a file merged from elsewhere can carry the cycle whole. Plant
  // A's copy of B by hand, past the creation check, and update it.
  auto planted = std::make_unique<DerivedFeature>("Planted", b);
  planted->sourceName = "B";
  REQUIRE(derived::TakeSnapshot(*planted, *state.parts[1].doc, *state.document)
              .empty());
  state.document->AddFeature(std::move(planted));
  REQUIRE(state.document->Regenerate());
  const uint64_t pid =
      state.document->GetFeature(state.document->FeatureCount() - 1)->GetId();
  CHECK_THAT(ui::UpdateDerived(state, pid),
             ContainsSubstring("would derive from"));
  // ...and the gate does not take either side of it.
  CHECK(ui::RefreshLinks(state).empty());
}

TEST_CASE("update all is one undo step and lists only what it took",
          "[links]") {
  ui::AppState state = MakeState();
  const uint64_t a = state.parts[0].id;
  const uint64_t s1 = AddCircleSketch(*state.document, "One", 5.0);
  const uint64_t s2 = AddCircleSketch(*state.document, "Two", 3.0);
  ui::AddPart(state, "Spare");
  const uint64_t spare = state.parts[1].id;
  AddCircleSketch(*state.document, "Spare outline", 2.0);
  ui::AddPart(state, "B");
  std::vector<uint64_t> ids;
  for (const auto& [src, sk] : {std::pair{a, s1}, std::pair{a, s2}}) {
    auto d = std::make_unique<DerivedFeature>("Copy", 0);
    d->sketches = {sk};
    d->follow = false; // so nothing but the verb takes them
    ui::DerivedSource from;
    from.slotId = src;
    REQUIRE(ui::PointDerivedAt(state, *d, from).empty());
    state.document->AddFeature(std::move(d));
    ids.push_back(state.document->GetFeature(state.document->FeatureCount() - 1)
                      ->GetId());
  }
  auto gone = std::make_unique<DerivedFeature>("Gone", 0);
  gone->sketchNames = {"Spare outline"};
  ui::DerivedSource from;
  from.slotId = spare;
  REQUIRE(ui::PointDerivedAt(state, *gone, from).empty());
  state.document->AddFeature(std::move(gone));
  REQUIRE(state.document->Regenerate());

  ui::SetActivePart(state, 0);
  SetRadius(state, s1, 6.0);
  SetRadius(state, s2, 4.0);
  ui::SetActivePart(state, 2);
  std::string why;
  REQUIRE(ui::DeletePart(state, 1, &why)); // the spare's source is gone
  REQUIRE(state.document != nullptr);
  const size_t depth = state.commands->UndoDepth();
  std::vector<uint64_t> updated;
  std::vector<std::string> skipped;
  ui::UpdateAllDerived(state, updated, skipped);
  CHECK(updated == ids);
  REQUIRE(skipped.size() == 1);
  CHECK_THAT(skipped[0], ContainsSubstring("Gone"));
  CHECK(state.commands->UndoDepth() == depth + 1);
  for (uint64_t id : ids)
    CHECK(ui::DerivedStale(state, *DerivedIn(*state.document, id)) ==
          std::optional<bool>{false});
  state.commands->Undo(*state.document);
  for (uint64_t id : ids)
    CHECK(ui::DerivedStale(state, *DerivedIn(*state.document, id)) ==
          std::optional<bool>{true});
}

TEST_CASE("the link audit counts a part read past the gates", "[links]") {
  ui::AppState state = MakeState();
  const uint64_t a = state.parts[0].id;
  const uint64_t sa = AddCircleSketch(*state.document, "Outline", 5.0);
  ui::AddPart(state, "B");
  const uint64_t b = state.parts[1].id;
  LinkAndExtrude(state, a, sa);
  ui::SetActivePart(state, 0);
  SetRadius(state, sa, 6.0); // B is behind now, and no gate has run

  ui::ResetLinkAudit();
  ui::SetLinkAudit(true);
  CHECK(ui::PartDocById(state, a) != nullptr); // A itself is fine
  CHECK(ui::LinkAuditMisses() == 0);
  CHECK(ui::PartDocById(state, b) != nullptr); // the planted read
  CHECK(ui::LinkAuditMisses() == 1);
  CHECK_THAT(ui::LinkAuditLastMiss(), ContainsSubstring("'B'"));
  ui::RefreshLinks(state);
  CHECK(ui::PartDocById(state, b) != nullptr);
  CHECK(ui::LinkAuditMisses() == 1);
  ui::SetLinkAudit(false);
  ui::ResetLinkAudit();
}
