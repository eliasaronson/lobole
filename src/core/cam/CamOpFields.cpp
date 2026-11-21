// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/cam/CamOpFields.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    {"cutter_comp", OpField::CutterComp, &Operation::cutterComp},
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
    {"rest_from_tool", OpField::RestFromTool, &Operation::restFromTool},
    {"index_a_deg", OpField::Indexed, &Operation::indexA},
    {"width_mm", OpField::EngraveWidth, &Operation::engraveWidth},
    {"depth_mm", OpField::EngraveDepth, &Operation::engraveDepth},
    {"passes", OpField::ThreadPasses, &Operation::threadPasses},
    {"enabled", OpField::None, &Operation::enabled},
};

// Enum names, through the one table per enum in CamNames.
const char* NameOf(Side v) { return SideName(v); }
const char* NameOf(CornerRelief v) { return CornerReliefName(v); }
const char* NameOf(RotaryPattern v) { return RotaryPatternName(v); }
const char* NameOf(CutterComp v) { return CutterCompName(v); }
bool FromName(const std::string& s, CutterComp& out) {
  return TryCutterCompFromName(s, out);
}
std::string NamesOf(CutterComp*) { return CutterCompNameList(); }
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
template <class T>
constexpr bool kIsExpr =
    std::is_same_v<T, ExprValue> || std::is_same_v<T, ExprInt>;

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
        } else if constexpr (kIsExpr<T>) {
          // The value; the text is the `_expr` key's, read beside it.
          op.*member = value.get<decltype(T::value)>();
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
        else if constexpr (kIsExpr<T>)
          return (op.*member).value;
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
    std::visit(
        [&](auto member) {
          using T = std::remove_cvref_t<decltype(op.*member)>;
          if constexpr (kIsExpr<T>)
            WriteExprText(out, row.key, (op.*member).expr);
          if constexpr (std::is_same_v<T, std::vector<double>>)
            if (op.indexAExpr.size() == op.indexA.size())
              WriteExprTexts(out, row.key, op.indexAExpr);
        },
        row.member);
  }
}

void ReadOpFields(const json& in, Operation& op) {
  for (const OpFieldRow& row : kRows) {
    if (row.flags & kOpFieldByHand)
      continue;
    if (in.contains(row.key))
      SetOpField(row, in.at(row.key), op);
    std::visit(
        [&](auto member) {
          using T = std::remove_cvref_t<decltype(op.*member)>;
          if constexpr (kIsExpr<T>)
            (op.*member).expr = ReadExprText(in, row.key);
        },
        row.member);
  }
  // The index stops' texts, one per stop.
  if (in.contains("index_a_deg_expr")) {
    std::vector<std::string> t =
        ReadExprTexts(in, "index_a_deg", op.indexA.size());
    op.indexAExpr = std::move(t);
  }
}

std::vector<double> OpFieldValues(const Operation& op) {
  std::vector<double> out;
  out.reserve(std::size(kRows));
  for (const OpFieldRow& row : kRows)
    std::visit(
        [&](auto member) {
          using T = std::remove_cvref_t<decltype(op.*member)>;
          if constexpr (std::is_same_v<T, std::vector<double>>) {
            // a list: its length, then each value, so [90] and [90, 0]
            // differ
            out.push_back(static_cast<double>((op.*member).size()));
            for (const double v : op.*member)
              out.push_back(v);
          } else {
            out.push_back(static_cast<double>(op.*member));
          }
        },
        row.member);
  return out;
}

void VisitOpExprs(Operation& op, const ExprVisitor& visit) {
  visit(ExprSlot(op.topZ, ExprQuantity::Length, "top_z"));
  visit(ExprSlot(op.bottomZ, ExprQuantity::Length, "bottom_z"));
  for (const OpFieldRow& row : kRows) {
    if (!OpTypeHasRow(op.type, row))
      continue;
    const std::string key = row.key;
    std::visit(
        [&](auto member) {
          using T = std::remove_cvref_t<decltype(op.*member)>;
          if constexpr (std::is_same_v<T, ExprInt>) {
            if (key == "tabs") {
              visit(ExprSlot(op.tabCount, "tabs/count"));
              visit(ExprSlot(op.tabWidth, ExprQuantity::Length, "tabs/width"));
              visit(
                  ExprSlot(op.tabHeight, ExprQuantity::Length, "tabs/height"));
            } else {
              visit(ExprSlot(op.*member, key));
            }
          } else if constexpr (std::is_same_v<T, ExprValue>) {
            // The finish stock is saved only beside a finish tool.
            if (key == "finish_stock" && op.finishTool == 0)
              return;
            const ExprQuantity q = key == "cut_angle_deg" ? ExprQuantity::Angle
                                   : key == "stepover"    ? ExprQuantity::Other
                                                       : ExprQuantity::Length;
            visit(ExprSlot(op.*member, q, key));
          } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            if (op.indexAExpr.size() != op.indexA.size())
              op.indexAExpr.resize(op.indexA.size());
            for (size_t k = 0; k < op.indexA.size(); k++)
              visit(ExprSlot(op.indexAExpr[k], op.indexA[k],
                             ExprQuantity::Angle,
                             key + "/" + std::to_string(k)));
            // None bound is none at all: an op that binds no stop equals
            // one that never had texts.
            if (std::all_of(op.indexAExpr.begin(), op.indexAExpr.end(),
                            [](const std::string& t) { return t.empty(); }))
              op.indexAExpr.clear();
          }
        },
        row.member);
  }
}

} // namespace cam
