#pragma once

#include "core/AssemblyLoopSolve.h"
#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Document;
class AssemblyDocument;

// The assembly placement pass ("the solve"): given occurrences and mates,
// position each mated occurrence relative to its neighbours by direct frame
// alignment. Each mate fully determines one occurrence relative to the other
// (given its stored DOF pose), so placement over a mate *tree* is a
// closed-form transform propagated sequentially from the `fixed` anchors
// through the mate graph. Genuine loops are closed afterwards by the numeric
// loop solver (AssemblyLoopSolve.h), which adjusts the mates' DOF fields; a
// loop it cannot close leaves an error on its cut mates. See
// docs/assemblies/solver.md.
namespace assembly {

// Resolves an occurrence's part_id to its already-regenerated part Document
// (GetBodies() must be populated — the caller keeps parts regenerated), in
// `configurationId` -- the configuration the placement asked for, 0 for
// whatever the part has active. The UI passes ui::PartResolverFor, which
// regenerates a copy in the asked configuration; tests pass a lambda over
// locally-built documents.
using PartResolver =
    std::function<const Document*(uint64_t partId, uint64_t configurationId)>;

// Resolves a slot id to an assembly document (nullptr when it is not one).
// Declared beside PartResolver because everything from mate connectors to
// Flatten needs both to see through a sub-assembly.
using AssemblyResolver =
    std::function<const AssemblyDocument*(uint64_t slotId)>;

struct SolveReport {
  int placed = 0;       // occurrences positioned by a mate (excludes anchors)
  int erroredMates = 0; // mates left in an error state this solve
  // Riders whose hole no longer resolves (Occurrence::attachError): left
  // where they were, and counted here so a solve's answer says so.
  int erroredAttachments = 0;

  // ---- A6: what the numeric loop pass found -------------------------------
  // All zero/empty on a loop-free assembly, which never enters that pass.
  int loopMates = 0;       // mates that closed a loop (the cut set)
  int dof = 0;             // degrees of freedom left in the looped components
  bool loopsClosed = true; // every loop reached its tolerance
  double residualMm = 0.0; // worst remaining closure error, mm
  // Mates whose constraint rows were dependent but CONSISTENT — the third
  // and fourth bolt through a flange. Not an error: they are satisfied, they
  // just say nothing new. Named rather than counted, because the point is to
  // let a human see WHICH.
  std::vector<uint64_t> redundantMates;
  // One per looped component that did not close, when the solve ran the
  // diagnosis: which single tilted connector would let it close
  // (docs/assemblies/solver.md#naming-the-mate-that-keeps-a-loop-open).
  std::vector<LoopDiagnosis> diagnoses;

