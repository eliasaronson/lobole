#include "core/cam/CamNames.h"

#include <iterator>
#include <stdexcept>

namespace cam {

namespace {

// The order is the order every refusal message lists, and it matches the one
// RemoteCam's table used before this file existed -- so `add_cam_op` answers
// an unknown type with exactly the same sentence it always has.
// The `fields` column is what each generator READS, traced through the
// HELPERS and not guessed from the field comments or from what the panel
// happens to show -- both were already wrong. `cornerRelief` reaches Pocket
// through `WallRelief`, which a grep of GeneratePocket's own body misses;
// `useOutline` reaches only Pocket and Adaptive, because `TargetPaths`
// short-circuits before it when `holesToo` is false and Face never calls it;
// `side` is read by Profile and Adaptive alone ("Adaptive is the one clearing
// strategy that reads `side`"); `stepdown` is read only through
// `DepthPasses`, which Drill and EdgeRound never call; and Drill DOES read
// `stepover`, for the bore it mills instead of drilling.
//
// Two carry-overs are deliberate. Pocket and EdgeRound do not READ `side` --
// a pocket's is forced to Inside and an edge round takes its direction from
// the wall -- but both store, derive and display one, so it stays in their
// row. Removing it is a behaviour change and belongs with the refusals.
constexpr OpTypeRow kRows[] = {
    {OpType::Profile, "profile", true,
     OpField::Stepdown | OpField::StockToLeave | OpField::Climb |
         OpField::Side | OpField::Tabs | OpField::CornerRelief |
         OpField::LeadArcs | OpField::DrilledBores | OpField::Finish |
         OpField::FullWidth},
    {OpType::Pocket, "pocket", true,
     OpField::Stepdown | OpField::StockToLeave | OpField::Climb |
         OpField::Side | OpField::Stepover | OpField::UseOutline |
         OpField::CornerRelief | OpField::Finish | OpField::ClearsInside},
    {OpType::Face, "face", false,
     OpField::Stepdown | OpField::StockToLeave | OpField::Climb |
         OpField::Stepover | OpField::Finish},
    {OpType::Drill, "drill", false,
     OpField::StockToLeave | OpField::Stepover | OpField::Peck |
         OpField::Finish | OpField::FullWidth},
    {OpType::Adaptive, "adaptive", true,
     OpField::Stepdown | OpField::StockToLeave | OpField::Side |
         OpField::Stepover | OpField::UseOutline | OpField::Finish |
         OpField::ClearsInside},
    {OpType::EdgeRound, "edge_round", true,
     OpField::Climb | OpField::Side | OpField::Stepover | OpField::EdgeSize |
         OpField::Finish},
};

// A row per type, IN ENUM ORDER. Both halves matter: without the count a
// type appended to the enum has no row and used to save as "profile"; without
// the order a mis-placed row answers for its neighbour, which is the same
// failure wearing a different hat.
static_assert(std::size(kRows) == static_cast<size_t>(OpType::Count),
              "every OpType needs a row in kRows");
constexpr bool RowsAreInEnumOrder() {
  for (size_t i = 0; i < std::size(kRows); i++)
    if (kRows[i].type != static_cast<OpType>(i))
      return false;
  return true;
}
static_assert(RowsAreInEnumOrder(), "kRows must be in OpType order");

} // namespace

std::span<const OpTypeRow> OpTypes() { return kRows; }

const char* OpTypeName(OpType t) {
  for (const OpTypeRow& row : kRows)
    if (row.type == t)
      return row.name;
  // NOT "profile". A type with no row is a programming error, and answering
  // with another type's name is how it would reach a FILE.
  return nullptr;
}

const char* OpTypeNameOrThrow(OpType t) {
  if (const char* name = OpTypeName(t))
    return name;
  throw std::logic_error("cam::OpType " + std::to_string(static_cast<int>(t)) +
                         " has no row in CamNames.cpp, so it has no name to "
                         "save or report -- add one beside the others");
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
  // Not `false`: that is the drill's answer, and a new type would have
  // quietly generated one toolpath for every target at once.
  throw std::logic_error("cam::OpType " + std::to_string(static_cast<int>(t)) +
                         " has no row in CamNames.cpp");
}

bool OpTypeHas(OpType t, OpField f) {
  for (const OpTypeRow& row : kRows)
    if (row.type == t)
      return (static_cast<uint32_t>(row.fields) & static_cast<uint32_t>(f)) !=
             0;
  throw std::logic_error("cam::OpType " + std::to_string(static_cast<int>(t)) +
                         " has no row in CamNames.cpp");
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
