#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Document;
struct Tolerance;
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

// Every feature id a MappedName mentions, in encounter order: leading
// "<id>/" base segments (tool faces/seams: "4/face1;U2", "4/seam0"), and
// ";M<id>" / ";G<id>" modifier suffixes ("2/cap0;G4.0" — G ordinals after
// '.' are stripped). Duplicates preserved; parse failures skipped.
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
// text vocabulary lives, Latin-1 and all.
std::string FormatToleranceSuffix(const Tolerance& tol);

// "Ø4.5 THRU" / "Ø4.5 DEEP 6" (+" CBORE Ø9 DEEP 3" / " CSK Ø9.5 × 90°").
//
// A hole that recorded a FIT says what it is for rather than only how wide it
// is: a clearance hole appends its intent ("Ø3.4 THRU, M3 NORMAL CLEARANCE")
// and a TAPPED one leads with the thread instead of the tap drill ("M3 THRU",
// "M3×0.35 DEEP 8"), because the drill is not what gets made. A hole with no
// fit is formatted exactly as it always was.
std::string FormatHoleCallout(const HoleFeature& hole);

// The uniform centre-to-centre pitch of a collinear row, or nullopt when the
// centres aren't collinear (within tol) or the gaps aren't uniform. Two
// points are trivially a row; fewer than two never are.
std::optional<double> UniformCollinearPitch(std::span<const glm::dvec2> centers,
                                            double tol = 1e-3);

} // namespace drawing
