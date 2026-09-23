#include "render/FrameTimer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

// The timer takes its timestamps as arguments, so the statistics can be
// checked against a sequence with known percentiles instead of against a HUD
// (docs/rendering/frame-pacing.md#the-frame-time-instrument).
using render::FramePhase;
using render::FrameTimer;

namespace {

// One frame of exactly `ms`, split evenly across the five phases.
void PushFrame(FrameTimer& t, double& clock, double ms) {
  const double s = ms / 1000.0;
  t.BeginFrame(clock);
  clock += s * 0.2;
  t.Mark(FramePhase::Update, clock);
  clock += s * 0.2;
  t.Mark(FramePhase::BuildUI, clock);
  clock += s * 0.2;
  t.Mark(FramePhase::Wait, clock);
  clock += s * 0.2;
  t.Mark(FramePhase::Record, clock);
  clock += s * 0.2;
  t.EndFrame(clock);
}

} // namespace

TEST_CASE("frame timer: percentiles over a known sequence", "[frametimer]") {
  FrameTimer t;
  double clock = 100.0; // an arbitrary epoch: only differences matter
  // Exactly 1..100 ms, interleaved slow/fast so a percentile that secretly
  // reads "the last frame" or "the sequence as given" cannot pass.
  for (int k = 0; k < 100; k++)
    PushFrame(t, clock, k % 2 == 0 ? k / 2 + 1.0 : 100.0 - k / 2);

  const FrameTimer::Stats s = t.Snapshot();
  REQUIRE(s.frames == 100);
  REQUIRE(t.TotalFrames() == 100);
  CHECK_THAT(s.avgMs, Catch::Matchers::WithinRel(50.5, 1e-4)); // mean of 1..100
  CHECK_THAT(s.p50Ms, Catch::Matchers::WithinRel(50.0, 1e-4));
  CHECK_THAT(s.p95Ms, Catch::Matchers::WithinRel(95.0, 1e-4));
  CHECK_THAT(s.maxMs, Catch::Matchers::WithinRel(100.0, 1e-4));
  CHECK_THAT(s.fps, Catch::Matchers::WithinRel(1000.0 / 50.5, 1e-4));

  // The phases account for the WHOLE frame — an unattributed sliver is how
  // "every phase is fast but the frame is slow" happens.
  double phaseSum = 0.0;
  for (size_t p = 0; p < render::kPhaseCount; p++)
    phaseSum += s.phaseMs[p];
  CHECK_THAT(phaseSum, Catch::Matchers::WithinRel(s.avgMs, 1e-3));
  for (size_t p = 0; p < render::kPhaseCount; p++)
    CHECK_THAT(s.phaseMs[p], Catch::Matchers::WithinRel(s.avgMs * 0.2, 1e-3));
}

TEST_CASE("frame timer: the window reads only the newest frames",
          "[frametimer]") {
  FrameTimer t;
  double clock = 0.0;
  for (int k = 0; k < 20; k++)
    PushFrame(t, clock, 10.0);
  for (int k = 0; k < 5; k++)
    PushFrame(t, clock, 40.0);

  // Last five only: the slow tail, not diluted by the twenty fast frames.
  const FrameTimer::Stats recent = t.Snapshot(5);
  REQUIRE(recent.frames == 5);
  CHECK_THAT(recent.avgMs, Catch::Matchers::WithinRel(40.0, 1e-3));
  // All 25: 20*10 + 5*40 = 400 / 25 = 16.
  const FrameTimer::Stats all = t.Snapshot();
  REQUIRE(all.frames == 25);
  CHECK_THAT(all.avgMs, Catch::Matchers::WithinRel(16.0, 1e-3));

  const std::vector<float> recentTotals = t.Recent(3);
  REQUIRE(recentTotals.size() == 3);
  for (float v : recentTotals)
    CHECK_THAT(v, Catch::Matchers::WithinRel(40.0f, 1e-3f));
}

