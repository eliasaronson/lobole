#pragma once

#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class Document;

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
AssemblyMass MassProperties(const AssemblyDocument& asmDoc,
                            const PartResolver& partById);

} // namespace assembly
