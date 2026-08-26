#include "app/FramePacer.h"

namespace app {

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
    return false;
  }

  const bool active = in.inputActivity || in.wantTextInput ||
                      in.cameraAnimating || in.jobRunning ||
                      in.remoteRequests > 0 || versionChanged;
  if (active || m_lastActivity < 0.0)
    m_lastActivity = now;

  const bool idle = now - m_lastActivity >= kCooldownSeconds;
  if (idle)
    m_idleFrames++;
  return idle;
}

} // namespace app
