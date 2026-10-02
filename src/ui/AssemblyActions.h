#pragma once

#include "core/AssemblySolve.h"

#include "cad/MeshExport.h"
#include "core/io/ImportAssembly.h"
#include "render/MeshRenderer.h"
#include "ui/AppState.h"

// Assembly editing: occurrences, mates, the drag gizmo,
// exploded views, the BOM, and STEP/CAD import.
//
// Split out of ui/AppActions.h, which had grown to ~180 declarations spanning
// every domain in the app and is included by 40 translation units — so every
// panel and RPC module paid for all of it. See
// docs/architecture.md#the-ui-actions-layer.
namespace ui {

// Resolves a viewport ray to a face of an occurrence in the active assembly:
// the occurrence hit and a persistent ElementRef naming the face (for building
// a mate connector). Returns false if the ray misses, hits an occurrence whose
// part isn't loaded, or lands on an unnamed face (e.g. a pattern output). The
// `ray` is in the same space FaceSelector::PickFace expects.
struct AssemblyFacePick {
  enum class SnapType { FaceCenter, HoleCenter, EdgeMidpoint, Corner };

  uint64_t occurrenceId = 0;
  ElementRef face;
  // A persistently named edge when the nearest candidate is a circular-edge
  // center or a straight/curved edge midpoint; unset means face center.
  ElementRef snapEdge;
  // The second edge of a CORNER, which is where two named edges meet. Set
  // only alongside snapEdge, and only when snapType is Corner.
  ElementRef snapEdge2;
  SnapType snapType = SnapType::FaceCenter;
  // A5: when the face belongs to a part INSIDE the picked occurrence, the
  // ids down to it. A connector built from this pick carries it, so a mate
  // can anchor on geometry inside a placed unit.
  std::vector<uint64_t> innerPath;
};

// Bill of materials (A4): derived from the active assembly every call, never
// stored. One row per (part, configuration, material) placed, in
// first-occurrence order; masses via
// Measure::MassOfBodies (a part with any density-less body reports its mass
// unknown and taints the total). `visible=false` occurrences count —
// visibility is a display state, not suppression.
struct BomRow {
  uint64_t partId = 0;
  std::string partName; // "(missing part)" when the reference dangles
  // The configuration these are placed in (v135): one row per part,
  // configuration and material, so a pawn and a rook of one piece part --
  // or a maple pawn and a walnut one -- are two lines.
  uint64_t configurationId = 0;
  std::string configuration; // its name; "" = the part's active one
  int quantity = 0;
  std::string material;    // slug; "" = none, "mixed" when bodies differ
  double unitVolume = 0.0; // mm^3, all bodies of the part
  bool massKnown = false;
  double unitMassG = 0.0; // valid when massKnown
  // A BOUGHT part, and what to order it as: the standard's name ("iso4762")
  // and the designation ("ISO 4762 M6x50"). Both empty for a part somebody
  // modelled (docs/parts/README.md#bom-and-drawings).
  //
  // Derived, not stored: a part whose whole feature list is one Fastener is a
  // bought item by construction. That keeps the distinction right when the
  // part tab is renamed, which is the case a stored flag gets wrong.
  //
  // There is deliberately no sixth COLUMN in the panel for it. The table
  // already clips at five in a 210 px dock — three parts all read "Gang" —
  // and a fastener's part name IS its designation, so the column would repeat
  // what column one says while making every other cell narrower. It goes in
  // the CSV, where a purchaser reads it, and in the row's tooltip.
  std::string standard;
  std::string designation;
  // The top-level occurrences this row counts, one per leaf: what a balloon
  // on a drawing finds its item number by.
  std::vector<uint64_t> rootOccurrences;
  [[nodiscard]] bool bought() const { return !standard.empty(); }
};

struct BomTable {
  std::vector<BomRow> rows;
  bool massKnown = true;   // false when any row's mass is unknown
  double totalMassG = 0.0; // valid when massKnown
  // V2: WHY a mass is unknown ("part 'X': body 'Y' has no material"), so a
  // null total explains itself instead of silently tainting the table.
  std::vector<std::string> warnings;
};

// Assembly occurrence-edit undo (snapshot-based; A2 phase 3). Call
// SnapshotAssembly *before* mutating the active assembly's occurrences; Undo/
// Redo restore a snapshot and force a renderer resync. No-ops without an active
// assembly / empty stack (Undo/Redo return whether they did anything).
void SnapshotAssembly(AppState& state);

bool UndoAssembly(AppState& state);

bool RedoAssembly(AppState& state);

// Selects an occurrence in the active assembly (0 = clear): records it and
// highlights all its rendered instances. Safe with null renderers.
void SelectOccurrence(AppState& state, uint64_t occId);

// Re-solves the active assembly's mates (A3): places every mated, non-fixed
// occurrence by direct frame alignment, then bumps the version so the renderer
// resyncs. No-op without an active assembly. Call after any mate/occurrence
// edit and after loading. Returns the number of mates left in an error state.
int SolveActiveAssembly(AppState& state);

// The same, holding the DOF the caller just drove while a loop closes around
// it (a drag, a set_mate, a motion sample). Without it the loop solver treats
// that DOF as one more unknown and moves it. `diagnose` as assembly::Solve's:
// a motion sample passes false.
int SolveActiveAssemblyDriven(AppState& state, uint64_t mateId, MateDof dof,
                              bool diagnose = true,
                              const std::vector<uint64_t>& path = {});

// The SAME pass, reporting everything it learned rather than just the error
// count: A6's loop closure, remaining degrees of freedom, and which mates were
// redundant-but-consistent. Additive — SolveActiveAssembly is this narrowed to
// `erroredMates`, which is all its existing callers ever wanted.
assembly::SolveReport
SolveActiveAssemblyReport(AppState& state, const assembly::Driven& driven = {},
                          bool diagnose = true);

// Part→assembly change propagation (A4): true when any part referenced by the
// active assembly has regenerated since the assembly's last solve (compared
// against the version stamps SolveActiveAssembly records). SetActivePart and
// OpenProject use it to re-solve automatically; in-context editing re-checks
// after every part regeneration. False without an active assembly.
bool AssemblyPartsStale(AppState& state);

// In-context editing (A4): switch to the part tab of `occurrenceId` (in the
// active assembly) with the assembly ghosted around it — the part renders at
// identity and every other occurrence at OccMatrix(edited)⁻¹·OccMatrix(other),
// alpha-blended, unpickable, no outlines. The context re-solves after each
// part regeneration so ghosts track mates live. Exit = activating any other
// tab (SetActivePart clears the mode). False when the occurrence or its part
// can't be resolved.
bool EnterInContextEdit(AppState& state, uint64_t occurrenceId);

bool PickAssemblyFace(AppState& state, const Ray& ray, AssemblyFacePick& out);

// Mate creation tool (A3 phase 3, U7c slots). StartMateTool opens the
// session and arms connector slot A; the selection pipeline resolves
// Connector candidates and routes picks to MateConnectorPicked (slot A →
// advance to B; slot B → live preview mate; edit session → rewrite the
// mate's connector and re-solve). Accept keeps the preview / closes the
// session; Cancel removes it. All no-op without an active assembly.
void StartMateTool(AppState& state, MateType type);

// Re-pick session for an existing mate: opens the mate session with both
// connectors filled from the mate; clicking a connector row re-arms it and
// the next pick rewrites that side (one undo step per re-pick).
void StartMateEdit(AppState& state, uint64_t mateId);

// The armed connector slot consumed a pipeline pick (SlotPickClicked routes
// kMateSessionSlot here).
[[nodiscard]] bool IsOccurrenceSelected(const AppState& state, uint64_t occId);
// Remove every selected occurrence. Returns how many went. One undo snapshot
// and one solve for the batch. The list's click gesture (Ctrl/Shift) and the
// Delete key reach this through ListSelection.h.
// `takeSnapshot` false when the CALLER has already snapshotted: Delete on an
// assembly tab can remove mates and occurrences together, and two snapshots
// would make that two Ctrl+Z presses for one press of Delete.
int DeleteSelectedOccurrences(AppState& state, bool takeSnapshot = true);

// Show/hide occurrences — the display flag every renderer, the BOM and the
// mass total already read (`Occurrence::visible`), NOT a suppression: a
// hidden part still solves, so hiding the block to see the crank inside it
// cannot move anything. One snapshot for the batch, so unhiding twenty parts
// is one Ctrl+Z. Returns how many rows actually changed.
int SetOccurrencesVisible(AppState& state, const std::vector<uint64_t>& ids,
                          bool visible);

// True when the selection's Hide/Show verb would HIDE: any of it is still
// visible. False once every selected row is hidden, which is what makes the
// one verb (the menu item, and the key) undo itself. The menu labels itself
// from this, so the label and the action can never disagree.
[[nodiscard]] bool SelectionWouldHide(const AppState& state);

// Hide the selected occurrences, or show them once they are all hidden.
// Returns how many changed; 0 with nothing selected.
int ToggleSelectedOccurrencesVisible(AppState& state);

// Un-hide every occurrence in the active assembly — the way back when the
// hidden row is off the bottom of a 200-part list. Returns how many were
// hidden.
int ShowAllOccurrences(AppState& state);

void MateConnectorPicked(AppState& state, const SelectionItem& item);

void AcceptMateTool(AppState& state);

void CancelMateTool(AppState& state);

// Arms connector slot A or B on the shared armed-slot state (the mate
// session's slot rows re-arm through this too).
void ArmMateConnector(AppState& state, bool second);

// The world frame of a connector (occurrence placement ∘ its part-local frame),
// or nullopt if it doesn't resolve. For drawing the connector triad.
std::optional<glm::dmat4> ConnectorWorldFrame(AppState& state,
                                              const Connector& c);

// Appends the mate tool's connector triads (picked connectors) to `lines`.
void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines);