TEST_CASE("frame timer: the ring drops the oldest past capacity",
          "[frametimer]") {
  FrameTimer t;
  double clock = 0.0;
  // Fill the ring with slow frames, then overwrite every one of them.
  for (size_t k = 0; k < FrameTimer::kHistory; k++)
    PushFrame(t, clock, 100.0);
  for (size_t k = 0; k < FrameTimer::kHistory; k++)
    PushFrame(t, clock, 5.0);

  const FrameTimer::Stats s = t.Snapshot();
  CHECK(s.frames == FrameTimer::kHistory);
  CHECK(t.TotalFrames() == FrameTimer::kHistory * 2);
  // Not a trace of the slow half: a stale worst-ever max would make the HUD
  // permanently alarming after one hitch.
  CHECK_THAT(s.avgMs, Catch::Matchers::WithinRel(5.0, 1e-3));
  CHECK_THAT(s.maxMs, Catch::Matchers::WithinRel(5.0, 1e-3));
}

TEST_CASE("frame timer: nothing measured yet is zero, not garbage",
          "[frametimer]") {
  FrameTimer t;
  const FrameTimer::Stats s = t.Snapshot();
  CHECK(s.frames == 0);
  CHECK(s.fps == 0.0);
  CHECK(s.avgMs == 0.0);
  CHECK(s.maxMs == 0.0);
  CHECK(t.Recent(10).empty());

  // Marks outside a frame are ignored rather than fabricating a frame from an
  // uninitialised start time (--headless never calls BeginFrame at all).
  t.Mark(FramePhase::Record, 5.0);
  t.EndFrame(9.0);
  CHECK(t.Snapshot().frames == 0);

  // A single frame is its own p50/p95/max.
  double clock = 0.0;
  PushFrame(t, clock, 12.0);
  const FrameTimer::Stats one = t.Snapshot();
  REQUIRE(one.frames == 1);
  CHECK_THAT(one.p95Ms, Catch::Matchers::WithinRel(12.0, 1e-3));
  CHECK_THAT(one.maxMs, Catch::Matchers::WithinRel(12.0, 1e-3));
}

TEST_CASE("frame timer: the worst frame carries its own phase split",
          "[frametimer]") {
  FrameTimer t;
  double clock = 5.0;
  // 50 even 10 ms frames, then ONE 60 ms frame that is almost entirely UI —
  // a one-off hitch. The window averages dilute it 50:1, so an
  // implementation that returns the averaged split scaled up (rather than
  // THAT frame's own) fails the ui-share check below.
  for (int k = 0; k < 50; k++)
    PushFrame(t, clock, 10.0);
  t.BeginFrame(clock);
  clock += 0.002;
  t.Mark(FramePhase::Update, clock);
  clock += 0.050;
  t.Mark(FramePhase::BuildUI, clock);
  clock += 0.002;
  t.Mark(FramePhase::Wait, clock);
  clock += 0.002;
  t.Mark(FramePhase::Record, clock);
  clock += 0.004;
  t.EndFrame(clock);

  const FrameTimer::Stats s = t.Snapshot();
  CHECK_THAT(s.maxMs, Catch::Matchers::WithinRel(60.0, 1e-3));
  CHECK_THAT(s.worstPhaseMs[static_cast<size_t>(FramePhase::BuildUI)],
             Catch::Matchers::WithinRel(50.0, 1e-3));
  CHECK(!s.worstIdle);
  // Its split accounts for its whole frame, as the averages do for theirs.
  double sum = 0.0;
  for (size_t pIdx = 0; pIdx < render::kPhaseCount; pIdx++)
    sum += s.worstPhaseMs[pIdx];
  CHECK_THAT(sum, Catch::Matchers::WithinRel(s.maxMs, 1e-3));

  // An idle-paced frame that happens to be the worst says so: at the ~10 Hz
  // floor the same work just runs later, which is not a hitch.
  FrameTimer t2;
  double c2 = 0.0;
  t2.BeginFrame(c2);
  t2.SetIdlePaced(true);
  c2 += 0.020;
  t2.EndFrame(c2);
  CHECK(t2.Snapshot().worstIdle);
}
