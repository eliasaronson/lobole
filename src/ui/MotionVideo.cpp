// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A motion study to a video (docs/assemblies/README.md#motion-video).
//
// The shape: a job of kind "motion-video" with NO worker of its own on the
// registry's side — showing a pose and capturing it need the renderers, a
// main-thread thing — so its `finish` is the slice (the ImportJob's
// materialize is the other one). The poses are a VideoPoses: the Motion
// window's Play -- a run's samples, interpolated at the playback's rate
// (PosesToPlay makes a collision-free run when there is no fresh one to
// play) -- or an assembly sequence's timeline (ui/SequencePlayback.cpp); the
// capture, the encoder and the job are one for both. Each slice streams
// frames of the viewport's own render through a FrameCapture for a few
// milliseconds; the frames go to an ENCODER thread, which hands them on — to
// ffmpeg's stdin, or to a PNG file — so a slow encoder costs the frame loop
// nothing but a short queue.

#include "ui/MotionActions.h"

#include "common/ChildProcess.h"
#include "common/Logger.h"
#include "core/Occurrence.h"
#include "imgui.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJob.h"
#include "ui/BackgroundJobs.h"
#include "ui/SequencePlayback.h"

#include "thirdparty/stb_image_write.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace ui {
namespace {

// Frames captured ahead of the encoder, counting those still on the GPU.
// Enough to keep both busy; few enough that a slow encoder holds a few
// frames of memory, not the whole study.
constexpr size_t kQueueDepth = 6;
// How long one slice of the frame loop streams frames for. The capture aims
// the renderers at the video's size and back once per slice, so a slice is
// many frames; short enough that the windowed app keeps drawing its progress.
constexpr auto kSliceBudget = std::chrono::milliseconds(100);

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool IsVideo(const std::string& f) {
  return f == "mp4" || f == "webm" || f == "gif";
}

// What the quality words mean to each encoder: x264's and VP9's constant
// rate factor (lower is better; x264's 18 is about where a CAD model's thin
// outlines stop smearing), the GIF palette's size.
std::string QualityValue(const std::string& format, const std::string& q) {
  const int i = q == "low" ? 0 : q == "high" ? 2 : 1;
  if (format == "mp4")
    return std::array<const char*, 3>{"23", "18", "14"}[i];
  if (format == "webm")
    return std::array<const char*, 3>{"36", "30", "24"}[i];
  return std::array<const char*, 3>{"64", "128", "256"}[i]; // gif colours
}

std::string FpsText(double fps) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.6g", fps);
  return buf;
}

std::vector<std::string> FfmpegArgs(const MotionVideoPlan& p) {
  std::vector<std::string> a = {
      "-hide_banner", "-loglevel",
      "error",        "-y",
      "-f",           "rawvideo",
      "-pix_fmt",     "rgb24",
      "-s",           std::to_string(p.width) + "x" + std::to_string(p.height),
      "-framerate",   FpsText(p.fps),
      "-i",           "pipe:0"};
  const std::string q = QualityValue(p.format, p.quality);
  if (p.format == "mp4" || p.format == "webm") {
    // The frames are sRGB, full range. Converted to 4:2:0 with the BT.709
    // matrix and SAID to be BT.709, which is what a player assumes of an
    // untagged HD video anyway — ffmpeg's own default converts with BT.601
    // and tags nothing, so every player shifted the colours a little. The
    // tags go on the frames (setparams): the encoder takes them from there.
    a.push_back("-vf");
    a.push_back("scale=out_color_matrix=bt709:out_range=tv:"
                "flags=accurate_rnd+full_chroma_int,"
                "setparams=color_primaries=bt709:color_trc=bt709:"
                "colorspace=bt709:range=tv");
    a.push_back("-pix_fmt");
    a.push_back("yuv420p");
  }
  if (p.format == "mp4") {
    for (const char* s : {"-c:v", "libx264", "-preset", "medium", "-profile:v",
                          "high", "-movflags", "+faststart", "-crf"})
      a.push_back(s);
    a.push_back(q);
  } else if (p.format == "webm") {
    for (const char* s : {"-c:v", "libvpx-vp9", "-b:v", "0", "-row-mt", "1",
                          "-deadline", "good", "-cpu-used", "4", "-crf"})
      a.push_back(s);
    a.push_back(q);
  } else { // gif: ONE palette for the whole animation, from its own colours
    a.push_back("-filter_complex");
    a.push_back("[0:v]split[a][b];[a]palettegen=max_colors=" + q +
                ":stats_mode=full[p];[b][p]paletteuse=dither=sierra2_4a");
    a.push_back("-loop");
    a.push_back("0");
  }
  a.push_back(p.path);
  return a;
}

