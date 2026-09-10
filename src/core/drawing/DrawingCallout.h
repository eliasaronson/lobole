#pragma once

#include "core/Units.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

class Document;
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

// "Ø4.5 THRU" / "Ø4.5 DEEP 6" (+" CBORE Ø9 DEEP 3" / " CSK Ø9.5 × 90°"), with
// every length in the document's unit and no suffix — the title block says
// which, once, as it does for every other number on the sheet.
std::string FormatHoleCallout(const HoleFeature& hole,
                              const units::System& units = {});

// The uniform centre-to-centre pitch of a collinear row, or nullopt when the
// centres aren't collinear (within tol) or the gaps aren't uniform. Two
// points are trivially a row; fewer than two never are.
std::optional<double> UniformCollinearPitch(std::span<const glm::dvec2> centers,
                                            double tol = 1e-3);

} // namespace drawing
