#include "core/cam/CamFeeds.h"

#include "core/cam/CamNames.h"

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
  char buf[160];
  std::snprintf(buf, sizeof(buf), "scaling rule, %s at %.3g-%.3g x diameter",
                CutClassName(c), f.min, f.max);
  out.source = buf;
  return out;
}

ChiploadBand ToolBandFor(const Tool& t, CutClass c) {
  // The cutter's own figures win. A vendor's chart for THIS tool in THIS
  // material is evidence; the scaling rule is a stand-in for not having any,
  // and the finding says which one it used so the limit can be argued with.
  if (t.chiploadMin > 0.0 && t.chiploadMax > t.chiploadMin) {
    ChiploadBand out;
    out.min = t.chiploadMin;
    out.max = t.chiploadMax;
    out.known = true;
    out.source = "this cutter's own stated chip load";
    return out;
  }
  return BandFor(c, t.diameter);
}

FeedReport FeedsFor(const Setup& setup, const std::vector<Body>& bodies) {
  FeedReport out;
  // The material of what is being cut. Several bodies may differ; the
  // hardest-to-cut class governs, because that is the one that bites.
  CutClass klass = CutClass::Unknown;
  for (const Body& b : bodies) {
    const CutClass c = CutClassOf(b.material);
    if (c != CutClass::Unknown && static_cast<int>(c) > static_cast<int>(klass))
      klass = c;
  }

  // Only tools that actually cut in this setup.
  std::set<int> used;
  for (const Operation& op : setup.operations)
    if (op.enabled) {
      used.insert(op.toolNumber);
      if (op.finishTool != 0)
        used.insert(op.finishTool);
    }
  if (used.empty())
    return out; // nothing is cut, so there is nothing to say either way

  if (klass == CutClass::Unknown) {
    // NOT silence. An empty answer reads as "checked, all fine", and this is
    // "nobody looked" -- which is how a 4-flute cutter posted a job at a
    // third of the chip load it needed with nothing said about it.
    out.unchecked.push_back(
        "no material on the bodies this setup cuts, so no chip load was "
        "checked against anything. Set one (set_material) and the feeds are "
        "measured against it");
    return out;
  }

  for (const Tool& t : setup.tools) {
    if (used.find(t.number) == used.end())
      continue;
    const std::string who = "T" + std::to_string(t.number) + " (" +
                            Fmt("%.0f", t.diameter) + " mm, " +
                            std::to_string(t.flutes) + " flute)";
    const ChiploadBand band = ToolBandFor(t, klass);
    if (!band.known) {
      out.unchecked.push_back(who +
                              ": no chip-load band for this cutter, so its "
                              "feed and speed were not checked");
      continue;
    }
    // What the tool does not state cannot be multiplied. Say which field.
    std::vector<std::string> missing;
    if (t.flutes <= 0)
      missing.push_back("flutes");
    if (t.spindleRpm <= 0.0)
      missing.push_back("spindle rpm");
    if (t.feedCut <= 0.0)
      missing.push_back("cutting feed");
    if (!missing.empty()) {
      std::string list;
      for (size_t i = 0; i < missing.size(); i++)
        list += (i ? ", " : "") + missing[i];
      out.unchecked.push_back(who + " states no " + list +
                              ", so its chip load could not be worked out");
      continue;
    }

    const double chip =
        t.feedCut / (t.spindleRpm * static_cast<double>(t.flutes));
    const auto finding = [&](const std::string& text) {
      Finding f;
      f.tool = t.number;
      f.measured = chip;
      f.bandMin = band.min;
      f.bandMax = band.max;
      f.source = band.source;
      f.text = text;
      out.findings.push_back(f);
    };

    if (chip < band.min) {
      // NOT "it will not cut": a 4-flute d6 ran three of four operations on a
      // real MDF baffle at 0.033 and cut them cleanly. Under the band costs
      // heat and tool life, and it is the first thing to change when a cut
      // DOES burn -- so this is said, not shouted.
      finding(who + " cuts " + std::string(CutClassName(klass)) + " at " +
              Fmt("%.3f", chip) + " mm/tooth, under " +
              Fmt("%.2f-%.2f", band.min, band.max) + " (" + band.source +
              "). Light machines often run under the band and still cut; the "
              "cost is heat and tool life, and it is the first thing to "
              "change if a cut burns. Raise the feed, or drop the spindle");
      // The one that decides whether the tool is usable here AT ALL: the feed
      // the band needs at this spindle speed, against what the machine has.
      const double need =
          t.spindleRpm * static_cast<double>(t.flutes) * band.min;
      const double have = setup.machine.rapidFeed;
      if (have <= 0.0) {
        out.unchecked.push_back(
            who + ": this setup's machine states no maximum feed, so whether "
                  "it can reach that band was not checked");
      } else if (need > have + 1e-6) {
        const int fewest =
            std::max(1, static_cast<int>(have / (t.spindleRpm * band.min)));
        finding(who + " cannot reach that band on " +
                (setup.machine.name.empty() ? std::string("this machine")
                                            : setup.machine.name) +
                " at all: it would need " + Fmt("%.0f", need) + " mm/min at " +
                Fmt("%.0f", t.spindleRpm) +
                " rpm and the machine tops out at " + Fmt("%.0f", have) +
                ", so no feed here reaches it. That is worth knowing rather "
                "than fixing -- " +
                std::to_string(fewest) +
                " flutes would fit inside the ceiling");
      }
    } else if (chip > band.max) {
      finding(who + " cuts " + std::string(CutClassName(klass)) + " at " +
              Fmt("%.3f", chip) + " mm/tooth, over " +
              Fmt("%.2f-%.2f", band.min, band.max) + " (" + band.source +
              "): that overloads the cutter or the machine");
    }

    // Chip ROOM, which is a different question from chip LOAD. A slot is cut
    // full width, so every chip leaves up the flutes; deep in a chip-heavy
    // material there is nowhere for them to go and the flutes pack, heat and
    // grab. THE one that predicted the real failure: the same cutter, at the
    // same chip load, took the shallow recesses and a d148 opening cleanly
    // and then packed and grabbed in a d100 slot of the same depth.
    if (t.flutes > 2 &&
        (klass == CutClass::Sheet || klass == CutClass::SoftWood ||
         klass == CutClass::HardWood)) {
      double deepest = 0.0;
      for (const Operation& op : setup.operations)
        if (op.enabled && op.toolNumber == t.number &&
            OpTypeHas(op.type, OpField::FullWidth))
          deepest = std::max(deepest, op.topZ - op.bottomZ);
      if (deepest > t.diameter * 2.0) {
        Finding f;
        f.tool = t.number;
        f.source = "chip room, not chip load";
        f.text = who + " cuts a slot " + Fmt("%.0f", deepest) + " mm deep in " +
                 std::string(CutClassName(klass)) + " -- over " +
                 Fmt("%.1f", deepest / t.diameter) +
                 " diameters, full width. This is the cut that bites: more "
                 "than two flutes leaves little room to carry that dust out, "
                 "and packed flutes burn and grab even where shallower cuts "
                 "with the same tool went fine. One or two flutes for a slot "
                 "this deep";
        out.findings.push_back(f);
      }
    }

    // Reach. The one failure here is not a finish problem: past the flutes a
    // cutter is a plain shank, and past the shank is the collet and the
    // spindle nose, so a cut deeper than the cutter is long rubs the work
    // with something that has no edge and then drives the holder into it.
    //
    // EVERY op that cuts at depth, not just the two chip room asks about: a
    // pocket or an adaptive pass buries a short cutter exactly as well as a
    // slot does, and it is the deepest one that decides.
    double reach = 0.0;
    for (const Operation& op : setup.operations)
      if (op.enabled && op.toolNumber == t.number)
        reach = std::max(reach, op.topZ - op.bottomZ);
    if (t.fluteLength > 0.0 && reach > t.fluteLength) {
      Finding f;
      f.tool = t.number;
      f.source = "the cutter's own usable length";
      f.text = who + " cuts " + Fmt("%.1f", reach) + " mm deep and has " +
               Fmt("%.1f", t.fluteLength) + " mm of flute, so the last " +
               Fmt("%.1f", reach - t.fluteLength) +
               " mm is cut by the shank and the holder is in the work. Use a "
               "longer cutter, or take the depth in two setups";
      out.findings.push_back(f);
    }
  }
  return out;
}

} // namespace cam
