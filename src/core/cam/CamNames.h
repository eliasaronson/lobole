#pragma once

#include "core/cam/CamJob.h" // cam::OpType

#include <cstdint>
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
// (docs/architecture.md#one-table-per-kind-of-thing).
//
// The generator itself stays a switch in CamStrategies.cpp on purpose: it has
// no `default:`, so -Wswitch fails the build when a new type has no
// generator. That guarantee was notional until src/core/cam/ got
// `-Werror=switch` (CMakeLists.txt): -Werror is off tree-wide, so the warning
// went into a build log while `DeriveOperation` fell through and returned a
// DEFAULT Operation -- top 0, bottom -1, no target -- reporting success. A
// table would trade that compile-time guarantee for a runtime one. What
// belongs here is everything the compiler CANNOT check -- the names, the
// predicate that pairs with the generator, and which fields a type uses.
namespace cam {

// Which of `Operation`'s fields a type actually uses.
//
// `Operation` is ONE struct for every op type and carries no discriminant, so
// "does a drill have tabs?" was answered independently wherever it came up --
// and the answers disagreed. `stepover` alone had four: the panel showed it
// for four types, the serializer wrote it for three, `OpJson` reported it for
// ONE, and the deriver set it for all six, so a pocket's stepover was
// settable and unreadable. This is that question, answered once.
//
// It describes what each generator READS, traced through the helpers. Where
// a type stores and shows a field it never reads, the row says so rather than
// dropping it: narrowing is a behaviour change and belongs with the refusals
// it enables. What DOES change is the panel, which showed tabs on a drill and
// a stepdown on an edge round because those widgets were unconditional.
enum class OpField : uint32_t {
  None = 0,
  Stepdown = 1u << 0,
  StockToLeave = 1u << 1,
  Stepover = 1u << 2,
  Side = 1u << 3,
  Climb = 1u << 4,
  Tabs = 1u << 5,
  CornerRelief = 1u << 6,
  LeadArcs = 1u << 7,
  DrilledBores = 1u << 8,
  Peck = 1u << 9,
  EdgeSize = 1u << 10,
  UseOutline = 1u << 11,
  Finish = 1u << 12,
  // Not a field on Operation: two per-type FACTS that were also written as
  // scattered predicates, in three files between them.
  //
  // FullWidth -- the cut engages the whole width of the cutter, which is what
  // the chip-room check is really asking when it names profile and drill.
  // ClearsInside -- the op clears INSIDE its boundary, so a missing `side`
  // defaults to Inside rather than to a profile's Outside. Both the loader
  // and the RPC had their own copy of that list.
  FullWidth = 1u << 13,
  ClearsInside = 1u << 14,
  // Surfacing (v118). Three names because they are three quantities: across,
  // along, and down. A single "stepover" carrying all three is the defect
  // every surveyed CAM shipped.
  CutAngle = 1u << 15,
  SampleStep = 1u << 16,
  ZStep = 1u << 17,
  // Not a field either: this op puts the END of the cutter on a FLOOR, so a
  // cone ploughs a groove the depth of the pass instead of clearing flat.
  // The derivation refused a V-bit and a drill by naming three op types;
  // this is that list, in the one place op types are described.
  NeedsFlatFloor = 1u << 18,
  // SurfaceRotary's pattern (v123): parallel passes, or one spiral.
  RotaryPattern = 1u << 19,
  // A helical rotary pass's lead (v124).
  Lead = 1u << 20,
  // Rest machining: cut only where a larger cutter could not reach (v125).
  RestFromTool = 1u << 21,
};

constexpr OpField operator|(OpField a, OpField b) {
  return static_cast<OpField>(static_cast<uint32_t>(a) |
                              static_cast<uint32_t>(b));
}

struct OpTypeRow {
  OpType type;
  // The name in a saved job file AND over the RPC surface. They have always
  // been the same string; they are now the same character.
  const char* name;
  // One toolpath per target (a profile follows one wall at a time), or one
  // toolpath for all of them at once (a drill visits every hole).
  bool followsOneTargetAtATime;
  // The fields above that this type reads.
  OpField fields;
};

std::span<const OpTypeRow> OpTypes();

// The name for a type, or NULLPTR for a value with no row. It used to fall
// back to "profile", which is how a type whose row was forgotten would have
// SAVED as a profile and reloaded as one. A caller that cannot handle null
// should not be asking; `OpTypeNameOrThrow` is for the ones that must have a
// string and would rather fail loudly than write a plausible wrong one.
const char* OpTypeName(OpType t);
// The same, refusing rather than returning null. Every serializer and every
// RPC response goes through this.
const char* OpTypeNameOrThrow(OpType t);

// The type for a name, or false if it is not one we know. Callers REFUSE on
// false, naming what is accepted -- they do not substitute a default. A job
// file saying `"type": "pocekt"` used to load as a PROFILE: a different
// toolpath that cuts the wrong thing while the file looks fine.
bool TryOpTypeFromName(const std::string& s, OpType& out);

// "profile, pocket, face, drill, adaptive, edge_round" -- for those refusals.
std::string OpTypeNameList();

// Refuses (throws) for a value with no row rather than answering `false`,
// which silently made a new type behave like a drill.
bool FollowsOneTargetAtATime(OpType t);

// Does this op type use that field? The one answer the panel, `OpJson`,
// `ApplyOpFields` and the feeds checks all read.
bool OpTypeHas(OpType t, OpField f);

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

// A corner relief, by name: none, dogbone, tbone.
struct CornerReliefRow {
  CornerRelief relief;
  const char* name;
};
std::span<const CornerReliefRow> CornerReliefs();
const char* CornerReliefName(CornerRelief r);
bool TryCornerReliefFromName(const std::string& s, CornerRelief& out);
std::string CornerReliefNameList();

// The rotary finish's pattern, refused by name like every enum here.
struct RotaryPatternRow {
  RotaryPattern pattern;
  const char* name;
};
std::span<const RotaryPatternRow> RotaryPatterns();
const char* RotaryPatternName(RotaryPattern p);
bool TryRotaryPatternFromName(const std::string& s, RotaryPattern& out);
std::string RotaryPatternNameList();

} // namespace cam
