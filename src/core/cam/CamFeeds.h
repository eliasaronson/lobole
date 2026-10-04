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
  // A material in the catalogue that a router does not cut -- concrete,
  // glass, brick: no chip-load band, and the feed report says WHY rather
  // than "no material".
  NotCut,
};

[[nodiscard]] CutClass CutClassOf(const std::string& materialSlug);
[[nodiscard]] const char* CutClassName(CutClass c);

// Chip load per tooth, in mm, for a cutter of `diameter`. `known` false for
// a material nobody stated -- a band from nowhere is worse than none, and
// `FeedReport::unchecked` says so out loud rather than going quiet.
//
// `source` is where the numbers came from, and it travels with every finding:
// a limit nobody can trace is a limit nobody can argue with.
struct ChiploadBand {
  double min = 0.0;
  double max = 0.0;
  bool known = false;
  std::string source;
};
// The default band: a SCALING RULE, a fraction of the cutter's diameter per
// material class. `ToolBandFor` prefers the cutter's own stated figures when
// it has them -- a vendor's chart for that exact tool beats any rule.
[[nodiscard]] ChiploadBand BandFor(CutClass c, double diameter);
[[nodiscard]] ChiploadBand ToolBandFor(const Tool& t, CutClass c);

// One thing worth saying about a cutter, with the arithmetic that says it.
// The numbers are carried beside the sentence so a caller can compare them
// instead of parsing prose, and so the limit can be argued with.
struct Finding {
  int tool = 0;
  double measured = 0.0; // mm/tooth this setup actually runs at, 0 if n/a
  double bandMin = 0.0;
  double bandMax = 0.0;
  std::string source; // where the band came from
  std::string text;   // the sentence, citation inline
};

// What can be said about this setup's feeds, speeds and cutters -- and what
// CANNOT, which is the half that used to be silence. An empty `findings` with
// an empty `unchecked` means checked and fine; an empty `findings` with
// something in `unchecked` means nobody looked, and those are not the same
// answer (text-to-cad's dfam-check grades the second "need more info", and
// it is right to).
struct FeedReport {
  std::vector<Finding> findings;
  std::vector<std::string> unchecked;
  [[nodiscard]] bool Empty() const {
    return findings.empty() && unchecked.empty();
  }
};

[[nodiscard]] FeedReport FeedsFor(const Setup& setup,
                                  const std::vector<Body>& bodies);

// Can this tool reach this operation's depth (docs/cam/setup.md#reach)?
// A fact about the op's depth and the tool alone, so it is asked when the op
// is MADE or edited, and when its tool is -- not first when the file is
// posted. `Collides` is refused by every edit that would create it: a shank
// wider than the cut, driven below the flutes into a wall, is a broken tool.
// The rest are said and allowed.
struct Reach {
  enum class Level {
    Fine,
    Tight,    // within reach, with less to spare than the collet nut needs
    Rubs,     // past the flutes on a shank no wider than the cut
    Collides, // past the flutes on a shank WIDER than the cut
  };
  Level level = Level::Fine;
  double depth = 0.0; // mm the op cuts, top to bottom
  std::string text;   // empty when Fine
};
[[nodiscard]] Reach ReachCheck(const Operation& op, const Tool& t);
// ReachCheck for every tool an op cuts with -- its own and its finishing
// tool -- in the setup's current tools, worded with the op's name. Nothing
// for a disabled op, which cuts nothing and is asked again when enabled.
// What the RPCs and the panel both ask, so they cannot disagree about
// which edit is refused.
[[nodiscard]] std::vector<Reach> ReachChecks(const Setup& setup,
                                             const Operation& op);

} // namespace cam
