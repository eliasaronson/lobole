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
// See docs/architecture.md#the-ui-actions-layer.

#include "core/cam/CamToolpath.h"
#include "ui/CreateActions.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h"
#include "ui/ProjectActions.h"
#include "ui/StudyPanel.h" // SimulationLoadLabel
#include "ui/ViewActions.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>

#include "cad/CADKernel.h"
#include "core/AssemblySolve.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/drawing/DrawingDocument.h"
#include "core/features/BooleanFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/SketchFeature.h"
#include "core/features/SplitFeature.h"
#include "core/fem/FemStudy.h"
#include "core/io/GitVault.h"
#include "core/parts/FastenerTable.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJobs.h"
#include "ui/CamJobs.h"
#include "ui/CamPanel.h"
#include "ui/CamTool.h"
#include "ui/FeatureTreePanel.h"
#include "ui/FileDialogs.h"
#include "ui/ImportJob.h"
#include "ui/JobActions.h"
#include "ui/Overlays.h"
#include "ui/SimulationActions.h"
#include "ui/SketchInteraction.h" // UpdateSketchStatus
#include "ui/SyncJob.h"
#include "ui/TerminalPanel.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <unistd.h>

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
// partition happened to separate them
// (docs/ui/testing.md#independence-any-partition-is-valid). Splitting it puts
// the assertions that are not about widgets somewhere a dock node cannot reach
// them: NewTerminalTab, ShutdownTerminal and TerminalRunning are free functions
// over AppState with no ImGui in them, so they run here in milliseconds rather
// than ~1 s on a GPU.
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
    CHECK(ui::FeatureRowBadge(*f, false, f->IsSuppressed()) == nullptr);
  }

  SECTION("every state has one, and they are all different") {
    const char* rolled = ui::FeatureRowBadge(*f, true, f->IsSuppressed());
    f->SetSuppressed(true);
    const char* supp = ui::FeatureRowBadge(*f, false, f->IsSuppressed());
    f->SetSuppressed(false);
    f->SetError("boom");
    const char* err = ui::FeatureRowBadge(*f, false, f->IsSuppressed());
    f->SetError("");
    f->SetWarning("removed no material");
    const char* warn = ui::FeatureRowBadge(*f, false, f->IsSuppressed());

    for (const char* b : {rolled, supp, err, warn})
      CHECK(b != nullptr);
    // The whole point: four states, four DISTINCT glyphs. Suppressed and
    // rolled-past share a colour, so if they shared a glyph too the badge
    // would add nothing where it is needed most.
    const std::set<std::string> distinct{rolled, supp, err, warn};
    CHECK(distinct.size() == 4);
  }

  SECTION("a feature the active configuration suppresses is badged") {
    // The row follows what regeneration does, not the authored flag alone.
    Configuration& cfg = doc.AddConfiguration("Without");
    cfg.suppressedFeatures = {f->GetId()};
    doc.SetActiveConfiguration(cfg.id);
    CHECK_FALSE(f->IsSuppressed());
    CHECK(doc.EffectiveSuppressed(0));
    const char* supp =
        ui::FeatureRowBadge(*f, false, doc.EffectiveSuppressed(0));
    f->SetSuppressed(true);
    const char* own = ui::FeatureRowBadge(*f, false, true);
    CHECK(std::string(supp) == std::string(own));
  }

  SECTION("precedence matches the colour the row already gets") {
    // The panel dims a suppressed row whatever else is true of it, so the
    // badge must agree — two channels disagreeing about the same row is
    // worse than one channel saying too little.
    f->SetSuppressed(true);
    f->SetError("boom");
    const std::string suppressedAndBroken =
        ui::FeatureRowBadge(*f, false, f->IsSuppressed());
    f->SetError("");
    const std::string suppressedOnly =
        ui::FeatureRowBadge(*f, false, f->IsSuppressed());
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
  // The architectural claims of
  // docs/parts/README.md#the-model-a-fastener-is-a-part-placed-as-an-occurrence:
  // if part-per-spec is the wrong shape it is cheap to discover with one
  // fastener type and expensive with twenty.
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
  ElementRef bottomRim; // the far face's rim, for a second pick
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
  // The rim is the highest circular edge: the one a head would sit on. The
  // LOWEST is the far face's, which is what a second pick names.
  const Body& body = doc.GetBodies().front();
  double best = -1e9;
  double worst = 1e9;
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
    if (edge->midpoint.z < worst) {
      worst = edge->midpoint.z;
      out.bottomRim = ElementRef{body.createdBy, mapped};
    }
  }
  return out;
}

// A plate with SEVERAL holes through it, and the top rim of each in the order
// the bores were asked for. MakePlate's single bore cannot say what a second
// bolt between the same two parts should do, which is the whole question a
// mate-per-joint rule has to answer.
std::vector<ElementRef> MakeMultiBorePlate(ui::AppState& state,
                                           const std::string& name,
                                           double thickness, double boreDia,
                                           const std::vector<double>& boreXs) {
  const size_t tab = ui::AddPart(state, name);
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  doc.AddFeature(std::make_unique<BoxFeature>(
      name, glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, thickness));
  uint64_t solid = doc.GetFeature(0)->GetId();
  for (size_t i = 0; i < boreXs.size(); i++) {
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Bore " + std::to_string(i),
        glm::vec3(static_cast<float>(boreXs[i]), 0.0f, -1.0f),
        glm::vec3(0, 0, 1), boreDia / 2, thickness + 2.0));
    const uint64_t bore = doc.GetFeatures().back()->GetId();
    doc.AddFeature(std::make_unique<BooleanFeature>(
        "Drill " + std::to_string(i), solid, bore, BooleanOp::Subtract));
    solid = doc.GetFeatures().back()->GetId();
  }

  std::vector<ElementRef> rims(boreXs.size());
  std::vector<double> best(boreXs.size(), -1e9);
  const Body& body = doc.GetBodies().front();
  for (const auto& [index, mapped] : body.elementMap.edgeNames) {
    const std::optional<Measure::EdgeInfo> edge =
        Measure::DescribeEdgeAt(body, index);
    if (!edge || edge->kind != "circle")
      continue;
    const std::optional<glm::vec3> centre =
        Measure::CentreOf(doc.GetBodies(), ElementRef{body.createdBy, mapped});
    if (!centre)
      continue;
    for (size_t i = 0; i < boreXs.size(); i++) {
      if (std::abs(centre->x - boreXs[i]) > 1e-3)
        continue;
      if (centre->z > best[i]) {
        best[i] = centre->z;
        rims[i] = ElementRef{body.createdBy, mapped};
      }
    }
  }
  return rims;
}

} // namespace

TEST_CASE("Deleting a placed part refuses with a reason, and says where",
          "[actions]") {
  // Reported from use 2026-09-01: right-clicking a tab and choosing Delete
  // did nothing. DeletePart was already refusing correctly — a part placed in
  // an assembly cannot go without leaving a dangling reference — but the menu
  // item was enabled whenever more than one tab existed and threw both the
  // `false` and the reason away. On any real project every part is placed in
  // something, so Delete never worked and never said why.
  //
  // The reason is now computed BEFORE the item is offered, which is what lets
  // the menu disable it and put the explanation on it.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const size_t plate = ui::AddPart(state, "Plate");
  ui::SetActivePart(state, plate);
  state.document->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0.0f), 10.0, 10.0, 10.0));
  const size_t spare = ui::AddPart(state, "Spare");

  // Unplaced: deletable, and the menu is told so with an empty reason.
  CHECK(ui::WhyPartUndeletable(state, spare).empty());

  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  Occurrence occ;
  occ.partId = state.parts[plate].id;
  occ.name = "Plate";
  state.assembly->AddOccurrence(occ);

  const std::string why = ui::WhyPartUndeletable(state, plate);
  INFO(why);
  CHECK_FALSE(why.empty());
  CHECK(why.find("Rig") != std::string::npos);         // WHERE it is placed
  CHECK(why.find("right-click") != std::string::npos); // and what to do
  // The refusal and the action agree — the menu cannot offer what the action
  // would reject, which is the whole point of asking first.
  std::string fromAction;
  CHECK_FALSE(ui::DeletePart(state, plate, &fromAction));
  CHECK(fromAction == why);

  // Remove the occurrence and it becomes deletable, through the same call
  // the row's context menu makes.
  state.assembly->RemoveOccurrence(state.assembly->GetOccurrences().front().id);
  CHECK(ui::WhyPartUndeletable(state, plate).empty());
  CHECK(ui::DeletePart(state, plate, nullptr));
}

TEST_CASE("A circular edge that is the end of a solid is not a hole",
          "[actions][parts]") {
  // From the dock (docs/parts/README.md#deriving-a-fastener-from-a-hole): a
  // 49-part assembly whose every circular edge was the end cap of a SOLID — a
  // roller axle, a ladder rung, a hinge pin, a mooring eye. Not one hole. The
  // verb takes any circular edge as a rim, so all of them were pickable, and
  // the refusal a person got talked about plates and reaching past things and
  // never once said "that is a solid, not a hole".
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  // A plate with a PIN standing on it, rather than a bore through it.
  const size_t tab = ui::AddPart(state, "Axle");
  ui::SetActivePart(state, tab);
  Document& doc = *state.document;
  doc.AddFeature(std::make_unique<BoxFeature>(
      "Plate", glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, 10.0));
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Pin", glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0, 0, 1), 3.3, 40.0));
  doc.AddFeature(std::make_unique<BooleanFeature>(
      "Weld", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
      BooleanOp::Union));

  // The pin's free end: a circle of exactly the diameter an M6 clearance hole
  // would be, so the size lookup happily agrees it is one.
  const Body& body = doc.GetBodies().front();
  ElementRef tip;
  double best = -1e9;
  for (const auto& [index, mapped] : body.elementMap.edgeNames) {
    const std::optional<Measure::EdgeInfo> edge =
        Measure::DescribeEdgeAt(body, index);
    if (!edge || edge->kind != "circle")
      continue;
    if (edge->midpoint.z > best) {
      best = edge->midpoint.z;
      tip = ElementRef{body.createdBy, mapped};
    }
  }
  REQUIRE(tip.IsSet());

  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  Occurrence occ;
  occ.partId = state.parts[tab].id;
  occ.name = "Axle";
  const uint64_t id = state.assembly->AddOccurrence(occ);

  ui::FastenerRequest dry;
  dry.dryRun = true;
  const ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{id, tip}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("end of a SOLID") != std::string::npos);
  CHECK(fit.why.find("Axle") != std::string::npos);
  // And not "-0 mm out", which is what the old message measured it at.
  CHECK(fit.why.find("-0 mm") == std::string::npos);
}

