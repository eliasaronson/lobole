// Headless unit tests for the ui/ actions layer.
//
// THE PATTERN THIS FILE ESTABLISHES
// ---------------------------------
// The actions layer (AppActions / AssemblyActions / SimulationActions) is
// ~3,800 lines of document logic that happens to live under src/ui/. Until
// now it could only be reached through the ImGui test engine, so pinning a
// piece of arithmetic — where a split's knife lands, what a camera should
// frame — cost a GPU, an Xvfb display and a place in the 80-second GUI suite.
//
// It needs NO renderer stub and no abstract interface. Every renderer pointer
// in AppState is already null-checked, because HeadlessMain.cpp runs the whole
// application that way. So a test constructs an AppState, leaves those
// pointers null, and calls the real function: the sync paths no-op exactly as
// they do headless, and the logic under test runs for real against a real
// OpenCASCADE body.
//
//   ui::AppState state;
//   state.document = std::make_unique<Document>();
//   state.commands = std::make_unique<CommandStack>();
//   ui::EnsureParts(state);          // renderers stay null
//
// What belongs here: geometry and document logic — derived defaults, bounds,
// BOM totals, explode offsets, undo/redo semantics. What does NOT: anything
// about widgets. designer_tests links ImGui as a side effect of linking
// designer_app, so calling an ImGui function without a context CRASHES rather
// than failing to compile. Click paths stay in the GUI suite, where they are
// the actual subject.
//
// See docs/architecture-review-2026-08-24.md.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h"
#include "core/features/BooleanFeature.h"
#include "core/features/SplitFeature.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/FeatureTreePanel.h"
#include "ui/Overlays.h"
#include "ui/TerminalPanel.h"

#include <glm/gtc/quaternion.hpp>

#include <memory>

using Catch::Matchers::WithinAbs;

namespace {

// An AppState shaped exactly like the headless one: a document, a command
// stack, and every renderer/selector pointer left null.
ui::AppState MakeHeadlessState() {
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);
  REQUIRE(state.meshRenderer == nullptr);
  REQUIRE(state.outlineRenderer == nullptr);
  REQUIRE(state.faceSelector == nullptr);
  return state;
}

// A box whose min corner is `at`, so "the body is not at the origin" — the
// case a default derived from a literal gets wrong.
//
// Returns the id AFTER the add: Document::AddFeature is what assigns the
// stable id, so a Feature's GetId() is still 0 while it is being built. That
// id is what Body::createdBy carries and what a Split's bodyId references.
uint64_t AddBox(Document& doc, const glm::vec3& at, double w, double h,
                double d) {
  doc.AddFeature(std::make_unique<BoxFeature>("Box", at, w, h, d));
  return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
}

} // namespace

// ---------------------------------------------------------------------------
// RecenterSplitOffset — the derived-defaults rule, in arithmetic
//
// CLAUDE.md: "A default carrying a unit — mm, degrees, a scale, a position —
// must be derived from what it acts on, never a literal." Split is the tool
// where that is easiest to get wrong and hardest to see: an offset is measured
// FROM THE CHOSEN PLANE, so a value that cuts the body through one plane is a
// knife in mid-air on the next. A silent miss regenerates as "fewer than two
// solids" — a red feature, with nothing saying the number was the problem.
//
// The dogfooding round found exactly this ("a split plane at the origin
// missing the body"). Nothing tested it until now.
// ---------------------------------------------------------------------------

TEST_CASE("a split's offset is derived from the body, not a literal",
          "[actions][defaults]") {
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;

  // A body sitting well off the origin, 20 tall, spanning y in [30, 50].
  const uint64_t boxId = AddBox(doc, glm::vec3(5.0f, 30.0f, -8.0f), 10, 20, 6);
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 1);

  SplitFeature split("Split", boxId);
  split.basePlane = PlaneType::XZ; // normal +Y, origin at 0
  ui::RecenterSplitOffset(state, split);

  // Mid-body along +Y is y = 40. A literal default (0, or 1.0) would put the
  // knife 30-40 units below the body and the split would find nothing to cut.
  CHECK_THAT(split.offset, WithinAbs(40.0, 1e-6));
}

