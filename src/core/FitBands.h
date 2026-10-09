// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The size bands a part's round faces carry (FitBand, core/Feature.h), for
// the contact checks' press fits (docs/tolerances/README.md#press-fits).
// Read from what the document already stores — its size tolerances, its
// print process, a generated bearing's designation — and stamped on its
// bodies, so every copy of a body that reaches a contact check carries them,
// the way a RingGrip rides on a ring.

#include <optional>
#include <vector>

class Document;
class TopoDS_Face;
struct Body;

namespace fitband {

// Replaces every body's `fitBands` with what `doc` says of its faces: a size
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

} // namespace fitband
