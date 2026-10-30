// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The features a mate connector and the `frame` query may anchor to by id
// (docs/assemblies/README.md, docs/modeling/README.md#datums-on-a-scan): a
// named Frame, and a DatumPlane or DatumAxis -- how a scanned part, which
// has no faces to mate, takes part in an assembly through the datums
// fitted to it.

#include "core/Feature.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

class Document;

namespace datumframe {

// A Frame, a DatumPlane or a DatumAxis.
bool IsAnchor(const Feature& f);

// The anchor's frame in part space, columns X, Y, Z, origin. A Frame's own;
// a DatumPlane's origin, its in-plane X and its normal as Z; a DatumAxis's
// origin and its direction as Z, X being the world's X laid square to it
// (the world's Y when the axis is near X) -- an axis fixes no turn about
// itself, so a mate that needs one (rigid, planar) takes the world's.
glm::dmat4 Matrix(const Feature& f);

// "frame", "datum plane" or "datum axis", for a reply's `source`.
const char* KindLabel(const Feature& f);

// The anchor of `part` by id (a number) or name (a string), or null with
// `listing` naming every anchor there: "12 (Frame1, frame), 14 (Seat,
// datum plane)", empty when there is none.
const Feature* Find(const Document& part, const nlohmann::json& idOrName,
                    std::string& listing);

} // namespace datumframe
