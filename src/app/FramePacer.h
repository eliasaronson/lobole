#pragma once

#include <cstdint>

// Decides whether the next main-loop iteration runs at full rate or waits
// for events with a timeout (docs/rendering/idle-throttle.md). Pure logic —
// main.cpp gathers the Inputs at the end of an iteration and the verdict
// paces the next one — so the decision is testable without a GPU or a frame
// loop. Every iteration that runs still renders exactly as before; the pacer
// only controls WHEN iterations happen.
namespace app {

struct PacerInputs {
  bool enabled = true;      // the idle_throttle preference
  bool testMode = false;    // --gui-tests: the engine drives frames
  bool forceEnable = false; // a pacer test opts back in under testMode
  // Activity this iteration. Input covers every drag (a button is down) and
  // keyboard-driven commands; the rest are the ways the picture changes with
  // no input at all.
  bool inputActivity = false;   // mouse moved/down/wheel, any key down
  bool wantTextInput = false;   // caret blink
  bool cameraAnimating = false; // view tweens
  bool simPlaying = false;      // CAM cutting simulation running
  bool jobRunning = false;      // any registry job, or an offline render
  int remoteRequests = 0;       // executed by this iteration's pump
  uint64_t documentVersion = 0; // active tab's change counter (0 = no tab)
};

class FramePacer {
public:
  // WHY the loop is at full rate — the diagnosis readout. The first HUD
  // screenshots from the field showed "idle 1%" after seconds of stillness,
  // and the cause (an agent session's RPC traffic) took a reproduction to
  // rule in; naming the waker makes that a one-glance read.
  enum class Wake : uint8_t {
    None = 0, // idling, or never woken
    Input,
    Text,
    Camera,
    Sim, // a cutting simulation is playing: the picture changes with no input
    Job,
    Remote,
    Edit,
    Off, // pacer disabled (setting, or test mode without force)
  };
  static const char* WakeName(Wake w);

  // Full-rate window after the last activity: longer than ImGui's tooltip
  // delay, so a still-mouse hover still produces its tooltip and nav fades
  // finish before the rate drops.
  static constexpr double kCooldownSeconds = 1.0;
  // The idle floor (WaitEventsTimeout argument). 10 Hz bounds the staleness
  // of any signal this file fails to list at 100 ms — which is the safety
  // argument for the whole feature.
  static constexpr double kIdleTimeoutSeconds = 0.1;

  // True = wait for events before the next iteration (idle); false = poll
  // at full rate. `now` is the caller's monotonic clock in seconds.
  bool IdleNext(const PacerInputs& in, double now);

  // Iterations that ran idle-paced, ever. Monotonic — tests watch it move.
  uint64_t IdleFrames() const { return m_idleFrames; }

  // The most recent wake signal and how long ago it fired (seconds; negative
  // = never). During the cooldown these say what STARTED it.
  Wake LastWake() const { return m_lastWake; }
  double LastWakeAge(double now) const {
    return m_lastActivity < 0.0 ? -1.0 : now - m_lastActivity;
  }

private:
  double m_lastActivity = -1.0; // <0 = never; first call counts as activity
  Wake m_lastWake = Wake::None;
  uint64_t m_lastVersion = 0;
  bool m_haveVersion = false;
  uint64_t m_idleFrames = 0;
};

} // namespace app
