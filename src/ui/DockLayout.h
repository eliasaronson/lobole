#pragma once

#include "ui/AppState.h"

namespace ui {

// Submits the workbench dockspace (3D scene visible through the passthru
// central node) and builds the default layout the first time there is no
// saved one. Call after the main menu bar, before any panels.
void BeginDockspace(AppState& state);

// Height of the bottom part-tabs bar. The dockspace reserves this strip so
// docked panels end above the bar instead of running beneath it. Grows by one
// scrollbar while the tab strip overflows, so the scrollbar has somewhere to
// live instead of eating the tabs' own row.
float PartTabsBarHeight();

// Told by DrawPartTabs once per frame, read by PartTabsBarHeight. A frame
// late by construction: BeginDockspace has to reserve the strip before the
// tabs that decide its height have been laid out (the toolbar row measures
// itself the same way, and for the same reason).
void SetPartTabsOverflowing(bool overflowing);
bool PartTabsOverflowing();

// The bar's own vertical window padding, which is tighter than the default —
// PartTabsBarHeight is derived from it, so the two cannot drift.
float PartTabsBarPaddingY();

// How wide the left dock should be in a dockspace `dockWidth` across: a
// share of it, bounded in font units at both ends. One function because the
// default LAYOUT and the launch check that narrows an inherited panel must
// agree — see BeginDockspace.
float DefaultSidePanelWidth(float dockWidth);
inline constexpr float kSidePanelFraction = 0.17f;
// Past this share of the window, a side panel is crowding the viewport out
// rather than expressing a preference, and the launch check puts it back.
inline constexpr float kMaxSidePanelFraction = 0.33f;

// The panel's name, written at the top of its own body. The dock tabs are
// icon-only (PanelTitles.h), so this is where the word lives now — and these
// panels never carried a title of their own before, docked panels having no
// title bar. Call first thing after Begin().
void PanelHeading(const char* panelTitle);

} // namespace ui