TEST_CASE("a split re-centres when its plane changes", "[actions][defaults]") {
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  // Deliberately different extents per axis, so an axis mix-up cannot hide.
  const uint64_t boxId = AddBox(doc, glm::vec3(2.0f, 30.0f, -8.0f), 10, 20, 6);
  doc.Regenerate();

  SplitFeature split("Split", boxId);
  split.basePlane = PlaneType::XZ; // +Y: mid-body at 40
  ui::RecenterSplitOffset(state, split);
  CHECK_THAT(split.offset, WithinAbs(40.0, 1e-6));

  // Switching the plane must re-derive, not keep the old number: 40 measured
  // along +Z is nowhere near this body (z spans [-8, -2]).
  split.basePlane = PlaneType::XY; // +Z: mid-body at -5
  ui::RecenterSplitOffset(state, split);
  CHECK_THAT(split.offset, WithinAbs(-5.0, 1e-6));

  split.basePlane = PlaneType::YZ; // +X: mid-body at 7
  ui::RecenterSplitOffset(state, split);
  CHECK_THAT(split.offset, WithinAbs(7.0, 1e-6));
}

TEST_CASE("re-centring never overwrites a user's expression",
          "[actions][defaults]") {
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  const uint64_t boxId = AddBox(doc, glm::vec3(0.0f, 30.0f, 0.0f), 10, 20, 6);
  doc.Regenerate();

  SplitFeature split("Split", boxId);
  split.basePlane = PlaneType::XZ;
  split.offset = 3.0;
  split.offsetExpr = "plate_t / 2"; // the user drove it

  ui::RecenterSplitOffset(state, split);

  // An expression is the user's statement of intent. Re-deriving over it would
  // silently break the parametric link — the value AND the expression must
  // both survive.
  CHECK_THAT(split.offset, WithinAbs(3.0, 1e-9));
  CHECK(split.offsetExpr == "plate_t / 2");
}

TEST_CASE("re-centring finds the body before AND after the split runs",
          "[actions][defaults]") {
  // The subtle branch. Before the split executes, the source body is present
  // and stamped with the box's id. After a SUCCESSFUL split the source is
  // GONE, replaced by pieces stamped with the split's own id. Matching only
  // the source id would make a plane change on a working split find nothing
  // and silently leave a stale offset behind.
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  const uint64_t boxId = AddBox(doc, glm::vec3(0.0f, 30.0f, 0.0f), 10, 20, 6);
  doc.Regenerate();

  auto owned = std::make_unique<SplitFeature>("Split", boxId);
  SplitFeature* split = owned.get();
  split->basePlane = PlaneType::XZ;
  ui::RecenterSplitOffset(state, *split); // pre-execute: source body present
  const double before = split->offset;
  CHECK_THAT(before, WithinAbs(40.0, 1e-6));

  doc.AddFeature(std::move(owned));
  doc.Regenerate();
  REQUIRE(split->GetError().empty()); // the derived offset actually cut
  REQUIRE(doc.GetBodies().size() == 2);
  for (const Body& b : doc.GetBodies())
    REQUIRE(b.createdBy == split->GetId()); // source replaced by the pieces

  // Now CHANGE the plane. This is the assertion that has teeth: a lookup that
  // only matches the source id finds nothing post-split, leaves the offset
  // untouched, and the stale 40 survives. Asserting "unchanged on the same
  // plane" would pass against exactly that bug — it did, until sabotage said
  // so — because doing nothing and re-deriving the same number look alike.
  split->basePlane = PlaneType::XY; // +Z: the pieces span z [0, 6], mid at 3
  ui::RecenterSplitOffset(state, *split);
  CHECK_THAT(split->offset, WithinAbs(3.0, 1e-6));
  CHECK(split->offset != before); // and it is not the stale value
}

// ---------------------------------------------------------------------------
// VisibleSceneBounds — what the camera frames and where a section starts
//
// Feeds FitCameraToDocument and a new section's default cut. On an assembly it
// must walk to the LEAF parts, place each by its world transform, apply the
// explode offset and skip hidden occurrences. Bounding only the top level put
// the camera and the default cut in the wrong place for a nested model; the
// only thing pinning that today is a screenshot.
// ---------------------------------------------------------------------------

TEST_CASE("scene bounds cover an off-origin part", "[actions][camera]") {
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  AddBox(doc, glm::vec3(5.0f, 30.0f, -8.0f), 10, 20, 6);
  doc.Regenerate();

  glm::vec3 lo(0.0f), hi(0.0f);
  REQUIRE(ui::VisibleSceneBounds(state, lo, hi));
  CHECK_THAT(lo.x, WithinAbs(5.0f, 1e-4));
  CHECK_THAT(lo.y, WithinAbs(30.0f, 1e-4));
  CHECK_THAT(lo.z, WithinAbs(-8.0f, 1e-4));
  CHECK_THAT(hi.x, WithinAbs(15.0f, 1e-4));
  CHECK_THAT(hi.y, WithinAbs(50.0f, 1e-4));
  CHECK_THAT(hi.z, WithinAbs(-2.0f, 1e-4));
}

