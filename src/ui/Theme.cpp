// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/Theme.h"

#include "common/Logger.h"
#include "common/RuntimePaths.h"

#include "IconsDesigner.h"
#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <filesystem>
#include <string>

namespace ui {

namespace {

// Palette: near-black backgrounds with a single blue accent. Raised
// surfaces get progressively lighter; interactive states brighten.
constexpr ImVec4 kBg{0.106f, 0.113f, 0.136f, 1.00f};         // panels
constexpr ImVec4 kBgDark{0.086f, 0.092f, 0.110f, 1.00f};     // title/tab bar
constexpr ImVec4 kSurface{0.161f, 0.173f, 0.208f, 1.00f};    // frames, inputs
constexpr ImVec4 kSurfaceHi{0.216f, 0.231f, 0.278f, 1.00f};  // hovered
constexpr ImVec4 kSurfaceHi2{0.263f, 0.282f, 0.341f, 1.00f}; // active
constexpr ImVec4 kAccent{0.275f, 0.557f, 0.906f, 1.00f};
constexpr ImVec4 kAccentDim{0.275f, 0.557f, 0.906f, 0.55f};
constexpr ImVec4 kText{0.910f, 0.918f, 0.937f, 1.00f};
constexpr ImVec4 kTextDim{0.478f, 0.500f, 0.549f, 1.00f};
constexpr ImVec4 kBorder{1.000f, 1.000f, 1.000f, 0.06f};

void ApplyColors(ImGuiStyle& style) {
  ImVec4* c = style.Colors;

  c[ImGuiCol_Text] = kText;
  c[ImGuiCol_TextDisabled] = kTextDim;
  c[ImGuiCol_WindowBg] = kBg;
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = kBgDark;
  c[ImGuiCol_Border] = kBorder;
  c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_FrameBg] = kSurface;
  c[ImGuiCol_FrameBgHovered] = kSurfaceHi;
  c[ImGuiCol_FrameBgActive] = kSurfaceHi2;

  c[ImGuiCol_TitleBg] = kBgDark;
  c[ImGuiCol_TitleBgActive] = kBgDark;
  c[ImGuiCol_TitleBgCollapsed] = kBgDark;
  c[ImGuiCol_MenuBarBg] = kBgDark;

  // The grab is one step ABOVE the surface ramp, not on it. At kSurface it
  // was the same value as every button, frame and input — so a scrollbar
  // beside a control was invisible, and the part-tabs strip's horizontal one
  // (buttons directly above it) could not be seen at all. The track stays
  // transparent: an opaque one would draw a dark gutter down the edge of
  // every panel for a bar that is usually not needed.
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = kSurfaceHi;
  c[ImGuiCol_ScrollbarGrabHovered] = kSurfaceHi2;
  c[ImGuiCol_ScrollbarGrabActive] = kAccentDim;

  c[ImGuiCol_CheckMark] = kAccent;
  c[ImGuiCol_SliderGrab] = kAccent;
  c[ImGuiCol_SliderGrabActive] = kAccent;

  c[ImGuiCol_Button] = kSurface;
  c[ImGuiCol_ButtonHovered] = kSurfaceHi;
  c[ImGuiCol_ButtonActive] = kSurfaceHi2;

  c[ImGuiCol_Header] = kSurfaceHi; // Selectable selected
  c[ImGuiCol_HeaderHovered] = kSurfaceHi;
  c[ImGuiCol_HeaderActive] = kSurfaceHi2;

  c[ImGuiCol_Separator] = kBorder;
  c[ImGuiCol_SeparatorHovered] = kAccentDim;
  c[ImGuiCol_SeparatorActive] = kAccent;

  c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ResizeGripHovered] = kAccentDim;
  c[ImGuiCol_ResizeGripActive] = kAccent;

  c[ImGuiCol_Tab] = kBgDark;
  c[ImGuiCol_TabHovered] = kSurfaceHi;
  c[ImGuiCol_TabSelected] = kBg;
  c[ImGuiCol_TabSelectedOverline] = kAccent;
  c[ImGuiCol_TabDimmed] = kBgDark;
  c[ImGuiCol_TabDimmedSelected] = kBg;
  c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);

  c[ImGuiCol_DockingPreview] = kAccentDim;
  c[ImGuiCol_DockingEmptyBg] = kBgDark;

  c[ImGuiCol_PlotLines] = kAccent;
  c[ImGuiCol_PlotHistogram] = kAccent;

  c[ImGuiCol_TableHeaderBg] = kBgDark;
  c[ImGuiCol_TableBorderStrong] = kBorder;
  c[ImGuiCol_TableBorderLight] = kBorder;
  c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);

  c[ImGuiCol_TextSelectedBg] = kAccentDim;
  c[ImGuiCol_DragDropTarget] = kAccent;
  c[ImGuiCol_NavCursor] = kAccent;
  c[ImGuiCol_NavWindowingHighlight] = kAccentDim;
  c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.5f);
}

