#pragma once

#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Document;
class AssemblyDocument;

// The A3 assembly placement pass ("the solve"): given occurrences and mates,
// position each mated occurrence relative to its neighbours by direct frame
// alignment — no numeric 3D solver. Each mate fully determines one occurrence
// relative to the other (given its stored DOF pose), so placement is a
// closed-form transform propagated sequentially from the `fixed` anchors
// through the mate graph. Mate *trees* are solved; genuine loops are detected
// and reported (per-mate error), not solved. See docs/assemblies/a3-design.md.
namespace assembly {

// Resolves an occurrence's part_id to its already-regenerated part Document
// (GetBodies() must be populated — the caller keeps parts regenerated). The UI
// passes ui::PartDocById; tests pass a lambda over locally-built documents.
using PartResolver = std::function<const Document*(uint64_t partId)>;

struct SolveReport {
  int placed = 0;       // occurrences positioned by a mate (excludes anchors)
  int erroredMates = 0; // mates left in an error state this solve
};

// The connector's local frame in its part's coordinates (columns X, Y, Z,
// origin). nullopt if the occurrence is missing, its part isn't loaded, or a
// face anchor's ElementRef no longer resolves. Exposed for the GUI (draw the
// connector triad, preview a mate) as well as the solve. When `whyNot` is
// non-null a failure fills it with a user-facing diagnosis — including
// near-miss face names when the referenced name is a stale prefix of a
// current one (the modified-by-a-later-feature case, e.g. "6/g1;M6" ->
// "6/g1;M6;M7").
std::optional<glm::mat4> ConnectorLocalFrame(const Connector& c,
                                             const AssemblyDocument& asmDoc,
                                             const PartResolver& partById,
                                             std::string* whyNot = nullptr);

// Re-places every mated, non-`fixed` occurrence and writes the result back to
// its transform (position + orientation). `fixed` occurrences and occurrences
// in no mate keep their stored transforms. Clears then re-stamps each mate's
// transient error. Does NOT bump the assembly version — the caller does that
// to trigger a renderer resync (so a no-op solve doesn't churn the renderer).
SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById);

// ---- Flatten: the one tree walk (A5) --------------------------------------
//
// An assembly may place other assemblies. Every consumer that wants GEOMETRY
// — render, BOM, mass, contact, export, drawings, ray tracing, section caps —
// wants the same thing from that tree: the leaf PARTS, each with a world
// transform. Flatten is that answer, and it exists so those consumers do not
// each grow their own recursion (which is how they would drift apart).
//
// v1 sub-assemblies are RIGID: a placed assembly is one solid unit at
// whatever placements its own document stores. Flatten never solves.
struct FlatInstance {
  uint64_t partId = 0;            // always a PART: the leaves of the tree
  const Document* part = nullptr; // resolved, regenerated
  glm::mat4 world{1.0f};          // composed down the tree
  std::string path;               // "Gangway/Board 03" — display, not a key
  uint64_t rootOccurrenceId = 0;  // the TOP-LEVEL occurrence this belongs to
  bool visible = true;            // every level's visibility, ANDed
};

// Resolves a slot id to an assembly document (nullptr when it is not one).
using AssemblyResolver =
    std::function<const AssemblyDocument*(uint64_t slotId)>;

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
  glm::mat4 world{1.0f}; // composed placement, in the top's space
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
std::vector<std::pair<uint64_t, uint64_t>>
SubtreeVersions(const AssemblyDocument& top, const PartResolver& partById,
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
