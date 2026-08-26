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
  // A resting app reads ~10 fps BY DESIGN (docs/rendering/idle-throttle.md);
  // without this line that is indistinguishable from "slow". And when it is
  // NOT resting, the wake name says who keeps it awake — the field's first
  // screenshots read "idle 1%" after seconds of hands-off, and the cause (an
  // agent session's RPCs) was only findable by reproduction.
  if (state.settings.idleThrottle) {
    ImGui::SameLine();
    const int share =
        s.frames > 0 ? static_cast<int>(100 * s.idleFrames / s.frames) : 0;
    if (state.pacerWakeAgeS >= 0.0)
      ImGui::TextDisabled("(idle %d%% · wake %s %.1fs ago)", share,
                          state.pacerWake, state.pacerWakeAgeS);
    else
      ImGui::TextDisabled("(idle %d%%)", share);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Share of the window the idle throttle paced at ~10 fps, and the\n"
          "signal that last held or restored full rate: input, text (caret),\n"
          "camera (tween), job (solver/render), remote (RPC), edit "
          "(document\nchange), off (throttle disabled).");
  }
  // The tail, not just the average: a drag that hitches every twentieth frame
  // has a fine average and an unusable feel.
  ImGui::TextDisabled("p50 %.2f   p95 %.2f   max %.2f", s.p50Ms, s.p95Ms,
                      s.maxMs);
  // WHICH phase held the worst frame — the averages dilute a one-off 50 ms
  // hitch far below visibility, so `max` alone says "something spiked" and
  // nothing else. This is the spike's own breakdown.
  if (s.frames > 0) {
    size_t worstPhase = 0;
    for (size_t p = 1; p < render::kPhaseCount; p++)
      if (s.worstPhaseMs[p] > s.worstPhaseMs[worstPhase])
        worstPhase = p;
    ImGui::TextDisabled(
        "worst %.2f ms · %s %.2f%s", s.maxMs,
        render::PhaseName(static_cast<render::FramePhase>(worstPhase)),
        s.worstPhaseMs[worstPhase], s.worstIdle ? "  (idle-paced)" : "");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "The worst frame's own phase split:\n"
          "update %.2f  ui %.2f  wait %.2f  record %.2f  present %.2f\n"
          "wait/present = display or vsync; the others are our CPU work.\n"
          "Caveat: a swapchain-rebuild frame skips phase marks, so its\n"
          "time lands in the next closed phase and can read oddly.",
          s.worstPhaseMs[0], s.worstPhaseMs[1], s.worstPhaseMs[2],
          s.worstPhaseMs[3], s.worstPhaseMs[4]);
  }

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
  // What the GPU itself spends per pass — the half `display` cannot split.
  // On an unsupported queue say so; silence would read as "no GPU cost".
  if (!state.gpuStats.supported) {
    ImGui::TextDisabled("gpu: no timestamp support on this queue");
  } else if (state.gpuStats.samples > 0) {
    ImGui::Text("gpu shadow %5.2f  ssao %5.2f  main %5.2f  = %5.2f ms",
                state.gpuStats.shadowMs, state.gpuStats.ssaoMs,
                state.gpuStats.mainMs, state.gpuStats.totalMs);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(
          "Timestamp queries around the shadow pass, the SSAO chain\n"
          "(prepass + AO + blur) and the main pass (scene + UI).\n"
          "GPU-bound means this total approaches the frame time.");
  }

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