void ApplyMetrics(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(12, 10);
  style.FramePadding = ImVec2(9, 5);
  style.CellPadding = ImVec2(6, 4);
  style.ItemSpacing = ImVec2(8, 6);
  style.ItemInnerSpacing = ImVec2(6, 4);
  style.IndentSpacing = 18;
  style.ScrollbarSize = 12;
  style.GrabMinSize = 10;
  // Floor for windows AND dock nodes (ImGui uses it for node resize limits):
  // panels can't be dragged to a sliver where every label clips (U4).
  style.WindowMinSize = ImVec2(180, 60);

  style.WindowBorderSize = 1;
  style.ChildBorderSize = 1;
  style.PopupBorderSize = 1;
  style.FrameBorderSize = 0;
  style.TabBorderSize = 0;
  style.TabBarBorderSize = 0;

  style.WindowRounding = 6;
  style.ChildRounding = 6;
  style.FrameRounding = 4;
  style.PopupRounding = 6;
  style.ScrollbarRounding = 8;
  style.GrabRounding = 4;
  style.TabRounding = 5;

  style.WindowTitleAlign = ImVec2(0.5f, 0.5f);
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.SeparatorTextBorderSize = 1;
  style.SeparatorTextPadding = ImVec2(16, 4);
  style.DockingSeparatorSize = 2;
}

} // namespace

// The terminal font, or null when it could not be loaded (the panel then
// says so rather than drawing a TUI in a proportional font).
static ImFont* s_MonoFont = nullptr;

// Latin + the box-drawing/block/arrow ranges a TUI actually draws in. The
// full BMP would quadruple the atlas for glyphs nothing here emits.
static const ImWchar kMonoRanges[] = {
    0x0020, 0x00FF, // Latin + Latin-1
    0x2010, 0x203A, // dashes, quotes, bullet, ellipsis
    0x2190, 0x21FF, // arrows
    0x2500, 0x259F, // box drawing + block elements
    0x25A0, 0x25FF, // geometric shapes (spinners)
    0x2713, 0x2718, // check marks and crosses
    0x276C, 0x2771, // the heavy angle brackets Claude Code prompts with
    0,
};

// The UI font's range. ImGui's default stops at U+00FF, so every em-dash and
// ellipsis the app's own prose contains rendered as "?" — visible in a CAM
// screenshot as "raise Safe Z ? or start higher", and equally in the section
// tool's "source missing — this cut is off". The punctuation block is a few
// dozen glyphs; the alternative is policing every user-visible string for
// ASCII, forever.
static const ImWchar kUiRanges[] = {
    0x0020, 0x00FF, // Latin + Latin-1 (degree, superscripts, accents)
    0x2010, 0x203A, // dashes, curly quotes, bullet, ellipsis
    0,
};

ImFont* MonoFont() { return s_MonoFont; }

namespace {
float s_UiScale = 1.0f;
}

float UiScale() { return s_UiScale; }

