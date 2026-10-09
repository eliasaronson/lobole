// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamFeeds.h"

#include "cad/CADMaterials.h"
#include "core/cam/CamNames.h"

#include <algorithm>
#include <cstdio>
#include <limits>
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
  case CutClass::NotCut:
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
      {"wood_maple", CutClass::HardWood},
      {"wood_beech", CutClass::HardWood},
      {"epoxy", CutClass::Plastic},
      {"plastic_white", CutClass::Plastic},
      {"plastic_black", CutClass::Plastic},
      {"petg", CutClass::Plastic},
      {"pla", CutClass::Plastic},
      {"aluminum", CutClass::Aluminium},
      {"aluminium", CutClass::Aluminium},
      {"brass", CutClass::Brass},
      {"copper", CutClass::Brass},
      {"steel", CutClass::Steel},
      {"stainless", CutClass::Steel},
      {"stainless_steel", CutClass::Steel},
      {"iron", CutClass::Steel},
      // In the catalogue for mass, studies and pictures, and not cut on a
      // router: a stone, a ceramic, a building's fabric. No chip-load band
      // exists for them, and the report says so by name rather than
      // reading them as no material at all.
      {"concrete", CutClass::NotCut},
      {"glass", CutClass::NotCut},
      {"ruby", CutClass::NotCut},
      {"brick", CutClass::NotCut},
      {"roof_tile", CutClass::NotCut},
      {"plaster", CutClass::NotCut},
      {"roofing_felt", CutClass::NotCut},
      {"render", CutClass::NotCut},
      {"soil", CutClass::NotCut},
  };
  // A grade cuts as its family: 6082-T6 is aluminium to a chipload table.
  const auto it = kMap.find(CADMaterials::MaterialLibrary::FamilySlug(slug));
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
  case CutClass::NotCut:
    return "not a router material";
  case CutClass::Unknown:
    break;
  }
  return "unknown material";
}