TEST_CASE("A rim split into arcs is still the rim", "[actions][parts]") {
  // From the PES-on-OM615 dogfood: the caller picked a circular edge, and
  // was told to "pick the RIM ... a cylindrical face has an axis but no
  // direction". It WAS the rim — split into arcs by the faces round it, so
  // the edge picked was an arc and not a circle. Here the seat face was two
  // coplanar faces (two blocks fused side by side) before the hole was cut
  // across the seam between them.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const auto plate = [&](const std::string& name, bool groove) {
    const size_t tab = ui::AddPart(state, name);
    ui::SetActivePart(state, tab);
    Document& doc = *state.document;
    doc.AddFeature(std::make_unique<BoxFeature>(
        "West", glm::vec3(-30.0f, -30.0f, 0.0f), 30.0, 60.0, 12.0));
    doc.AddFeature(std::make_unique<BoxFeature>(
        "East", glm::vec3(0.0f, -30.0f, 0.0f), 30.0, 60.0, 12.0));
    doc.AddFeature(std::make_unique<BooleanFeature>(
        "Join", doc.GetFeature(0)->GetId(), doc.GetFeature(1)->GetId(),
        BooleanOp::Union));
    uint64_t solid = doc.GetFeatures().back()->GetId();
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Bore", glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0, 0, 1), 3.3, 14.0));
    doc.AddFeature(std::make_unique<BooleanFeature>(
        "Drill", solid, doc.GetFeatures().back()->GetId(),
        BooleanOp::Subtract));
    solid = doc.GetFeatures().back()->GetId();
    if (groove) {
      // A groove across the seat, through the rim: what is left of it
      // does not go all the way round.
      doc.AddFeature(std::make_unique<BoxFeature>(
          "Groove tool", glm::vec3(-1.0f, -30.0f, 10.0f), 2.0, 60.0, 3.0));
      doc.AddFeature(std::make_unique<BooleanFeature>(
          "Groove", solid, doc.GetFeatures().back()->GetId(),
          BooleanOp::Subtract));
    }
    REQUIRE(doc.Regenerate());
    // The highest ARC of radius 3.3: a piece of the rim, not all of it.
    const Body& body = doc.GetBodies().front();
    ElementRef piece;
    double best = -1e9;
    int circles = 0;
    for (const auto& [index, mapped] : body.elementMap.edgeNames) {
      const std::optional<Measure::EdgeInfo> edge =
          Measure::DescribeEdgeAt(body, index);
      if (!edge || !edge->hasRadius || std::abs(edge->radius - 3.3) > 1e-6)
        continue;
      if (edge->kind == "circle" && edge->midpoint.z > 9.0)
        circles++;
      if (edge->kind == "arc" && edge->midpoint.z > best) {
        best = edge->midpoint.z;
        piece = ElementRef{body.createdBy, mapped};
      }
    }
    // The case is only worth anything if the rim really IS in pieces.
    REQUIRE(circles == 0);
    REQUIRE(piece.IsSet());
    return std::pair{state.parts[tab].id, piece};
  };
  const auto [splitId, splitArc] = plate("Split seat", false);
  const auto [grooveId, grooveArc] = plate("Grooved seat", true);

  ui::SetActivePart(state, ui::AddAssembly(state, "Rig"));
  const auto place = [&](uint64_t partId, float x) {
    Occurrence occ;
    occ.partId = partId;
    occ.name = "Plate";
    occ.position = glm::vec3(x, 0.0f, 0.0f);
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t splitOcc = place(splitId, 0.0f);
  const uint64_t grooveOcc = place(grooveId, 200.0f);

  ui::FastenerRequest req;
  req.dryRun = true;
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{splitOcc, splitArc}, req);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  // Fitted to the whole circle: an M6 by its 6.6 mm clearance, through the
  // 12 mm plate — which an arc's own centroid would have got wrong.
  CHECK(fit.size == "M6");
  CHECK_THAT(fit.grip, WithinAbs(12.0, 1e-3));

  // Arcs that do not close the circle are refused, in those terms.
  fit = ui::FitFastenerToHole(state, ui::RimPick{grooveOcc, grooveArc}, req);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  // Three arcs: the seam splits one of the two the groove leaves. What they
  // cover is the circle less the groove's two chords, 2 x 2 asin(1/3.3).
  CHECK(fit.why.find("this rim is 3 arcs covering 289.4°") !=
        std::string::npos);
  CHECK(fit.why.find("all the way round") != std::string::npos);
}

TEST_CASE("A second pick says where the joint ends", "[actions][parts]") {
  // docs/parts/README.md#deriving-a-fastener-from-a-hole, the second pick. The
  // stack the axis passes through IS the joint — until the axis carries on into
  // something the fastener is not clamping, and then a screw sized to span
  // the lot is twice as long as it should be and looks entirely reasonable.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  // Two flanges to bolt together, and a BOSS further down the same axis that
  // the bolt has no business reaching.
  const DrilledPlate front = MakePlate(state, "Front", 12.0, 6.6, -1.0, 0.0);
  const DrilledPlate rear = MakePlate(state, "Rear", 12.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Boss", 40.0, 6.6, -1.0, 0.0);

  ui::SetActivePart(state, ui::AddAssembly(state, "Housing"));
  const auto place = [&](const char* part, float z) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = glm::vec3(0.0f, 0.0f, z);
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t frontOcc = place("Front", 0.0f); // z 0..12
  const uint64_t rearOcc = place("Rear", -12.0f); // z -12..0
  place("Boss", -52.0f);                          // z -52..-12

  ui::FastenerRequest req;
  req.dryRun = true;

  // Without the second pick it spans everything on the axis — both flanges
  // AND the boss behind them — and here that is not merely oversized, it is
  // unbuildable: 64 mm is past the longest M6 anybody makes. Which is a
  // sharper argument for the second pick than "the screw is a bit long".
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{frontOcc, front.topRim}, req);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("no nominal length reaches 64") != std::string::npos);
  CHECK(fit.why.find("Boss 40") !=
        std::string::npos); // it says what it spanned

  // With it, the joint ends at the rear flange's far face — and everything
  // between the two picks is still traversed, so the summary still names
  // both flanges.
  req.farEnd = ui::RimPick{rearOcc, rear.bottomRim};
  fit = ui::FitFastenerToHole(state, ui::RimPick{frontOcc, front.topRim}, req);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK_THAT(fit.grip, WithinAbs(24.0, 1e-3)); // the two flanges only
  CHECK(fit.summary.find("Front") != std::string::npos);
  CHECK(fit.summary.find("Rear") != std::string::npos);
  // The summary must agree with the grip beside it. Found on a real casing:
  // it kept listing the part past the cut, so "grip 65: ... DE rib 30 ..."
  // showed numbers totalling 95 — the decomposition is the product, and one
  // that does not add up is worse than none.
  const std::string listed = fit.summary.substr(0, fit.summary.find(';'));
  CHECK(listed.find("Boss") == std::string::npos);
  // But the axis DOES carry on, and saying into what is the confirmation
  // that the second pick did what you meant.
  CHECK(fit.summary.find("carries on into Boss") != std::string::npos);
  // 24 is a length an M6 is made in; 64 would have been a 65.
  CHECK_THAT(fit.length, WithinAbs(25.0, 1e-9));

  // A far end that is not on the axis is a mis-pick, and says which.
  const DrilledPlate off = MakePlate(state, "Off", 10.0, 6.6, -1.0, 25.0);
  ui::SetActivePart(state, ui::AddAssembly(state, "Skew rig"));
  const uint64_t f2 = place("Front", 0.0f);
  const uint64_t offOcc = place("Off", -30.0f);
  ui::FastenerRequest bad;
  bad.dryRun = true;
  bad.farEnd = ui::RimPick{offOcc, off.topRim};
  fit = ui::FitFastenerToHole(state, ui::RimPick{f2, front.topRim}, bad);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("off this hole's axis") != std::string::npos);

  // And one on the head's side of the rim: it names where the fastener
  // STOPS, which has to be past where it starts.
  ui::FastenerRequest backwards;
  backwards.dryRun = true;
  backwards.farEnd = ui::RimPick{f2, front.topRim};
  fit = ui::FitFastenerToHole(state, ui::RimPick{f2, front.topRim}, backwards);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("head's side") != std::string::npos);
}

TEST_CASE("A far end on several placements is refused, not taken as the first",
          "[actions][parts]") {
  // An element name is unique within a PART, so several placements answer to
  // it — every part placed twice, and every part built the same way as
  // another. The RIM has always refused that and named them; the second pick
  // said it "resolved the same way" and took the first, which fitted a bolt
  // on a grill's right-hand cart side to the hole in its LEFT one, 624 mm
  // away, and only a disagreement about the axis gave it away
  // (docs/parts/README.md#deriving-a-fastener-from-a-hole).
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate front = MakePlate(state, "Front", 12.0, 6.6, -1.0, 0.0);
  const DrilledPlate rear = MakePlate(state, "Rear", 12.0, 6.6, -1.0, 0.0);

  ui::SetActivePart(state, ui::AddAssembly(state, "Two of them"));
  const auto place = [&](const char* part, float z) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = glm::vec3(0.0f, 0.0f, z);
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t frontOcc = place("Front", 0.0f); // z 0..12
  const uint64_t rearOcc = place("Rear", -12.0f); // z -12..0
  place("Rear", -40.0f); // the same part again, further down the axis

  ui::FastenerRequest req;
  req.dryRun = true;
  req.farEnd = ui::RimPick{0, rear.bottomRim}; // which Rear? which plate?
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{frontOcc, front.topRim}, req);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  // THREE, not two: Front is a different part that MakePlate built the same
  // way, so it answers to the name as well — which is the half of this
  // ambiguity that is easy to forget, and the reason the message lists them.
  CHECK(fit.why.find("3 placements") != std::string::npos);
  CHECK(fit.why.find("which occurrence") != std::string::npos);
  CHECK(fit.why.find("occurrence 2") != std::string::npos);
  CHECK(fit.why.find("occurrence 3") != std::string::npos);

  // Said which, it is the ordinary two-plate joint it always was.
  req.farEnd = ui::RimPick{rearOcc, rear.bottomRim};
  fit = ui::FitFastenerToHole(state, ui::RimPick{frontOcc, front.topRim}, req);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK_THAT(fit.grip, WithinAbs(24.0, 1e-3));
}

