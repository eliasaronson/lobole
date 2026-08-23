#pragma once

#include "IconsFontAwesome6.h"

// The dock panels' titles, in one place.
//
// Five panels share the left dock, and at its default 210 px their tabs
// rendered as `M`, `V…`, `C…`, `H…`, `C…` — Configurations and CAM the same
// string. Truncation is unavoidable there (five words do not fit at any font
// size worth using), so the fix is to make the characters that SURVIVE
// truncation the distinguishing ones: an icon leads every title. That is the
// same trick the toolbar already uses, and what an IDE's activity bar does.
//
// Each title carries a "###id" so the window ID stays the plain name: every
// `//Model/...` test ref, `focus_panel("Variables")` call and dock-layout
// lookup keeps working when a title's wording or icon changes. That is also
// why they live here rather than as literals at each Begin() — DockLayout
// re-docks strays BY NAME, and a redock that silently stops matching is
// invisible until someone's layout breaks.
namespace ui {

inline constexpr const char* kPanelModel = ICON_FA_SITEMAP " Model###Model";
inline constexpr const char* kPanelVariables =
    ICON_FA_SLIDERS " Variables###Variables";
inline constexpr const char* kPanelConfigurations =
    ICON_FA_LAYER_GROUP " Configurations###Configurations";
inline constexpr const char* kPanelHistory =
    ICON_FA_CLOCK_ROTATE_LEFT " History###History";
inline constexpr const char* kPanelConflicts =
    ICON_FA_CODE_MERGE " Conflicts###Conflicts";
inline constexpr const char* kPanelCam = ICON_FA_INDUSTRY " CAM###CamPanel";
inline constexpr const char* kPanelTerminal =
    ICON_FA_TERMINAL " Terminal###TerminalPanel";

} // namespace ui