TEST_CASE("an empty document has no bounds to frame", "[actions][camera]") {
  ui::AppState state = MakeHeadlessState();
  glm::vec3 lo(0.0f), hi(0.0f);
  // False, not a zero-size box at the origin — the caller must be able to tell
  // "nothing to frame" from "something tiny at the origin", or an empty tab
  // slams the camera to a point.
  CHECK_FALSE(ui::VisibleSceneBounds(state, lo, hi));
}

TEST_CASE("assembly bounds place each occurrence and skip hidden ones",
          "[actions][camera][assembly]") {
  ui::AppState state = MakeHeadlessState();

  // One part: a 10x10x10 box at the origin.
  Document& part = *state.document;
  AddBox(part, glm::vec3(0.0f), 10, 10, 10);
  part.Regenerate();
  REQUIRE(part.GetBodies().size() == 1);
  const uint64_t partId = state.parts[state.activePart].id;

  const size_t asmIndex = ui::AddAssembly(state, "Asm");
  REQUIRE(state.assembly != nullptr);
  (void)asmIndex;

  Occurrence a; // at the origin
  a.partId = partId;
  a.name = "a";
  state.assembly->AddOccurrence(a);

  Occurrence b; // 100 along +X — this is what a top-level-only walk misses
  b.partId = partId;
  b.name = "b";
  b.position = glm::vec3(100.0f, 0.0f, 0.0f);
  state.assembly->AddOccurrence(b);

  glm::vec3 lo(0.0f), hi(0.0f);
  REQUIRE(ui::VisibleSceneBounds(state, lo, hi));
  CHECK_THAT(lo.x, WithinAbs(0.0f, 1e-3));
  CHECK_THAT(hi.x, WithinAbs(110.0f, 1e-3)); // 100 + the box's own 10

  // Hiding the far one must pull the bounds back in — otherwise the camera
  // frames empty space around something the user cannot see.
  state.assembly->GetOccurrences()[1].visible = false;
  glm::vec3 lo2(0.0f), hi2(0.0f);
  REQUIRE(ui::VisibleSceneBounds(state, lo2, hi2));
  CHECK_THAT(hi2.x, WithinAbs(10.0f, 1e-3));
}

TEST_CASE("assembly bounds rotate a placed part's corners",
          "[actions][camera][assembly]") {
  // A rotated part is where transforming only the min/max corners is wrong:
  // the rotated box's extent comes from corners that were neither. All eight
  // have to be transformed.
  ui::AppState state = MakeHeadlessState();
  Document& part = *state.document;
  AddBox(part, glm::vec3(0.0f), 20, 2, 2); // a long thin bar along +X
  part.Regenerate();
  const uint64_t partId = state.parts[state.activePart].id;

  ui::AddAssembly(state, "Asm");
  Occurrence o;
  o.partId = partId;
  o.name = "bar";
  o.orientation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 0, 1));
  state.assembly->AddOccurrence(o);

  glm::vec3 lo(0.0f), hi(0.0f);
  REQUIRE(ui::VisibleSceneBounds(state, lo, hi));
  // Rotated 90 deg about +Z, the 20-long X extent becomes the Y extent.
  CHECK_THAT(hi.y - lo.y, WithinAbs(20.0f, 1e-3));
  CHECK_THAT(hi.x - lo.x, WithinAbs(2.0f, 1e-3));
}