// The 3D drag gizmo (A2.5). PublishGizmoHandles publishes the translate/rotate
// handles for the selected non-fixed occurrence (or the selected job part's
// placement) into state.viewportHandles, where the shared hit test AND the
// shared drawer read them (ui/ViewportHandleDraw.h); HandleGizmoInput runs the
// drag state machine (grab → live per-frame transform → release
// snapshots+solves) and returns true when it consumed the viewport (so the
// caller skips pick/orbit).
void PublishGizmoHandles(AppState& state);

// Pushes an occurrence's current placement to the renderers WITHOUT a resync
// — the live-drag path. Composes each mesh's place inside the occurrence, so
// dragging a sub-assembly moves the unit rather than collapsing its parts
// onto its origin.
void PushOccurrenceLive(AppState& state, uint64_t occId);

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released);

BomTable ComputeBom(AppState& state); // empty without an active assembly
// The same table over any assembly's flattened leaves -- a drawing's parts
// list draws an assembly that is not the active one, and leaves out what
// its view hides (docs/drawings/README.md#parts-lists-and-balloons).
BomTable ComputeBomOf(AppState& state,
                      const std::vector<assembly::FlatInstance>& leaves);

// What is wrong with a fastener already placed, or an empty list.
//
// The staleness gate (docs/parts/README.md#checking-placed-fasteners): a
// fastener whose stack has changed under it must REPORT that rather than
// silently keeping the length it was fitted with. Deriving from the plates is a
// dependency on other documents' geometry that regeneration cannot express
// (A2), so the value is derived once at placement — and this is what makes a
// stale one visible.
//
// Nothing is stored to make it work. A placed fastener's position and
// orientation ARE its rim and its axis, so the stack is re-traversed from
// scratch, and the washers and nut on the same axis are part of what it has to
// span — they are occurrences like any other. That means it also checks a
// fastener nobody derived, which a stored-at-placement record could not.
struct FastenerCheck {
  uint64_t occurrence = 0;
  std::string part; // the designation
  std::string why;  // decision-complete
};