TEST_CASE("A thread ends a joint the way a nut does",
          "[actions][parts][thread]") {
  // A screw into a tapped BOSS has no nut to end its joint, and its bore runs
  // out into open air rather than stopping on a floor. Re-derived from the
  // rim — which is all check_fasteners has — the stack carried on down the
  // axis into whatever was behind it, and two correct screws in a grill's lid
  // handle were reported as having "0 mm of thread" in a part they do not
  // touch (docs/parts/README.md#deriving-a-fastener-from-a-hole). The threads
  // were in the model the whole time; nothing read them.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate cover = MakePlate(state, "Cover", 3.0, 6.6, -1.0, 0.0);
  {
    const size_t tab = ui::AddPart(state, "Boss");
    ui::SetActivePart(state, tab);
    Document& doc = *state.document;
    doc.AddFeature(std::make_unique<BoxFeature>(
        "Boss", glm::vec3(-20.0f, -20.0f, 0.0f), 40.0, 40.0, 12.0));
    auto centre = std::make_unique<SketchFeature>(
        "Centre", SketchPlane(glm::vec3(0, 0, 12), glm::vec3(0, 0, 1),
                              glm::vec3(1, 0, 0)));
    centre->AddEntity(sketch::Point{{0, 0}, 1});
    doc.AddFeature(std::move(centre));
    auto hole = std::make_unique<HoleFeature>(
        "Tapping", doc.GetFeature(doc.FeatureCount() - 1)->GetId(),
        HoleKind::Simple, 5.0, 12.0);
    hole->through = true; // tapped right through, into open air below
    hole->fitThread = "M6";
    hole->fitKind = "tapped";
    doc.AddFeature(std::move(hole));
    doc.Regenerate();
    REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
    REQUIRE(doc.GetBodies()[0].threadRegions.size() == 1);
  }
  MakePlate(state, "Far", 10.0, 6.6, -1.0, 0.0);

  ui::SetActivePart(state, ui::AddAssembly(state, "Boss rig"));
  const auto place = [&](const char* part, float z) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = glm::vec3(0.0f, 0.0f, z);
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t coverOcc = place("Cover", 0.0f); // z 0..3
  place("Boss", -12.0f);                          // z -12..0, tapped through
  place("Far", -40.0f);                           // z -40..-30, not in it

  ui::FastenerRequest req;
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{coverOcc, cover.topRim}, req);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK(fit.blind); // held by the thread, not by anything beyond it
  CHECK_THAT(fit.grip, WithinAbs(15.0, 1e-3)); // the cover and the tapping
  // The joint is the cover and the boss; what the axis carries on into is
  // NAMED but not counted, as it is for a second pick.
  const std::string listed = fit.summary.substr(0, fit.summary.find(';'));
  CHECK(listed.find("Far") == std::string::npos);
  CHECK(fit.summary.find("carries on into Far") != std::string::npos);

  // And the check, which stores nothing and re-derives all of it, agrees.
  const ui::FastenerReport report = ui::CheckFasteners(state);
  for (const ui::FastenerCheck& finding : report.findings)
    INFO(finding.part << ": " << finding.why);
  CHECK(report.examined == 1);
  CHECK(report.findings.empty());
}

TEST_CASE("A countersunk screw needs a countersunk hole", "[actions][parts]") {
  // F1 refused iso10642 outright: the rim you pick on a plain bore is the
  // bore's, so the head would end up inside solid material. Two things
  // shipped since dissolve that — hole-standards gives a hole a recorded
  // thread, and FindHoleForRef already matches a picked circle against the
  // COUNTERSINK radius as well as the bore — so the blanket refusal becomes
  // three real questions (docs/parts/README.md).
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  // Read from the table rather than typed, so this tests against ISO 10642.
  std::string why;
  const auto dims = parts::Dimensions("iso10642", "M6", &why);
  INFO(why);
  REQUIRE(dims.has_value());
  const double headHeight = dims->at("csh_height");
  const double csDia = dims->at("csh_diam");

  const auto plate = [&](const char* name, HoleKind kind, double csDepth) {
    const size_t tab = ui::AddPart(state, name);
    ui::SetActivePart(state, tab);
    Document& doc = *state.document;
    doc.AddFeature(std::make_unique<BoxFeature>(
        name, glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, 20.0));
    doc.AddFeature(std::make_unique<SketchFeature>(
        "Centre", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f))));
    const uint64_t sketchId = doc.GetFeatures().back()->GetId();
    sketch::Point centre;
    centre.p = glm::vec2(0.0f, 0.0f);
    centre.id = 1;
    dynamic_cast<SketchFeature*>(doc.GetFeatures().back().get())
        ->entities.push_back(centre);
    auto hole =
        std::make_unique<HoleFeature>("Drill", sketchId, kind, 1.0, 25.0);
    hole->through = true;
    hole->fitThread = "M6";
    hole->fitKind = "normal";
    hole->csDiameter = csDia;
    hole->csDepth = csDepth;
    doc.AddFeature(std::move(hole));
    doc.Regenerate();
    return tab;
  };

  const auto rimOf = [&](size_t tab, double wantDia) {
    ui::SetActivePart(state, tab);
    const Body& body = state.document->GetBodies().front();
    ElementRef rim;
    for (const auto& [index, mapped] : body.elementMap.edgeNames) {
      const std::optional<Measure::EdgeInfo> edge =
          Measure::DescribeEdgeAt(body, index);
      if (!edge || edge->kind != "circle")
        continue;
      if (std::abs(edge->radius * 2.0 - wantDia) < 1e-3 &&
          edge->midpoint.z > 19.0)
        rim = ElementRef{body.createdBy, mapped};
    }
    return rim;
  };

  const auto rig = [&](const char* name, size_t tab) {
    ui::SetActivePart(state, ui::AddAssembly(state, name));
    Occurrence o;
    o.partId = state.parts[tab].id;
    o.name = name;
    return state.assembly->AddOccurrence(o);
  };

  ui::FastenerRequest csk;
  csk.standard = "iso10642";
  csk.dryRun = true;

  // 1. A plain bore: refused, naming what the hole has to be.
  const size_t plainTab = plate("Plain", HoleKind::Simple, 0.0);
  const ElementRef plainRim = rimOf(plainTab, 6.6);
  REQUIRE(plainRim.IsSet());
  const uint64_t plainOcc = rig("Plain rig", plainTab);
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{plainOcc, plainRim}, csk);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("COUNTERSUNK hole") != std::string::npos);

  // 2. Countersunk but too SHALLOW: the head would stand proud of a surface
  //    machined to swallow it, which is invisible at render distance.
  const size_t shallowTab = plate("Shallow", HoleKind::Countersink, 2.0);
  const ElementRef shallowRim = rimOf(shallowTab, csDia);
  REQUIRE(shallowRim.IsSet());
  const uint64_t shallowOcc = rig("Shallow rig", shallowTab);
  fit = ui::FitFastenerToHole(state, ui::RimPick{shallowOcc, shallowRim}, csk);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("stand proud") != std::string::npos);

  // 3. Deep enough: it fits.
  const size_t deepTab = plate("Deep", HoleKind::Countersink, headHeight + 0.5);
  const ElementRef deepRim = rimOf(deepTab, csDia);
  REQUIRE(deepRim.IsSet());
  const uint64_t deepOcc = rig("Deep rig", deepTab);
  fit = ui::FitFastenerToHole(state, ui::RimPick{deepOcc, deepRim}, csk);
  INFO(fit.why);
  REQUIRE(fit.ok);
  CHECK(fit.size == "M6");
  CHECK_THAT(fit.grip, WithinAbs(20.0, 1e-3)); // the plate, from its surface

  // And a washer under a countersunk head is refused: the head bears on the
  // cone, not on the surface.
  ui::FastenerRequest washered = csk;
  washered.washerUnderHead = true;
  fit = ui::FitFastenerToHole(state, ui::RimPick{deepOcc, deepRim}, washered);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("bears on the cone") != std::string::npos);
}

