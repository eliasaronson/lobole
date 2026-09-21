#pragma once

#include "core/cam/CamJob.h"
#include "core/cam/CamNames.h"

#include <nlohmann/json_fwd.hpp>

#include <span>
#include <string_view>
#include <variant>
#include <vector>

// An operation's fields, as ONE table (docs/cam/op-field-table-plan.md).
//
// A field used to be written out by hand in about ten places -- the save
// file's writer and reader, the RPC's owned list, apply step and answer, its
// describe rows, the surface key -- and the copies drifted without a sound:
// `tabs`, then four surfacing fields, were accepted by the server and refused
// by the generated SDK. The table is what every one of those reads now. The
// same shape the preferences have (core/io/Settings.h), and the same shape
// C++26 reflection would produce from annotated members, so moving to that
// later replaces the rows and nothing that reads them.
//
// Built in every configuration: the save file is part of the document model
// whether or not this build can generate a toolpath.
namespace cam {

using OpMember =
    std::variant<double Operation::*, bool Operation::*, int Operation::*,
                 Side Operation::*, CornerRelief Operation::*,
                 RotaryPattern Operation::*>;

enum OpFieldFlag : unsigned {
  // Named here -- so the owned list, describe and the capability gate see it
  // -- but saved, applied and reported by hand, because it has a rule of its
  // own: `tabs` is three members as one object; `finish_tool` is checked
  // against the setup's tools and `finish_stock` is saved only beside it.
  kOpFieldByHand = 1u << 0,
};

struct OpFieldRow {
  const char* key; // the save file's key AND the RPC's
  OpField field;   // the capability bit; OpField::None = every type has it
  OpMember member;
  unsigned flags = 0;
};

// Every row, in the order refusals list them.
[[nodiscard]] std::span<const OpFieldRow> OpFieldRows();
[[nodiscard]] const OpFieldRow* FindOpFieldRow(std::string_view key);
// Does an op of this type have this row? OpField::None means every type.
[[nodiscard]] bool OpTypeHasRow(OpType type, const OpFieldRow& row);

// One row's value from JSON onto `op`. Throws, naming what is accepted, on
// an enum name this build does not know -- never a quiet default.
void SetOpField(const OpFieldRow& row, const nlohmann::json& value,
                Operation& op);
// One row's value as JSON: a number, a bool, or an enum's name.
[[nodiscard]] nlohmann::json OpFieldJson(const OpFieldRow& row,
                                         const Operation& op);

// The save file: every table row not saved by hand, left out when it equals
// a default-constructed Operation's -- the rule every field already had, so
// a file re-saves byte for byte. The reader takes the keys PRESENT, which is
// the same as reading with defaults, because `op` starts default-built.
void WriteOpFields(const Operation& op, nlohmann::json& out);
void ReadOpFields(const nlohmann::json& in, Operation& op);

// Every table value as a number, in row order: what the surface cache key
// hashes, so a field added to the table is in the key without anyone
// remembering to add it.
[[nodiscard]] std::vector<double> OpFieldValues(const Operation& op);

} // namespace cam