// Findings AND how many screws produced them. Empty findings over zero screws
// and empty findings over forty-two are the same `ok: true` without the
// counts, which is the "valid over nothing" trap one document over — an
// assembly with no fasteners passes a fastener check vacuously. `skipped` is
// the other half: a screw whose standard or size this build cannot resolve
// was not judged, and must not read as one that passed.
struct FastenerReport {
  std::vector<FastenerCheck> findings;
  size_t examined = 0;
  size_t skipped = 0;
};
[[nodiscard]] FastenerReport CheckFasteners(AppState& state);

// The table as CSV (quoted where needed); unknown masses are empty cells.
std::string BomToCsv(const BomTable& bom);

// Visual-mesh export items (interchange X2) for the active tab: a part's
// bodies at identity, or an assembly's occurrences at their SOLVED placements
// (occurrences of one part share the underlying shape, so the writers emit
// instanced geometry). Each item carries the body's material color/PBR (or a
// palette color for an unassigned body). Empty when nothing is active.
// Defined in AppActions.cpp; the return type is MeshExport::Item.
std::vector<MeshExport::Item> BuildVisualExportItems(AppState& state);

// Exploded views (A4). AutoExplode replaces `viewId`'s steps with a radial
// seed (each occurrence pushed away from the assembly centroid, scaled by the
// assembly's size) — a starting point to refine by gizmo drag. Snapshots for
// undo; false without an active assembly / unknown view / fewer than two
// placed occurrences. ApplyExplodeTransforms re-applies display transforms
// (solved ∘ explode) to every occurrence's render instances WITHOUT a resync
// — the factor slider's cheap live path (picking refreshes on the next real
// resync).
bool AutoExplode(AppState& state, uint64_t viewId);