TEST_CASE("A bolt through two parts mates them", "[actions][parts][assembly]") {
  // Reported from use 2026-09-01, twice: "Did we not build the mate for the
  // fasteners?", then "I also don't see the mates in the mate list".
  //
  // The hardware WAS following its plate — attachment worked. What was
  // missing is the thing a bolt is actually for: it holds the two parts to
  // EACH OTHER. Rotating one bar was still legal, so the joint tore open with
  // the screw riding one half and the nut the other, and the place a person
  // looks to see what holds an assembly together was empty.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const std::vector<double> bores{-15.0, 15.0};
  const std::vector<ElementRef> topRims =
      MakeMultiBorePlate(state, "Top", 12.0, 6.6, bores);
  MakeMultiBorePlate(state, "Bottom", 26.0, 6.6, bores);
  REQUIRE(topRims.size() == 2);
  REQUIRE(topRims[0].IsSet());
  REQUIRE(topRims[1].IsSet());

  ui::SetActivePart(state, ui::AddAssembly(state, "Bolted"));
  const auto place = [&](const char* part, glm::vec3 at) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = at;
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t topOcc = place("Top", glm::vec3(0.0f));
  const uint64_t bottomOcc = place("Bottom", glm::vec3(0.0f, 0.0f, -26.0f));

  const auto occOf = [&](uint64_t id) { return state.assembly->FindById(id); };
  const auto worldOf = [&](uint64_t id) {
    return ui::OccurrenceMatrix(*occOf(id));
  };
  const glm::mat4 before = glm::inverse(worldOf(topOcc)) * worldOf(bottomOcc);

  ui::FastenerRequest request;
  request.nut = "iso4032";
  const ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{topOcc, topRims[0]}, request);
  INFO(fit.why);
  REQUIRE(fit.ok);
  REQUIRE(fit.mates.size() == 1);
  REQUIRE(state.assembly->GetMates().size() == 1);

  // Placing a fastener must not MOVE anything. The mate records the pose the
  // parts are already in, so the solve that follows is a no-op — a mate that
  // "corrects" a person's placement is a mate that breaks the model it was
  // meant to record.
  CHECK(ui::SolveActiveAssembly(state) == 0);
  const glm::mat4 placedAt = glm::inverse(worldOf(topOcc)) * worldOf(bottomOcc);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      CHECK_THAT(placedAt[c][r], WithinAbs(before[c][r], 1e-3));

  // Now the point: move ONE bar and the other comes with it. Rotation as well
  // as translation, because a mate that only held the offset would have let
  // the joint twist, and twisting is exactly what the report showed.
  Occurrence* top = occOf(topOcc);
  top->position += glm::vec3(0.0f, 5.0f, 3.0f);
  top->orientation =
      glm::angleAxis(glm::radians(-21.432f), glm::vec3(0, 0, 1)) *
      top->orientation;
  CHECK(ui::SolveActiveAssembly(state) == 0);

  const glm::mat4 after = glm::inverse(worldOf(topOcc)) * worldOf(bottomOcc);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      CHECK_THAT(after[c][r], WithinAbs(before[c][r], 1e-3));

  // And the hardware rode along with its own half, so the joint is whole.
  const glm::vec3 axisAfter =
      glm::vec3(worldOf(topOcc) * glm::vec4(0, 0, 1, 0));
  const glm::vec3 screwToNut =
      occOf(fit.placed.front())->position - occOf(fit.placed.back())->position;
  CHECK_THAT(glm::length(glm::cross(glm::normalize(screwToNut), axisAfter)),
             WithinAbs(0.0, 1e-3));

  // A SECOND bolt between the same two parts gets its OWN mate, anchored at
  // its own hole — delete either bolt and the other still holds the joint,
  // which is what a bolt pattern means. It closes a loop, and A6 solves that
  // loop as redundant-but-consistent rather than erroring on it (which is
  // exactly why this used to add nothing).
  const ui::FastenerFit second =
      ui::FitFastenerToHole(state, ui::RimPick{topOcc, topRims[1]}, request);
  INFO(second.why);
  REQUIRE(second.ok);
  CHECK(second.mates.size() == 1);
  CHECK(state.assembly->GetMates().size() == 2);

  // The loop is REDUNDANT, not errored, and the second mate is the one that
  // says nothing new. Nothing moves: both mates record a pose the parts are
  // already in.
  const assembly::SolveReport bolted = ui::SolveActiveAssemblyReport(state);
  CHECK(bolted.erroredMates == 0);
  CHECK(bolted.loopMates == 1);
  CHECK(bolted.loopsClosed);
  CHECK(bolted.dof == 0);
  CHECK(bolted.redundantMates.size() == 1);
  CHECK(bolted.redundantMates.front() == second.mates.front());
  for (const Mate& m : state.assembly->GetMates())
    CHECK(m.error.empty());
  const glm::mat4 twoBolts = glm::inverse(worldOf(topOcc)) * worldOf(bottomOcc);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++)
      CHECK_THAT(twoBolts[c][r], WithinAbs(before[c][r], 1e-3));
  // A part clamped in the MIDDLE of the stack is joined too. The alternator's
  // stator sits between two housings and is bolted to neither, which is the
  // same report one layer in: mating only the two ENDS of a joint leaves
  // whatever it clamps between them floating.
  MakeMultiBorePlate(state, "Middle", 8.0, 6.6, bores);
  ui::SetActivePart(state, ui::AddAssembly(state, "Sandwich"));
  const uint64_t s1 = place("Top", glm::vec3(0.0f));
  const uint64_t s2 = place("Middle", glm::vec3(0.0f, 0.0f, -8.0f));
  const uint64_t s3 = place("Bottom", glm::vec3(0.0f, 0.0f, -34.0f));
  const ui::FastenerFit sandwich =
      ui::FitFastenerToHole(state, ui::RimPick{s1, topRims[0]}, request);
  INFO(sandwich.why);
  REQUIRE(sandwich.ok);
  CHECK(sandwich.mates.size() == 2);             // one per interface
  CHECK(state.assembly->GetMates().size() == 2); // Top-Middle, Middle-Bottom
  CHECK(ui::SolveActiveAssembly(state) == 0);
  const auto sWorld = [&](uint64_t id) {
    return ui::OccurrenceMatrix(*state.assembly->FindById(id));
  };
  const glm::mat4 midWas = glm::inverse(sWorld(s1)) * sWorld(s2);
  const glm::mat4 lowWas = glm::inverse(sWorld(s1)) * sWorld(s3);
  state.assembly->FindById(s1)->position += glm::vec3(4.0f, 0.0f, 7.0f);
  CHECK(ui::SolveActiveAssembly(state) == 0);
  for (int c = 0; c < 4; c++)
    for (int r = 0; r < 4; r++) {
      CHECK_THAT((glm::inverse(sWorld(s1)) * sWorld(s2))[c][r],
                 WithinAbs(midWas[c][r], 1e-3));
      CHECK_THAT((glm::inverse(sWorld(s1)) * sWorld(s3))[c][r],
                 WithinAbs(lowWas[c][r], 1e-3));
    }
}

TEST_CASE("A placed fastener rides on the part it bears on",
          "[actions][parts]") {
  // Reported from use: "I get surprised by the parts and fasteners being
  // movable after having placed them". Two halves to that — a placed screw
  // had a drag gizmo (GizmoOccurrence hands one to any selected non-fixed
  // occurrence) AND did not follow its plate, because the solver leaves an
  // unmated occurrence untouched. Neither held nor carried.
  //
  // Not a mate: the HARDWARE is a zero-DOF leaf, so this is a composition
  // (docs/parts/README.md#attached-and-mated). The joint between the two PARTS
  // is a mate, and it took a second report to see that — see "A bolt through
  // two parts mates them" below.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate top = MakePlate(state, "Top", 12.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Bottom", 26.0, 6.6, -1.0, 0.0);
  ui::SetActivePart(state, ui::AddAssembly(state, "Bolted"));
  const auto place = [&](const char* part, glm::vec3 at) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = at;
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t topOcc = place("Top", glm::vec3(0.0f));
  const uint64_t bottomOcc = place("Bottom", glm::vec3(0.0f, 0.0f, -26.0f));

  ui::FastenerRequest request;
  request.nut = "iso4032";
  request.washerUnderHead = true;
  request.washerUnderNut = true;
  const ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{topOcc, top.topRim}, request);
  INFO(fit.why);
  REQUIRE(fit.ok);
  REQUIRE(fit.placed.size() == 4);

  const auto occOf = [&](uint64_t id) { return state.assembly->FindById(id); };
  // The head end rides the part the rim was picked in; the far end rides
  // whatever the axis leaves LAST. Attaching the nut to the near plate would
  // stretch a rigid group across a joint that is allowed to open.
  CHECK(occOf(fit.placed[0])->attachedTo == topOcc);    // screw
  CHECK(occOf(fit.placed[1])->attachedTo == topOcc);    // washer under head
  CHECK(occOf(fit.placed[2])->attachedTo == bottomOcc); // washer under nut
  CHECK(occOf(fit.placed[3])->attachedTo == bottomOcc); // nut

  // Move the plate: the screw goes with it. This is the half check_fasteners
  // could only report after the fact.
  const glm::vec3 screwWas = occOf(fit.placed[0])->position;
  const glm::vec3 nutWas = occOf(fit.placed[3])->position;
  state.assembly->FindById(topOcc)->position += glm::vec3(0.0f, 5.0f, 0.0f);
  state.assembly->FindById(bottomOcc)->position += glm::vec3(0.0f, 5.0f, 0.0f);
  CHECK(assembly::ApplyAttachments(*state.assembly) == 4);
  CHECK_THAT(occOf(fit.placed[0])->position.y,
             WithinAbs(screwWas.y + 5.0, 1e-4));
  CHECK_THAT(occOf(fit.placed[3])->position.y, WithinAbs(nutWas.y + 5.0, 1e-4));
  // And nothing else moved: an attachment is a composition, so re-running it
  // with nothing changed is a no-op rather than a slow drift.
  CHECK(assembly::ApplyAttachments(*state.assembly) == 0);

  // Deleting the plate takes what rides on it — a screw whose plate is gone
  // is not a screw, and an orphan is the floating hardware this was for.
  ui::SelectOccurrence(state, topOcc);
  CHECK(ui::DeleteSelectedOccurrences(state) == 3); // the plate + 2 riders
  CHECK(occOf(fit.placed[0]) == nullptr);
  CHECK(occOf(fit.placed[1]) == nullptr);
  CHECK(occOf(fit.placed[3]) != nullptr); // the nut rides the OTHER plate
}

TEST_CASE("A hole that says what it is for is believed over its diameter",
          "[actions][parts]") {
  // The payoff hole-standards.md was written for, and the refusal F1 promised
  // and could not make. A blind CLEARANCE hole and a blind TAPPED hole look
  // identical to a traversal — same axis, same depth — and a machine screw
  // has nothing to bite in the first. Only the hole can say which it is.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const auto plate = [&](const char* name, const char* fitKind) {
    const size_t tab = ui::AddPart(state, name);
    ui::SetActivePart(state, tab);
    Document& doc = *state.document;
    doc.AddFeature(std::make_unique<BoxFeature>(
        name, glm::vec3(-30.0f, -30.0f, 0.0f), 60.0, 60.0, 20.0));
    // On the plate's top face: the hole drills along -normal, i.e. down.
    auto sketch = std::make_unique<SketchFeature>(
        "Centre", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f)));
    sketch::Point centre;
    centre.p = glm::vec2(0.0f, 0.0f);
    centre.id = 1;
    sketch->entities.push_back(centre);
    doc.AddFeature(std::move(sketch));
    // AFTER adding: the document assigns the id, so asking before gives 0 and
    // the hole then reports a missing sketch.
    const uint64_t sketchId = doc.GetFeatures().back()->GetId();
    auto hole = std::make_unique<HoleFeature>("Drill", sketchId,
                                              HoleKind::Simple, 1.0, 12.0);
    hole->fitThread = "M6";
    hole->fitKind = fitKind;
    doc.AddFeature(std::move(hole));
    const auto* made =
        dynamic_cast<const HoleFeature*>(doc.GetFeatures().back().get());
    INFO(made->GetError());
    REQUIRE(made->GetError().empty());
    return std::make_pair(tab, made->diameter);
  };

  // The fit DERIVED the bore: 6.6 for a normal clearance, 5.0 tapped.
  const auto [clearTab, clearDia] = plate("Clearance", "normal");
  CHECK_THAT(clearDia, WithinAbs(6.6, 1e-9));
  const auto [tapTab, tapDia] = plate("Tapped", "tapped");
  CHECK_THAT(tapDia, WithinAbs(5.0, 1e-9));

  const auto topRimOf = [&](size_t tab) {
    ui::SetActivePart(state, tab);
    const Body& body = state.document->GetBodies().front();
    ElementRef rim;
    double best = -1e9;
    for (const auto& [index, mapped] : body.elementMap.edgeNames) {
      const std::optional<Measure::EdgeInfo> edge =
          Measure::DescribeEdgeAt(body, index);
      if (!edge || edge->kind != "circle")
        continue;
      if (edge->midpoint.z > best) {
        best = edge->midpoint.z;
        rim = ElementRef{body.createdBy, mapped};
      }
    }
    return rim;
  };
  const ElementRef clearRim = topRimOf(clearTab);
  const ElementRef tapRim = topRimOf(tapTab);
  REQUIRE(clearRim.IsSet());
  REQUIRE(tapRim.IsSet());

  ui::FastenerRequest dry;
  dry.dryRun = true;

  // The TAPPED one takes a screw, and its size is a FACT out of the hole
  // rather than a diameter read backwards — 5.0 is no clearance size at all,
  // so the backwards lookup would have refused it outright.
  ui::SetActivePart(state, ui::AddAssembly(state, "Tapped rig"));
  Occurrence t;
  t.partId = state.parts[tapTab].id;
  t.name = "Tapped";
  const uint64_t tapOcc = state.assembly->AddOccurrence(t);
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{tapOcc, tapRim}, dry);
  INFO(fit.why);
  REQUIRE(fit.ok);
  CHECK(fit.size == "M6");
  CHECK(fit.clearance == "tapped");
  CHECK(fit.blind);

  // The CLEARANCE one is the same shape and holds nothing, and now says so.
  ui::SetActivePart(state, ui::AddAssembly(state, "Clearance rig"));
  Occurrence c;
  c.partId = state.parts[clearTab].id;
  c.name = "Clearance";
  const uint64_t clearOcc = state.assembly->AddOccurrence(c);
  fit = ui::FitFastenerToHole(state, ui::RimPick{clearOcc, clearRim}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("CLEARANCE hole") != std::string::npos);
  CHECK(fit.why.find("nothing to bite") != std::string::npos);
}

