#pragma once

#include "cad/CADKernel.h"
#include "core/Feature.h" // Body
#include "core/tube/Check.h" // Bend

#include <optional>
#include <string>
#include <utility>
#include <vector>

class Document;

// How much of a member's path still has metal on it (docs/tube/plan.md, T3).
//
// A tube is cut to the DEVELOPED length of its centreline — but a member that
// has been coped is shorter than the path it was swept along, by the depth of
// the cope at each end. Reporting the node-to-node centreline instead is 8-9
// mm long per coped end on a Ø19 brace into a Ø25.4 run, which is a number
// that arrives as forty tubes that do not fit.
//
// Measured rather than derived: every vertex of the body is projected onto the
// path and its ARC LENGTH from the start taken, so the answer is right for a
// bent member as well as a straight one, and right for whatever shape the cope
// happens to be.
namespace tube {

// The arc-length span [from, to] of `pathEdge` that `body` still occupies, in
// millimetres from the path's start. nullopt when the path is not a usable
// edge or the body has no vertices to project.
[[nodiscard]] std::optional<std::pair<double, double>>
MetalAlongPath(const Body& body, const CADShape& pathEdge);

// How close two cut lengths have to be to be the same piece: finer than any
// saw is set, and coarser than the noise a boolean leaves behind.
inline constexpr double kSameLength = 0.1; // mm

// One row of the cut list: a member, and the identical ones it stands for.
struct MemberRow {
  std::string name;                // the centreline's name
  std::vector<std::string> bodies; // every body this row orders
  int quantity = 1;
  double cutLength = 0.0;  // the METAL, with any cope taken off
  double pathLength = 0.0; // the node-to-node centreline it was swept along
  double outerDiameter = 0.0;
  double wallThickness = 0.0;
  std::vector<Bend> bends;
  uint64_t curveId = 0;
};

// Every tube member of `doc`, as the cut list and the bend schedule both read
// it — ONE derivation, so the schedule an agent queries and the CSV a shop is
// sent cannot disagree.
//
// A member is a length of tube that EXISTS: this walks the bodies and finds
// each one's path by provenance (back through whatever made it — a Cope
// leaves the member's own Pipe in place, a Pattern names its source), so a
// curve whose tube was consumed is not a member and one a mirror made is.
// Members that are the same PIECE collapse into a single row carrying the
// quantity — same stock, same bends, and a cut length within kSameLength.
// Not exact equality: two mirrored members of a symmetric frame come off a
// boolean microns apart, and four rows of "one each" where a shop wants "four
// of these" is a cut list nobody can work from. The row reports the LONGEST
// of its group, so cutting to it can only leave a member long, never short.
// `onlyCurve` (0 = all) narrows it to one path.
[[nodiscard]] std::vector<MemberRow> Members(const Document& doc,
                                             uint64_t onlyCurve = 0);

// One stick of stock and what was cut from it.
struct Stick {
  std::vector<double> pieces; // cut lengths, in the order they come off
  double used = 0.0;          // including the saw kerf between them
  double offcut = 0.0;        // what is left of the stick
};

// How many lengths of stock a cut list needs, by first-fit decreasing: the
// longest piece finds the first stick it fits, which is the pack a saw
// operator does by hand and close enough to optimal that the difference is
// not worth a solver. `kerf` is taken off between two pieces on one stick —
// forgetting it is how the last piece comes up short.
//
// It is NOT the sheet-nesting workstream: one dimension, exact, no rotation
// and no grain.
[[nodiscard]] std::vector<Stick> PackIntoStock(std::vector<double> pieces,
                                               double stockLength, double kerf);

} // namespace tube
