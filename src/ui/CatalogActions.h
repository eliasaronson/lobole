#pragma once
#include "core/catalog/Catalog.h"
namespace ui {
struct AppState;
bool InstallCatalog(AppState& state, const catalog::Json& entry);
bool UndoCatalog(AppState& state);
bool RedoCatalog(AppState& state);
void DrawCatalogSection(AppState& state);
} // namespace ui
