#pragma once

#include <optional>
#include <string>
#include <vector>

// Which standard is which shape
// (docs/parts/README.md#the-model-a-fastener-is-a-part-placed-as-an-occurrence).
//
// The plan's interface table said the head and the drive are CHOSEN, and that
// is wrong in a way worth writing down: you do not order an ISO 4762 with a
// torx drive, you order an ISO 14579. The standard fixes the head, the recess
// and whether the nominal length is measured over the head or under it — a
// fastener is a catalogue item, not a configuration of one. So a `drive` that
// disagrees with the standard is REFUSED naming the one that standard uses,
// rather than quietly cutting a recess nobody makes.
//
// That is also what keeps this table small. Everything a size varies is in the
// vendored CSV; everything a STANDARD varies is one row here.
namespace parts {

enum class Kind {
  Screw,  // a head and a shank; has a length
  Nut,    // a hex prism and a thread; no length
  Washer, // an annulus; no length
};

enum class Head {
  SocketCap,   // a plain cylinder (ISO 4762, ISO 14579)
  HexHead,     // a hexagonal prism, turned from the outside (ISO 4017)
  Countersunk, // a cone, sunk INTO the part (ISO 10642)
  Button,      // a spherical cap (ISO 7380)
  None,        // a nut or a washer
};

enum class Drive {
  Hex,  // hexagon socket, across flats
  Torx, // hexalobular socket (ISO 10664 profile)
  None, // turned from the outside, or not a screw
};

struct Profile {
  std::string name;        // what a caller asks for: "iso4762"
  std::string stem;        // the vendored file: "iso4762def"
  std::string designation; // what a supplier calls it: "ISO 4762"
  std::string what;        // "socket head cap screw", for a refusal
  Kind kind = Kind::Screw;
  Head head = Head::SocketCap;
  Drive drive = Drive::Hex;
  // Where the head dimensions come from when this standard's own file does
  // not carry them. ISO 14579 is ISO 4762 with a different recess, and its
  // vendored file says so by holding ONLY the recess — the driver size and
  // depth per screw size, no dk and no k. Named here rather than inferred, so
  // the borrowing is auditable: a standard silently reading another one's
  // dimensions is exactly the substitution this codebase refuses.
  std::string headFrom;
  // Whether the nominal length is measured over the head. True for a
  // countersunk screw, and it is not a detail: an M6x20 countersunk is 20 mm
  // OVERALL, so reading it as 20 mm under the head makes every one of them
  // one head-height too long.
  bool lengthOverHead = false;
};

// The profile for a standard, by its name ("iso4762") or by the vendored file
// stem ("iso4762def"). Both resolve, because documents written before this
// table existed store the stem.
[[nodiscard]] const Profile* ProfileFor(const std::string& standard);

// Every standard this build makes, as a refusal names them.
[[nodiscard]] std::string KnownStandards();

[[nodiscard]] const std::vector<Profile>& AllProfiles();

// A few words for what the thing IS, derived from the head and the drive
// rather than written out again: "socket cap, hex key", "hex head, spanner".
//
// Because a catalogue number is not a description. The tool asks one question
// — which standard — and answered it with ISO 4762 / ISO 14579 / ISO 4017 /
// ISO 7380, which says nothing about the only thing the choice is about. The
// long form is `what`; this is the short form a button can carry.
[[nodiscard]] std::string ShortDescription(const Profile& profile);

[[nodiscard]] std::string DriveName(Drive drive);
[[nodiscard]] std::optional<Drive> DriveFromName(const std::string& name);

} // namespace parts
