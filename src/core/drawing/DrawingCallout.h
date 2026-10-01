#pragma once

#include "core/Units.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Document;
struct Tolerance;
struct ThreadMark;
class HoleFeature;

// Callout dimension support (drawings round 2): resolving a projected hole
// circle back to the HoleFeature that drilled it (via the feature ids that
// topological naming bakes into element MappedNames), composing the callout
// text from the feature's parameters, and detecting uniform pitch across a
// row of pattern-callout circles. GUI-free; unit-testable without OCC.
//
// Text is Latin-1-only by design (words, not ISO glyphs): the PDF exporter's
// WinAnsi map and the ImGui font atlas cover Ø/°/× but not ⌴⌵↧. Swapping in
// the drafting symbols later is a change to FormatHoleCallout alone.

namespace drawing {

// Every feature id a MappedName mentions, in encounter order: each comma
// part's leading "<id>/" and every M and G step. It is
// topo::FeatureIdsMentioned (core/ElementOrigin.h), the one grammar reader.
std::vector<uint64_t> MappedNameFeatureIds(const std::string& mappedName);

// The HoleFeature that made the circle this element belongs to, or null.
// Candidates come from MappedNameFeatureIds; each must be a HoleFeature in
// `doc`, and `modelRadius` (the projected circle's radius divided by the
// view scale) must match one of the hole's radii (bore, counterbore, or
// countersink; rel tol 1e-3) — the cross-check that keeps a miss dangling
// instead of mislabeled.
const HoleFeature* FindHoleForRef(const Document& doc,
                                  const std::string& mappedName,
                                  double modelRadius);

// The SIZE tolerance on the cylindrical face that made this circle, or null.
// Same provenance trick as FindHoleForRef — the feature ids in the MappedName
// narrow the candidates and the radius cross-checks them — and the same
// refusal: several matches is no match, because labelling a bore with another
// bore's band is worse than labelling it with none.
//
// Distance tolerances are never returned: a gap between two faces is not the
// size of this circle.
const Tolerance* FindToleranceForRef(const Document& doc,
                                     const std::string& mappedName,
                                     double modelRadius);

// What a toleranced dimension gains on the sheet: " H7" for a class, or
// " +0.05/-0.02" for an explicit band — upper over lower, the order a drawing
// writes them in. Empty for a distance tolerance, which is not a size.
//
// Here rather than at the call site because this file is where the sheet's
// text vocabulary lives, Latin-1 and all. Deviations follow the sheet's own
// rule: the document's unit, no suffix, the title block says which once.
//
// Given the `nominal` size it is on, a class states its limits too -- " K7
// (+0.007/-0.018)" -- because the shop holds the numbers, and a reader with
// no ISO 286 table to hand cannot make a K7 bore from the letters alone.
std::string FormatToleranceSuffix(const Tolerance& tol,
                                  const units::System& units = {},
                                  double nominal = 0.0);

// "Ø4.5 THRU" / "Ø4.5 DEEP 6" (+" CBORE Ø9 DEEP 3" / " CSK Ø9.5 × 90°"), with
// every length in the document's unit and no suffix — the title block says
// which, once, as it does for every other number on the sheet.
//
// A hole that recorded a FIT says what it is for rather than only how wide it
// is: a clearance hole appends its intent ("Ø3.4 THRU, M3 NORMAL CLEARANCE")
// and a TAPPED one leads with the thread instead of the tap drill ("M3 THRU",
// "M3×0.35 DEEP 8"), because the drill is not what gets made. A hole with no
// fit is formatted exactly as it always was.
//
// Given the thread the hole's bores carry (FindThreadForHole), a tapped hole
// says all a machinist needs and nothing he must guess: the designation with
// its class, how deep it is threaded, and the drill under it --
// "3/8-16 UNC-2B DEEP 12.7, DRILL Ø7.94 DEEP 12.7". Without one it reads as
// it always has.
std::string FormatHoleCallout(const HoleFeature& hole,
                              const units::System& units = {},
                              const ThreadMark* thread = nullptr);

// The thread on a bore this hole drilled, the LAST one written (a Thread
// feature on a tapped Hole's bore comes after the Hole and says more: the
// class), or null.
const ThreadMark* FindThreadForHole(const Document& doc,
                                    const HoleFeature& hole);

// The uniform centre-to-centre pitch of a collinear row, or nullopt when the
// centres aren't collinear (within tol) or the gaps aren't uniform. Two
// points are trivially a row; fewer than two never are.
std::optional<double> UniformCollinearPitch(std::span<const glm::dvec2> centers,
                                            double tol = 1e-3);

} // namespace drawing
