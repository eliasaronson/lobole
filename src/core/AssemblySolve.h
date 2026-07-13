#pragma once

#include "core/Occurrence.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <optional>

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
  int placed = 0;      // occurrences positioned by a mate (excludes anchors)
  int erroredMates = 0; // mates left in an error state this solve
};

// The connector's local frame in its part's coordinates (columns X, Y, Z,
// origin). nullopt if the occurrence is missing, its part isn't loaded, or a
// face anchor's ElementRef no longer resolves. Exposed for the GUI (draw the
// connector triad, preview a mate) as well as the solve.
std::optional<glm::mat4> ConnectorLocalFrame(const Connector& c,
                                             const AssemblyDocument& asmDoc,
                                             const PartResolver& partById);

// Re-places every mated, non-`fixed` occurrence and writes the result back to
// its transform (position + orientation). `fixed` occurrences and occurrences
// in no mate keep their stored transforms. Clears then re-stamps each mate's
// transient error. Does NOT bump the assembly version — the caller does that
// to trigger a renderer resync (so a no-op solve doesn't churn the renderer).
SolveReport Solve(AssemblyDocument& asmDoc, const PartResolver& partById);

} // namespace assembly
