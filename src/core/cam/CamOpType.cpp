#include "core/cam/CamOpType.h"

namespace cam {

namespace {

// The order is the order every refusal message lists, and it matches the one
// RemoteCam's table used before this file existed -- so `add_cam_op` answers
// an unknown type with exactly the same sentence it always has.
constexpr OpTypeRow kRows[] = {
    {OpType::Profile, "profile", true},
    {OpType::Pocket, "pocket", true},
    {OpType::Face, "face", false},
    {OpType::Drill, "drill", false},
    {OpType::Adaptive, "adaptive", true},
    {OpType::EdgeRound, "edge_round", true},
};

} // namespace

std::span<const OpTypeRow> OpTypes() { return kRows; }

const char* OpTypeName(OpType t) {
  for (const OpTypeRow& row : kRows)
    if (row.type == t)
      return row.name;
  return "profile";
}

bool TryOpTypeFromName(const std::string& s, OpType& out) {
  for (const OpTypeRow& row : kRows)
    if (s == row.name) {
      out = row.type;
      return true;
    }
  return false;
}

std::string OpTypeNameList() {
  std::string known;
  for (const OpTypeRow& row : kRows)
    known += (known.empty() ? "" : ", ") + std::string(row.name);
  return known;
}

bool FollowsOneTargetAtATime(OpType t) {
  for (const OpTypeRow& row : kRows)
    if (row.type == t)
      return row.followsOneTargetAtATime;
  return false;
}

} // namespace cam
