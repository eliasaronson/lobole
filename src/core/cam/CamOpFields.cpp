#include "core/cam/CamOpFields.h"

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>

namespace cam {

namespace {

using json = nlohmann::json;

// The table. `field` is the capability bit the panel, the RPC's refusal and
// the capability test read; the key is the one the file and the RPC both
// speak, and a length carries its unit (tests/rpc_units_gate.py).
const OpFieldRow kRows[] = {
    {"stepdown", OpField::Stepdown, &Operation::stepdown},
    {"stepover", OpField::Stepover, &Operation::stepover},
    {"stock_to_leave", OpField::StockToLeave, &Operation::stockToLeave},
    {"side", OpField::Side, &Operation::side},
    {"climb", OpField::Climb, &Operation::climb},
    {"tabs", OpField::Tabs, &Operation::tabCount, kOpFieldByHand},
    {"corner_relief", OpField::CornerRelief, &Operation::cornerRelief},
    {"lead_arcs", OpField::LeadArcs, &Operation::leadArcs},
    {"drilled_bores", OpField::DrilledBores, &Operation::drilledBores},
    {"peck", OpField::Peck, &Operation::peck},
    {"edge_size", OpField::EdgeSize, &Operation::edgeSize},
    {"use_outline", OpField::UseOutline, &Operation::useOutline},
    {"finish_tool", OpField::Finish, &Operation::finishTool, kOpFieldByHand},
    {"finish_stock", OpField::Finish, &Operation::finishStock, kOpFieldByHand},
    {"cut_angle_deg", OpField::CutAngle, &Operation::cutAngleDeg},
    {"sample_step_mm", OpField::SampleStep, &Operation::sampleStep},
    {"z_step_mm", OpField::ZStep, &Operation::zStep},
    {"rotary_pattern", OpField::RotaryPattern, &Operation::rotaryPattern},
    {"lead_mm", OpField::Lead, &Operation::leadMm},
    {"enabled", OpField::None, &Operation::enabled},
};

// Enum names, through the one table per enum in CamNames.
const char* NameOf(Side v) { return SideName(v); }
const char* NameOf(CornerRelief v) { return CornerReliefName(v); }
const char* NameOf(RotaryPattern v) { return RotaryPatternName(v); }
bool FromName(const std::string& s, Side& out) {
  return TrySideFromName(s, out);
}
bool FromName(const std::string& s, CornerRelief& out) {
  return TryCornerReliefFromName(s, out);
}
bool FromName(const std::string& s, RotaryPattern& out) {
  return TryRotaryPatternFromName(s, out);
}
std::string NamesOf(Side*) { return SideNameList(); }
std::string NamesOf(CornerRelief*) { return CornerReliefNameList(); }
std::string NamesOf(RotaryPattern*) { return RotaryPatternNameList(); }

template <class T> constexpr bool kIsEnum = std::is_enum_v<T>;

} // namespace

std::span<const OpFieldRow> OpFieldRows() { return kRows; }

const OpFieldRow* FindOpFieldRow(std::string_view key) {
  for (const OpFieldRow& row : kRows)
    if (key == row.key)
      return &row;
  return nullptr;
}

bool OpTypeHasRow(OpType type, const OpFieldRow& row) {
  return row.field == OpField::None || OpTypeHas(type, row.field);
}

void SetOpField(const OpFieldRow& row, const json& value, Operation& op) {
  std::visit(
      [&](auto member) {
        using T = std::remove_reference_t<decltype(op.*member)>;
        if constexpr (kIsEnum<T>) {
          const std::string name = value.get<std::string>();
          T parsed{};
          if (!FromName(name, parsed))
            throw std::runtime_error(std::string("unknown ") + row.key + " '" +
                                     name + "'; this build accepts " +
                                     NamesOf(static_cast<T*>(nullptr)));
          op.*member = parsed;
        } else {
          op.*member = value.get<T>();
        }
      },
      row.member);
}

json OpFieldJson(const OpFieldRow& row, const Operation& op) {
  return std::visit(
      [&](auto member) -> json {
        using T = std::remove_reference_t<decltype(op.*member)>;
        if constexpr (kIsEnum<T>)
          return NameOf(op.*member);
        else
          return op.*member;
      },
      row.member);
}

void WriteOpFields(const Operation& op, json& out) {
  static const Operation kDefault{};
  for (const OpFieldRow& row : kRows) {
    if (row.flags & kOpFieldByHand)
      continue;
    const bool atDefault =
        std::visit([&](auto member) { return op.*member == kDefault.*member; },
                   row.member);
    if (!atDefault)
      out[row.key] = OpFieldJson(row, op);
  }
}

void ReadOpFields(const json& in, Operation& op) {
  for (const OpFieldRow& row : kRows)
    if (!(row.flags & kOpFieldByHand) && in.contains(row.key))
      SetOpField(row, in.at(row.key), op);
}

std::vector<double> OpFieldValues(const Operation& op) {
  std::vector<double> out;
  out.reserve(std::size(kRows));
  for (const OpFieldRow& row : kRows)
    out.push_back(
        std::visit([&](auto member) { return static_cast<double>(op.*member); },
                   row.member));
  return out;
}

} // namespace cam
