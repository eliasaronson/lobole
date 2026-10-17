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

#include <string>

// --capture-dir: where a demo scene records its clip. Empty (the default, and
// every ordinary suite run) = the scenes run without recording anything
// (docs/ui/testing.md#recording-a-scene).
void SetGuiTestCaptureDir(const std::string& dir);
const std::string& GuiTestCaptureDir();