#ifdef HAVE_TERMINAL
// The terminal panel's LIFECYCLE, headlessly.
//
// This half used to live only in the GUI suite, in a test that also asserted
// docking — and that test is the one which failed when it ran behind
// cam/tool_window_follows_the_viewport, invisibly, because the shard
// partition happened to separate them (docs/versioning/f7-handoff.md).
// Splitting it puts the assertions that are not about widgets somewhere a
// dock node cannot reach them: NewTerminalTab, ShutdownTerminal and
// TerminalRunning are free functions over AppState with no ImGui in them, so
// they run here in milliseconds rather than ~1 s on a GPU.
//
// What CANNOT move is the docking, which is genuinely about ImGui. It stays
// in GuiTestsTerminal.cpp, where a case now exercises the layout rebuild
// deliberately instead of inheriting it from a CAM test.
//
// What this does NOT pin, despite the obvious temptation: the `everStarted`
// latch of 2eda31d, where turning the preference off and on left a panel that
// could never start a shell again. That bug cannot be re-armed — writing this
// test is what turned up that nothing had READ the field since the fix, so it
// was deleted. Forgetting the sessions is what carries the behaviour now, and
// the session-count checks below are what hold it.
TEST_CASE("terminal: a tab starts a shell, and shutdown really ends it",
          "[actions][terminal]") {
  ui::AppState state = MakeHeadlessState();

  CHECK(!ui::TerminalRunning(state)); // nothing runs until asked
  CHECK(ui::TerminalSessionCount(state) == 0);

  // The panel's own path: opening it with no sessions calls exactly this.
  ui::NewTerminalTab(state);
  CHECK(ui::TerminalRunning(state));
  CHECK(ui::TerminalSessionCount(state) == 1);

  // "Ends the session rather than hiding it" — a process kept alive behind a
  // closed panel is a process nobody can see, and on this app that process is
  // usually a Claude Code session holding the instance's port.
  //
  // The FORGETTING is what these two lines actually pin (verified by
  // sabotage: killing without forgetting fails them). Whether the child is
  // reaped is not observable from here — Session's destructor ends the pty,
  // so removing the explicit Terminate() call leaves both checks green.
  ui::ShutdownTerminal(state);
  CHECK(!ui::TerminalRunning(state));
  CHECK(ui::TerminalSessionCount(state) == 0); // forgotten, not just killed

  // And the panel opens again afterwards, from a clean slate.
  ui::NewTerminalTab(state);
  CHECK(ui::TerminalRunning(state));
  CHECK(ui::TerminalSessionCount(state) == 1);

  ui::ShutdownTerminal(state); // no pty left behind for the next test
}
#endif

// The prompt pill must not run underneath the floating tool window.
//
// Both are anchored to the same viewport at the same height: the tool window
// top-right, the pill centred. A dogfooding round found the Hole tool's
// "Step 2 of 3" pill printed straight across its own dialog's first row —
// in every screenshot anyone had ever taken of a tool session, which is how
// long it had been there without being written down as a defect.
//
// This is arithmetic, not a widget, and the pill is drawn to a foreground
// draw list that no GUI test can address. So it is pinned here instead —
// the trigger the architecture review names for writing a headless test
// beside a GUI one.
TEST_CASE("prompt pill keeps clear of the tool window", "[actions][overlay]") {
  ui::AppState state = MakeHeadlessState();
  state.viewportPos = glm::vec2(200.0f, 50.0f);
  state.viewportSize = glm::vec2(1000.0f, 700.0f);
  const glm::vec2 text(300.0f, 16.0f);
  const float y = state.viewportPos.y + 34.0f;

  SECTION("no tool window: centred on the viewport") {
    state.toolWindowRect = glm::vec4(0.0f);
    const glm::vec4 r = ui::PromptPillRect(state, y, text);
    const float centre = state.viewportPos.x + state.viewportSize.x * 0.5f;
    CHECK_THAT((r.x + r.z) * 0.5f, WithinAbs(centre, 0.5f));
  }

  SECTION("tool window at the same height: the pill clears its left edge") {
    // Where BeginToolWindowFrame puts it: top-right, 12 px in. The WIDTH
    // matters and is the reason this section is written twice over: a
    // narrow tool window does not reach the centred pill at all, so the
    // assertion passes with the clamp deleted and proves nothing. 420 px is
    // the Hole tool's rough width — the case the dogfooding round hit.
    const float twRight = state.viewportPos.x + state.viewportSize.x - 12.0f;
    state.toolWindowRect =
        glm::vec4(twRight - 420.0f, y - 10.0f, twRight, y + 200.0f);
    // Unclamped, a pill this wide centred on the viewport WOULD overlap:
    // that is what makes the next line a real assertion.
    const float centre = state.viewportPos.x + state.viewportSize.x * 0.5f;
    REQUIRE(centre + text.x * 0.5f > state.toolWindowRect.x);
    const glm::vec4 r = ui::PromptPillRect(state, y, text);
    CHECK(r.z <= state.toolWindowRect.x); // the whole pill, not just its centre
  }

  SECTION("tool window BELOW the pill does not move it") {
    // The check is against the pill's own band, not merely "a window exists":
    // a tool window further down the viewport is not in the way, and shoving
    // the pill left for it would be a different wrong answer.
    state.toolWindowRect = glm::vec4(900.0f, y + 400.0f, 1150.0f, y + 600.0f);
    const glm::vec4 r = ui::PromptPillRect(state, y, text);
    const float centre = state.viewportPos.x + state.viewportSize.x * 0.5f;
    CHECK_THAT((r.x + r.z) * 0.5f, WithinAbs(centre, 0.5f));
  }

  SECTION("a pill too wide to fit beside it is never pushed off-screen") {
    // A cramped pill that overlaps slightly beats one whose text starts
    // outside the viewport, where it cannot be read at all.
    const float twRight = state.viewportPos.x + state.viewportSize.x - 12.0f;
    state.toolWindowRect =
        glm::vec4(twRight - 900.0f, y - 10.0f, twRight, y + 200.0f);
    const glm::vec4 r = ui::PromptPillRect(state, y, glm::vec2(700.0f, 16.0f));
    CHECK(r.x >= state.viewportPos.x);
  }
}

