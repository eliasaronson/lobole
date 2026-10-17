// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The size bands a part's round faces carry (FitBand, core/Feature.h), for
// the contact checks' press fits (docs/tolerances/README.md#press-fits).
// Read from what the document already stores — its size tolerances, its
// print process, a generated bearing's designation — and stamped on its
// bodies, so every copy of a body that reaches a contact check carries them,
// the way a RingGrip rides on a ring.

#include "core/PrintAllowance.h"

#include <optional>
#include <string>
#include <vector>

class Document;
struct ElementRef;
class TopoDS_Face;
struct Body;

namespace fitband {

// Replaces every body's `fitBands` with what `doc` says of its faces -- and,
// on a Derived copy, the bands its snapshot carries as the source read them,
// under the copy's face names (docs/modeling/derived.md#size-bands-on-a-copy):
// a size
// tolerance on a round face (its class's band at the size the face is MADE
// at — the printed size on a printed part — or its explicit deviations), and
// a generated bearing's bore and outside with their ISO 492 normal bands. A
// band the tables cannot give (a class refused at that size, a process the
// table does not carry) is left off: list_tolerances and check_fits say why.
void Stamp(const Document& doc, std::vector<Body>& bodies);

// Whether a cylindrical face is a BORE (its material outside the cylinder)
// rather than a shaft, from its outward normal. nullopt for a face that is
// not a cylinder.
[[nodiscard]] std::optional<bool> IsBore(const TopoDS_Face& face);

// A printed press's band at the face `face` of `doc`
// (docs/tolerances/README.md#printed-press-fits): the face's modelled
// diameter and side read off the geometry, the band off the part's print
// profile (printfit::PrintedPress). Refused (nullopt, `why` set) on a part
// with no print process, a face that does not resolve or is not a cylinder,
// and whatever PrintedPress refuses. set_tolerance asks it before storing
// one, so a press that cannot be banded is never set.
struct PrintedBand {
  printfit::PressBand band;
  bool bore = false;
  double modelledMm = 0.0;
};
[[nodiscard]] std::optional<PrintedBand>
PrintedPressBand(const Document& doc, const ElementRef& face, std::string* why);

} // namespace fitband
