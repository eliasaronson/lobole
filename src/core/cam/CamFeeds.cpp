#include "core/cam/CamFeeds.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>

namespace cam {
namespace {

// The band as a FRACTION OF DIAMETER, which is how chip load scales: a 3 mm
// cutter in MDF wants a third of what a 9 mm one does. Per class, {min, max}.
struct Fraction {
  double min;
  double max;
};

Fraction FractionFor(CutClass c) {
  switch (c) {
  case CutClass::Sheet:
    return {0.020, 0.040};
  case CutClass::SoftWood:
    return {0.020, 0.045};
  case CutClass::HardWood:
    return {0.015, 0.035};
  case CutClass::Plastic:
    return {0.015, 0.035};
  case CutClass::Aluminium:
    return {0.008, 0.020};
  case CutClass::Brass:
    return {0.010, 0.025};
  case CutClass::Steel:
    return {0.004, 0.012};
  case CutClass::Unknown:
    break;
  }
  return {0.0, 0.0};
}

std::string Fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
  char buf[320];
  std::snprintf(buf, sizeof(buf), f, a, b, c);
  return buf;
}

} // namespace

CutClass CutClassOf(const std::string& slug) {
  static const std::map<std::string, CutClass> kMap = {
      {"mdf", CutClass::Sheet},
      {"plywood_birch", CutClass::Sheet},
      {"plywood_pine", CutClass::Sheet},
      {"wood_pine", CutClass::SoftWood},
      {"wood_oak", CutClass::HardWood},
      {"wood_walnut", CutClass::HardWood},
      {"plastic_white", CutClass::Plastic},
      {"plastic_black", CutClass::Plastic},
      {"aluminum", CutClass::Aluminium},
      {"aluminium", CutClass::Aluminium},
      {"brass", CutClass::Brass},
      {"copper", CutClass::Brass},
      {"steel", CutClass::Steel},
      {"stainless", CutClass::Steel},
      {"iron", CutClass::Steel},
  };
  const auto it = kMap.find(slug);
  return it == kMap.end() ? CutClass::Unknown : it->second;
}

const char* CutClassName(CutClass c) {
  switch (c) {
  case CutClass::Sheet:
    return "sheet goods";
  case CutClass::SoftWood:
    return "softwood";
  case CutClass::HardWood:
    return "hardwood";
  case CutClass::Plastic:
    return "plastic";
  case CutClass::Aluminium:
    return "aluminium";
  case CutClass::Brass:
    return "brass";
  case CutClass::Steel:
    return "steel";
  case CutClass::Unknown:
    break;
  }
  return "unknown material";
}

ChiploadBand BandFor(CutClass c, double diameter) {
  ChiploadBand out;
  if (c == CutClass::Unknown || diameter <= 0.0)
    return out;
  const Fraction f = FractionFor(c);
  out.min = f.min * diameter;
  out.max = f.max * diameter;
  out.known = true;
  return out;
}

