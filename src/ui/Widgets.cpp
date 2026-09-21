#include "ui/Widgets.h"

#include "IconsFontAwesome6.h"
#include <imgui.h>

namespace ui {

bool DrawJobProgress(const JobProgress& job) {
  if (!job.status.empty())
    ImGui::TextUnformatted(job.status.c_str());
  if (job.fraction >= 0.0) {
    ImGui::ProgressBar(static_cast<float>(job.fraction), ImVec2(-FLT_MIN, 0),
                       job.overlay.empty() ? nullptr : job.overlay.c_str());
  } else {
    // Indeterminate. A bar parked at 0% because the work has no fraction to
    // report reads as stuck rather than as working, and every one of the
    // five hand-rolled versions of this had learned that separately.
    ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()),
                       ImVec2(-FLT_MIN, 0),
                       job.overlay.empty() ? "working" : job.overlay.c_str());
  }
  if (!job.cancellable)
    return false;
  ImGui::BeginDisabled(job.cancelAsked);
  const std::string label = std::string(ICON_FA_STOP " ") +
                            (job.cancelAsked ? "Stopping" : "Cancel") +
                            job.cancelId;
  const bool pressed = ImGui::Button(label.c_str());
  ImGui::EndDisabled();
  if (!job.cancelTip.empty())
    ImGui::SetItemTooltip("%s", job.cancelTip.c_str());
  return pressed;
}

} // namespace ui