  // v129: the mates INSIDE flexible placements, by path, since their ids
  // are the sub-assembly's and mean nothing in this document's list. One row
  // per mate that is not simply satisfied (an error, or redundant); `mate`
  // 0 = the placement itself could not move, and `error` says why.
  struct InnerMate {
    uint64_t occurrence = 0; // the flexible placement (its path, one level)
    uint64_t mate = 0;       // the sub-assembly's mate id; 0 = the unit
    std::string name;
    std::string error;
    double residualMm = 0.0;
    bool redundant = false;
  };
  std::vector<InnerMate> innerMates;
};

// The connector's local frame in its part's coordinates (columns X, Y, Z,
// origin). nullopt if the occurrence is missing, its part isn't loaded, or a
// face anchor's ElementRef no longer resolves. Exposed for the GUI (draw the
// connector triad, preview a mate) as well as the solve. When `whyNot` is
// non-null a failure fills it with a user-facing diagnosis — including
// near-miss face names when the referenced name is a stale prefix of a
// current one (the modified-by-a-later-feature case, e.g. "6/g1;M6" ->
// "6/g1;M6;M7").
// `asmById` (A5/v57) lets a face anchor reach a part INSIDE a placed
// sub-assembly, via the connector's innerPath. The returned frame is always
// in the TOP-LEVEL occurrence's space, so the solve composes it exactly as
// before — a rigid unit's inner face sits at a known place in the unit, and
// a flexible one's where its own inner poses put it. An ORIGIN anchor with a
// path is the origin of what the path names, not the unit's.
//
// `followModified` (a seat's faces, SeatFrame): a face name that no longer
// resolves is followed to the ONE face it became through later
// modifications — its name plus whole `;M<tag>` steps, unsplit (a chamfer on
// a bolt hole's rim modifies the bore and the face round it, and neither
// stops being the face the screw sits in). A split (`.i`), or two faces
// claiming the name, still fails: there is no one face to sit on.
std::optional<glm::dmat4>
ConnectorLocalFrame(const Connector& c, const AssemblyDocument& asmDoc,
                    const PartResolver& partById, std::string* whyNot = nullptr,
                    const AssemblyResolver& asmById = nullptr,
                    bool followModified = false);

// Re-places every mated, non-`fixed` occurrence and writes the result back to
// its transform (position + orientation). `fixed` occurrences and occurrences
// in no mate keep their stored transforms. Clears then re-stamps each mate's
// transient error. Does NOT bump the assembly version — the caller does that
// to trigger a renderer resync (so a no-op solve doesn't churn the renderer).
// `asmById` (A5) is needed only for connectors whose anchor reaches into a
// sub-assembly; omit it and those mates report that they cannot resolve.
// Re-seat everything that RIDES on something else: an occurrence with
// `attachedTo` gets its placement recomputed from its parent's, so a screw
// follows the plate it is bolted to (docs/parts/README.md#attached-and-mated).
// A rider with an `attachSeat` is placed from that seat's frame (SeatFrame),
// so it also follows the seat — a hole, a groove — when it moves INSIDE the
// part; one whose seat no longer resolves keeps its pose and gets
// `attachError`, counted in `*errored`. `partById` resolves those seats;
// nullptr leaves a seated rider in error rather than guessing.
//
// Composition, not constraint — it runs AFTER the solve rather than inside
// it, because there is nothing to solve: a zero-DOF leaf's pose is a matrix
// multiply. A seat's frame is cached on the rider against the part's
// regeneration, and each face is resolved once per call however many riders
// share it (every screw of a pattern bears on one face). Returns how many
// moved.
//
// ONE LEVEL, on purpose. An attachment whose parent is itself attached is
// left alone rather than chased: chains want a rigid-group feature with its
// own UI, and inventing one here would be building for a case nobody has.
int ApplyAttachments(AssemblyDocument& asmDoc, const PartResolver& partById,
                     const AssemblyResolver& asmById = nullptr,
                     int* errored = nullptr);

// The frame of the seat `seat` names on `parent`, in the parent's own space
// (a sub-assembly's unit space when `seat.innerPath` is set): origin where
// the axis face's axis meets the seat face's plane, Z the seat face's
// outward normal, X the parent's X projected into that plane (Y when X is
// square to it). Each face is resolved through ConnectorLocalFrame, as a
// mate connector on it would be, so a rider behaves as a concentric +
// coincident mate to the two faces. nullopt with `whyNot` when either face
// is gone or the axis is no longer square to the face.
std::optional<glm::dmat4>
SeatFrame(const AttachSeat& seat, const Occurrence& parent,
          const AssemblyDocument& asmDoc, const PartResolver& partById,
          const AssemblyResolver& asmById, std::string* whyNot = nullptr);

// Seats `rider` (attached, its pose already set) on `seat` of its parent:
// re-expresses `attachOffset`/`attachRotation` in the seat's frame, so it
// stays exactly where it is now and follows the seat from here on. False
// with `whyNot` (and nothing changed) when the seat does not resolve. The
// general entry point: any standard part placed against two named faces.
bool SeatRider(const AssemblyDocument& asmDoc, Occurrence& rider,
               const AttachSeat& seat, const PartResolver& partById,
               const AssemblyResolver& asmById, std::string* whyNot = nullptr);

// The HOLE a fastener placed at `riderWorld` sits in on `parent`, as a seat:
// the nearest planar face of the parent square to the piece's Z, facing it,
// with a circular edge centred on that axis (the face), and a coaxial
// cylinder — else a cone — with a circle in that face's plane (the axis).
// nullopt with `whyNot` when there is none.
std::optional<AttachSeat>
FindHoleSeat(const AssemblyDocument& asmDoc, const Occurrence& parent,
             const glm::dmat4& riderWorld, const PartResolver& partById,
             const AssemblyResolver& asmById, std::string* whyNot = nullptr);

// Seats `rider` (attached, its pose already set) in the hole under it:
// FindHoleSeat, then SeatRider. False with `attachWarning` set when there is
// none — it then rides at its offset in the parent's frame.
bool BindRiderToHole(const AssemblyDocument& asmDoc, Occurrence& rider,
                     const PartResolver& partById,
                     const AssemblyResolver& asmById);

// Every attached occurrence with no seat that has not been looked at this
// session (a file from before v157): its pose from the old offset, then
// BindRiderToHole. Returns how many it seated.
int BindRidersToHoles(AssemblyDocument& asmDoc, const PartResolver& partById,
                      const AssemblyResolver& asmById);

// Relations (docs/assemblies/relations.md): make every relation hold
// by setting the dependent mate's DOF from the driving one. `drivenMateId`
// names the mate that was just changed (a drag, set_mate) and the walk goes
// outward from it, so dragging either gear turns the other; 0 means an
// unprompted solve, where A drives B — deterministic, and a no-op after a
// driven pass because propagation is idempotent. Sets `Relation::error`
// for a mate that is gone or lacks the DOF, and returns how many DOFs it
// wrote. Solve() calls it with 0; callers that changed a mate call it with
// that mate before Solve().
int PropagateRelations(AssemblyDocument& asmDoc, uint64_t drivenMateId = 0,
                       const PartResolver& partById = nullptr);

// Read/write one DOF of a mate by name; false when the mate type lacks it.
bool MateHasDof(MateType type, MateDof dof);

// What a relation makes of its driving value: `ratio * a + phase` for every
// kind but a cam, which reads its lift table instead.
double RelationValue(const Relation& r, double drivingValue);
[[nodiscard]] double MateDofValue(const Mate& m, MateDof dof);
void SetMateDof(Mate& m, MateDof dof, double value);

// What the caller just drove, held while a loop closes around it. Zero =
// nothing driven, which is every call that is not a drag, a set_mate or a
// motion sample.
struct Driven {
  uint64_t mate = 0;
  MateDof dof = MateDof::Rotation;
  // v129: the flexible placement the mate is inside (Relation::mateAPath);
  // empty = a mate of the document being solved.
  std::vector<uint64_t> path;
};

// `diagnose`: when a loop does not close, look for the one tilted connector
// that would close it and name it in the cut mates' errors. It costs a few
// extra solves of the failing component, only on failure; a motion study's
// per-sample solve turns it off, since a sample that fails is reported with
// its residual and nothing reads the text.
SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById,
                  const AssemblyResolver& asmById = nullptr,
                  const Driven& driven = {}, bool diagnose = true);
