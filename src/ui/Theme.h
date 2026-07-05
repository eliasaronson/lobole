#pragma once

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

} // namespace ui