TEST_CASE("A rim on two placements of one part names which",
          "[actions][parts]") {
  // The bug the click path would have hit on its first try, found while
  // planning it (docs/parts/README.md#deriving-a-fastener-from-a-hole): every
  // occurrence of a part carries every one of its element names, so an
  // ElementRef alone does not name a rim in an assembly. Resolving it by first
  // match put the fastener at the NEAR end of a joint made of two of the same
  // plate — the ordinary case, and wrong in a way nothing shows.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate bar = MakePlate(state, "Bar", 10.0, 6.6, -1.0, 0.0);
  ui::SetActivePart(state, ui::AddAssembly(state, "Stack"));
  const auto place = [&](float z) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, "Bar");
    occ.name = "Bar";
    occ.position = glm::vec3(0.0f, 0.0f, z);
    return state.assembly->AddOccurrence(occ);
  };
  const uint64_t near = place(0.0f);  // spans z 0..10
  const uint64_t far = place(-10.0f); // spans z -10..0

  ui::FastenerRequest dry;
  dry.dryRun = true;

  // With no occurrence named it REFUSES, naming both placements. Taking the
  // first is exactly the substitution this codebase refuses everywhere else,
  // and here the substitute looks perfect in the viewport.
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{0, bar.topRim}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("2 placements") != std::string::npos);
  CHECK(fit.why.find("unique within a PART") != std::string::npos);
  CHECK(fit.why.find("occurrence") != std::string::npos);

  // Named, each resolves to ITS OWN rim. The near plate's rim is at the top
  // of the stack and spans both; the far plate's is 10 mm lower and spans
  // only itself, because the stack below it is empty.
  fit = ui::FitFastenerToHole(state, ui::RimPick{near, bar.topRim}, dry);
  INFO(fit.why);
  REQUIRE(fit.ok);
  CHECK_THAT(fit.grip, WithinAbs(20.0, 1e-3));

  fit = ui::FitFastenerToHole(state, ui::RimPick{far, bar.topRim}, dry);
  INFO(fit.why);
  REQUIRE(fit.ok);
  CHECK_THAT(fit.grip, WithinAbs(10.0, 1e-3));

  // And an occurrence that does not carry it says so rather than falling
  // back to one that does.
  fit = ui::FitFastenerToHole(state, ui::RimPick{9999, bar.topRim}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("does not carry that rim") != std::string::npos);
}

TEST_CASE("Save works from every kind of tab, not just a part", "[actions]") {
  // Reported 2026-09-01 from a real session: File > Save and File > Save As
  // did nothing and opened no dialog, while File > Open opened one — which
  // read as a broken dialog and was a broken GUARD. Both save paths tested
  // `state.document`, and that is NULL on an assembly or a drawing tab
  // (AddAssembly/AddDrawing null it), so you could build a whole assembly,
  // press Ctrl+S, and get no file and no message.
  //
  // Driven through ui::SaveProjectAction — the same entry the menu item and
  // the Ctrl+S binding both call — with a project path already set, so it
  // takes the save-in-place branch and never reaches a native dialog. That
  // matters here beyond speed: a stray dialog in a test run puts a real file
  // picker on the developer's desktop.
  namespace fs = std::filesystem;

  const auto saveFrom = [](const std::string& what, auto&& openTab) {
    ui::AppState state;
    state.document = std::make_unique<Document>();
    state.commands = std::make_unique<CommandStack>();
    ui::EnsureParts(state);
    state.document->AddFeature(std::make_unique<BoxFeature>(
        "Block", glm::vec3(0.0f), 10.0, 10.0, 10.0));

    const fs::path dir =
        fs::temp_directory_path() / ("designer_save_" + what + "_tab");
    fs::remove_all(dir);
    // Stamped against a path that does not exist yet, so the F8 "changed on
    // disk" guard has nothing to complain about and the write goes through.
    ui::MarkProjectSaved(state, dir.string(), "save");

    openTab(state);
    ui::SaveProjectAction(state);

    INFO("saving from a " << what << " tab wrote nothing to " << dir);
    const bool wrote = fs::exists(dir);
    fs::remove_all(dir);
    return wrote;
  };

  CHECK(saveFrom("part", [](ui::AppState&) {}));
  CHECK(saveFrom("assembly", [](ui::AppState& state) {
    ui::SetActivePart(state, ui::AddAssembly(state, "Assembly"));
    REQUIRE(state.document == nullptr); // the condition that broke it
  }));
  CHECK(saveFrom("drawing", [](ui::AppState& state) {
    ui::SetActivePart(state, ui::AddDrawing(state, "Drawing"));
    REQUIRE(state.document == nullptr);
  }));
}

TEST_CASE(
    "A joint is derived whole: washers, nut and a length that covers them",
    "[actions][parts]") {
  // docs/parts/README.md#deriving-a-fastener-from-a-hole. The argument for
  // deriving at all is that a typed length is right for the stack somebody had
  // in mind and wrong for every other — and a joint is where that bites
  // hardest, because a screw sized for the plates and THEN fitted with two
  // washers and a nut is too short by the 8.4 mm of hardware and looks
  // perfectly correct in a viewport.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  const DrilledPlate top = MakePlate(state, "Top", 12.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Bottom", 26.0, 6.6, -1.0, 0.0);
  const DrilledPlate blind = MakePlate(state, "Blind", 20.0, 6.6, 8.0, 0.0);
  const DrilledPlate tall = MakePlate(state, "Tall", 110.0, 6.6, -1.0, 0.0);

  // Returns the occurrence id: an element name is unique within a PART, so
  // two plates built the same way carry the same one and the id is what tells
  // them apart (docs/parts/README.md#deriving-a-fastener-from-a-hole).
  const auto place = [&](const char* part, glm::vec3 at) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = at;
    return state.assembly->AddOccurrence(occ);
  };

  ui::SetActivePart(state, ui::AddAssembly(state, "Bolted"));
  const uint64_t topOcc = place("Top", glm::vec3(0.0f, 0.0f, 0.0f));
  place("Bottom", glm::vec3(0.0f, 0.0f, -26.0f));

  ui::FastenerRequest request;
  request.nut = "iso4032";
  request.washerUnderHead = true;
  request.washerUnderNut = true;
  request.dryRun = true;
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{topOcc, top.topRim}, request);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  // 38 mm of plate, two 1.6 mm washers, and the nut's own 5.2 mm — "enough
  // thread to fill the nut" is its height, which is the shortest length at
  // which every one of its threads is engaged.
  CHECK_THAT(fit.grip, WithinAbs(1.6 + 38.0 + 1.6 + 5.2, 1e-3));
  CHECK_THAT(fit.length, WithinAbs(50.0, 1e-9));
  // Without them the same hole wants 40, so the hardware moves the answer two
  // nominal lengths — not a rounding, and not something a person adds in their
  // head reliably.
  ui::FastenerRequest bare;
  bare.dryRun = true;
  CHECK_THAT(ui::FitFastenerToHole(state, ui::RimPick{topOcc, top.topRim}, bare)
                 .length,
             WithinAbs(40.0, 1e-9));
  CHECK(fit.summary.find("washer 1.6") != std::string::npos);
  CHECK(fit.summary.find("nut 5.2") != std::string::npos);

  // And placed: four occurrences off one pick, three part documents, because
  // the two washers are the same spec and `bom` aggregates them.
  request.dryRun = false;
  fit = ui::FitFastenerToHole(state, ui::RimPick{topOcc, top.topRim}, request);
  INFO(fit.why);
  REQUIRE(fit.ok);
  REQUIRE(fit.placed.size() == 4);
  CHECK(fit.parts[0] == "ISO 4762 M6x50");
  CHECK(fit.parts[1] == "ISO 7089 M6");
  CHECK(fit.parts[2] == "ISO 7089 M6");
  CHECK(fit.parts[3] == "ISO 4032 M6");

  // Each one bearing-face down on the surface it clamps. The screw's own
  // origin is pushed OUT by the washer under its head — placing it at the rim
  // would bury the head in the washer, which renders as a screw sitting a
  // millimetre and a half too low and nothing else.
  const auto at = [&](uint64_t id) {
    for (const Occurrence& o : state.assembly->GetOccurrences())
      if (o.id == id)
        return o.position;
    return glm::vec3(0.0f);
  };
  CHECK_THAT(at(fit.placed[0]).z, WithinAbs(12.0 + 1.6, 1e-3));  // screw
  CHECK_THAT(at(fit.placed[1]).z, WithinAbs(12.0, 1e-3));        // its washer
  CHECK_THAT(at(fit.placed[2]).z, WithinAbs(-26.0, 1e-3));       // far washer
  CHECK_THAT(at(fit.placed[3]).z, WithinAbs(-26.0 - 1.6, 1e-3)); // the nut

  // Nothing is wrong with the joint that was just derived, and the check
  // proves it by RE-deriving rather than by trusting what placed it: the
  // washers and the nut are occurrences on the same axis, so re-traversing
  // finds exactly the 46.4 mm the length was snapped for.
  CHECK(ui::CheckFasteners(state).findings.empty());

  // Then the stack changes under it: the bottom plate is moved 15 mm in a
  // redesign and the joint is left behind. Deriving a length from other
  // documents' geometry cannot be a live binding (A2), so what this has to do
  // is SAY so rather than quietly keep the number it was fitted with.
  //
  // What it says changed with the alternator round, and the change is the
  // point rather than a weakening. A joint now ends at its NUT — the check
  // was calling three correctly sized through-bolts short because the axis
  // ran on into a rib they never spanned — and under that rule this screw
  // does reach its own nut, so "too short" is no longer the true complaint.
  // The true one is that the plate slid over the hardware: the nut is now
  // 15 mm INSIDE the bottom plate, bearing on nothing.
  for (Occurrence& o : state.assembly->GetOccurrences())
    if (o.name == "Bottom")
      o.position.z -= 15.0f;
  state.assembly->BumpVersion(); // the solved poses are cached per version
  const std::vector<ui::FastenerCheck> stale =
      ui::CheckFasteners(state).findings;
  REQUIRE(stale.size() == 1);
  INFO(stale[0].why);
  CHECK(stale[0].part == "ISO 4762 M6x50");
  CHECK(stale[0].why.find("nut buried in Bottom") != std::string::npos);
  CHECK(stale[0].why.find("clamps nothing") != std::string::npos);

  // A nut on a blind hole has nothing to thread onto. Refused by name: the
  // alternative is a nut floating inside the plate, which is invisible from
  // outside and exactly the class of error this whole verb exists to stop.
  ui::SetActivePart(state, ui::AddAssembly(state, "Tall blind joint"));
  const uint64_t tallOcc = place("Tall", glm::vec3(0, 0, 20));
  place("Blind", glm::vec3(0));
  fit = ui::FitFastenerToHole(state, ui::RimPick{tallOcc, tall.topRim}, bare);
  REQUIRE_FALSE(fit.ok);
  CHECK(fit.why.find("thread engagement") != std::string::npos);

  ui::SetActivePart(state, ui::AddAssembly(state, "Tapped"));
  const uint64_t blindOcc = place("Blind", glm::vec3(0.0f));
  ui::FastenerRequest tapped;
  tapped.nut = "iso4032";
  tapped.dryRun = true;
  fit =
      ui::FitFastenerToHole(state, ui::RimPick{blindOcc, blind.topRim}, tapped);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("no far side") != std::string::npos);

  // The other way a fastener goes wrong, which its length alone cannot show:
  // a screw longer than the blind hole it sits in lands on the floor and
  // clamps nothing, while looking identical from outside.
  ui::AddFastener(state, "iso4762", "M6", 20.0, "",
                  glm::vec3(0.0f, 0.0f, 20.0f),
                  glm::quat(0.0f, 1.0f, 0.0f, 0.0f), nullptr);
  const std::vector<ui::FastenerCheck> bottomed =
      ui::CheckFasteners(state).findings;
  REQUIRE(bottomed.size() == 1);
  INFO(bottomed[0].why);
  CHECK(bottomed[0].why.find("bottoms out") != std::string::npos);
  CHECK(bottomed[0].why.find("8 mm") != std::string::npos);

  // And a washer standard that is not a washer, which is the same mistake one
  // level up: it would place a nut under the head and look almost right.
  ui::FastenerRequest wrong;
  wrong.washerUnderHead = true;
  wrong.washer = "iso4032";
  wrong.dryRun = true;
  fit =
      ui::FitFastenerToHole(state, ui::RimPick{blindOcc, blind.topRim}, wrong);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("not a washer") != std::string::npos);
}