struct Frame {
  std::vector<uint8_t> pixels; // RGBA8 in memory order B,G,R,A when bgra
  bool bgra = true;
  int index = 0;
};

// The encoder's side: a queue in, and one outcome out.
struct Encoder {
  MotionVideoPlan plan;
  proc::PipedChild child; // only the encoder thread touches it after Start
  std::mutex m;
  std::condition_variable cv;
  std::deque<Frame> queue;
  bool closing = false; // no more frames are coming
  std::atomic<bool> cancel{false};
  std::atomic<bool> failed{false};
  std::atomic<int> written{0};
  std::atomic<bool> finished{false};
  std::string error; // read after `finished`
  std::thread thread;

  ~Encoder() {
    cancel.store(true);
    cv.notify_all();
    if (thread.joinable())
      thread.join();
  }
};

std::string FramePath(const MotionVideoPlan& p, int index) {
  const int digits =
      std::max(4, static_cast<int>(std::to_string(p.frames - 1).size()));
  char name[64];
  std::snprintf(name, sizeof name, "frame_%0*d.png", digits, index);
  return (fs::path(p.path) / name).string();
}

// Four bytes a pixel in either order to three, R G B.
void ToRgb(const Frame& f, std::vector<uint8_t>* rgb) {
  const size_t n = f.pixels.size() / 4;
  rgb->resize(n * 3);
  const size_t r = f.bgra ? 2 : 0, b = f.bgra ? 0 : 2;
  const uint8_t* src = f.pixels.data();
  uint8_t* dst = rgb->data();
  for (size_t i = 0; i < n; i++, src += 4, dst += 3) {
    dst[0] = src[r];
    dst[1] = src[1];
    dst[2] = src[b];
  }
}

void EncoderLoop(Encoder* e) {
  const bool video = IsVideo(e->plan.format);
  const auto fail = [&](const std::string& why) {
    e->error = why;
    e->failed.store(true);
  };
  std::vector<uint8_t> rgb;
  for (;;) {
    Frame f;
    {
      std::unique_lock lock(e->m);
      e->cv.wait(lock, [&] {
        return !e->queue.empty() || e->closing || e->cancel.load();
      });
      if (e->cancel.load())
        break;
      if (e->queue.empty()) // closing, and drained
        break;
      f = std::move(e->queue.front());
      e->queue.pop_front();
    }
    e->cv.notify_all(); // room in the queue
    if (e->failed.load())
      continue; // drain without work; the main side stops feeding
    ToRgb(f, &rgb);
    const uint32_t w = e->plan.width, h = e->plan.height;
    if (rgb.size() != size_t(w) * h * 3) {
      fail("frame " + std::to_string(f.index) + " is not " + std::to_string(w) +
           "x" + std::to_string(h) + " px");
      continue;
    }
    if (video) {
      if (!e->child.Write(rgb.data(), rgb.size())) {
        std::string tail;
        const int code = e->child.Finish(&tail);
        fail("ffmpeg stopped reading at frame " + std::to_string(f.index) +
             " (exit " + std::to_string(code) + ")" +
             (tail.empty() ? std::string() : ": " + tail));
        continue;
      }
    } else if (stbi_write_png(FramePath(e->plan, f.index).c_str(), int(w),
                              int(h), 3, rgb.data(), int(w) * 3) == 0) {
      fail("could not write " + FramePath(e->plan, f.index));
      continue;
    }
    e->written.fetch_add(1);
  }
  if (video && e->child.Running()) {
    if (e->cancel.load()) {
      e->child.Kill();
    } else {
      std::string tail;
      const int code = e->child.Finish(&tail);
      if (code != 0 && !e->failed.load())
        fail("ffmpeg exited with code " + std::to_string(code) +
             (tail.empty() ? std::string(" and said nothing") : ": " + tail));
    }
  }
  e->finished.store(true);
  e->cv.notify_all();
}