void ApplyExplodeTransforms(AppState& state);

// Imports an assembly STEP as parts + an assembly (interchange I2): one part
// tab per unique product (geometry extracted as .brep into
// <project>/imported, or the staging area before the first save),
// occurrences placed from the file's instance tree. Not undoable (creates
// documents, like Open).
bool ImportStepAsAssembly(AppState& state, const std::string& stepPath,
                          std::string* error);

// Same, for a caller that already parsed the file (the GUI import worker in
// ui/ImportJob.cpp probes the structure to choose this path). `stepPath` names
// the source in messages and the log; nothing is read from it. Parsing
// dominates the cost of an import, so a caller holding the model must never
// re-read the file.
bool ImportStepAsAssembly(AppState& state,
                          const CADKernel::ImportedModel& model,
                          const std::string& stepPath, std::string* error);

// The two halves of materializing a plan, in order: a part tab per planned
// part, then the assembly that places them. Split out of ImportStepAsAssembly
// so the background import (ui/ImportJob.cpp) can run the first a slice per
// frame while its progress modal keeps painting. Returns the new part's slot
// id; the ids, in plan order, are what MaterializeImportedAssembly places.
// The tab-bar folder generated standard parts live in. One name, here: a
// project with five sizes across four lengths is twenty documents, and a tab
// bar nobody can find their own part in is a failure discovered late and
// blamed on something else (docs/parts/README.md).
inline constexpr const char* kStandardPartsFolder = "Standard parts";

// Place a standard fastener in the ACTIVE ASSEMBLY
// (docs/parts/README.md#the-model-a-fastener-is-a-part-placed-as-an-occurrence).
// Returns the new occurrence's id, or 0 with `error` filled.
//
// One generated part document per distinct (standard, size, length, drive),
// reused by every occurrence of that spec and named as its BOM designation —
// so `bom` counts eight identical screws as one line of quantity 8 without
// knowing anything about fasteners, and mass follows from the material.
//
// Refused on a part tab, and not for tidiness: CAM generates toolpaths over
// the ACTIVE PART's bodies, so a screw body sitting in a part would be
// sectioned, stocked around and cut; mass and BOM would count it as the part
// rather than as a bought item; and check_geometry would flag it as a
// disconnected component, correctly.
//
// This phase places where it is told. Deriving the size and length from a
// picked hole — which is the point of the feature — is phase 3.
uint64_t AddFastener(AppState& state, const std::string& standard,
                     const std::string& size, double length,
                     const std::string& drive, const glm::dvec3& position,
                     const glm::dquat& orientation, std::string* error,
                     uint64_t ridesOn = 0, std::string* warning = nullptr);

// The rim, as a pick names it: WHICH occurrence, and which circular edge on
// it (docs/parts/README.md#deriving-a-fastener-from-a-hole).
//
// The occurrence is not decoration. Every occurrence of one part shares every
// element name, so an ElementRef alone does not name a rim in an assembly —
// it names one rim per placement. Resolving it by taking the first match,
// which is what this did until 2026-09-01, puts the fastener at the near end
// of a joint made of two occurrences of the same plate. That is the ordinary
// case, and the wrongness is silent.
//
// 0 means "infer", and infers only when there is nothing to infer between: a
// rim that resolves on several occurrences with no occurrence given is
// REFUSED naming them, not guessed at. A viewport pick always carries one.
struct RimPick {
  uint64_t occurrence = 0;
  ElementRef element;
  // The occurrence chain below `occurrence` down to the leaf that carries
  // the element (FlatInstance::innerIds); empty = any leaf under it. A
  // sub-assembly's parts can share feature ids and element names, and a
  // flexible pump's housing is one leaf of seventy.
  std::vector<uint64_t> innerPath;
};

// What deriving a fastener from a picked hole worked out, whether or not it
// could place one. Returned so the caller can SAY it: the decomposition is the
// product, not a by-product — a gap in the stack is either an unmodelled
// gasket or a defect, and printing "Plate 12.0, gap 3.0, Bracket 26.5" is what
// makes the second kind visible
// (docs/parts/README.md#deriving-a-fastener-from-a-hole). PlannedPart is
// declared in AppState.h: the tool's own state holds a plan (it is what the
// ghost draws), and AppState.h is the header this one includes rather than the
// other way round.

