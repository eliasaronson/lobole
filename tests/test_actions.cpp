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

#include "core/Document.h"
#include "core/Feature.h"
#include "core/Occurrence.h"
#include "core/features/SplitFeature.h"
#include "ui/AppActions.h"
#include "ui/AppState.h"
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
