// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The check every solid an Import brings in is given where it is made
// (docs/interchange/README.md#an-imported-body-is-checked). A file another
// program wrote can hold a solid that is open, crosses itself or is inside
// out, and the feature after the import inherits it; the body still comes
// in, since the geometry is what the user wanted, and the import's warning
// names it and why.

#include <cstddef>
#include <optional>
#include <string>

class CADShape;

namespace interchange {

// Faces past which a solid is not checked: BRepCheck runs about linearly in
// the faces, and an import of a large assembly must not spend its budget on
// a check the user did not ask for. Such a solid is named as not checked.
inline constexpr size_t kImportCheckFaceLimit = 20000;

// What is wrong with an imported solid, as the rest of a sentence that starts
// with its name: "came in open: 3 free edges", "came in invalid:
// self-intersecting wire", "came in inside out (its volume is negative)",
// "was not checked: 31204 faces, past the 20000 the import checks". Empty
// when it is a valid closed solid of positive volume. `volume` is the
// solid's signed volume when the caller measured it already.
std::string CheckImportedSolid(const CADShape& shape,
                               std::optional<double> volume = std::nullopt);

} // namespace interchange
