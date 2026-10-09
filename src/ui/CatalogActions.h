// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once
#include "core/catalog/Catalog.h"
namespace ui {
struct AppState;
bool InstallCatalog(AppState& state, const catalog::Json& entry);
bool UndoCatalog(AppState& state);
bool RedoCatalog(AppState& state);
void DrawCatalogSection(AppState& state);
} // namespace ui
