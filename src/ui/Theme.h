#pragma once

struct ImFont;

namespace ui {

// Applies the application theme: colors, metrics, and font, scaled for the
// display (pass the window content scale; 1.0 on standard-DPI screens).
// Call once after ImGui::CreateContext(), before the first frame.
void ApplyTheme(float scale);

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