// How many times Solve has run in this process: what a test counts to show
// `leave_at` poses with one solve rather than a walk.
uint64_t SolveCount();

// ---- Flexible sub-assemblies (v129) ----------------------------------------
// (docs/assemblies/README.md#flexible-sub-assemblies)
//
// The solver does not know about them. It is WRAPPED: ExpandFlexible copies
// the parent and adds, for each flexible placement, the sub-assembly's moving
// occurrences, mates and relations under synthetic ids from a range no
// document reaches; Solve runs on that copy; CollapseFlexible writes the
// placements, the inner poses, the inner mate values and the errors back to
// the parent. Solve() does all three itself when anything is flexible, and
// a motion study expands ONCE and runs every sample on the expanded copy.

// Where an occurrence of `owner`'s sub-assembly stands in owner's frame: the
// pose the placement keeps for it when `owner` is flexible and has one, else
// the one its own document stores.
glm::dmat4 InnerPlacement(const Occurrence& owner, const Occurrence& inner);

// Why `owner` cannot move as a flexible placement, or empty when it can:
// it places a part, its sub-assembly is not loaded, has nothing fixed to
// move against, holds a freewheel or a click (their history would be
// per-placement state), or places something flexible itself (one level).
std::string FlexibleRefusal(const Occurrence& owner,
                            const AssemblyResolver& asmById);

struct FlexibleUnit {
  uint64_t root = 0; // the flexible occurrence
  const AssemblyDocument* sub = nullptr;
  std::string error; // why it stays rigid this solve; empty = expanded
  // Its occurrences that MOVE (reached through mates from a fixed one); the
  // rest fold into the root — they cannot move relative to it.
  std::vector<uint64_t> moving;
  std::vector<uint64_t> mates; // its mates copied in (the sub's ids)
};

struct FlexibleExpansion {
  AssemblyDocument doc; // the parent, with every unit's inside added
  std::vector<FlexibleUnit> units;
  bool any = false; // at least one unit expanded
  // synthetic id <-> (root, the sub-assembly's id), for occurrences and mates
  std::unordered_map<uint64_t, std::pair<uint64_t, uint64_t>> occOf, mateOf;
  std::map<std::pair<uint64_t, uint64_t>, uint64_t> occIdOf, mateIdOf;
  // A parent relation whose path named no mate: its error, by relation id.
  std::map<uint64_t, std::string> relationErrors;

  // The expanded id of a mate named {path, mate}: the mate itself for an
  // empty path, 0 when the path names no expanded unit or no mate in it.
  uint64_t MateId(const std::vector<uint64_t>& path, uint64_t mate) const;
  const FlexibleUnit* UnitFor(uint64_t root) const;
};