// Feature-tree rows must differ by more than colour.
//
// The four states a row can be in — failed, warned, suppressed, rolled past —
// were red, amber, dimmed and dimmed. Red against amber is the pair
// red-green colour blindness confuses, and the last two were not
// distinguishable by anyone: same colour, different meaning, with only a
// hover tooltip to tell them apart. This is the U5 "badges beyond colour"
// item, and it is a legibility floor rather than a polish nicety.
//
// The glyph is drawn to a window draw list, which no GUI test can address —
// but the CHOICE is the logic, and precedence between four states is the part
// that can quietly go wrong.
TEST_CASE("feature tree rows carry a badge, not just a colour",
          "[actions][overlay]") {
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  AddBox(doc, glm::vec3(0.0f), 10, 10, 10);
  doc.Regenerate();
  Feature* f = doc.GetFeature(0);
  REQUIRE(f != nullptr);

  SECTION("a healthy row has no badge") {
    CHECK(ui::FeatureRowBadge(*f, false) == nullptr);
  }

  SECTION("every state has one, and they are all different") {
    const char* rolled = ui::FeatureRowBadge(*f, true);
    f->SetSuppressed(true);
    const char* supp = ui::FeatureRowBadge(*f, false);
    f->SetSuppressed(false);
    f->SetError("boom");
    const char* err = ui::FeatureRowBadge(*f, false);
    f->SetError("");
    f->SetWarning("removed no material");
    const char* warn = ui::FeatureRowBadge(*f, false);

    for (const char* b : {rolled, supp, err, warn})
      CHECK(b != nullptr);
    // The whole point: four states, four DISTINCT glyphs. Suppressed and
    // rolled-past share a colour, so if they shared a glyph too the badge
    // would add nothing where it is needed most.
    const std::set<std::string> distinct{rolled, supp, err, warn};
    CHECK(distinct.size() == 4);
  }

  SECTION("precedence matches the colour the row already gets") {
    // The panel dims a suppressed row whatever else is true of it, so the
    // badge must agree — two channels disagreeing about the same row is
    // worse than one channel saying too little.
    f->SetSuppressed(true);
    f->SetError("boom");
    const std::string suppressedAndBroken = ui::FeatureRowBadge(*f, false);
    f->SetError("");
    const std::string suppressedOnly = ui::FeatureRowBadge(*f, false);
    CHECK(suppressedAndBroken == suppressedOnly);
  }
}

// ---------------------------------------------------------------------------
// DuplicateDocument — the bottom tab bar's right-click Duplicate
//
// A duplicate is only useful if it is INDEPENDENT (editing the copy leaves the
// original alone) while what it POINTS AT is not: a copied assembly places the
// same parts, not forked ones. Both halves are checked here; the click path
// that reaches this function is a GUI test.
// ---------------------------------------------------------------------------