uint64_t OutputBytes(const MotionVideoPlan& p, int frames) {
  std::error_code ec;
  if (IsVideo(p.format)) {
    const auto n = fs::file_size(p.path, ec);
    return ec ? 0 : n;
  }
  uint64_t total = 0;
  for (int i = 0; i < frames; i++) {
    const auto n = fs::file_size(FramePath(p, i), ec);
    if (!ec)
      total += n;
  }
  return total;
}

// A study's run as the poses to film: Play's poses at its rate.
class StudyPoses final : public VideoPoses {
public:
  StudyPoses(uint64_t studyId, std::string name,
             std::shared_ptr<MotionPoses> poses)
      : m_studyId(studyId), m_name(std::move(name)), m_poses(std::move(poses)) {
  }
  bool Ready() const override { return m_poses->done.load(); }
  std::string Refusal() const override {
    return m_poses->error.empty()
               ? std::string()
               : "motion study '" + m_name +
                     "' has no poses to film: " + m_poses->error;
  }
  double Seconds() const override {
    return double(m_poses->result->samples.size() - 1) /
           kPlaybackSamplesPerSecond;
  }
  void Show(AppState& s, double fraction) override {
    const double span = double(m_poses->result->samples.size() - 1);
    ShowMotionPoses(s, m_studyId, m_poses->result, span * fraction);
  }
  bool Release(AppState& s) override {
    m_poses->cancel.store(true);
    if (m_poses->result && s.motionPlaybackPinned == m_poses->result)
      StopMotionPlayback(s);
    return m_poses->done.load(); // its run stops at the next sample
  }

private:
  uint64_t m_studyId;
  std::string m_name;
  std::shared_ptr<MotionPoses> m_poses;
};

// Everything one export carries between slices.
struct VideoRun {
  MotionVideoPlan plan;
  int framesAsked = 0; // 0: as many as Play takes, once the poses are made
  const AssemblyDocument* ranAgainst = nullptr; // identity, never dereferenced
  std::shared_ptr<VideoPoses> poses;
  bool shown = false; // the poses are pinned to the viewport
  int next = 0;       // the next frame to submit
  int collected = 0;  // frames read back (in order) so far
  std::shared_ptr<Encoder> encoder;
  std::vector<std::string> warnings;
  std::string error; // the main side's (a capture, the poses, a tab switch)
  std::chrono::steady_clock::time_point started;
};

// Frame k's place in the poses: their whole span over the frames, the
// first frame on the first pose and the last on the last.
double FramePos(const VideoRun& run, int k) {
  return run.plan.frames > 1 ? double(k) / double(run.plan.frames - 1) : 0.0;
}

// One slice of frames: aim a capture at the video's size, submit frames as
// long as the encoder has room and the slice has time, reading back each as
// the pipeline fills, then read back what is still in flight. False (with
// `run.error`) when the capture fails.
bool StreamFrames(AppState& s, VideoRun& run) {
  Encoder& e = *run.encoder;
  std::string err;
  std::unique_ptr<FrameCapture> cap =
      s.beginFrameCapture(run.plan.width, run.plan.height, &err);
  if (!cap) {
    run.error = "the frames could not be captured: " + err;
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() + kSliceBudget;
  const auto collect = [&]() -> bool {
    Frame f;
    if (!cap->Collect(&f.pixels, &err)) {
      run.error = "frame " + std::to_string(run.collected) +
                  " could not be read back: " + err;
      return false;
    }
    f.bgra = cap->Bgra();
    f.index = run.collected++;
    {
      std::lock_guard lock(e.m);
      e.queue.push_back(std::move(f));
    }
    e.cv.notify_all();
    return true;
  };
  bool ok = true;
  while (ok && run.next < run.plan.frames && !e.cancel.load() &&
         !e.failed.load()) {
    {
      std::unique_lock lock(e.m);
      if (e.queue.size() + cap->InFlight() >= kQueueDepth) {
        if (std::chrono::steady_clock::now() >= deadline)
          break;
        // A short wait, not a spin: the encoder frees a place soon.
        e.cv.wait_for(lock, std::chrono::milliseconds(2));
        continue;
      }
    }
    if (cap->Full() && !collect()) {
      ok = false;
      break;
    }
    run.poses->Show(s, FramePos(run, run.next));
    if (!cap->Submit(&err)) {
      run.error = "frame " + std::to_string(run.next) +
                  " could not be rendered: " + err;
      ok = false;
      break;
    }
    run.next++;
    if (std::chrono::steady_clock::now() >= deadline)
      break;
  }
  while (ok && cap->InFlight() > 0)
    ok = collect();
  return ok;
}

} // namespace