struct FastenerFit {
  bool ok = false;
  std::string why;         // when !ok, decision-complete
  std::string summary;     // "M6x25 — grip 22.7: Plate 12.0, gap 3.0, …"
  std::string size;        // derived, e.g. "M6"
  std::string clearance;   // "Close" | "Normal" | "Loose", from the hole
  double grip = 0.0;       // material + gaps the screw must span
  double length = 0.0;     // snapped to a stocked nominal
  bool blind = false;      // the last thing the axis met has no far side
  uint64_t occurrence = 0; // the screw; 0 when nothing was placed
  // Every occurrence placed, screw first: the washers and the nut too, so a
  // caller that has to undo the whole joint is not left hunting for the parts
  // it did not hear about.
  std::vector<uint64_t> placed;
  std::vector<std::string> parts; // their designations, in the same order
  // What Place WOULD do, filled on a dry run as well as a real one: screw
  // first, then the washers, then the nut. The commit path walks this rather
  // than recomputing the same arithmetic a second time.
  std::vector<PlannedPart> plan;
  // The mates the joint became: one per INTERFACE it spans — two for a bolt
  // through three parts — made for every bolt, so a second bolt through a
  // stack the first already joined adds its own (redundant but consistent;
  // docs/assemblies/solver.md#closing-loops).
  std::vector<uint64_t> mates;
  // What was placed but could not be tied to its hole: a piece that rides
  // its part at a fixed offset will not follow the hole if the part changes
  // (docs/parts/README.md#attached-and-mated), and the fit says so.
  std::vector<std::string> warnings;
};

// What to put on the axis
// (docs/parts/README.md#deriving-a-fastener-from-a-hole).
//
// The screw's own dimensions are still derived — this is only the CHOICE of
// what a joint is made of, which is the one thing the geometry cannot answer.
// A washer is not implied by a hole and a nut is not implied by a through
// hole: plenty of joints have neither, and adding them because they would fit
// is exactly the "plausible and wrong" this codebase refuses.
struct FastenerRequest {
  std::string standard = "iso4762"; // the screw
  std::string drive;                // empty: the one that standard is made with
  bool washerUnderHead = false;
  bool washerUnderNut = false;
  // A nut on the far side, by standard ("iso4032"), or empty for none.
  // Refused on a blind hole, where there is no far side to put one on.
  std::string nut;
  std::string washer = "iso7089";
  // The thread, when the hole cannot say it: "M10".
  //
  // Normally empty and normally derived — that is the whole point of picking
  // a rim. But 46 of the clearance table's 111 diameters are a clearance for
  // two different threads, and for those the hole genuinely does not know
  // which fastener it was drilled for. The derivation refuses there, naming
  // the candidates, and this is the way through. Given, it is CHECKED against
  // the hole rather than believed: a size whose clearances do not include
  // this diameter is refused.
  std::string size;
  // WHERE THE JOINT ENDS, when the axis carries on past it.
  //
  // One pick derives; this is the second, and it is optional because most
  // joints do not need it — the stack the axis passes through IS the joint.
  // It stops being true the moment the axis continues into something the
  // fastener is not clamping: a bolt through a housing flange whose line
  // carries on through a boss behind it would otherwise be sized to span the
  // lot, and a screw twice as long as it should be looks entirely reasonable
  // in a viewport.
  //
  // Names a rim like the first pick does. It TRUNCATES rather than replaces:
  // everything between the two picks is still traversed, because the grip and
  // the far-side hardware still depend on what is in there
  // (docs/parts/README.md#deriving-a-fastener-from-a-hole, the second pick).
  RimPick farEnd;
  bool dryRun = false; // derive and explain without placing
};

// Derive a fastener from a picked hole RIM and place it (phase 3).
//
// `rim` names a circular edge — the rim the head sits against. A cylindrical
// face has an axis but no direction, so it cannot say which end the head goes
// on; a rim answers both in one pick, and it is the same gesture as picking an
// edge to fillet.
//
// Everything but the head style is then derived: the SIZE from the hole's own
// diameter (the clearance table read backwards), the LENGTH from the stack the
// axis passes through, snapped to a length that size is actually made in.
//
// `request.dryRun` derives and explains without placing, which is what a GUI
// preview and a "what would this be" query both want.
//
// Phase 4 completes the JOINT rather than placing a lone screw: washers and a
// far-side nut come off the same traversal, so the length accounts for them
// instead of the person having to. That is the whole argument for deriving —
// a screw sized for the plates and then fitted with two washers and a nut is
// too short, and it looks exactly right in the viewport.
FastenerFit FitFastenerToHole(AppState& state, const RimPick& rim,
                              const FastenerRequest& request);

