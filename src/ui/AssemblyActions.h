#pragma once

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
// docs/architecture-review-2026-08-24.md.
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
// stored. One row per referenced part, in first-occurrence order; masses via
// Measure::MassOfBodies (a part with any density-less body reports its mass
// unknown and taints the total). `visible=false` occurrences count —
// visibility is a display state, not suppression.
struct BomRow {
  uint64_t partId = 0;
  std::string partName; // "(missing part)" when the reference dangles
  int quantity = 0;
  std::string material;    // slug; "" = none, "mixed" when bodies differ
  double unitVolume = 0.0; // mm^3, all bodies of the part
  bool massKnown = false;
  double unitMassG = 0.0; // valid when massKnown
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
void MateConnectorPicked(AppState& state, const SelectionItem& item);

void AcceptMateTool(AppState& state);

void CancelMateTool(AppState& state);

// Arms connector slot A or B on the shared armed-slot state (the mate
// session's slot rows re-arm through this too).
void ArmMateConnector(AppState& state, bool second);

// The world frame of a connector (occurrence placement ∘ its part-local frame),
// or nullopt if it doesn't resolve. For drawing the connector triad.
std::optional<glm::mat4> ConnectorWorldFrame(AppState& state,
                                             const Connector& c);

// Appends the mate tool's connector triads (picked connectors) to `lines`.
void BuildMateToolOverlay(AppState& state, std::vector<LineVertex3D>& lines);

// The 3D drag gizmo (A2.5). BuildGizmoOverlay appends the translate/rotate
// handles for the selected non-fixed occurrence; HandleGizmoInput runs the drag
// state machine (grab → live per-frame transform → release snapshots+solves)
// and returns true when it consumed the viewport (so the caller skips
// pick/orbit).
void BuildGizmoOverlay(AppState& state, std::vector<LineVertex3D>& lines);

// Pushes an occurrence's current placement to the renderers WITHOUT a resync
// — the live-drag path. Composes each mesh's place inside the occurrence, so
// dragging a sub-assembly moves the unit rather than collapsing its parts
// onto its origin.
void PushOccurrenceLive(AppState& state, uint64_t occId);

bool HandleGizmoInput(AppState& state, const Ray& ray, bool clicked, bool down,
                      bool released);

BomTable ComputeBom(AppState& state); // empty without an active assembly

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
// blamed on something else (docs/parts/plan.md).
inline constexpr const char* kStandardPartsFolder = "Standard parts";

// Place a standard fastener in the ACTIVE ASSEMBLY (docs/parts/plan.md,
// phase 2). Returns the new occurrence's id, or 0 with `error` filled.
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
                     const std::string& drive, const glm::vec3& position,
                     const glm::quat& orientation, std::string* error);

// What deriving a fastener from a picked hole worked out, whether or not it
// could place one. Returned so the caller can SAY it: the decomposition is the
// product, not a by-product — a gap in the stack is either an unmodelled
// gasket or a defect, and printing "Plate 12.0, gap 3.0, Bracket 26.5" is what
// makes the second kind visible (docs/parts/plan.md, phase 3).
struct FastenerFit {
  bool ok = false;
  std::string why;         // when !ok, decision-complete
  std::string summary;     // "M6x25 — grip 22.7: Plate 12.0, gap 3.0, …"
  std::string size;        // derived, e.g. "M6"
  std::string clearance;   // "Close" | "Normal" | "Loose", from the hole
  double grip = 0.0;       // material + gaps the screw must span
  double length = 0.0;     // snapped to a stocked nominal
  bool blind = false;      // the last thing the axis met has no far side
  uint64_t occurrence = 0; // 0 when nothing was placed
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
// `dryRun` derives and explains without placing, which is what a GUI preview
// and a "what would this be" query both want.
FastenerFit FitFastenerToHole(AppState& state, const ElementRef& rim,
                              const std::string& standard,
                              const std::string& drive, bool dryRun);

uint64_t MaterializeImportedPart(AppState& state,
                                 const interchange::PlannedPart& part,
                                 const std::string& asmName);
void MaterializeImportedAssembly(AppState& state,
                                 const interchange::AssemblyImportPlan& plan,
                                 const std::string& asmName,
                                 const std::vector<uint64_t>& partIds);

} // namespace ui
