#include "core/cam/CamNames.h"

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

// The order RemoteCam's table used, so `set_cam_tool` answers an unknown
// shape with exactly the sentence it always has.
namespace {
constexpr ToolTypeRow kToolRows[] = {
    {ToolType::Flat, "flat"},
    {ToolType::Ball, "ball"},
    {ToolType::VBit, "vbit"},
    {ToolType::Drill, "drill"},
};
constexpr SideRow kSideRows[] = {
    {Side::Outside, "outside"},
    {Side::Inside, "inside"},
    {Side::On, "on"},
};
} // namespace

std::span<const ToolTypeRow> ToolTypes() { return kToolRows; }

const char* ToolTypeName(ToolType t) {
  for (const ToolTypeRow& row : kToolRows)
    if (row.type == t)
      return row.name;
  return "flat";
}

bool TryToolTypeFromName(const std::string& s, ToolType& out) {
  for (const ToolTypeRow& row : kToolRows)
    if (s == row.name) {
      out = row.type;
      return true;
    }
  return false;
}

std::string ToolTypeNameList() {
  std::string known;
  for (const ToolTypeRow& row : kToolRows)
    known += (known.empty() ? "" : ", ") + std::string(row.name);
  return known;
}

std::span<const SideRow> Sides() { return kSideRows; }

const char* SideName(Side s) {
  for (const SideRow& row : kSideRows)
    if (row.side == s)
      return row.name;
  return "outside";
}

bool TrySideFromName(const std::string& s, Side& out) {
  for (const SideRow& row : kSideRows)
    if (s == row.name) {
      out = row.side;
      return true;
    }
  return false;
}

std::string SideNameList() {
  std::string known;
  for (const SideRow& row : kSideRows)
    known += (known.empty() ? "" : ", ") + std::string(row.name);
  return known;
}

} // namespace cam
