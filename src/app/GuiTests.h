// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

struct ImGuiTestEngine;
namespace ui {
struct AppState;
}

// Registers all GUI tests. Takes the app state so tests can both drive the
// UI (through the test engine) and assert on model/camera/selection state.
void RegisterGuiTests(ImGuiTestEngine* engine, ui::AppState* state);