TEST_CASE("a duplicated part is an independent copy of the original",
          "[actions][tabs][duplicate]") {
  ui::AppState state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0.0f), 10, 20, 30);
  state.document->SetParameter("w", "10");
  const uint64_t sourceId = state.parts[state.activePart].id;

  const size_t copy = ui::DuplicateDocument(state, 0);
  REQUIRE(copy == 1);
  REQUIRE(state.parts.size() == 2);
  CHECK(state.activePart == 1); // duplicating switches to the copy
  CHECK(state.parts[1].kind == ui::DocKind::Part);
  // A name of its own: the RPC refuses a duplicate name, and the name is the
  // document's directory on save.
  CHECK(state.parts[1].name == "Part 1 copy");
  CHECK(state.parts[1].id != sourceId);

  // Same recipe, same geometry, its own document.
  REQUIRE(state.document != nullptr);
  CHECK(state.document->FeatureCount() == 1);
  REQUIRE(state.document->GetBodies().size() == 1);
  CHECK(state.document->InstanceId() !=
        state.parts[0].doc->InstanceId()); // the original is stashed now
  CHECK(state.document->FindParameter("w") != nullptr);

  // Independent: an edit to the copy does not reach the original.
  AddBox(*state.document, glm::vec3(50.0f), 5, 5, 5);
  CHECK(state.document->FeatureCount() == 2);
  CHECK(state.parts[0].doc->FeatureCount() == 1);
  // ...and its undo stack is its own, and empty: undoing here must not undo
  // edits that were made to the other document.
  REQUIRE(state.commands != nullptr);
  CHECK_FALSE(state.commands->CanUndo());

  // A second copy cannot take the first one's name either.
  REQUIRE(ui::DuplicateDocument(state, 0) == 2);
  CHECK(state.parts[2].name == "Part 1 copy 2");
}

TEST_CASE("a duplicated assembly places the same parts",
          "[actions][tabs][duplicate][assembly]") {
  ui::AppState state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0.0f), 10, 10, 10);
  const uint64_t partId = state.parts[state.activePart].id;

  ui::AddAssembly(state, "Asm");
  Occurrence occ;
  occ.partId = partId;
  occ.name = "a";
  state.assembly->AddOccurrence(occ);
  const size_t asmIndex = static_cast<size_t>(state.activePart);

  const size_t copy = ui::DuplicateDocument(state, asmIndex);
  REQUIRE(copy == 2);
  CHECK(state.parts[copy].kind == ui::DocKind::Assembly);
  CHECK(state.parts[copy].name == "Asm copy");
  REQUIRE(state.assembly != nullptr);
  REQUIRE(state.assembly->OccurrenceCount() == 1);
  // The point: the copy places the ORIGINAL part. Duplicating an assembly
  // copies the arrangement; it does not fork the parts arranged in it.
  CHECK(state.assembly->GetOccurrences()[0].partId == partId);

  // And adding to the copy leaves the original assembly alone.
  state.assembly->AddOccurrence(occ);
  CHECK(state.assembly->OccurrenceCount() == 2);
  REQUIRE(state.parts[asmIndex].assembly != nullptr);
  CHECK(state.parts[asmIndex].assembly->OccurrenceCount() == 1);
}

TEST_CASE("a duplicated drawing draws the same source document",
          "[actions][tabs][duplicate][drawing]") {
  ui::AppState state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0.0f), 10, 10, 10);
  const uint64_t partId = state.parts[state.activePart].id;

  ui::AddDrawing(state, "Sheet");
  DrawingView view;
  view.sourceDocId = partId;
  view.scale = 0.5;
  state.drawing->AddView(view);
  const size_t sheetIndex = static_cast<size_t>(state.activePart);

  const size_t copy = ui::DuplicateDocument(state, sheetIndex);
  REQUIRE(copy == 2);
  CHECK(state.parts[copy].kind == ui::DocKind::Drawing);
  CHECK(state.parts[copy].name == "Sheet copy");
  REQUIRE(state.drawing != nullptr);
  REQUIRE(state.drawing->ViewCount() == 1);
  // Views reference their source by stable document id, so the copy draws the
  // same part — it does not draw the drawing it was copied from.
  CHECK(state.drawing->GetViews()[0].sourceDocId == partId);
  CHECK(state.drawing->GetViews()[0].scale == 0.5);
}