std::string FfmpegPath() { return proc::FindOnPath("ffmpeg"); }

bool PlanVideoFile(AppState& state, const MotionVideoRequest& req,
                   const std::string& rpc, double seconds,
                   MotionVideoPlan* plan, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  MotionVideoPlan p = *plan; // what the caller has said of it (its name)

  // The format: said, or read off the path. Never guessed.
  if (req.path.empty())
    return fail("path is required: a file for mp4, webm or gif, a folder "
                "for png");
  p.path = req.path;
  const std::string ext = Lower(fs::path(req.path).extension().string());
  p.format = Lower(req.format);
  if (!p.format.empty() && !IsVideo(p.format) && p.format != "png")
    return fail("format '" + req.format +
                "' is not one of: " + kMotionVideoFormats);
  if (p.format.empty()) {
    const std::string fromExt = ext.empty() ? "" : ext.substr(1);
    if (!IsVideo(fromExt))
      return fail("path '" + req.path +
                  "' does not say a format (it ends in '" + ext +
                  "'); pass format: one of " + kMotionVideoFormats +
                  " — png writes numbered frames into a folder");
    p.format = fromExt;
  }
  std::error_code ec;
  if (p.format == "png") {
    const bool isDir = fs::is_directory(req.path, ec);
    if (!isDir && (fs::exists(req.path, ec) || !ext.empty()))
      return fail("format png writes numbered frames into a FOLDER, and '" +
                  req.path +
                  "' names a file; give a folder (it is made if it does not "
                  "exist)");
  } else if (ext != "." + p.format) {
    return fail("path '" + req.path + "' ends in '" + ext + "' but format is " +
                p.format + "; they must agree (." + p.format + ")");
  }
  const fs::path parent = fs::absolute(req.path, ec).parent_path();
  if (!parent.empty() && !fs::is_directory(parent, ec))
    return fail("the folder '" + parent.string() + "' does not exist");

  // Quality: words, for what each encoder means by them.
  if (!req.quality.empty()) {
    if (p.format == "png")
      return fail("quality applies to mp4, webm and gif; png frames are "
                  "lossless");
    p.quality = Lower(req.quality);
    if (p.quality != "low" && p.quality != "medium" && p.quality != "high")
      return fail("quality '" + req.quality +
                  "' is not one of: " + kMotionVideoQualities);
  } else {
    p.quality = "medium";
  }

  // Timing: the poses play for as long as Play takes, at a display's rate,
  // one frame per display frame -- Play's own poses in between.
  if (req.frames < 0)
    return fail("frames must be positive (or omitted for as many as the "
                "playback takes at fps)");
  if (req.fps < 0.0 || !std::isfinite(req.fps))
    return fail("fps must be a positive number of frames per second");
  p.fps = req.fps > 0.0 ? req.fps : kMotionVideoFps;
  p.frames = req.frames > 0 ? req.frames
             : seconds >= 0.0
                 ? static_cast<int>(std::lround(seconds * p.fps)) + 1
                 : 0; // once the poses are made
  if (p.frames == 1 || (p.frames == 0 && seconds >= 0.0))
    return fail("a video needs at least 2 frames; pass frames, or a higher "
                "fps");

  // Size: the viewport's, in the pixels it is drawn with, unless asked. The
  // encoders' 4:2:0 needs even sides; a derived size is rounded down to one,
  // an asked-for odd one is refused rather than changed.
  const bool yuv = p.format == "mp4" || p.format == "webm";
  if ((req.width == 0) != (req.height == 0))
    return fail("width and height go together (pixels); pass both, or "
                "neither for the viewport's size");
  if (req.width == 0) {
    // On a high-DPI screen the viewport is drawn at more pixels than its
    // size in points: a video of the points would be the blurrier picture.
    glm::vec2 scale(1.0f);
    if (ImGui::GetCurrentContext() != nullptr)
      scale = {ImGui::GetIO().DisplayFramebufferScale.x,
               ImGui::GetIO().DisplayFramebufferScale.y};
    p.width = static_cast<uint32_t>(
        std::max(0.0f, state.viewportSize.x * std::max(scale.x, 1.0f)));
    p.height = static_cast<uint32_t>(
        std::max(0.0f, state.viewportSize.y * std::max(scale.y, 1.0f)));
    if (yuv) {
      p.width &= ~1u;
      p.height &= ~1u;
    }
    if (p.width < 2 || p.height < 2)
      return fail("the viewport has no size to take; pass width and height "
                  "(pixels)");
  } else {
    p.width = req.width;
    p.height = req.height;
    if (yuv && (p.width % 2 != 0 || p.height % 2 != 0))
      return fail(p.format +
                  " needs an even width and height (its colour is "
                  "sampled in 2x2 blocks); " +
                  std::to_string(p.width) + "x" + std::to_string(p.height) +
                  " px is not");
  }

  // The encoder, by name. A video is never quietly made into something else.
  if (IsVideo(p.format)) {
    p.ffmpeg = FfmpegPath();
    if (p.ffmpeg.empty())
      return fail(rpc + " format " + p.format +
                  " needs ffmpeg on PATH; install it, or use format png");
  }
  if (!state.beginFrameCapture)
    return fail("a video is captured with the renderer, and this instance "
                "has none (--headless); run it --offscreen or windowed");
  *plan = std::move(p);
  return true;
}