TEST_CASE(
    "A fastener derives its size and length from the hole it is picked in",
    "[actions][parts]") {
  // docs/parts/README.md#deriving-a-fastener-from-a-hole, and the four cases
  // that decide whether "derive from the hole" is a real interface or a demo.
  // Each is a finding rather than an obstacle: what the geometry cannot
  // disambiguate gets reported, and what it can rules a fastener out gets
  // refused by name.
  ui::AppState state;
  state.document = std::make_unique<Document>();
  state.commands = std::make_unique<CommandStack>();
  ui::EnsureParts(state);

  ui::FastenerRequest dry;
  dry.dryRun = true;

  const DrilledPlate top = MakePlate(state, "Top", 12.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Bottom", 26.0, 6.6, -1.0, 0.0);
  const DrilledPlate blind = MakePlate(state, "Blind", 20.0, 6.6, 8.0, 0.0);
  const DrilledPlate off = MakePlate(state, "Off", 10.0, 6.6, -1.0, 0.0);
  MakePlate(state, "Skew", 10.0, 6.6, -1.0, 20.0);

  // Returns the occurrence id: an element name is unique within a PART, so
  // two plates built the same way carry the same one and the id is what tells
  // them apart (docs/parts/README.md#deriving-a-fastener-from-a-hole).
  const auto place = [&](const char* part, glm::vec3 at) {
    Occurrence occ;
    occ.partId = ui::PartIdByName(state, part);
    occ.name = part;
    occ.position = at;
    return state.assembly->AddOccurrence(occ);
  };

  // 1. Two plates with a 3 mm gap. The gap is SPANNED and named: it is either
  //    an unmodelled gasket or two parts that were meant to touch, and the
  //    geometry cannot tell those apart — so the summary says so and the
  //    length covers it.
  ui::SetActivePart(state, ui::AddAssembly(state, "Gap"));
  uint64_t occ = place("Top", glm::vec3(0.0f, 0.0f, 3.0f));
  place("Bottom", glm::vec3(0.0f, 0.0f, -26.0f));
  ui::FastenerFit fit =
      ui::FitFastenerToHole(state, ui::RimPick{occ, top.topRim}, dry);
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
  occ = place("Blind", glm::vec3(0.0f));
  fit = ui::FitFastenerToHole(state, ui::RimPick{occ, blind.topRim}, dry);
  INFO(fit.why << " / " << fit.summary);
  REQUIRE(fit.ok);
  CHECK(fit.blind);
  CHECK_THAT(fit.grip, WithinAbs(8.0, 1e-3));
  CHECK_THAT(fit.length, WithinAbs(8.0, 1e-9));

  // 3. The second plate's hole is 20 mm off the axis. Refused, and named as a
  //    misalignment rather than as a shallow hole: told "the hole is 0 mm
  //    deep" a person goes looking for a drilling depth.
  ui::SetActivePart(state, ui::AddAssembly(state, "Skewed"));
  occ = place("Off", glm::vec3(0.0f));
  place("Skew", glm::vec3(0.0f, 0.0f, -10.0f));
  fit = ui::FitFastenerToHole(state, ui::RimPick{occ, off.topRim}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("not lined up") != std::string::npos);
  CHECK(fit.why.find("Skew") != std::string::npos);

  // 4. A rim that is not a rim. A cylindrical FACE has an axis and no
  //    direction, which is why the pick is an edge — and asking with one is
  //    refused rather than guessed at.
  ui::SetActivePart(state, ui::AddAssembly(state, "Bad"));
  occ = place("Top", glm::vec3(0.0f));
  fit = ui::FitFastenerToHole(
      state, ui::RimPick{occ, ElementRef{top.topRim.featureId, "nope"}}, dry);
  CHECK_FALSE(fit.ok);
  INFO(fit.why);
  CHECK(fit.why.find("RIM") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Long operations off the frame (docs/ui/long-operations.md#the-registry)
//
// The registry is polled by the loop and nowhere else; here the loop is the
// test, calling PollBackgroundJobs the way HeadlessMain.cpp does. Headless,
// because whether a job finishes has nothing to do with a widget.
// ---------------------------------------------------------------------------

namespace {
void PumpUntil(ui::AppState& state, const std::function<bool()>& done,
               int budgetMs = 20000) {
  const auto t0 = std::chrono::steady_clock::now();
  while (!done()) {
    ui::PollBackgroundJobs(state);
    if (std::chrono::steady_clock::now() - t0 >
        std::chrono::milliseconds(budgetMs))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}
} // namespace

TEST_CASE("a background job is reaped by the loop, listed, and cancelled",
          "[actions][jobs]") {
  ui::AppState state = MakeHeadlessState();
  REQUIRE(ui::ListBackgroundJobs(state).empty());

  std::atomic<int> ticks{0};
  int finished = 0;
  auto job = std::make_unique<ui::BackgroundJob>();
  job->kind = "test";
  job->label = "Counting to fifty";
  job->cancellable = true;
  ui::BackgroundJob* raw = job.get();
  job->thread = std::thread([raw, &ticks] {
    while (!raw->cancel.load() && ticks.load() < 50) {
      ticks++;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    raw->done.store(true);
  });
  job->finish = [&finished](ui::AppState&) {
    finished++;
    return true;
  };
  ui::StartBackgroundJob(state, std::move(job));

  CHECK(ui::BackgroundJobRunning(state, "test"));
  CHECK_FALSE(ui::BackgroundJobRunning(state, "cam"));
  const std::vector<ui::BackgroundJobInfo> listed =
      ui::ListBackgroundJobs(state);
  REQUIRE(listed.size() == 1);
  CHECK(listed[0].kind == "test");
  CHECK(listed[0].label == "Counting to fifty");
  CHECK(listed[0].cancellable);
  CHECK(listed[0].fraction < 0.0); // indeterminate, honestly

  // Cancel is a request the worker honours between ticks.
  CHECK(ui::CancelBackgroundJobs(state, "test") == 1);
  CHECK(ui::CancelBackgroundJobs(state, "nothing") == 0);
  PumpUntil(state, [&] { return !ui::BackgroundJobRunning(state, "test"); });
  CHECK_FALSE(ui::BackgroundJobRunning(state, "test"));
  CHECK(finished == 1);
  CHECK(ticks.load() < 50);
  CHECK(state.backgroundJobs.empty());

  // A finish that asks to be called again is, once per poll, until it says
  // it is done — the sliced-tail shape.
  int calls = 0;
  auto sliced = std::make_unique<ui::BackgroundJob>();
  sliced->kind = "sliced";
  sliced->done.store(true); // no thread at all: finish is the whole job
  sliced->finish = [&calls](ui::AppState&) { return ++calls >= 3; };
  ui::StartBackgroundJob(state, std::move(sliced));
  ui::PollBackgroundJobs(state);
  CHECK(calls == 1);
  CHECK(ui::BackgroundJobRunning(state, "sliced"));
  ui::PollBackgroundJobs(state);
  ui::PollBackgroundJobs(state);
  CHECK(calls == 3);
  CHECK_FALSE(ui::BackgroundJobRunning(state, "sliced"));

  // Join blocks for a running job and applies its finish.
  int late = 0;
  auto slow = std::make_unique<ui::BackgroundJob>();
  slow->kind = "slow";
  ui::BackgroundJob* slowRaw = slow.get();
  slow->thread = std::thread([slowRaw] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    slowRaw->done.store(true);
  });
  slow->finish = [&late](ui::AppState&) {
    late++;
    return true;
  };
  ui::StartBackgroundJob(state, std::move(slow));
  ui::JoinBackgroundJobs(state, "slow");
  CHECK(late == 1);
  CHECK(state.backgroundJobs.empty());
}

TEST_CASE("a fetch runs on the registry and reports through the loop",
          "[actions][jobs]") {
  // A3 phase 2: the sync job (fetch/push/clone) has no poll of its own any
  // more — it is a "sync" entry the loop reaps like any other. A repository
  // with no remote makes the fetch fail fast, which is enough to see the
  // whole path: started, listed, reaped, and its verdict applied.
  ui::AppState state = MakeHeadlessState();
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("designer-sync-" + std::to_string(::getpid()));
  std::filesystem::create_directories(dir);
  REQUIRE(GitVault::Init(dir.string()));
  state.projectPath = dir.string();

  REQUIRE(ui::StartFetch(state, "origin", false));
  CHECK(ui::SyncRunning(state));
  CHECK(ui::BackgroundJobRunning(state, "sync"));
  CHECK_FALSE(ui::StartFetch(state, "origin", false)); // one at a time
  {
    const std::vector<ui::BackgroundJobInfo> jobs =
        ui::ListBackgroundJobs(state);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].kind == "sync");
    CHECK(jobs[0].label == "Fetching origin");
  }
  PumpUntil(state, [&] { return !ui::SyncRunning(state); });
  CHECK_FALSE(ui::SyncRunning(state));
  CHECK(state.syncFailed); // there is no origin
  CHECK_FALSE(state.syncMessage.empty());
  std::filesystem::remove_all(dir);
}

// Netgen-gated: the job machinery under test is the same either way, but the
// assertions read the RESULTS of a solve, and a build without a mesher stores
// none (fem::Mesh returns "built without FEM support"). The stock-Ubuntu CI
// job is exactly that build.
#ifdef HAVE_NETGEN

TEST_CASE("a study solved on the registry lands in its own part, not the "
          "active tab",
          "[actions][jobs][fem]") {
  // A3 phase 3. The FEM polls used to take "the active document", so a solve
  // started on a part tab and left for another tab was not reaped until you
  // came back — and would have been stored into the WRONG document had the
  // poll not been guarded. The job now remembers its part slot.
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  auto bar =
      std::make_unique<BoxFeature>("Bar", glm::vec3(0, 0, 0), 100, 10, 10);
  bar->SetMaterial("steel");
  doc.AddFeature(std::move(bar));
  doc.Regenerate();
  REQUIRE(doc.GetBodies().size() == 1);
  const uint64_t cb = doc.GetBodies()[0].createdBy;
  auto face = [&](int i) {
    return ElementRef{cb, std::to_string(cb) + "/face" + std::to_string(i)};
  };
  fem::Study& s = doc.AddStudy("Bar");
  s.meshSize = 6.0;
  fem::Load fx;
  fx.id = doc.AllocateId();
  fx.type = fem::LoadType::Fixed;
  fx.face = face(0);
  s.loads.push_back(fx);
  fem::Load fo;
  fo.id = doc.AllocateId();
  fo.type = fem::LoadType::Force;
  fo.face = face(1);
  fo.magnitude = 2000.0;
  fo.direction = glm::dvec3(1, 0, 0);
  s.loads.push_back(fo);
  const uint64_t studyId = s.id;
  const uint64_t barSlot = state.parts[state.activePart].id;

  std::string err;
  REQUIRE(ui::StartStudyRun(state, doc, studyId, &err));
  CHECK(ui::GetStudyRunStatus(state, studyId).running);
  CHECK(ui::BackgroundJobRunning(state, "study"));
  CHECK_FALSE(ui::StartStudyRun(state, doc, studyId, &err)); // one at a time
  {
    const std::vector<ui::BackgroundJobInfo> jobs =
        ui::ListBackgroundJobs(state);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].kind == "study");
    CHECK(jobs[0].label == "Solving 'Bar'");
  }

  // Leave for another part while it solves.
  ui::AddPart(state, "Other");
  REQUIRE(state.parts[state.activePart].id != barSlot);
  PumpUntil(state, [&] { return !ui::BackgroundJobRunning(state, "study"); });
  CHECK_FALSE(ui::GetStudyRunStatus(state, studyId).running);
  CHECK(state.studyJob == nullptr);

  // The bar has its results; the other part has nothing of the sort.
  Document* barDoc = ui::PartDocById(state, barSlot);
  REQUIRE(barDoc != nullptr);
  REQUIRE(barDoc != state.document.get());
  const fem::StudyRuntime* rt = barDoc->StudyResults(studyId);
  REQUIRE(rt != nullptr);
  CHECK(rt->bodies.size() == 1);
  CHECK(rt->bodies[0].results.maxVonMises > 0.0);
  CHECK(barDoc->FindStudy(studyId)->error.empty());
  CHECK(state.document->StudyResults(studyId) == nullptr);

  // mesh_study's async path: the plan on this thread, the mesh on a worker,
  // the report where study_status reads it.
  REQUIRE(ui::StartMeshRun(state, *barDoc, studyId, &err));
  CHECK(ui::GetMeshRunStatus(state, studyId).running);
  CHECK(ui::MeshReportFor(state, *barDoc, studyId) == nullptr);
  PumpUntil(state, [&] { return !ui::BackgroundJobRunning(state, "mesh"); });
  const ui::MeshReport* report = ui::MeshReportFor(state, *barDoc, studyId);
  REQUIRE(report != nullptr);
  CHECK(report->error.empty());
  REQUIRE(report->bodies.size() == 1);
  CHECK(report->bodies[0].name == "Bar");
  CHECK(report->bodies[0].nodes > 0);
  CHECK(report->bodies[0].elements > 0);
  CHECK(ui::MeshReportFor(state, *state.document, studyId) == nullptr);
}

#endif // HAVE_NETGEN

TEST_CASE("a STEP import runs on the registry and materializes a slice a "
          "poll",
          "[actions][jobs][import]") {
  // A3 phase 4: the worker reads the file, and the main-thread half — one
  // part tab per product, then the assembly — is the job's own `finish`,
  // returning false until the last part so a modal can keep painting.
  ui::AppState state = MakeHeadlessState();
  CADKernel& k = CADKernel::Instance();
  CADKernel::ImportedModel model;
  for (int i = 0; i < 3; i++) {
    model.products.push_back({k.CreateBox(2.0, 2.0, 2.0),
                              "Widget " + std::to_string(i), false,
                              glm::vec3(0.0f)});
    model.instances.push_back(
        {static_cast<size_t>(i),
         glm::translate(glm::mat4(1.0f),
                        glm::vec3(10.0f * static_cast<float>(i), 0, 0)),
         "I" + std::to_string(i)});
  }
  model.instances.push_back(
      {0, glm::translate(glm::mat4(1.0f), glm::vec3(0, 10, 0)), "W0 again"});
  const std::filesystem::path stepPath =
      std::filesystem::temp_directory_path() /
      ("designer-import-" + std::to_string(::getpid()) + ".step");
  REQUIRE(k.SaveSTEPStructured(stepPath.string(), model, "Rig"));

  const size_t tabsBefore = state.parts.size();
  std::string error;
  REQUIRE(ui::StartImport(state, stepPath.string(), &error));
  CHECK(ui::ImportRunning(state));
  CHECK(ui::BackgroundJobRunning(state, "import"));
  CHECK_FALSE(ui::StartImport(state, stepPath.string(), &error));
  {
    const std::vector<ui::BackgroundJobInfo> jobs =
        ui::ListBackgroundJobs(state);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].kind == "import");
    CHECK(jobs[0].cancellable);
  }
  CHECK_FALSE(ui::ImportStatusLine(state).empty());
  PumpUntil(state, [&] { return !ui::ImportRunning(state); });
  CHECK_FALSE(ui::ImportRunning(state));
  CHECK(state.importJob == nullptr);
  INFO(state.importError);
  CHECK(state.importError.empty());
  // Three part tabs and the assembly.
  CHECK(state.parts.size() == tabsBefore + 4);
  REQUIRE(state.assembly != nullptr);
  CHECK(state.assembly->GetOccurrences().size() == 4);
  std::filesystem::remove(stepPath);
}