TEST_CASE("duplicating a tab you are not looking at copies that tab",
          "[actions][tabs][duplicate]") {
  // The active tab's document lives in AppState and every other one in its
  // slot — the copy has to come from the right place either way.
  ui::AppState state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0.0f), 10, 10, 10);
  ui::MoveToFolder(state, 0, "Imported");

  ui::AddPart(state, "Part 2"); // now active; Part 1 is stashed
  REQUIRE(state.activePart == 1);
  REQUIRE(state.document->FeatureCount() == 0);

  const size_t copy = ui::DuplicateDocument(state, 0);
  REQUIRE(copy == 2);
  CHECK(state.parts[copy].name == "Part 1 copy");
  REQUIRE(state.document != nullptr);
  CHECK(state.document->FeatureCount() == 1); // Part 1's box, not Part 2's none
  // A copy lands in its original's folder rather than loose on the bar.
  CHECK(state.parts[copy].folder == "Imported");
}

TEST_CASE("A fastener is one part per spec, placed in an assembly",
          "[actions][parts]") {
  // The architectural claims of docs/parts/plan.md phase 2, which is the
  // phase that exists to test them: if part-per-spec is the wrong shape it is
  // cheap to discover with one fastener type and expensive with twenty.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  // A part tab REFUSES, and not for tidiness: CAM generates over the active
  // part's bodies, so a screw there would be machined.
  std::string error;
  CHECK(ui::AddFastener(state, "iso4762def", "M6", 20.0, "hex", glm::vec3(0.0f),
                        glm::quat(1, 0, 0, 0), &error) == 0);
  INFO(error);
  CHECK(error.find("ASSEMBLY") != std::string::npos);

  const size_t asmTab = ui::AddAssembly(state, "Joint");
  ui::SetActivePart(state, asmTab);
  REQUIRE(state.assembly != nullptr);

  // Eight of one spec and one of another: NINE occurrences, TWO parts.
  for (int i = 0; i < 8; i++) {
    error.clear();
    const uint64_t id =
        ui::AddFastener(state, "iso4762def", "M6", 20.0, "hex",
                        glm::vec3(static_cast<float>(i) * 12.0f, 0.0f, 0.0f),
                        glm::quat(1, 0, 0, 0), &error);
    INFO(error);
    CHECK(id != 0);
  }
  CHECK(ui::AddFastener(state, "iso4762def", "M8", 30.0, "hex", glm::vec3(0.0f),
                        glm::quat(1, 0, 0, 0), &error) != 0);
  CHECK(state.assembly->OccurrenceCount() == 9);

  size_t generated = 0;
  for (const ui::PartSlot& slot : state.parts)
    if (slot.folder == ui::kStandardPartsFolder)
      generated++;
  // Two, not nine: the eight identical screws share one document, which is
  // what makes `bom` aggregate them without knowing anything about fasteners,
  // and what keeps a project from growing a tab per screw.
  CHECK(generated == 2);
  CHECK(ui::PartIdByName(state, "ISO 4762 M6x20") != 0);
  CHECK(ui::PartIdByName(state, "ISO 4762 M8x30") != 0);

  // Placing one did not leave the caller on some other tab. A tab switch
  // nobody asked for reads as a bug, and the importer's own pattern switches
  // twice on the way through.
  CHECK(state.assembly != nullptr);
  CHECK(state.activePart == static_cast<int>(asmTab));

  // The refusal reaches through the placement layer intact, and no half-made
  // part is left behind when it fires.
  const size_t partsBefore = state.parts.size();
  error.clear();
  CHECK(ui::AddFastener(state, "iso4762def", "M7", 20.0, "hex", glm::vec3(0.0f),
                        glm::quat(1, 0, 0, 0), &error) == 0);
  INFO(error);
  CHECK(error.find("M6 and M8") != std::string::npos);
  CHECK(state.parts.size() == partsBefore);
}

namespace {

// A plate with a bore down its Z axis, as its own part slot. `boreDepth` < 0
// drills through; `boreX` offsets the hole so a stack can be misaligned.
struct DrilledPlate {
  uint64_t partId = 0;
  ElementRef topRim;
};

DrilledPlate MakePlate(ui::AppState& state, const std::string& name,
                       double thickness, double boreDia, double boreDepth,
                       double boreX) {
  const size_t tab = ui::AddPart(state, name);
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  doc.AddFeature(std::make_unique<BoxFeature>(
      name, glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, thickness));
  const double z0 = boreDepth < 0.0 ? -1.0 : thickness - boreDepth;
  const double h = boreDepth < 0.0 ? thickness + 2.0 : boreDepth + 0.001;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Bore",
      glm::vec3(static_cast<float>(boreX), 0.0f, static_cast<float>(z0)),
      glm::vec3(0, 0, 1), boreDia / 2, h));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Drill", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Subtract));

  DrilledPlate out;
  out.partId = state.parts[tab].id;
  // The rim is the highest circular edge: the one a head would sit on.
  const Body& body = doc.GetBodies().front();
  double best = -1e9;
  // edgeNames, not names: faces and edges have separate index spaces, and
  // DescribeEdgeAt on a face index is a different edge or none.
  for (const auto& [index, mapped] : body.elementMap.edgeNames) {
    const std::optional<Measure::EdgeInfo> edge =
        Measure::DescribeEdgeAt(body, index);
    if (!edge || edge->kind != "circle")
      continue;
    if (edge->midpoint.z > best) {
      best = edge->midpoint.z;
      out.topRim = ElementRef{body.createdBy, mapped};
    }
  }
  return out;
}

} // namespace