FlexibleExpansion ExpandFlexible(const AssemblyDocument& parent,
                                 const PartResolver& partById,
                                 const AssemblyResolver& asmById);
// Writes the expanded solve back into `parent` (the document it was expanded
// from): placements, mate values and errors, relation state, every unit's
// inner poses and mate values; and translates `report` (optional) so it names
// inner mates by path rather than by synthetic id.
void CollapseFlexible(const FlexibleExpansion& ex, AssemblyDocument& parent,
                      SolveReport* report);

// A mate named {path, mate} (v129): the mate a relation, a study driver or
// set_mate means, read from the sub-assembly the path's placement holds —
// or this document's own for an empty path. nullptr with `why` filled when
// the path is not one flexible placement, or names no mate there.
const Mate* MateAtPath(const AssemblyDocument& doc,
                       const std::vector<uint64_t>& path, uint64_t mate,
                       const AssemblyResolver& asmById,
                       std::string* why = nullptr);
// Its DOF value as the document has it posed: a flexible placement's own
// value for an inner mate (or the sub's authored one before the first solve).
std::optional<double> MateDofAt(const AssemblyDocument& doc,
                                const std::vector<uint64_t>& path,
                                uint64_t mate, MateDof dof,
                                const AssemblyResolver& asmById);
// ...and set, into the placement's innerMateDofs for an inner mate.
bool SetMateDofAt(AssemblyDocument& doc, const std::vector<uint64_t>& path,
                  uint64_t mate, MateDof dof, double value,
                  const AssemblyResolver& asmById);

// ---- Flatten: the one tree walk (A5) --------------------------------------
//
// An assembly may place other assemblies. Every consumer that wants GEOMETRY
// — render, BOM, mass, contact, export, drawings, ray tracing, section caps —
// wants the same thing from that tree: the leaf PARTS, each with a world
// transform. Flatten is that answer, and it exists so those consumers do not
// each grow their own recursion (which is how they would drift apart).
//
// A placed assembly is one solid unit at whatever placements its own document
// stores — unless the placement is FLEXIBLE (v129), when the poses it keeps
// for its sub-assembly's occurrences place them instead. Flatten never
// solves.
struct FlatInstance {
  uint64_t partId = 0;            // always a PART: the leaves of the tree
  // The part configuration the placement asked for; 0 = the part's active
  // one. `part` is the document evaluated in it, so two placements of one
  // part in two configurations are two different documents here.
  uint64_t configurationId = 0;
  // The placement's material, overriding every body's; "" = the part's own.
  std::string material;
  const Document* part = nullptr; // resolved, regenerated
  glm::dmat4 world{1.0};          // composed down the tree
  std::string path;               // "Gangway/Board 03" — display, not a key
  uint64_t rootOccurrenceId = 0;  // the TOP-LEVEL occurrence this belongs to
  // The occurrence ids BELOW the root, down to this leaf — the key form of
  // `path`, and exactly what a mate connector's innerPath needs. Empty when
  // the root places this part directly.
  std::vector<uint64_t> innerIds;
  bool visible = true; // every level's visibility, ANDed

  // The tree `world` and `path` flatten away, for a consumer that writes the
  // tree back out (STEP export). `levels` are the sub-assembly placements
  // above this leaf, outermost first — empty when the top document places
  // the part directly; `occurrenceId`/`local`/`name` are the leaf's own
  // placement in the document that holds it. world == levels' locals * local.
  struct Level {
    uint64_t occurrenceId = 0;    // the placement, in its parent document
    uint64_t assemblyId = 0;      // the sub-assembly's slot id
    uint64_t configurationId = 0; // what the placement asked for (0 = active)
    glm::dmat4 local{1.0};
    std::string name;
  };
  std::vector<Level> levels;
  uint64_t occurrenceId = 0;
  glm::dmat4 local{1.0};
  std::string name;
};

// The leaf parts of `top`, in document order, depth first.
//
// Fails (rather than rendering something plausible) on a reference cycle,
// naming the path that closes it, and on exceeding `maxDepth` — a runaway
// backstop, not a modelling limit. A reference that resolves to nothing is
// SKIPPED, not an error: a dangling occurrence is already how a deleted part
// behaves, and the tree should still show what remains.
std::expected<std::vector<FlatInstance>, std::string>
Flatten(const AssemblyDocument& top, const PartResolver& partById,
        const AssemblyResolver& asmById, int maxDepth = 8);

