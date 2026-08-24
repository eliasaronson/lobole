#include "ui/PerfHud.h"

#include "render/FrameTimer.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

// Colour the total by how it FEELS, not by a number nobody remembers: 60 fps
// is the target, 30 is where a drag starts to feel like it lags.
ImVec4 TotalColor(double ms) {
  if (ms <= 17.0)
    return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
  if (ms <= 34.0)
    return ImVec4(0.95f, 0.80f, 0.40f, 1.0f);
  return ImVec4(0.95f, 0.45f, 0.40f, 1.0f);
}

} // namespace

void DrawPerfHud(AppState& state) {
  // The chord belongs to `view.perf_hud` in the one command table, marked
  // hidden there — so it still works everywhere, and still appears in no
  // menu, no Preferences row and no shortcuts window
  // (docs/rendering/frame-timing.md).
  if (!state.perfHudOpen)
    return;

  const render::FrameTimer& timer = state.frameTimer;
  const render::FrameTimer::Stats s = timer.Snapshot();

  const ImGuiViewport* vp = ImGui::GetMainViewport();
  const float em = ImGui::GetFontSize();
  ImGui::SetNextWindowPos(
      ImVec2(vp->WorkPos.x + vp->WorkSize.x - em * 0.8f, vp->WorkPos.y + em),
      ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(em * 17.0f, 0.0f), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(0.82f);
  if (!ImGui::Begin("Frame time###PerfHud", &state.perfHudOpen,
                    ImGuiWindowFlags_NoFocusOnAppearing |
                        ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }

  if (s.frames == 0) {
    ImGui::TextUnformatted("no frames measured yet");
    ImGui::End();
    return;
  }

  ImGui::TextColored(TotalColor(s.avgMs), "%.1f fps  %.2f ms", s.fps, s.avgMs);
  // The tail, not just the average: a drag that hitches every twentieth frame
  // has a fine average and an unusable feel.
  ImGui::TextDisabled("p50 %.2f   p95 %.2f   max %.2f", s.p50Ms, s.p95Ms,
                      s.maxMs);

  const std::vector<float> recent = timer.Recent(120);
  if (!recent.empty()) {
    const float peak =
        std::max(20.0f, *std::max_element(recent.begin(), recent.end()));
    ImGui::PlotLines("###PerfHudPlot", recent.data(),
                     static_cast<int>(recent.size()), 0, nullptr, 0.0f, peak,
                     ImVec2(-FLT_MIN, em * 2.5f));
  }

  ImGui::Separator();
  // The question the split exists to answer, answered directly: our CPU work
  // versus waiting on the display. Which of `wait`/`present` blocks is a
  // driver and present-mode detail (measured on an RTX 4060 under Xvfb: it
  // blocks in present, not in acquire), so reading either one alone misleads.
  const double display =
      s.phaseMs[static_cast<size_t>(render::FramePhase::Wait)] +
      s.phaseMs[static_cast<size_t>(render::FramePhase::Present)];
  const double cpu = s.avgMs - display;
  ImGui::Text("cpu %5.2f ms   display %5.2f ms", cpu, display);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("cpu = update + ui + record (our work).\n"
                      "display = wait + present: idling until the display is "
                      "ready.\nWith vsync on, a large display share is "
                      "NORMAL — it only means GPU-bound\nwhen the total "
                      "frame time is above the refresh period too.");

  ImGui::Separator();
  for (size_t p = 0; p < render::kPhaseCount; p++) {
    const auto phase = static_cast<render::FramePhase>(p);
    const double ms = s.phaseMs[p];
    ImGui::Text("%-7s %6.2f ms  %4.0f%%", render::PhaseName(phase), ms,
                s.avgMs > 0.0 ? 100.0 * ms / s.avgMs : 0.0);
    // The two rows that get misread, because either can hold the whole vsync
    // idle depending on the driver.
    if (phase == render::FramePhase::Wait && ImGui::IsItemHovered())
      ImGui::SetTooltip("vkAcquireNextImage + the in-flight fence. Some "
                        "drivers park the vsync\nidle here; others park it "
                        "in present. Read the cpu/display line above.");
    if (phase == render::FramePhase::Present && ImGui::IsItemHovered())
      ImGui::SetTooltip("vkQueuePresentKHR. Usually the vsync idle rather "
                        "than work —\nread the cpu/display line above.");
  }

  ImGui::Separator();
  ImGui::TextDisabled("%zu frames  ·  Ctrl+Shift+F", timer.TotalFrames());
  ImGui::End();
}

} // namespace ui
