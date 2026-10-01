#include "core/parts/FastenerProfile.h"

#include <algorithm>

namespace parts {

namespace {

// One row per standard this build makes. Adding the next one is this row, its
// vendored CSV, and — only if its head is a shape none of these are — a
// builder in FastenerFeature.cpp.
const std::vector<Profile>& Table() {
  static const std::vector<Profile> profiles = {
      {"iso4762", "iso4762def", "ISO 4762", "socket head cap screw",
       Kind::Screw, Head::SocketCap, Drive::Hex, "", false},
      {"iso14579", "iso14579def", "ISO 14579",
       "hexalobular socket head cap screw", Kind::Screw, Head::SocketCap,
       Drive::Torx, "iso4762def", false},
      {"iso4017", "iso4017head", "ISO 4017", "hex head screw", Kind::Screw,
       Head::HexHead, Drive::None, "", false},
      {"iso10642", "iso10642def", "ISO 10642", "countersunk socket head screw",
       Kind::Screw, Head::Countersunk, Drive::Hex, "", true},
      {"iso7380", "iso7380def", "ISO 7380", "button head socket screw",
       Kind::Screw, Head::Button, Drive::Hex, "", false},
      // Typed from the standard rather than vendored: no permissively
      // licensed inch table carried B18.3's heads (assets/parts/
      // asmeb18_3def.csv says which table and which columns).
      {"asme_b18_3", "asmeb18_3def", "ASME B18.3",
       "socket head cap screw, inch (UNC)", Kind::Screw, Head::SocketCap,
       Drive::Hex, "", false, true},
      {"iso4032", "iso4032def", "ISO 4032", "hex nut", Kind::Nut, Head::None,
       Drive::None, "", false},
      {"iso7089", "iso7089def", "ISO 7089", "plain washer", Kind::Washer,
       Head::None, Drive::None, "", false},
  };
  return profiles;
}

} // namespace

const std::vector<Profile>& AllProfiles() { return Table(); }

const Profile* ProfileFor(const std::string& standard) {
  for (const Profile& p : Table())
    if (p.name == standard || p.stem == standard)
      return &p;
  return nullptr;
}

std::string KnownStandards() {
  std::string out;
  for (const Profile& p : Table()) {
    if (!out.empty())
      out += ", ";
    out += p.name + " (" + p.what + ")";
  }
  return out;
}

std::string ShortDescription(const Profile& profile) {
  const char* head = "";
  switch (profile.head) {
  case Head::SocketCap:
    head = "socket cap";
    break;
  case Head::HexHead:
    head = "hex head";
    break;
  case Head::Countersunk:
    head = "countersunk";
    break;
  case Head::Button:
    head = "button head";
    break;
  case Head::None:
    return profile.what; // a nut or a washer has no head to describe
  }
  // What you turn it with, which is the other half of the choice — and the
  // half a catalogue number hides completely. "spanner" rather than "none":
  // an external hex is not the absence of a drive.
  const char* turned = profile.drive == Drive::Hex    ? "hex key"
                       : profile.drive == Drive::Torx ? "torx"
                                                      : "spanner";
  return std::string(head) + ", " + turned;
}

std::string DriveName(Drive drive) {
  switch (drive) {
  case Drive::Hex:
    return "hex";
  case Drive::Torx:
    return "torx";
  case Drive::None:
    return "none";
  }
  return "none";
}

std::optional<Drive> DriveFromName(const std::string& name) {
  if (name == "hex")
    return Drive::Hex;
  if (name == "torx")
    return Drive::Torx;
  if (name == "none" || name.empty())
    return Drive::None;
  return std::nullopt;
}

} // namespace parts
