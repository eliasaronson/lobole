#pragma once

#include "core/Feature.h" // Body
#include "core/cam/CamJob.h"

#include <string>
#include <vector>

// Does this tool, at these numbers, actually CUT?
//
// Feeds and speeds stay the user's (CamJob.h says why: this app does not know
// the machine's rigidity, and a computed number breaks tools). That is right
// for PRESCRIBING a feed and was quietly extended to CHECKING one -- so a job
// posted at 0.033 mm/tooth in MDF, which is a cutter rubbing rather than
// cutting, and the app had every input it needed to doubt it: diameter,
// flutes, feed, spindle and the body's material. It burned a speaker baffle
// and moved it under the clamps.
//
// Chip load per tooth = feed / (rpm * flutes). Below the band the cutter rubs,
// heats and glazes; above it the tool or the machine is overloaded. The bands
// here are a SCALING RULE (a fraction of the cutter's diameter, per material
// class) rather than a copy of any vendor's chart -- the formula is not
// anybody's property and the ranges are common engineering knowledge, while
// every published table is vendor copyright. They are guidance, they are
// stated as such, and they never override what the user typed.
namespace cam {

// The render/physics slugs are many; a chip-load band cares about few.
enum class CutClass {
  Unknown,
  Sheet, // MDF, particleboard, plywood -- abrasive, chip-heavy
  SoftWood,
  HardWood,
  Plastic,
  Aluminium,
  Brass,
  Steel,
};

[[nodiscard]] CutClass CutClassOf(const std::string& materialSlug);
[[nodiscard]] const char* CutClassName(CutClass c);

// Chip load per tooth, in mm, for a cutter of `diameter`. `known` false for
// a material nobody stated -- silence is better than a band from nowhere.
struct ChiploadBand {
  double min = 0.0;
  double max = 0.0;
  bool known = false;
};
[[nodiscard]] ChiploadBand BandFor(CutClass c, double diameter);

// One line per thing worth saying about this setup's feeds, speeds and
// cutters, in the same words the panel and the RPC both use. Empty when
// everything is in band or no material is stated.
[[nodiscard]] std::vector<std::string>
FeedWarnings(const Setup& setup, const std::vector<Body>& bodies);

} // namespace cam
