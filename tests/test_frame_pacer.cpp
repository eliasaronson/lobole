#include <catch2/catch_test_macros.hpp>

#include "app/FramePacer.h"

// The idle-throttle decision (docs/rendering/frame-pacing.md#idle-throttle),
// headless. Each case was checked against sabotage: a pacer that never idles
// fails "goes idle after the cooldown"; one that ignores the cooldown fails the
// tooltip case; one that latches activity forever fails the expiry case.

using app::FramePacer;
using app::PacerInputs;

namespace {
PacerInputs Quiet() {
  PacerInputs in;
  in.enabled = true;
  in.documentVersion = 7; // an open document that is not changing
  return in;
}
} // namespace

TEST_CASE("Pacer goes idle only after the cooldown", "[pacer]") {
  FramePacer p;
  // First call is activity by definition (startup must render).
  CHECK_FALSE(p.IdleNext(Quiet(), 0.0));
  // Inside the cooldown a still frame keeps full rate — this is what lets a
  // tooltip appear on a motionless hover (ImGui's delay is shorter than the
  // cooldown).
  CHECK_FALSE(p.IdleNext(Quiet(), FramePacer::kCooldownSeconds * 0.9));
  // Past it, quiet frames pace idle, and the counter moves.
  CHECK(p.IdleNext(Quiet(), FramePacer::kCooldownSeconds + 0.01));
  CHECK(p.IdleNext(Quiet(), FramePacer::kCooldownSeconds + 0.2));
  CHECK(p.IdleFrames() == 2);
}

TEST_CASE("Every activity signal restores full rate", "[pacer]") {
  const auto wakes = [](void (*set)(PacerInputs&)) {
    FramePacer p;
    (void)p.IdleNext(Quiet(), 0.0);
    REQUIRE(p.IdleNext(Quiet(), 2.0)); // established idle
    PacerInputs in = Quiet();
    set(in);
    CHECK_FALSE(p.IdleNext(in, 2.1)); // the signal wakes it...
    // ...and re-arms the cooldown: still active shortly after.
    CHECK_FALSE(p.IdleNext(Quiet(), 2.2));
  };
  wakes([](PacerInputs& in) { in.inputActivity = true; });
  wakes([](PacerInputs& in) { in.wantTextInput = true; });
  wakes([](PacerInputs& in) { in.cameraAnimating = true; });
  wakes([](PacerInputs& in) { in.jobRunning = true; });
  wakes([](PacerInputs& in) { in.remoteRequests = 1; });
}

TEST_CASE("A document edit is activity exactly once", "[pacer]") {
  FramePacer p;
  (void)p.IdleNext(Quiet(), 0.0);
  REQUIRE(p.IdleNext(Quiet(), 2.0));
  PacerInputs edited = Quiet();
  edited.documentVersion = 8; // an RPC edited the model: repaint
  CHECK_FALSE(p.IdleNext(edited, 2.1));
  // The new version is now the seen one — holding it is not more activity.
  CHECK(p.IdleNext(edited, 4.0));
}

TEST_CASE("Disabled and test-mode pacers never idle, force wins", "[pacer]") {
  FramePacer off;
  PacerInputs in = Quiet();
  in.enabled = false;
  (void)off.IdleNext(in, 0.0);
  CHECK_FALSE(off.IdleNext(in, 10.0));

  FramePacer test;
  in = Quiet();
  in.testMode = true;
  (void)test.IdleNext(in, 0.0);
  CHECK_FALSE(test.IdleNext(in, 10.0));
  // The pacer's own GUI test opts back in under the engine.
  in.forceEnable = true;
  (void)test.IdleNext(in, 10.0);
  CHECK(test.IdleNext(in, 10.0 + FramePacer::kCooldownSeconds + 0.01));
}

TEST_CASE("The wake readout names the most specific cause", "[pacer]") {
  using W = FramePacer::Wake;
  FramePacer p;
  (void)p.IdleNext(Quiet(), 0.0);
  REQUIRE(p.IdleNext(Quiet(), 2.0));

  // Remote outranks input: an agent driving the app usually also LOOKS like
  // input-adjacent activity, and "remote" is the actionable name.
  PacerInputs in = Quiet();
  in.inputActivity = true;
  in.remoteRequests = 1;
  (void)p.IdleNext(in, 2.1);
  CHECK(p.LastWake() == W::Remote);
  // During the cooldown, quiet frames keep naming what STARTED it.
  (void)p.IdleNext(Quiet(), 2.2);
  CHECK(p.LastWake() == W::Remote);
  CHECK(p.LastWakeAge(2.5) > 0.3);
  CHECK(p.LastWakeAge(2.5) < 0.5);

  in = Quiet();
  in.inputActivity = true;
  (void)p.IdleNext(in, 3.0);
  CHECK(p.LastWake() == W::Input);

  FramePacer off;
  in = Quiet();
  in.enabled = false;
  (void)off.IdleNext(in, 0.0);
  CHECK(off.LastWake() == W::Off);
}

TEST_CASE("Re-enabling does not fire a stale version delta", "[pacer]") {
  // The version must keep updating WHILE disabled: an edit made during a
  // disabled stretch is not a wake-up call the moment the setting comes back.
  FramePacer p;
  PacerInputs in = Quiet();
  in.enabled = false;
  (void)p.IdleNext(in, 0.0);
  in.documentVersion = 9; // edited while disabled
  (void)p.IdleNext(in, 0.1);
  in.enabled = true;
  (void)p.IdleNext(in, 0.2); // re-enabled: version 9 is already the seen one
  CHECK(p.IdleNext(in, 0.2 + FramePacer::kCooldownSeconds + 0.01));
}