// One occurrence somewhere in the tree, addressed by a PATH (A5, read-only).
//
// Reading into a sub-assembly is supported; writing into one is not — it is
// edited on its own tab. So this hands back const pointers and a composed
// transform, and there is deliberately no mutable twin.
//
// The only thing it makes possible: a query ACROSS the boundary. "Is
// Gangway/Board 19 touching Gang hinge 1" cannot be asked on either tab —
// the board lives in the sub-assembly's document, the hinge at the top.
struct PathHit {
  const AssemblyDocument* owner = nullptr; // the document holding it
  const Occurrence* occ = nullptr;
  glm::dmat4 world{1.0}; // composed placement, in the top's space
  uint64_t rootId = 0;   // the top-level occurrence it belongs to
  std::string path;      // canonical id chain, e.g. "12/47"
};

// Resolves "12/47" (ids) or "Gangway/Board 19" (names, when unambiguous).
// A bare "12" or "Gangway" resolves to a top-level occurrence, so callers can
// route every reference through here.
//
// Fails with a message rather than a null: an ambiguous name path says WHICH
// names collided, because occurrence names are not unique and the same
// sub-assembly can be placed twice.
std::expected<PathHit, std::string> ResolvePath(const AssemblyDocument& top,
                                                const std::string& ref,
                                                const PartResolver& partById,
                                                const AssemblyResolver& asmById,
                                                int maxDepth = 8);

// Would placing `refId` (of kind `kind`) into `host` close a cycle? Used to
// refuse the occurrence at add time instead of discovering it at render time.
bool WouldCycle(const AssemblyDocument& host, uint64_t hostSlotId,
                uint64_t refId, OccurrenceRef kind,
                const AssemblyResolver& asmById, std::string* why = nullptr);

// Every document version in `top`'s subtree, so a caller can tell whether
// anything below it changed. Keyed by slot id: part versions and, for nested
// assemblies, their own document versions too. A top-level assembly that
// stamps only its DIRECT parts cannot see an edit inside a sub-assembly.
// A part's stamp also folds in its configurations' revision: editing a
// configuration a placement uses moves no Version. (Choosing another is an
// edit of the assembly, which its own Version says.)
std::vector<std::pair<uint64_t, uint64_t>>
SubtreeVersions(const AssemblyDocument& top, const PartResolver& partById,
                const AssemblyResolver& asmById, int maxDepth = 8);

// Every (part, configuration) placed anywhere in `top`'s subtree, once each,
// in walk order -- the documents a consumer that copies the subtree needs.
std::vector<std::pair<uint64_t, uint64_t>>
SubtreePartConfigurations(const AssemblyDocument& top,
                          const AssemblyResolver& asmById, int maxDepth = 8);

// ---- Assembly mass aggregation (finding #5) -------------------------------
// One placed occurrence's mass contribution, in WORLD space. Volume and mass
// are rigid-transform invariant (occurrence transforms are translate+rotate,
// never scaled); the center of mass is the part-local CoM carried through the
// occurrence matrix.
struct OccurrenceMass {
  uint64_t occurrenceId = 0;
  std::string name;
  uint64_t partId = 0;
  double volume = 0.0;          // mm^3
  glm::dvec3 centerOfMass{0.0}; // world space
  bool massKnown = false;       // false when the part has a density-less body
  double massG = 0.0;           // valid when massKnown
};

// Combined mass properties of an assembly: the total plus a per-occurrence
// breakdown, all in world space. Suppressed (by the active configuration) and
// invisible occurrences are skipped — the BOM convention. `massKnown` is false
// (and `massG` left 0) when any contributing occurrence's mass is unknown, but
// volume and the world center of mass are always aggregated. An occurrence
// whose part is missing or fails its mass computation is skipped and named in
// `skipped`.
struct AssemblyMass {
  std::vector<OccurrenceMass> occurrences;
  double volume = 0.0;
  glm::dvec3 centerOfMass{0.0}; // world, volume-weighted
  bool massKnown = true;
  double massG = 0.0;
  std::vector<std::string> skipped; // occurrences that could not be measured
};

// Aggregate every visible, non-suppressed occurrence's part mass into world
// space. Reuses Measure::MassOfBodies per part, then transforms each CoM by the
// occurrence matrix. Does not touch the document (a pure read).
// `asmById` (A5) makes this see through sub-assemblies: without it, an
// occurrence that references an assembly resolves to no part and its whole
// subtree weighs nothing. Pass a resolver that returns nullptr for everything
// to get the pre-A5, top-level-only behaviour.
AssemblyMass MassProperties(const AssemblyDocument& asmDoc,
                            const PartResolver& partById,
                            const AssemblyResolver& asmById = nullptr);

} // namespace assembly
