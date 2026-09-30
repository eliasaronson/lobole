#pragma once

#include <span>

#include "ui/AppState.h"

class Feature;

namespace ui {

// Draws editable widgets for a feature's parameters (used by both the
// Properties panel and the preview dialog). When commit is true, edits are
// pushed as undoable ModifyFeatureCommands at the given feature index; when
// false (a pending preview), edits only mutate the feature + regenerate.
void DrawFeatureParamWidgets(AppState& state, Feature& feature, size_t index,
                             bool commit);

// Dialog pick slots (U3): which reference field of the edited feature an
// armed SlotPickState fills. One global enum — slots are feature-specific
// but ids need only be unique. Only append: tests address rows as
// "###slot<value>".
enum class PickSlot : int {
  ExtrudeEndFace, // up-to-face target (Face)
  BooleanTarget,  // Body
  BooleanTool,    // Body
  PatternSource,  // Body
  ShellBody,      // Body
  ShellOpenFaces, // Face, multi
  DraftFaces,     // Face, multi
  ExtrudeRegions, // U2: sketch regions, multi
  RevolveRegions, // U2: sketch regions, multi
  RibBody,        // Body (0 = nearest overlapping)
  ThreadFace,     // a cylindrical Face
  BlendEdges,     // U7: fillet/chamfer edge collection (Edge, multi)
  MateConnectorA, // U7c: mate session (assembly; not a feature slot)
  MateConnectorB,
  SplitBody,      // Body to divide in two
  SplitKnifeFace, // a planar Face of that body, used as the cutting plane
  RefineBody,     // Body whose coplanar faces/seams get merged
  SectionFace,    // I3 P2: a planar Face a section cuts along (not a feature)
  CamTopFace,     // CAM G1: the face that defines Z zero / the stock top
  CamPocketFloor, // CAM G2: the floor of a pocket to clear
  CamProfileOutline, // CAM G2: the face whose outline gets cut out
  CamIndexFace,      // CAM C9: the face to turn up under the spindle (rotary)
  CamLayFaceUp,      // CAM S2: the face to clamp facing the spindle (fixture)
  CamEdgeWall,       // CAM R15: the wall whose top edge to round or chamfer
  CamDrillHole,      // CAM: the cylindrical face of a hole to drill
  CamSurfaceRegion,  // CAM v118: a face of ANY orientation to finish within
  FastenerRim,       // F1 p6: the circular EDGE a fastener's head sits on
  // The optional SECOND pick: where the joint ENDS, when the axis carries on
  // past it into material the fastener is not meant to span. Same session and
  // same sentinel as FastenerRim — one tool, two slots.
  FastenerFarEnd,
  // Sheet metal: the straight edges a wall is bent up from, and the body a
  // flat pattern lays flat.
  EdgeFlangeEdges,  // Edge, multi
  FlatPatternBody,  // Body
  UnfoldBody,       // Body
  HemEdges,         // Edge, multi
  CornerReliefBody, // Body
  // CAM: a job part's sketch label or curve to engrave (a SketchCurve item,
  // not a face; docs/cam/gui.md#engrave-from-the-viewport).
  CamEngraveCurve,
};

// SlotPickState.featureId sentinel for the mate session's connector slots —
// mates live on the AssemblyDocument, not the feature list. SlotPickClicked
// routes these to MateConnectorPicked (AssemblyActions).
inline constexpr uint64_t kMateSessionSlot = ~0ull;

// The same trick for the Section tool: sections are VIEW state, not features,
// so its face slot needs a sentinel of its own. SlotPickClicked routes these
// to SectionFacePicked (AppActions).
inline constexpr uint64_t kSectionToolSlot = ~1ull;

// And the CAM tool session (docs/cam/gui.md). CAM setups are document
// objects, not features, so its picks need a sentinel too.
inline constexpr uint64_t kCamToolSlot = ~2ull;

// And the fastener session (docs/parts/README.md). A fastener is an
// OCCURRENCE in the assembly, not a feature in any document, so it needs the
// same treatment. SlotPickClicked routes these to FastenerRimPicked.
inline constexpr uint64_t kFastenerToolSlot = ~3ull;

// --- the tool-session registry ----------------------------------------------
//
// A sentinel is not a value, it is a SESSION, and every session has to answer
// two questions in two different files:
//
//   * when a pick lands, who consumes it?   (SlotPickClicked, FeatureEditor)
//   * is this armed slot still real?        (ViewportInput, EVERY FRAME)
//
// For an ordinary feature slot the second answers itself — does the id still
// resolve to a feature in the active document? A non-feature session has no
// document to ask, so each needed its own predicate, and both places were
// hand-maintained `if` chains keyed on the same sentinel with nothing joining
// them.
//
// Miss the ViewportInput one and the slot is disarmed the same frame it is
// armed: no error, no log, no failed assertion — the tool simply ignores
// every click, which reads as a broken picker rather than a missing row. That
// cost a debugging round on the fastener session, and it is the third list in
// this codebase to have been the thing that was forgotten (see
// docs/rendering/README.md#scene-renderers for the last one).
//
// One row here answers both, and test_conventions.cpp asserts every `k*Slot`
// constant above appears in the table — so a half-registered session is a
// test failure instead of a debugging session.
struct ToolSession {
  uint64_t sentinel;
  // Still real? Called every frame while the slot is armed.
  bool (*alive)(const AppState& state);
  // A pick landed on it.
  void (*consume)(AppState& state, const SelectionItem& item);
};
[[nodiscard]] std::span<const ToolSession> ToolSessions();
// The session that owns an armed slot, or null for an ordinary feature slot.
[[nodiscard]] const ToolSession* ToolSessionFor(uint64_t sentinel);

// A tool's pick slots, declaratively (U7): the ordered list drives arming
// (kinds/prompt come from here, not the draw site), viewport routing, and —
// U7b — seeding from a pre-existing selection. Order = seed priority.
struct SlotSpec {
  PickSlot slot;
  const char* label;  // slot row label ("Target", "Edges")
  uint32_t kinds;     // SelectionItem::Kind bitmask accepted
  bool multi;         // toggle-list slot; stays armed after a pick
  const char* prompt; // status pill while armed
  // The feature can't resolve without this slot (drives auto-advance: after
  // a single-slot pick / at session start, the first unfilled REQUIRED slot
  // arms itself). Optional slots ("Profiles: empty = all") never self-arm.
  bool required = true;
};

// The slots the feature exposes IN ITS CURRENT STATE (e.g. the extrude
// end-face slot exists only in Up-to-face mode). Empty for features without
// pick slots.
std::vector<SlotSpec> SlotSpecsFor(const Feature& feature);

// Arms `slot` on the feature (kinds/prompt from SlotSpecsFor). Returns false
// when the feature doesn't currently expose that slot.
bool ArmSlot(AppState& state, const Feature& feature, PickSlot slot);

// Whether the slot's underlying feature field holds a value ("2 edges
// picked", "target set"). Multi slots count as filled with one item.
bool SlotFilled(const Feature& feature, PickSlot slot);

// Arms the first REQUIRED slot that is still unfilled, disarming instead
// when none remain (the auto-advance rule: boolean stays pick-pick-done).
void ArmNextUnfilledSlot(AppState& state, const Feature& feature);

// U7b select-first: distributes the current Selection into the feature's
// slots, greedily in declared order — single slots take the oldest matching
// item (overwriting any default the Create* action guessed; a session-start
// value is a guess, a pre-selected item is intent), multi slots append every
// matching item. Called by BeginPreviewFeature before it replaces the
// selection with the new feature.
void SeedSlotsFromSelection(AppState& state, Feature& feature);

// The fillet/chamfer whose BlendEdges slot is armed, or 0 — the blend pick
// overlay and the selection overlay's channel handoff key off this.
uint64_t ArmedBlendFeatureId(const AppState& state);

// The selection pipeline routes a resolved pick to the armed slot here:
// writes the feature's reference (one undoable edit when the feature is
// committed; live mutation while previewing) and disarms single slots.
// meshId/index are the render handles the pick resolved from (the blend
// edge remap anchors on them; 0 otherwise).
void SlotPickClicked(AppState& state, const SelectionItem& item,
                     uint32_t meshId = 0, uint32_t index = 0);

} // namespace ui
