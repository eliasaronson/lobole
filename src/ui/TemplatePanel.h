// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// The Save as template window (docs/modeling/templates.md#in-the-gui): the
// tree's context menu opens it on the selected features; it names the
// template, lists the variables the features read (each a parameter
// already) and what they stand on, and offers every literal they carry as a
// parameter to tick and name -- the candidates check_literals and
// extract_variables find (tmpl::Suggest).

#include "ui/AppState.h"

#include <cstdint>
#include <vector>

namespace ui {

void OpenSaveTemplate(AppState& state, const std::vector<uint64_t>& featureIds);
void DrawSaveTemplateWindow(AppState& state);

} // namespace ui
