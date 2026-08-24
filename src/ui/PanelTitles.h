#pragma once

#include "IconsFontAwesome6.h"

// The dock panels' titles, in one place.
//
// The tabs are ICON-ONLY, and that is the whole design. Five panels share the
// left dock; at its default width their tabs rendered as `M`, `V…`, `C…`,
// `H…`, `C…` — Configurations and CAM the same string. Leading each title
// with an icon fixed which characters survived truncation but left the `…`
// behind as noise, and the words never had a chance: "Configurations" alone
// is wider than half the panel, at any font size worth using. No arrangement
// of five words fits 218 px, so the words go, exactly as an IDE's activity
// bar drops them.
//
// What replaces them: DrawPanelTabTooltips names the hovered tab (this
// ImGui has no tab tooltip of its own — the truncated `Mo…` showed nothing
// on hover either), and PanelHeading writes the name inside the panel, which
// is a thing these panels never had.
//
// Each title carries a "###id" so the window ID stays the plain name: every
// `//###Model/...` test ref, `focus_panel("Variables")` call and dock-layout
// lookup keeps working when a title's wording or icon changes. That is also
// why they live here rather than as literals at each Begin() — DockLayout
// re-docks strays BY NAME, and a redock that silently stops matching is
// invisible until someone's layout breaks.
namespace ui {

inline constexpr const char* kPanelModel = ICON_FA_SITEMAP "###Model";
inline constexpr const char* kPanelVariables = ICON_FA_SLIDERS "###Variables";
inline constexpr const char* kPanelConfigurations =
    ICON_FA_LAYER_GROUP "###Configurations";
inline constexpr const char* kPanelHistory =
    ICON_FA_CLOCK_ROTATE_LEFT "###History";
inline constexpr const char* kPanelConflicts =
    ICON_FA_CODE_MERGE "###Conflicts";
inline constexpr const char* kPanelCam = ICON_FA_INDUSTRY "###CamPanel";
inline constexpr const char* kPanelTerminal =
    ICON_FA_TERMINAL "###TerminalPanel";

// Title paired with the word it used to show. ONE table, because three
// things need the pairing and none of them may drift from the others: the
// tab tooltip, the heading inside the panel, and `focus_panel`, which
// resolves the name a person says to the window ImGui knows. That last one
// used to strip the leading glyph off the title and compare what was left —
// which is now the empty string.
struct PanelInfo {
  const char* title;
  const char* name;
};
inline constexpr PanelInfo kPanels[] = {
    {kPanelModel, "Model"},
    {kPanelVariables, "Variables"},
    {kPanelConfigurations, "Configurations"},
    {kPanelHistory, "History"},
    {kPanelConflicts, "Conflicts"},
    {kPanelCam, "CAM"},
    {kPanelTerminal, "Terminal"},
};

} // namespace ui
