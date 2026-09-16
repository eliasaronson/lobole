#pragma once

#include "imgui.h" // ImVec4 (the palette accessors below)

struct ImFont;

namespace ui {

// The one accent, for the AFFIRMATIVE button of a pair (Accept, Apply).
//
// Its partner is a plain button, deliberately: Accept/Cancel shipped as a
// green/red pair, and red is this app's ERROR colour — it marks a failed
// feature in the tree. Cancel is not destructive and must not borrow it.
ImVec4 AccentColor();
ImVec4 AccentColorHovered();

// The edge of a window floating ABOVE the panels (the tool session, which
// hangs over the 3D scene), carried at more than the usual 6% white.
//
// NOT paired with a lighter background, which was the first thing tried: one
// step up the surface ramp lands exactly on kSurface, which is what every
// input, frame and button in that window already is — the fields would have
// dissolved into the window behind them. So the elevation is drawn instead,
// as a soft edge (DrawWindowShadow), ImGui having no shadow primitive of its
// own — ImGuiCol_BorderShadow is vestigial and set transparent here.
ImVec4 ElevatedWindowBorder();

// A few nested rounded rects just OUTSIDE the given rect, fading out: the
// standard ImGui stand-in for a drop shadow. Call after Begin(), with the
// window's final rect; it draws into the current window's draw list with the
// clip rect lifted, and stays outside the window's own background so nothing
// is tinted twice.
void DrawWindowShadow(const ImVec2& min, const ImVec2& max);

// Applies the application theme: colors, metrics, and font, scaled for the
// display (pass the window content scale; 1.0 on standard-DPI screens).
// Call once after ImGui::CreateContext(), before the first frame.
void ApplyTheme(float scale);

// The scale ApplyTheme last ran with (content scale x the font-size
// preference). Anything sizing a RAW PIXEL literal must multiply by this:
// ImGui's own metrics go through ScaleAllSizes, so a hand-written 4.0f beside
// them silently shrinks as the display or the preference grows.
float UiScale();

// Viewport (3D canvas) background, matching the theme.
struct ClearColor {
  float r, g, b;
};
ClearColor ViewportClearColor();

// The monospace font the terminal panel draws in, or null when it could not
// be loaded — the panel then says so rather than drawing a TUI in a
// proportional font. Valid until the next ApplyTheme (a font-size change).
ImFont* MonoFont();

} // namespace ui
