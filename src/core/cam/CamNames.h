#pragma once

#include "core/cam/CamJob.h" // cam::OpType

#include <span>
#include <string>

// ONE row per value of every CAM enum that has a NAME -- in a job file and
// over the RPC surface, which have always been the same strings.
//
// Before this, an OpType's properties were spread over six files: the
// generator switch and the `FollowsOneTargetAtATime` predicate in
// CamToolpath.cpp, the serialized name in SerializeCam.cpp, the RPC name in
// RemoteCam.cpp, plus per-type branches in CamPanel.cpp and CamTool.cpp.
// Adding a type meant finding all of them, and the two lists keyed by the
// same enum could disagree without any test noticing -- which is exactly the
// shape of the shipped defect where a `profile` followed only
// targets.front() while its predicate said otherwise
// (docs/architecture-review-2026-09-06.md, section 7).
//
// The generator itself stays a switch in CamStrategies.cpp on purpose: it has
// no `default:`, so -Wswitch already fails the build when a new type has no
// generator. A table would trade that compile-time guarantee for a runtime
// one. What belongs here is everything the compiler CANNOT check -- the
// names, and the predicate that pairs with the generator.
namespace cam {

struct OpTypeRow {
  OpType type;
  // The name in a saved job file AND over the RPC surface. They have always
  // been the same string; they are now the same character.
  const char* name;
  // One toolpath per target (a profile follows one wall at a time), or one
  // toolpath for all of them at once (a drill visits every hole).
  bool followsOneTargetAtATime;
};

std::span<const OpTypeRow> OpTypes();

// The name for a type. Falls back to "profile" for an out-of-range enum,
// which is what both former copies did.
const char* OpTypeName(OpType t);

// The type for a name, or false if it is not one we know. Callers REFUSE on
// false, naming what is accepted -- they do not substitute a default. A job
// file saying `"type": "pocekt"` used to load as a PROFILE: a different
// toolpath that cuts the wrong thing while the file looks fine.
bool TryOpTypeFromName(const std::string& s, OpType& out);

// "profile, pocket, face, drill, adaptive, edge_round" -- for those refusals.
std::string OpTypeNameList();

bool FollowsOneTargetAtATime(OpType t);

// The tool's shape. Refusing here is what RemoteCam's own table already said
// it was for -- "a name we do not know is refused, never silently read as the
// flat mill everything used to assume" -- but only the RPC side did it. The
// LOADER returned ToolType::Flat, so a job file with a typo came back as a
// flat mill: a ball nose's cusp stepover and a V-bit's chamfer both silently
// become a square-shouldered cut.
struct ToolTypeRow {
  ToolType type;
  const char* name;
};
std::span<const ToolTypeRow> ToolTypes();
const char* ToolTypeName(ToolType t);
bool TryToolTypeFromName(const std::string& s, ToolType& out);
std::string ToolTypeNameList();

// Which side of the contour the cutter runs. Silent on BOTH sides until now,
// and this one has already cost a bug once: a pocket whose `side` was not
// serialized came back clearing OUTSIDE its own boundary, where there is
// nothing, and produced no toolpath at all (the v62 fix). That was the field
// going missing; this is the same field arriving misspelt, with the same
// result and no message either way.
struct SideRow {
  Side side;
  const char* name;
};
std::span<const SideRow> Sides();
const char* SideName(Side s);
bool TrySideFromName(const std::string& s, Side& out);
std::string SideNameList();

} // namespace cam