std::vector<std::string> FeedWarnings(const Setup& setup,
                                      const std::vector<Body>& bodies) {
  std::vector<std::string> out;
  // The material of what is being cut. Several bodies may differ; the
  // hardest-to-cut class governs, because that is the one that bites.
  CutClass klass = CutClass::Unknown;
  std::string slug;
  for (const Body& b : bodies) {
    const CutClass c = CutClassOf(b.material);
    if (c != CutClass::Unknown &&
        static_cast<int>(c) > static_cast<int>(klass)) {
      klass = c;
      slug = b.material;
    }
  }
  if (klass == CutClass::Unknown)
    return out; // nobody said what this is; a band from nowhere is worse

  // Only tools that actually cut in this setup.
  std::set<int> used;
  for (const Operation& op : setup.operations)
    if (op.enabled) {
      used.insert(op.toolNumber);
      if (op.finishTool != 0)
        used.insert(op.finishTool);
    }

  for (const Tool& t : setup.tools) {
    if (used.find(t.number) == used.end())
      continue;
    const ChiploadBand band = BandFor(klass, t.diameter);
    if (!band.known || t.flutes <= 0 || t.spindleRpm <= 0.0 || t.feedCut <= 0.0)
      continue;
    const double chip =
        t.feedCut / (t.spindleRpm * static_cast<double>(t.flutes));
    const std::string who = "T" + std::to_string(t.number) + " (" +
                            Fmt("%.0f", t.diameter) + " mm, " +
                            std::to_string(t.flutes) + " flute)";
    if (chip < band.min) {
      // NOT "it will not cut": a 4-flute d6 ran three of four operations on
      // a real MDF baffle at 0.033 and cut them cleanly. Under the band
      // costs heat and tool life, and it is the first thing to change when a
      // cut DOES burn -- so this is said, not shouted.
      out.push_back(
          who + " cuts " + std::string(CutClassName(klass)) + " at " +
          Fmt("%.3f", chip) + " mm/tooth, under the usual " +
          Fmt("%.2f-%.2f", band.min, band.max) +
          " for this diameter. Light machines often run under the band and "
          "still cut; the cost is heat and tool life, and it is the first "
          "thing to change if a cut burns. Raise the feed, or drop the "
          "spindle");
      // The one that decides whether the tool is usable here AT ALL: the feed
      // the band needs at this spindle speed, against what the machine has.
      const double need =
          t.spindleRpm * static_cast<double>(t.flutes) * band.min;
      const double have = setup.machine.rapidFeed;
      if (have > 0.0 && need > have + 1e-6) {
        const int fewest =
            std::max(1, static_cast<int>(have / (t.spindleRpm * band.min)));
        out.push_back(who + " cannot reach that band on " +
                      (setup.machine.name.empty() ? std::string("this machine")
                                                  : setup.machine.name) +
                      " at all: it would need " + Fmt("%.0f", need) +
                      " mm/min at " + Fmt("%.0f", t.spindleRpm) +
                      " rpm and the machine tops out at " + Fmt("%.0f", have) +
                      ", so no feed here reaches it. That is worth "
                      "knowing rather than fixing -- " +
                      std::to_string(fewest) +
                      " flutes would fit inside the ceiling");
      }
    } else if (chip > band.max) {
      out.push_back(who + " cuts " + std::string(CutClassName(klass)) + " at " +
                    Fmt("%.3f", chip) + " mm/tooth, over the usual " +
                    Fmt("%.2f-%.2f", band.min, band.max) +
                    ": that overloads the cutter or the machine");
    }

    // Chip ROOM, which is a different question from chip LOAD. A slot is cut
    // full width, so every chip leaves up the flutes; deep in a chip-heavy
    // material there is nowhere for them to go and the flutes pack, heat and
    // grab. This is what actually took the baffle.
    if (t.flutes > 2 &&
        (klass == CutClass::Sheet || klass == CutClass::SoftWood ||
         klass == CutClass::HardWood)) {
      double deepest = 0.0;
      for (const Operation& op : setup.operations)
        if (op.enabled && op.toolNumber == t.number &&
            (op.type == OpType::Profile || op.type == OpType::Drill))
          deepest = std::max(deepest, op.topZ - op.bottomZ);
      if (deepest > t.diameter * 2.0)
        // THE one that predicted the real failure. The same cutter, at the
        // same chip load, took the shallow recesses and a d148 opening
        // cleanly and then packed and grabbed in a d100 slot of the same
        // depth -- chip ROOM, not chip load, and it is worth shouting.
        out.push_back(who + " cuts a slot " + Fmt("%.0f", deepest) +
                      " mm deep in " + std::string(CutClassName(klass)) +
                      " -- over " + Fmt("%.1f", deepest / t.diameter) +
                      " diameters, full width. This is the cut that bites: "
                      "more than two flutes leaves little room to carry that "
                      "dust out, and packed flutes burn and grab even where "
                      "shallower cuts with the same tool went fine. One or "
                      "two flutes for a slot this deep");
    }
  }
  return out;
}

} // namespace cam
