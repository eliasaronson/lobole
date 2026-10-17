// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "app/GuiTests.h"
#include "app/guitests/GuiTestsCommon.h"

#include "imgui_te_utils.h"   // ImOsPOpen
#include "ui/MotionActions.h" // FfmpegPath

#include <cstdio>
#include <filesystem>
#include <vector>

// Recording (docs/ui/testing.md#recording-a-scene): the engine's video
// capture, fed by main.cpp's CaptureForTestEngine. A demo scene records only
// when the run was given --capture-dir; in the ordinary suite it runs the
// same steps and records nothing, so a UI change that breaks a scene fails
// the suite the day it lands.

namespace {
std::string s_CaptureDir;

// The frames of an encoded clip, decoded back to RGB8 by ffmpeg.
std::vector<std::vector<uint8_t>> DecodeClip(const std::string& path, int w,
                                             int h) {
  std::vector<std::vector<uint8_t>> frames;
  const std::string cmd = "\"" + ui::FfmpegPath() +
                          "\" -nostdin -v error -i \"" + path +
                          "\" -f rawvideo -pix_fmt rgb24 -";
  FILE* pipe = ImOsPOpen(cmd.c_str(), "r");
  if (pipe == nullptr)
    return frames;
  const size_t bytes = size_t(w) * h * 3;
  for (;;) {
    std::vector<uint8_t> f(bytes);
    if (std::fread(f.data(), 1, bytes, pipe) != bytes)
      break;
    frames.push_back(std::move(f));
  }
  ImOsPClose(pipe);
  return frames;
}

double MeanLuma(const std::vector<uint8_t>& rgb) {
  double sum = 0;
  for (size_t i = 0; i + 2 < rgb.size(); i += 3)
    sum += 0.2126 * rgb[i] + 0.7152 * rgb[i + 1] + 0.0722 * rgb[i + 2];
  return sum / double(rgb.size() / 3);
}
} // namespace

void SetGuiTestCaptureDir(const std::string& dir) { s_CaptureDir = dir; }
const std::string& GuiTestCaptureDir() { return s_CaptureDir; }

// Starts recording the whole window to `path` at 60 fps -- one video frame
// per frame of the suite's fixed 1/60 s clock, however long each took.
static bool BeginRecording(ImGuiTestContext* ctx, const std::string& path) {
  ctx->CaptureReset();
  ImGuiCaptureArgs* args = ctx->CaptureArgs;
  std::snprintf(args->InOutputFile, sizeof(args->InOutputFile), "%s",
                path.c_str());
  args->InRecordFPSTarget = 60;
  args->InPadding = 0.0f;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  args->InCaptureRect =
      ImRect(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y));
  // The encoder wants even sides, so the engine rounds the size up to 2 --
  // and then down by 2 when it reaches the viewport's edge, which a window
  // already even does exactly (`>=` where `>` was meant): 1280x720 came out
  // 1278x718. An even window needs no rounding, so it is asked for none.
  if (int(vp->Size.x) % 2 == 0 && int(vp->Size.y) % 2 == 0)
    args->InSizeAlign = 1;
  return ctx->CaptureBeginVideo();
}

// Ends the recording and puts the suite's clock back: ending a video sets
// the engine's ConfigFixedDeltaTime to 0 (wall-clock time), and every test
// after it would then judge double-clicks by how loaded the machine is
// (docs/ui/testing.md#wait-on-the-condition-not-a-frame-count).
static bool EndRecording(ImGuiTestContext* ctx) {
  const float fixedDt = ctx->EngineIO->ConfigFixedDeltaTime;
  const bool ok = ctx->CaptureEndVideo();
  ctx->EngineIO->ConfigFixedDeltaTime = fixedDt;
  return ok;
}

void RegisterDemoGuiTests(ImGuiTestEngine* engine) {
  ImGuiTest* t = nullptr;

  // The recording itself, end to end: two seconds of an orbiting camera,
  // encoded by ffmpeg and decoded back. Every frame arrives (the clock is
  // fixed, so 120 frames are two seconds), at the window's size, showing the
  // window -- the interface as well as the scene -- and moving.
  t = IM_REGISTER_TEST(engine, "capture", "records_a_clip");
  t->TestFunc = [](ImGuiTestContext* ctx) {
    if (ui::FfmpegPath().empty()) {
      IM_ERRORF("%s", "ffmpeg is not on PATH; the clip cannot be encoded");
      return;
    }
    ctx->SetRef("//##MainMenuBar");
    AddFixtureBox(ctx);

    const std::filesystem::path clip =
        std::filesystem::temp_directory_path() /
        ("lobole_capture_" + std::to_string(ImGui::GetFrameCount()) + ".mp4");
    std::filesystem::remove(clip);
    IM_CHECK(BeginRecording(ctx, clip.string()));

    // The camera turned a step a frame, not by mouse: a recording runs at
    // Cinematic speed, where each mouse move glides and then pauses, so 120
    // moves were two minutes of video rather than two seconds.
    constexpr int kFrames = 120;
    const ImVec2 size = ImGui::GetMainViewport()->Size;
    for (int i = 0; i < kFrames; i++) {
      s_App->camera->Orbit(4.0, 0.0);
      ctx->Yield();
    }
    IM_CHECK(EndRecording(ctx));

    const int w = static_cast<int>(ctx->CaptureArgs->OutImageSize.x);
    const int h = static_cast<int>(ctx->CaptureArgs->OutImageSize.y);
    ctx->LogInfo("clip %dx%d at %s", w, h, clip.string().c_str());
    IM_CHECK(std::filesystem::exists(clip));
    // The whole window (to the encoder's even alignment), not a crop.
    IM_CHECK_LE(std::abs(w - int(size.x)), 1);
    IM_CHECK_LE(std::abs(h - int(size.y)), 1);

    const auto frames = DecodeClip(clip.string(), w, h);
    std::filesystem::remove(clip);
    ctx->LogInfo("decoded %d frames", int(frames.size()));
    IM_CHECK_GE(int(frames.size()), kFrames - 4);
    IM_CHECK_LE(int(frames.size()), kFrames + 8);
    IM_CHECK(frames.size() >= 2);
    // Not black: the scene and the panels are drawn.
    IM_CHECK_GT(MeanLuma(frames.back()), 10.0);
    // Moving: the camera turned between the first frame and the last.
    double diff = 0;
    for (size_t i = 0; i < frames.front().size(); i++)
      diff += std::abs(int(frames.front()[i]) - int(frames.back()[i]));
    IM_CHECK_GT(diff / double(frames.front().size()), 0.5);
  };
}
