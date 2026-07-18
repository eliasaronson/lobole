#include "ui/Theme.h"

#include "Logger.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

#include <filesystem>
#include <limits.h>
#include <string>
#include <unistd.h>

namespace ui {

namespace {

std::string ExecutableDirectory() {
  char result[PATH_MAX];
  ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
  if (count != -1) {
    result[count] = '\0';
    return std::filesystem::path(result).parent_path().string();
  }
  return ".";
}

// Palette: near-black backgrounds with a single blue accent. Raised
// surfaces get progressively lighter; interactive states brighten.
constexpr ImVec4 kBg{0.106f, 0.113f, 0.136f, 1.00f};        // panels
constexpr ImVec4 kBgDark{0.086f, 0.092f, 0.110f, 1.00f};    // title/tab bar
constexpr ImVec4 kSurface{0.161f, 0.173f, 0.208f, 1.00f};   // frames, inputs
constexpr ImVec4 kSurfaceHi{0.216f, 0.231f, 0.278f, 1.00f}; // hovered
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

  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = kSurface;
  c[ImGuiCol_ScrollbarGrabHovered] = kSurfaceHi;
  c[ImGuiCol_ScrollbarGrabActive] = kSurfaceHi2;

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

void ApplyTheme(float scale) {
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle(); // reset so repeated calls don't compound scaling
  ApplyColors(style);
  ApplyMetrics(style);
  style.ScaleAllSizes(scale);

  ImGuiIO& io = ImGui::GetIO();
  const std::string dir = ExecutableDirectory() + "/assets/fonts/";
  const std::string fontPath = dir + "Roboto-Medium.ttf";
  if (std::filesystem::exists(fontPath)) {
    io.Fonts->Clear();
    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f * scale);

    // Merge Font Awesome icons into the same atlas so ICON_FA_* works inline
    const std::string iconPath = dir + "fa-solid-900.ttf";
    if (std::filesystem::exists(iconPath)) {
      static const ImWchar kIconRange[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
      ImFontConfig cfg;
      cfg.MergeMode = true;
      cfg.PixelSnapH = true;
      cfg.GlyphMinAdvanceX = 15.0f * scale; // fixed-width icon column
      cfg.GlyphOffset = ImVec2(0.0f, 2.0f * scale);
      io.Fonts->AddFontFromFileTTF(iconPath.c_str(), 14.0f * scale, &cfg,
                                   kIconRange);
    } else {
      LOG_WARN("Icon font not found at {}", iconPath);
    }
  } else {
    LOG_WARN("Font not found at {}, using ImGui default", fontPath);
  }
}

ClearColor ViewportClearColor() {
  // Slightly lighter than the panels so the 3D canvas reads as its own
  // surface
  return {0.137f, 0.149f, 0.180f};
}

} // namespace ui
