// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What an Import says it recognised in a mesh, as the RPCs answer it
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include <nlohmann/json.hpp>

class ImportFeature;

namespace interchange {

// {representation, min_sides?, bodies: [{body, summary, triangles, planes,
// cylinders, cones, spheres, polygonal, facets_left, demoted?, fell_back?,
// tolerance_mm}]}. With `lists` each round and polygon is listed (radius,
// axis, centre, sides, sagitta); without, only how many -- get_state's row,
// which a part of a hundred holes would otherwise fill. Null when the import
// is not a mesh.
nlohmann::json RecognitionJson(const ImportFeature& imp, bool lists);

} // namespace interchange
