// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/FramePacer.h"

namespace app {

const char* FramePacer::WakeName(Wake w) {
  switch (w) {
  case Wake::None:
    return "none";
  case Wake::Input:
    return "input";
  case Wake::Text:
    return "text";
  case Wake::Camera:
    return "camera";
  case Wake::Sim:
    return "sim";
  case Wake::Job:
    return "job";
  case Wake::Remote:
    return "remote";
  case Wake::Edit:
    return "edit";
  case Wake::Off:
    return "off";
  }
  return "?";
}

bool FramePacer::IdleNext(const PacerInputs& in, double now) {
  // A version DELTA is activity; the version itself must keep updating even
  // while disabled, or re-enabling would see a stale number and fire once.
  bool versionChanged = false;
  if (in.documentVersion != 0) {
    versionChanged = m_haveVersion && in.documentVersion != m_lastVersion;
    m_lastVersion = in.documentVersion;
    m_haveVersion = true;
  }

  if (!in.enabled || (in.testMode && !in.forceEnable)) {
    m_lastActivity = now;
    m_lastWake = Wake::Off;
    return false;
  }

  // Ordered so the readout names the most specific cause when several fire:
  // input almost always accompanies the others, so it comes last.
  const Wake wake = in.remoteRequests > 0 ? Wake::Remote
                    : in.jobRunning       ? Wake::Job
                    : versionChanged      ? Wake::Edit
                    : in.simPlaying       ? Wake::Sim
                    : in.cameraAnimating  ? Wake::Camera
                    : in.wantTextInput    ? Wake::Text
                    : in.inputActivity    ? Wake::Input
                                          : Wake::None;
  if (wake != Wake::None || m_lastActivity < 0.0) {
    m_lastActivity = now;
    if (wake != Wake::None)
      m_lastWake = wake;
  }

  const bool idle = now - m_lastActivity >= kCooldownSeconds;
  if (idle)
    m_idleFrames++;
  return idle;
}

} // namespace app