bool PlanMotionVideo(AppState& state, const MotionVideoRequest& req,
                     MotionVideoPlan* plan, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!state.assembly)
    return fail("the active document is not an assembly");
  const MotionStudy* s = state.assembly->FindMotionStudyById(req.studyId);
  if (!s)
    return fail("no motion study with id " + std::to_string(req.studyId));
  const std::string why =
      motion::WhyNot(*state.assembly, *s, AssemblyResolverFor(state));
  if (!why.empty())
    return fail(why);
  if (s->samples < 2)
    return fail("motion study '" + s->name + "' has " +
                std::to_string(s->samples) +
                " samples; a video needs at least 2 — set its samples "
                "(set_motion_study)");
  MotionVideoPlan p;
  p.studyName = s->name;
  if (!PlanVideoFile(state, req, "export_motion_video",
                     (s->samples - 1) / kPlaybackSamplesPerSecond, &p, error))
    return false;
  *plan = std::move(p);
  return true;
}

std::string VideoJobRunningRefusal(AppState& state) {
  if (BackgroundJobRunning(state, "motion-video"))
    return "a video is already being exported (background_jobs lists it; "
           "cancel_background_job {kind: \"motion-video\"} stops it)";
  return {};
}

bool StartMotionVideo(AppState& state, const MotionVideoRequest& req,
                      std::string* error, MotionVideoPlan* planOut) {
  if (const std::string no = VideoJobRunningRefusal(state); !no.empty()) {
    if (error)
      *error = no;
    return false;
  }
  MotionVideoPlan plan;
  if (!PlanMotionVideo(state, req, &plan, error))
    return false;
  if (planOut)
    *planOut = plan;
  // The poses first: a study that cannot be run is refused before a file
  // or an encoder exists.
  auto poses = PosesToPlay(state, req.studyId, error);
  if (!poses)
    return false;
  return StartVideoJob(state, plan, req.frames,
                       std::make_shared<StudyPoses>(req.studyId, plan.studyName,
                                                    std::move(poses)),
                       "Motion video '" + plan.studyName + "' to " +
                           fs::path(plan.path).filename().string(),
                       error);
}