uint64_t MaterializeImportedPart(AppState& state,
                                 const interchange::PlannedPart& part,
                                 const std::string& asmName);
void MaterializeImportedAssembly(AppState& state,
                                 const interchange::AssemblyImportPlan& plan,
                                 const std::string& asmName,
                                 const std::vector<uint64_t>& partIds);

// Occurrence placement and part lookup.
//
// Split out of ui/AppActions.h, which declared 170 functions across every
// domain in the app and was included by 64 files -- so a change to any one
// of them rebuilt 73 targets (146 s) instead of the handful that care.
// docs/architecture.md#the-ui-actions-layer.

// The part document an occurrence (or a cross-part query) references, by
// stable id — active or stashed; never switches the active tab. Null when the
// id names no part slot.
Document* PartDocById(AppState& state, uint64_t partId);

// An occurrence's solved placement as a model matrix (translate ∘ rotate) —
// the transform assembly measure / interference place part bodies with.
glm::dmat4 OccurrenceMatrix(const Occurrence& o);

// A5 resolution: an occurrence's stable id names a part OR an assembly slot,
// per its refKind.
AssemblyDocument* AssemblyDocById(AppState& state, uint64_t slotId);

// Fill a fastened mate's DOFs from where its two occurrences ALREADY lie —
// see assembly::MateDerive. Resolves both connector frames (so the mate's
// anchors must already be set) and hands the relative transform to
// assembly::DeriveMateDofs. False when either anchor will not resolve, or the
// two are not square to one axis; the mate is left untouched.
[[nodiscard]] bool DeriveMateFromPlacement(AppState& state, Mate& mate,
                                           assembly::MateDerive how);

assembly::PartResolver PartResolverFor(AppState& state);

// The part an occurrence places, in the configuration it asks for: the live
// document when that is 0 or already active, else a copy regenerated in it
// (ConfiguredPartDocument). Read-only: to EDIT the part, open its slot.
// Every read of a placed part's geometry goes through here, not through
// PartDocById(occ.partId), which is the active configuration whatever the
// placement asked for (tests/test_conventions.cpp holds the line).
const Document* OccurrencePartDoc(AppState& state, const Occurrence& occ);

// The part a render mesh of `occ` shows -- its leaf part in the leaf's
// configuration, which for a sub-assembly occurrence is not the
// occurrence's own. What a viewport pick names its faces through.
const Document* MeshPartDoc(AppState& state, const DocumentRenderMesh& mesh,
                            const Occurrence& occ);

assembly::AssemblyResolver AssemblyResolverFor(AppState& state);

// The active assembly's leaf parts, placed in world space — the one walk
// every geometry consumer uses (render, BOM, mass, contact, export, drawings,
// ray tracing, section caps). Empty when the tree cannot be walked, with the
// reason logged.
std::vector<assembly::FlatInstance> FlattenActiveAssembly(AppState& state);

// The visible matter one TOP-LEVEL occurrence places, already in world space:
// every leaf part under it, each body transformed by its composed world.
// Derived ghosts are excluded — a reference copy is not the occurrence's
// matter, and counting it makes a part clash with its own shadow.
//
// One definition, because there were two callers about to have two: the exact
// clash sweep asks it of a pair the user named, and a clearance assertion
// asks it of the pair the DOCUMENT names. Those must agree — the assertion
// exists to be the same question, asked later.
std::vector<Body> OccurrenceSubtreeBodies(AppState& state,
                                          uint64_t rootOccurrenceId);

// The placement a mesh renders at (solved ∘ explode), exposed for tests and
// for anything that has to reproduce the live-drag composition.
glm::dmat4 DisplayMatrixOf(const AppState& state, const Occurrence& o);

// Drops every cached part tessellation and releases the GPU assets it holds
// (AppState::partRenderCache). MUST run while the renderers are still alive —
// the cache stores asset ids, and the release is what frees their buffers —
// so main.cpp calls it immediately before ShutdownSceneRenderers().
void ReleasePartRenderCache(AppState& state);

} // namespace ui
