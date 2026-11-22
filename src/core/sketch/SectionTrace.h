// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Tracing a section (docs/sketching/README.md#tracing-a-section): the pieces
// of a sketch's section, as its last regeneration cut it, added as real
// lines and arcs, coincident where they meet, over the section that stays
// as it is underneath; and how far the current cut stands from each traced
// entity.

#include "core/features/SketchFeature.h"
#include "core/sketch/SectionPieces.h"

#include <string>
#include <vector>

namespace sketch {

struct TraceResult {
  std::vector<uint32_t> ids;    // the traced entities, chain by chain
  std::vector<PieceFit> pieces; // each one's fit, in `ids` order
  size_t coincident = 0;        // the constraints joining them
  double deviation = 0.0;       // the largest point-to-piece distance (mm)
  std::string error;            // why nothing was traced
};

// Traces section `external[extIndex]` of `sk` at `detail` (0 to 1). Refused
// (`error`, the sketch unchanged) for an index that is no section, or a
// section with no cut. The caller solves and commits.
TraceResult TraceSection(SketchFeature& sk, size_t extIndex, double detail);

// One traced entity against the current cut of its section.
struct TracedMiss {
  TracedFrom from;
  // False when no point of the cut lies along it: the cut has left it, or
  // its section has no cut.
  bool cut = false;
  SpanMiss miss;
  double noiseMm = 0.0; // its chain's own scatter
};

// Every traced entity whose entity and section still exist, in `traced`
// order. A cut point counts for the traced entity nearest it whose span it
// falls within, on the chain nearest that entity.
std::vector<TracedMiss> SectionFitReport(const SketchFeature& sk);

} // namespace sketch