bool StartVideoJob(AppState& state, const MotionVideoPlan& plan,
                   int framesAsked, std::shared_ptr<VideoPoses> poses,
                   const std::string& label, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (const std::string no = VideoJobRunningRefusal(state); !no.empty())
    return fail(no);
  auto run = std::make_shared<VideoRun>();
  run->plan = plan;
  run->framesAsked = framesAsked;
  run->poses = std::move(poses);
  std::error_code ec;
  if (run->plan.format == "png") {
    fs::create_directories(run->plan.path, ec);
    if (ec)
      return fail("could not make the folder '" + run->plan.path +
                  "': " + ec.message());
  }
  run->encoder = std::make_shared<Encoder>();
  run->encoder->plan = run->plan;
  if (IsVideo(run->plan.format)) {
    std::string err;
    if (!run->encoder->child.Start(run->plan.ffmpeg, FfmpegArgs(run->plan),
                                   &err))
      return fail(err);
  }
  run->encoder->thread = std::thread(EncoderLoop, run->encoder.get());

  run->ranAgainst = state.assembly.get();
  run->started = std::chrono::steady_clock::now();
  // Playback moves the same renderers the frames are taken from.
  StopMotionPlayback(state);
  StopSequencePlayback(state);

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "motion-video";
  bg->label = label;
  bg->cancellable = true;
  bg->fraction.store(0.0);
  // No worker on this side: `done` from the start, and `finish` is the
  // slice, called every loop iteration until it returns true.
  bg->done.store(true);
  BackgroundJob* raw = &StartBackgroundJob(state, std::move(bg));
  raw->finish = [run, raw](AppState& s) -> bool {
    Encoder& e = *run->encoder;
    const bool ours = s.assembly.get() == run->ranAgainst;
    const auto stop = [&](const std::string& why) {
      if (run->error.empty())
        run->error = why;
      e.cancel.store(true);
      e.cv.notify_all();
    };
    if (!ours)
      stop("the assembly tab changed during the export");
    if (raw->cancel.load()) {
      e.cancel.store(true);
      e.cv.notify_all();
    }
    const bool stopping =
        e.cancel.load() || e.failed.load() || !run->error.empty();

    if (!stopping && (!run->shown || run->next < run->plan.frames)) {
      VideoPoses& poses = *run->poses;
      if (!poses.Ready()) { // the poses are still being made
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return false;
      }
      if (const std::string no = poses.Refusal(); !no.empty()) {
        stop(no);
        return false;
      }
      if (!run->shown) {
        if (run->framesAsked <= 0) {
          // As many as Play takes, now that it is known how long.
          const int frames =
              static_cast<int>(std::lround(poses.Seconds() * run->plan.fps)) +
              1;
          if (frames < 2) {
            stop("a video needs at least 2 frames; pass frames, or a "
                 "higher fps");
            return false;
          }
          run->plan.frames = frames;
          std::lock_guard lock(e.m);
          e.plan.frames = frames; // before any frame reaches it
        }
        run->shown = true;
        // Shown before the first capture aims the renderers, so the resync
        // that showing asks for happens once, not every slice.
        poses.Show(s, 0.0);
        return false;
      }
      if (!StreamFrames(s, *run))
        stop(run->error);
      raw->fraction.store(0.5 *
                          (double(run->collected) + double(e.written.load())) /
                          double(std::max(run->plan.frames, 1)));
      return false;
    }

    // Every frame captured (or the export stopped): the viewport goes back
    // to the document's placements, and no more frames come.
    if (run->poses) {
      if (!run->poses->Release(s))
        return false; // what makes them stops at its next step
      run->poses.reset();
      std::lock_guard lock(e.m);
      e.closing = true;
    }
    e.cv.notify_all();
    raw->fraction.store(0.5 + 0.5 * double(e.written.load()) /
                                  double(std::max(run->plan.frames, 1)));
    if (!e.finished.load()) {
      std::unique_lock lock(e.m);
      e.cv.wait_for(lock, std::chrono::milliseconds(5));
      return false;
    }
    if (e.thread.joinable())
      e.thread.join();

    auto result = std::make_shared<MotionVideoResult>();
    result->plan = run->plan;
    result->framesWritten = e.written.load();
    result->warnings = run->warnings;
    result->seconds = std::chrono::duration<double>(
                          std::chrono::steady_clock::now() - run->started)
                          .count();
    result->cancelled = raw->cancel.load();
    result->error = !run->error.empty() ? run->error
                    : e.failed.load()   ? e.error
                    : result->cancelled ? "cancelled"
                                        : std::string();
    result->ok = result->error.empty() && run->plan.frames >= 2 &&
                 result->framesWritten == run->plan.frames;
    if (result->error.empty() && !result->ok)
      result->error = "only " + std::to_string(result->framesWritten) + " of " +
                      std::to_string(run->plan.frames) + " frames were written";
    // A video that was not finished is not left looking like one that was.
    if (!result->ok && IsVideo(run->plan.format)) {
      std::error_code rm;
      fs::remove(run->plan.path, rm);
    }
    result->bytes =
        result->ok ? OutputBytes(run->plan, result->framesWritten) : 0;
    if (result->ok)
      LOG_INFO("Video written to {} ({} frames, {}x{}, {} fps, {} "
               "bytes, {:.1f} s)",
               run->plan.path, result->framesWritten, run->plan.width,
               run->plan.height, run->plan.fps, result->bytes, result->seconds);
    else
      LOG_WARN("Video to {} failed: {}", run->plan.path, result->error);
    s.motionVideoLast = std::move(result);
    return true;
  };
  return true;
}

} // namespace ui
