// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/PrintSettings.h"

#include <nlohmann/json.hpp>

// The wire and file form of a part's slicer settings (v181,
// core/PrintSettings.h): {walls?, top_layers?, bottom_layers?, infill_pct?,
// infill_pattern?, supports?, brim?, direction?}, only what is stated. The
// document carries {part: {...}, bodies: {"<created_by>": {...}}} as
// `print_settings`, and only when there is one.
namespace DocumentIO {

[[nodiscard]] nlohmann::json PrintSettingsToJson(const printset::Settings& s);
// Every key optional; a key of the wrong type, an unknown pattern or brim
// (naming the accepted ones) or an unknown key is refused, and so is a
// result printset::Validate refuses. `what` names it in a refusal.
[[nodiscard]] printset::Settings PrintSettingsFromJson(const nlohmann::json& j,
                                                       const std::string& what);
// The keys the object form carries, for the refusal of an unknown one.
[[nodiscard]] const char* PrintSettingsKeys();

[[nodiscard]] nlohmann::json
PartPrintSettingsToJson(const printset::PartSettings& s);
[[nodiscard]] printset::PartSettings
PartPrintSettingsFromJson(const nlohmann::json& j);

} // namespace DocumentIO