TEST_CASE(
    "A fastener derives its size and length from the hole it is picked in",
    "[actions][parts]") {
  // Phase 3 of docs/parts/plan.md, and the four cases that decide whether
  // "derive from the hole" is a real interface or a demo. Each is a finding
  // rather than an obstacle: what the geometry cannot disambiguate gets
  // reported, and what it can rules a fastener out gets refused by name.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate top = MakePlate(state, "Top", 12.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Bottom", 26.0, 6.6, -1.0, 0.0);
  const DrilledPlate blind = MakePlate(state, "Blind", 20.0, 6.6, 8.0, 0.0);
  const DrilledPlate off = MakePlate(state, "Off", 10.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Skew", 10.0, 6.6, -1.0, 20.0);

  const auto place = [&](const char* part, glm::vec3 at) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = at;
    state.assembly->AddOccurrence(occ);
  };

  // 1. Two plates with a 3 mm gap. The gap is SPANNED and named: it is either
  //    an unmodelled gasket or two parts that were meant to touch, and the
  //    geometry cannot tell those apart — so the summary says so and the
  //    length covers it.
  ui::SetActivePart(state, ui::AddAssembly(state, "Gap"));
  place("Top", glm::vec3(0.0f, 0.0f, 3.0f));
  place("Bottom", glm::vec3(0.0f, 0.0f, -26.0f));
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, top.topRim, "iso4762def", "hex", true);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK(fit.size == "M6"); // read back out of the clearance table
  CHECK(fit.clearance == "Normal");
  CHECK_THAT(fit.grip, WithinAbs(41.0, 1e-3)); // 12 + 3 + 26
  CHECK_FALSE(fit.blind);
  CHECK(fit.summary.find("gap 3") != std::string::npos);
  // 41 is not a length screws are made in; 45 is the next one that is.
  CHECK_THAT(fit.length, WithinAbs(45.0, 1e-9));

  // 2. A blind hole 8 mm into a 20 mm plate. The length is bounded from ABOVE
  //    — a screw that bottoms out clamps nothing — so it snaps DOWN, and the
  //    off-axis probe's 20 mm of plate is not the answer.
  ui::SetActivePart(state, ui::AddAssembly(state, "Blind"));
  place("Blind", glm::vec3(0.0f));
  fit = ui::FitFastenerToHole(state, blind.topRim, "iso4762def", "hex", true);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK(fit.blind);
  CHECK_THAT(fit.grip, WithinAbs(8.0, 1e-3));
  CHECK_THAT(fit.length, WithinAbs(8.0, 1e-9));

  // 3. The second plate's hole is 20 mm off the axis. Refused, and named as a
  //    misalignment rather than as a shallow hole: told "the hole is 0 mm
  //    deep" a person goes looking for a drilling depth.
  ui::SetActivePart(state, ui::AddAssembly(state, "Skewed"));
  place("Off", glm::vec3(0.0f));
  place("Skew", glm::vec3(0.0f, 0.0f, -10.0f));
  fit = ui::FitFastenerToHole(state, off.topRim, "iso4762def", "hex", true);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("not lined up") != std::string::npos);
  CHECK(fit.why.find("Skew") != std::string::npos);

  // 4. A rim that is not a rim. A cylindrical FACE has an axis and no
  //    direction, which is why the pick is an edge — and asking with one is
  //    refused rather than guessed at.
  ui::SetActivePart(state, ui::AddAssembly(state, "Bad"));
  place("Top", glm::vec3(0.0f));
  fit = ui::FitFastenerToHole(state, ElementRef{top.topRim.featureId, "nope"},
                              "iso4762def", "hex", true);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("RIM") != std::string::npos);
}