ChiploadBand BandFor(CutClass c, double diameter) {
  ChiploadBand out;
  if (c == CutClass::Unknown || c == CutClass::NotCut || diameter <= 0.0)
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

// How much flute a cut should leave ABOVE it, beyond its own depth.
//
// Derived from the cutter rather than typed, because the thing that has to
// clear the work is the collet nut, and that is several times the cutter
// across -- a 3 mm bit in an ER11 and a 12 mm bit in an ER20 do not want the
// same margin. Half a diameter is the honest small end of that.
double CollarClearance(const Tool& t) {
  // The collet grips the SHANK, so its nut scales with that when it is
  // stated: a 12 mm V-bit on a 6.35 mm shank sits in a quarter-inch collet.
  return std::max(1.0,
                  (t.shankDiameter > 0.0 ? t.shankDiameter : t.diameter) * 0.5);
}

Reach ReachCheck(const Operation& op, const Tool& t) {
  Reach out;
  // A ROTARY pass is skipped, not approximated. Its top and bottom Z are not
  // a depth: it cuts radially while the part turns, so its Z range is the
  // blank's DIAMETER -- and reading that as a plunge told the handle job that
  // a 6 mm ball was cutting 38.3 mm deep with 25 mm of flute, on a pass whose
  // deepest radial cut is about four millimetres. What a rotary pass asks of
  // a cutter's length is how far it must reach past the chuck, a fixturing
  // question this does not have the fixture to answer.
  if (op.type == OpType::SurfaceRotary || t.fluteLength <= 0.0)
    return out;
  out.depth = op.topZ - op.bottomZ;
  const std::string who = "T" + std::to_string(t.number) + " (" +
                          Fmt("%.0f", t.diameter) + " mm, " +
                          std::to_string(t.flutes) + " flute)";
  // A MICRON of overrun, not a nanometre. The depth comes from typed values
  // against a float-derived stock top, so a cut that is exactly the flute
  // length arrives as 25.00000025 and took the overrun branch -- which then
  // said "the last 0.0 mm is cut by the shank", a sentence that reads as
  // nonsense. Same trap as the low-top_z warning
  // (docs/cam/README.md#invariants).
  // A thread mill reaches with its neck as well as its head (ReachLength):
  // its "flute" is the head's own width.
  const double reach = ReachLength(t);
  const double over = out.depth - reach;
  const bool wider = t.shankDiameter > t.diameter + 1e-6;
  // A V-bit is widest at the top of its cone, so with no shank stated the
  // one above it can only be narrower: necked, the nut's height unknown.
  const bool necked = (t.shankDiameter > 0.0 && t.shankDiameter < t.diameter) ||
                      (t.type == ToolType::VBit && t.shankDiameter <= 0.0);
  if (over > 1e-3 && wider) {
    out.level = Reach::Level::Collides;
    out.text =
        who + " cuts " + Fmt("%.1f", out.depth) + " mm deep and has " +
        Fmt("%.1f", reach) + " mm of flute, and above the flutes it " + "is " +
        Fmt("%.1f", t.shankDiameter) +
        " mm across -- wider than the cut it is in. Below " +
        Fmt("%.1f", reach) +
        " mm that shank does not rub the wall, it is driven into it. Use a "
        "cutter with flutes at least " +
        Fmt("%.1f", out.depth) +
        " mm long, a shallower depth, or cut from both sides";
  } else if (over > 1e-3 && !necked) {
    // Past the flutes a cutter is a plain shank, and past the shank is the
    // collet and the spindle nose, so a cut deeper than the cutter is long
    // rubs the work with something that has no edge and then drives the
    // holder into it.
    out.level = Reach::Level::Rubs;
    out.text = who + " cuts " + Fmt("%.1f", out.depth) + " mm deep and has " +
               Fmt("%.1f", reach) + " mm of flute, so the last " +
               Fmt("%.1f", over) +
               " mm is cut by the shank and the holder is in the work. Use a "
               "longer cutter, or take the depth in two setups";
  } else if (const double collet =
                 // Where the nut is: the stated stickout, else the top of the
                 // flutes -- except on a tool narrower above its flutes than
                 // in them (a V-bit's cone, a necked cutter), whose shank goes
                 // on up to a nut nothing here says the height of. There a
                 // 1.5 mm chamfer with a 6 mm cone was "the depth where the
                 // nut touches the work".
             t.overallLength > 0.0 ? t.overallLength
             : necked              ? std::numeric_limits<double>::max()
                                   : reach;
             over <= 1e-3 && out.depth > collet - CollarClearance(t)) {
    // MARGIN, not just overrun. The first version of this check fired only
    // when the cut was DEEPER than the flute, so a 25.0 mm cut with a
    // 25.0 mm cutter passed in silence -- and that is a cut whose collet nut
    // sits exactly on the stock surface with nothing to spare. Stock
    // thickness and Z zero are each good to a few tenths, and they add.
    out.level = Reach::Level::Tight;
    // Named by what the room is measured FROM: with a stickout stated that
    // is the nut, and "25 mm of flute -- only 20 mm to spare" contradicted
    // itself.
    const std::string from = t.overallLength > 0.0
                                 ? "the collet nut " +
                                       Fmt("%.1f", t.overallLength) +
                                       " mm above the tip"
                                 : Fmt("%.1f", reach) + " mm of flute";
    out.text = who + " cuts " + Fmt("%.1f", out.depth) + " mm deep with " +
               from + " -- only " +
               Fmt("%.1f", std::max(0.0, collet - out.depth)) +
               " mm to spare, and the collet needs about " +
               Fmt("%.1f", CollarClearance(t)) +
               " mm of it. Stock thickness and Z zero are each good to a few "
               "tenths and they add, so this is the depth where the nut "
               "touches the work. A longer cutter, or flip the part and cut "
               "from both sides";
  }
  // A necked tool past its flutes: the neck clears the walls by design, and
  // how far it reaches is the holder's question (FoulsHolder), not this one.
  return out;
}

std::vector<Reach> ReachChecks(const Setup& setup, const Operation& op) {
  std::vector<Reach> out;
  if (!op.enabled)
    return out;
  for (const int number : {op.toolNumber, op.finishTool}) {
    const Tool* t = number != 0 ? setup.FindTool(number) : nullptr;
    if (t == nullptr)
      continue;
    Reach r = ReachCheck(op, *t);
    if (r.level == Reach::Level::Fine)
      continue;
    r.text = "operation \"" + op.name + "\": " + r.text;
    out.push_back(std::move(r));
  }
  return out;
}

FeedReport FeedsFor(const Setup& setup, const std::vector<Body>& bodies) {
  FeedReport out;
  // The material of what is being cut. Several bodies may differ; the
  // hardest-to-cut class governs, because that is the one that bites.
  CutClass klass = CutClass::Unknown;
  std::string notCut; // a material on the bodies no router cuts
  for (const Body& b : bodies) {
    const CutClass c = CutClassOf(b.material);
    if (c == CutClass::NotCut) {
      if (notCut.empty())
        notCut = b.material;
      continue;
    }
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

  // REACH FIRST, before the material gate below. A cutter too short for the
  // cut is too short in MDF, in aluminium, and in a setup where nobody has
  // said which -- it is a fact about the tool and the depth, and nothing to
  // do with chip load. This check spent its first day living under that gate
  // and therefore never ran at all in the default case, which is exactly the
  // case a job starts in.
  for (const Tool& t : setup.tools) {
    if (used.find(t.number) == used.end())
      continue;
    // EVERY op that cuts at depth, not just the two chip room asks about: a
    // pocket or an adaptive pass buries a short cutter exactly as well as a
    // slot does, and it is the deepest one that decides.
    const Operation* deepest = nullptr;
    for (const Operation& op : setup.operations) {
      if (!op.enabled ||
          (op.toolNumber != t.number && op.finishTool != t.number) ||
          op.type == OpType::SurfaceRotary)
        continue;
      if (deepest == nullptr ||
          op.topZ - op.bottomZ > deepest->topZ - deepest->bottomZ)
        deepest = &op;
    }
    if (deepest == nullptr)
      continue;
    const Reach r = ReachCheck(*deepest, t);
    if (r.level == Reach::Level::Fine)
      continue;
    Finding f;
    f.tool = t.number;
    f.source = "the cutter's own usable length";
    f.text = r.text;
    out.findings.push_back(f);
  }

  if (klass == CutClass::Unknown && !notCut.empty()) {
    out.unchecked.push_back("the bodies this setup cuts are " + notCut +
                            ", which is not a material a router cuts: no "
                            "chip load was checked against anything");
    return out;
  }
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
                "than fixing -- a " +
                std::to_string(fewest) +
                "-flute cutter would fit inside the ceiling");
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
  }
  return out;
}

} // namespace cam