#ifdef HAVE_CAM
namespace {
ElementRef UpFaceAtZ(const std::vector<Body>& bodies, double z) {
  for (const Body& b : bodies)
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 o(0.0f), n(0.0f), u(0.0f);
      if (b.shape.FacePlane(index, o, n, u) && n.z > 0.99f &&
          std::abs(o.z - static_cast<float>(z)) < 1e-4f)
        return ElementRef{b.createdBy, name};
    }
  return {};
}

bool SameMoves(const cam::Toolpath& a, const cam::Toolpath& b) {
  if (a.moves.size() != b.moves.size())
    return false;
  for (size_t i = 0; i < a.moves.size(); i++)
    if (a.moves[i].kind != b.moves[i].kind ||
        glm::length(a.moves[i].to - b.moves[i].to) > 1e-9)
      return false;
  return true;
}

bool AnyPending(const ui::AppState& state) {
  return ui::CamToolpathsPending(state);
}
} // namespace

TEST_CASE("an adaptive pocket is cleared off the frame and lands complete",
          "[actions][cam][jobs]") {
  if (!cam::AdaptiveAvailable())
    return;
  ui::AppState state = MakeHeadlessState();
  Document& doc = *state.document;
  const uint64_t block = AddBox(doc, glm::vec3(0, 0, 0), 40, 30, 20);
  const uint64_t voidId = AddBox(doc, glm::vec3(10, 8, 12), 20, 14, 20);
  doc.AddFeature(std::make_unique<BooleanFeature>("Pocket", block, voidId,
                                                  BooleanOp::Subtract));
  doc.Regenerate();
  const uint64_t partId = state.parts[state.activePart].id;
  ui::CreateJobForPart(state, partId);
  REQUIRE(state.job != nullptr);

  cam::Setup& setup = state.job->AddSetup("Job");
  cam::Tool tool;
  tool.number = 1;
  tool.diameter = 6.0;
  tool.feedCut = 600.0;
  tool.feedPlunge = 200.0;
  setup.tools.push_back(tool);
  setup.safeZ = cam::DeriveSafeZ(setup, ui::JobBodies(state));
  const ElementRef floor = UpFaceAtZ(ui::JobBodies(state), 12.0);
  REQUIRE(floor.IsSet());
  cam::Operation op =
      cam::DeriveOperation(setup, ui::JobBodies(state), cam::OpType::Pocket,
                           ui::TargetFor(state, floor))
          .op;
  op.id = state.job->AllocateId();
  op.name = "Pocket";
  op.type = cam::OpType::Adaptive;
  setup.operations.push_back(op);
  state.camActiveSetup = setup.id;
  const size_t passes = cam::DepthPasses(op).size();
  REQUIRE(passes >= 2);

  // The reference: synchronous, no cache.
  const std::vector<cam::Toolpath> ref =
      cam::GenerateAll(setup, ui::JobBodies(state));
  REQUIRE(ref.size() == 1);
  INFO(ref[0].error);
  REQUIRE(ref[0].error.empty());

  // The panel's path defers: pending at once, a "cam" job running.
  REQUIRE(state.camDeferPasses);
  ui::RefreshCamToolpaths(state);
  REQUIRE(state.camToolpaths.size() == 1);
  CHECK(state.camToolpaths[0].pending);
  CHECK(state.camToolpaths[0].moves.empty()); // nothing previous to keep
  CHECK(ui::BackgroundJobRunning(state, "cam"));
  CHECK(state.camPassBatchTotal == passes);
  CHECK_FALSE(ui::CamToolpathsPendingWhy(state).empty());
  {
    const std::vector<ui::BackgroundJobInfo> jobs =
        ui::ListBackgroundJobs(state);
    REQUIRE(jobs.size() == 1);
    CHECK(jobs[0].kind == "cam");
    CHECK(jobs[0].label.find("Pocket") != std::string::npos);
    CHECK(jobs[0].label.find("1 of " + std::to_string(passes)) !=
          std::string::npos);
    CHECK_FALSE(jobs[0].cancellable);
  }
  // Verification waits for the passes instead of judging the previous file.
  ui::RunCamVerification(state);
  CHECK(state.camVerifyWhenSettled);
  REQUIRE(state.camVerdict.size() == 1);
  CHECK(state.camVerdict[0].text.find("Not verified yet") == 0);

  // Refreshing again while it runs queues nothing twice.
  ui::RefreshCamToolpaths(state);
  CHECK(state.camPassBatchTotal == passes);

  // The loop reaps each pass; the last one regenerates and verifies.
  PumpUntil(state, [&] { return !AnyPending(state); });
  CHECK_FALSE(AnyPending(state));
  CHECK_FALSE(ui::BackgroundJobRunning(state, "cam"));
  REQUIRE(state.camToolpaths.size() == 1);
  CHECK(state.camToolpaths[0].error.empty());
  CHECK(SameMoves(state.camToolpaths[0], ref[0]));
  CHECK(state.camAdaptiveCache.Size() == passes);
  CHECK_FALSE(state.camVerifyWhenSettled);
  CHECK_FALSE(state.camVerdict.empty());
  CHECK(state.camVerdict[0].text.find("Not verified") == std::string::npos);
  CHECK(state.camPassBatchTotal == 0); // the batch is over

  // An unrelated regenerate (an undo would do this) is a cache hit: no job.
  const size_t hits = state.camAdaptiveCache.hits;
  ui::RefreshCamToolpaths(state);
  CHECK_FALSE(AnyPending(state));
  CHECK_FALSE(ui::BackgroundJobRunning(state, "cam"));
  CHECK(state.camAdaptiveCache.hits > hits);

  // A pending toolpath keeps the previous moves for the backplot.
  state.camAdaptiveCache.Clear();
  ui::RefreshCamToolpaths(state);
  REQUIRE(AnyPending(state));
  CHECK(state.camToolpaths[0].pending);
  CHECK(SameMoves(state.camToolpaths[0], ref[0])); // the previous, shown
  // …and a synchronous caller settles: joins, computes, regenerates.
  ui::SettleCamToolpaths(state);
  CHECK_FALSE(AnyPending(state));
  CHECK_FALSE(ui::BackgroundJobRunning(state, "cam"));
  CHECK(SameMoves(state.camToolpaths[0], ref[0]));

  // Cancel: what was waiting says so; what was cutting lands and is cached,
  // and lands WITHOUT queueing the rest again.
  state.camAdaptiveCache.Clear();
  ui::RefreshCamToolpaths(state);
  REQUIRE(AnyPending(state));
  REQUIRE(ui::BackgroundJobRunning(state, "cam"));
  ui::CancelCamPasses(state);
  CHECK_FALSE(AnyPending(state));
  CHECK(state.camToolpaths[0].error.find("cancelled") != std::string::npos);
  CHECK(state.camAdaptiveQueue.empty());
  PumpUntil(state, [&] { return !ui::BackgroundJobRunning(state, "cam"); });
  CHECK_FALSE(AnyPending(state));
  CHECK(state.camAdaptiveQueue.empty());
  CHECK(state.camAdaptiveCache.Size() == 1); // the pass that was cutting
  // The next Generate queues the rest again.
  ui::RefreshCamToolpaths(state);
  CHECK(AnyPending(state));
  PumpUntil(state, [&] { return !AnyPending(state); });
  CHECK(SameMoves(state.camToolpaths[0], ref[0]));
}
#endif // HAVE_CAM