void ApplyTheme(float uiScale, float framebufferScale) {
  const float fb = framebufferScale > 0.0f ? framebufferScale : 1.0f;
  // What ImGui's own coordinates must be scaled by. On X11 this IS uiScale;
  // on Wayland the backend already multiplies every coordinate by fb, so the
  // style must not do it a second time.
  const float scale = uiScale / fb;
  // Say what the UI is being sized at, and in what. A tester's screenshot
  // cannot answer "did Windows' display scaling reach the app?" -- the first
  // one's icons were reported unreadably small and the only way to tell a
  // display running at 100% (where 14 px icons are simply too small) from a
  // scale that never arrived was to ask him to read a control panel. The log
  // knows, so it should say.
  LOG_INFO("UI scale {:.2f} — text {:.0f} px, icons {:.0f} px (framebuffer "
           "scale {:.2f})",
           uiScale, 16.0f * uiScale, 14.0f * uiScale, fb);
  s_UiScale = scale;
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle(); // reset so repeated calls don't compound scaling
  ApplyColors(style);
  ApplyMetrics(style);
  style.ScaleAllSizes(scale);

  ImGuiIO& io = ImGui::GetIO();
  const std::filesystem::path dir = RuntimePaths::DataDir() / "fonts";
  const std::string fontPath = (dir / "Roboto-Medium.ttf").string();
  if (std::filesystem::exists(fontPath)) {
    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * uiScale, nullptr,
                                 kUiRanges);

    // Merge the icon fonts into the same atlas so ICON_FA_* and ICON_DS_*
    // work inline: Font Awesome, then Lobole's own CAD glyphs
    // (tools/icons/build_icon_font.py) in a range FA leaves empty.
    //
    // Every icon gets the SAME advance, min and max alike, so no glyph can
    // make its button wider than its neighbours (a 15 px floor alone let the
    // 16.8 px ones do it, and the planes toggle grew 2 px on every click).
    //
    // The slot is 14 px: exactly the grid Lobole's glyphs are drawn on,
    // centred in, and snapped to, so they fill it with no shift and their
    // edges stay on pixels. Font Awesome's are then CENTRED in it by ImGui —
    // which needs PixelSnapH off: with it on, the centring offset is
    // truncated, and every FA icon sat ~0.8 px left of its button's middle.
    // Their edges never landed on pixels at this size anyway, so they lose
    // nothing, and are oversampled like text. The wide ones (16.8 px)
    // overhang into the button's padding, centred (docs/ui/README.md#icons).
    static const ImWchar kFaRange[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    static const ImWchar kDsRange[] = {ICON_MIN_DS, ICON_MAX_DS, 0};
    struct IconFont {
      const char* file;
      const ImWchar* range;
      bool onPixelGrid; // drawn on the 14 px grid: keep it snapped
    };
    for (const IconFont& f : {IconFont{"fa-solid-900.ttf", kFaRange, false},
                              IconFont{"designer-icons.ttf", kDsRange, true}}) {
      const std::string iconPath = (dir / f.file).string();
      if (!std::filesystem::exists(iconPath)) {
        LOG_WARN("Icon font not found at {}", iconPath);
        continue;
      }
      ImFontConfig cfg;
      cfg.MergeMode = true;
      cfg.PixelSnapH = f.onPixelGrid;
      cfg.GlyphMinAdvanceX = cfg.GlyphMaxAdvanceX = 14.0f * uiScale;
      // No GlyphOffset. It was +2 px from the first toolbar on, which put
      // every icon 2 px below its button's middle (measured on all six bars).
      // Unshifted, FA's glyph box is centred 5.0 px above the baseline, where
      // the middle of Roboto's capitals is (5.7 px), so an icon inline with
      // text lines up with it too.
      io.Fonts->AddFontFromFileTTF(iconPath.c_str(), 14.0f * uiScale, &cfg,
                                   f.range);
    }

    // The terminal panel's font, as a SEPARATE atlas entry rather than a
    // merge: a terminal needs every glyph at one advance width, and merging
    // it into a proportional font would give it Roboto's metrics
    // (docs/ui/terminal.md). Loaded even when the panel is off — it is
    // 260 KB and the alternative is rebuilding the atlas on a preference.
    const std::string monoPath = (dir / "JetBrainsMono-Regular.ttf").string();
    if (std::filesystem::exists(monoPath)) {
      s_MonoFont = io.Fonts->AddFontFromFileTTF(
          monoPath.c_str(), 14.0f * uiScale, nullptr, kMonoRanges);
    } else {
      s_MonoFont = nullptr;
      LOG_WARN("Monospace font not found at {}", monoPath);
    }
  } else {
    LOG_WARN("Font not found at {}, using ImGui default", fontPath);
  }
  // The atlas was rasterized at uiScale (physical pixels); ImGui lays out in
  // logical ones. Dividing the whole font back by the framebuffer scale makes
  // a glyph occupy the space it would have at `scale`, drawn from a texture
  // with fb times the detail. On X11 fb is 1 and this is a no-op.
  io.FontGlobalScale = 1.0f / fb;
}

ImVec4 AccentColor() { return kAccent; }
ImVec4 AccentColorHovered() {
  return ImVec4(kAccent.x * 1.12f, kAccent.y * 1.12f, kAccent.z * 1.12f, 1.0f);
}

ImVec4 ElevatedWindowBorder() { return ImVec4(1.0f, 1.0f, 1.0f, 0.14f); }

void DrawWindowShadow(const ImVec2& min, const ImVec2& max) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  // The window clips its own draw list to its rect, and the shadow is
  // entirely outside it.
  dl->PushClipRectFullScreen();
  const float rounding = ImGui::GetStyle().WindowRounding;
  // Four rings, each a pixel further out and a third as strong. Cheap, and
  // at this size indistinguishable from a blur.
  for (int i = 1; i <= 4; i++) {
    const float o = static_cast<float>(i);
    dl->AddRect(ImVec2(min.x - o, min.y - o), ImVec2(max.x + o, max.y + o),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.20f / o)),
                rounding + o, 0, 1.0f);
  }
  dl->PopClipRect();
}

ClearColor ViewportClearColor() {
  // Slightly lighter than the panels so the 3D canvas reads as its own
  // surface
  return {0.137f, 0.149f, 0.180f};
}

} // namespace ui