TEST_CASE(
    "Drawing layout edits reuse model projection and keep geometry scaled",
    "[actions][drawing][hlr-cache]") {
  auto state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0), 40, 30, 20);
  const auto part = state.parts[state.activePart].id;
  ui::AddDrawing(state, "Sheet");
  DrawingView view;
  view.sourceDocId = part;
  const auto id = state.drawing->AddView(view);
  ui::UpdateDrawingRenders(state);
  const auto before = state.drawingRenders.at(id);
  REQUIRE(before.hasGeometry);
  REQUIRE(before.projectionStats.selfCalls == 1);
  auto* edited = state.drawing->FindView(id);
  REQUIRE(edited);
  edited->position = {100, 80};
  edited->scale = 0.5;
  state.drawing->BumpVersion();
  ui::UpdateDrawingRenders(state);
  const auto& after = state.drawingRenders.at(id);
  REQUIRE(after.hasGeometry);
  CHECK(after.projectionStats.projectionHit);
  CHECK(after.projectionStats.selfCalls == 0);
  CHECK(glm::length((after.bboxMax - after.bboxMin) -
                    0.5 * (before.bboxMax - before.bboxMin)) < 1e-8);
  DrawingNote note;
  note.text = "Cached sheet";
  state.drawing->AddNote(note);
  ui::UpdateDrawingRenders(state);
  CHECK(state.drawingRenders.at(id).projectionStats.projectionHit);
}

TEST_CASE(
    "Drawing cache notices a nested part edit without a root version bump",
    "[actions][drawing][hlr-cache]") {
  auto state = MakeHeadlessState();
  AddBox(*state.document, glm::vec3(0), 10, 10, 10);
  const auto partId = state.parts[0].id;
  const auto sub = ui::AddAssembly(state, "Sub");
  Occurrence leaf;
  leaf.partId = partId;
  state.assembly->AddOccurrence(leaf);
  const auto top = ui::AddAssembly(state, "Top");
  Occurrence child;
  child.partId = state.parts[sub].id;
  child.refKind = OccurrenceRef::Assembly;
  state.assembly->AddOccurrence(child);
  const auto topId = state.parts[top].id;
  ui::AddDrawing(state, "Sheet");
  DrawingView view;
  view.sourceDocId = topId;
  const auto id = state.drawing->AddView(view);
  ui::UpdateDrawingRenders(state);
  REQUIRE(state.drawingRenders.at(id).hasGeometry);
  const auto rootVersion = state.parts[top].assembly->Version();
  REQUIRE(state.parts[0].doc);
  AddBox(*state.parts[0].doc, {40, 0, 0}, 10, 10, 10);
  CHECK(state.parts[top].assembly->Version() == rootVersion);
  ui::UpdateDrawingRenders(state);
  const auto& result = state.drawingRenders.at(id);
  REQUIRE(result.hasGeometry);
  CHECK_FALSE(result.projectionStats.projectionHit);
  CHECK_THAT(result.bboxMax.x - result.bboxMin.x, WithinAbs(50, 1e-8));
}

// ---------------------------------------------------------------------------
// An EMPTY sketch has no constraint status
//
// The first outside tester (docs/ui/tool-sessions.md#sketch-sessions) opened a
// sketch, drew nothing, and the app told him it was "Fully defined". The solver
// was not wrong -- an empty system has no free parameters, so zero degrees of
// freedom is the arithmetic truth -- but that pill answers "am I done
// constraining?", and answering "yes" before there is anything to constrain is
// the app being confidently wrong about geometry it does not have.
// ---------------------------------------------------------------------------

TEST_CASE("an empty sketch reports no DOF status at all", "[actions][sketch]") {
  ui::AppState state = MakeHeadlessState();
  state.document->AddFeature(
      std::make_unique<SketchFeature>("Sketch", SketchPlane(PlaneType::XY)));
  SketchFeature* sk = dynamic_cast<SketchFeature*>(
      state.document->GetFeature(state.document->FeatureCount() - 1));
  REQUIRE(sk != nullptr);
  state.sketchEdit.activeSketchId = sk->GetId();
  REQUIRE(state.sketchEdit.IsEditing());

  ui::UpdateSketchStatus(state);
  CHECK(state.sketchEdit.dof == -1); // -1 = no status, NOT "fully defined"

  // One line, both endpoints free: now there is something to report, and it
  // is four degrees of freedom rather than silence.
  sk->AddEntity(sketch::Line{{0, 0}, {2, 0}});
  ui::UpdateSketchStatus(state);
  CHECK(state.sketchEdit.dof == 4);
}

// The label beside a boundary-condition glyph says the value AND its unit,
// scaled to read: a 1200 N force is "1.2 kN", a 0.02 MPa pressure "20 kPa".
// A label with the raw stored number ("1200", "0.02") is the panel again.
TEST_CASE("Simulation glyph labels carry a value and a unit", "[sim][ui]") {
  fem::Load l;
  l.type = fem::LoadType::Fixed;
  CHECK(ui::SimulationLoadLabel(l) == "fixed");
  l.type = fem::LoadType::Force;
  l.magnitude = 350.0;
  CHECK(ui::SimulationLoadLabel(l) == "350 N");
  l.magnitude = 1200.0;
  CHECK(ui::SimulationLoadLabel(l) == "1.2 kN");
  l.type = fem::LoadType::Pressure;
  l.magnitude = 0.5;
  CHECK(ui::SimulationLoadLabel(l) == "0.5 MPa");
  l.magnitude = 0.02;
  CHECK(ui::SimulationLoadLabel(l) == "20 kPa");
  l.magnitude = 0.0;
  CHECK(ui::SimulationLoadLabel(l) == "0 MPa");
  CHECK(ui::SimulationGravityLabel(glm::dvec3(0.0, 0.0, -9.81)) ==
        "g 9.81 m/s^2");
}
