// A motion study to a video (docs/assemblies/README.md#motion-video).
//
// The shape: a job of kind "motion-video" with NO worker of its own on the
// registry's side — posing needs the assembly and capturing needs the
// renderers, both main-thread things — so its `finish` is the slice (the
// ImportJob's materialize is the other one): each call poses one frame and
// captures it. The frames go to an ENCODER thread, which averages the
// supersampled capture down and hands it on — to ffmpeg's stdin, or to a PNG
// file — so a slow encoder costs the frame loop nothing but a short queue.

#include "ui/MotionActions.h"

#include "common/ChildProcess.h"
#include "common/Logger.h"
#include "core/Occurrence.h"
#include "render/Screenshot.h"
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/BackgroundJob.h"
#include "ui/BackgroundJobs.h"
#include "ui/RendererSync.h"

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

// The study plays in about this long unless `fps` says otherwise: the frame
// rate is derived from the frame count, so a 49-sample study is 12 fps and a
// 200-sample one 50, and neither is a number somebody typed.
constexpr double kDefaultPlaySeconds = 4.0;
// Captured this many times wider and taller and averaged down, as the quick
// render does by default: the edges of a moving part are what a video shows.
constexpr uint32_t kSupersample = 2;
// Frames captured ahead of the encoder. Enough to keep it busy; few enough
// that a slow encoder holds a few frames of memory, not the whole study.
constexpr size_t kQueueDepth = 3;

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

bool IsVideo(const std::string& f) {
  return f == "mp4" || f == "webm" || f == "gif";
}

// What the quality words mean to each encoder: x264's and VP9's constant
// rate factor (lower is better), the GIF palette's size.
std::string QualityValue(const std::string& format, const std::string& q) {
  const int i = q == "low" ? 0 : q == "high" ? 2 : 1;
  if (format == "mp4")
    return std::array<const char*, 3>{"28", "23", "18"}[i];
  if (format == "webm")
    return std::array<const char*, 3>{"40", "33", "24"}[i];
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
  if (p.format == "mp4") {
    for (const char* s : {"-c:v", "libx264", "-preset", "medium", "-pix_fmt",
                          "yuv420p", "-movflags", "+faststart", "-crf"})
      a.push_back(s);
    a.push_back(q);
  } else if (p.format == "webm") {
    for (const char* s :
         {"-c:v", "libvpx-vp9", "-pix_fmt", "yuv420p", "-b:v", "0", "-row-mt",
          "1", "-deadline", "good", "-cpu-used", "4", "-crf"})
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

// The pose a scrub changes: every placement, mate value and relation state
// the solve writes, and the configuration a study may switch to. Put back
// whole at the end, like a scrub that was never made.
struct SavedPose {
  std::vector<Occurrence> occurrences;
  std::vector<Mate> mates;
  std::vector<Relation> relations;
  uint64_t configuration = 0;
};

SavedPose SavePose(const AssemblyDocument& a) {
  return {a.GetOccurrences(), a.GetMates(), a.GetRelations(),
          a.ActiveConfiguration()};
}

void RestorePose(AppState& state, SavedPose& saved) {
  AssemblyDocument& a = *state.assembly;
  a.GetOccurrences() = std::move(saved.occurrences);
  a.GetMates() = std::move(saved.mates);
  a.GetRelations() = std::move(saved.relations);
  a.SetActiveConfigurationRaw(saved.configuration);
  a.BumpVersion(); // the placements changed back: renderers resync
  state.syncedDocumentVersion = UINT64_MAX;
}

struct Frame {
  std::vector<uint8_t> rgb; // supersampled
  uint32_t width = 0, height = 0, factor = 1;
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

void EncoderLoop(Encoder* e) {
  const bool video = IsVideo(e->plan.format);
  const auto fail = [&](const std::string& why) {
    e->error = why;
    e->failed.store(true);
  };
  std::vector<uint8_t> small;
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
    std::string err;
    if (!DownsampleRgb(f.rgb, f.width, f.height, f.factor, &small, &err)) {
      fail(err);
      continue;
    }
    const uint32_t w = f.width / f.factor, h = f.height / f.factor;
    if (video) {
      if (!e->child.Write(small.data(), small.size())) {
        std::string tail;
        const int code = e->child.Finish(&tail);
        fail("ffmpeg stopped reading at frame " + std::to_string(f.index) +
             " (exit " + std::to_string(code) + ")" +
             (tail.empty() ? std::string() : ": " + tail));
        continue;
      }
    } else if (stbi_write_png(FramePath(e->plan, f.index).c_str(), int(w),
                              int(h), 3, small.data(), int(w) * 3) == 0) {
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

// Everything one export carries between slices.
struct VideoRun {
  MotionVideoPlan plan;
  uint64_t studyId = 0;
  const AssemblyDocument* ranAgainst = nullptr; // identity, never dereferenced
  SavedPose saved;
  bool restored = false;
  int next = 0; // the next frame to capture
  std::shared_ptr<Encoder> encoder;
  std::vector<std::string> warnings;
  std::string error; // the main side's (a capture, a pose, a tab switch)
  std::chrono::steady_clock::time_point started;
};

} // namespace

std::string FfmpegPath() { return proc::FindOnPath("ffmpeg"); }

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
  MotionVideoPlan p;
  p.studyName = s->name;

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

  // Frames: the study's own samples, or as many as asked, over its range.
  p.frames = req.frames > 0 ? req.frames : s->samples;
  if (req.frames < 0)
    return fail("frames must be positive (or omitted for the study's " +
                std::to_string(s->samples) + " samples)");
  if (p.frames < 2)
    return fail("motion study '" + s->name + "' has " +
                std::to_string(p.frames) +
                " samples; a video needs at least 2 frames — set its "
                "samples (set_motion_study) or pass frames");

  // Size: the viewport's, unless asked. The encoders' 4:2:0 needs even
  // sides; a derived size is rounded down to one, an asked-for odd one is
  // refused rather than changed.
  const bool yuv = p.format == "mp4" || p.format == "webm";
  if ((req.width == 0) != (req.height == 0))
    return fail("width and height go together (pixels); pass both, or "
                "neither for the viewport's size");
  if (req.width == 0) {
    p.width = static_cast<uint32_t>(std::max(0.0f, state.viewportSize.x));
    p.height = static_cast<uint32_t>(std::max(0.0f, state.viewportSize.y));
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

  // Frame rate: the study in about four seconds, unless asked.
  if (req.fps < 0.0 || !std::isfinite(req.fps))
    return fail("fps must be a positive number of frames per second");
  p.fps = req.fps > 0.0 ? req.fps : p.frames / kDefaultPlaySeconds;

  // The encoder, by name. A video is never quietly made into something else.
  if (IsVideo(p.format)) {
    p.ffmpeg = FfmpegPath();
    if (p.ffmpeg.empty())
      return fail("export_motion_video format " + p.format +
                  " needs ffmpeg on PATH; install it, or use format png");
  }
  if (!state.captureRaster)
    return fail("a video is captured with the renderer, and this instance "
                "has none (--headless); run it --offscreen or windowed");
  *plan = std::move(p);
  return true;
}

bool StartMotionVideo(AppState& state, const MotionVideoRequest& req,
                      std::string* error, MotionVideoPlan* planOut) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (BackgroundJobRunning(state, "motion-video"))
    return fail("a motion video is already being exported (background_jobs "
                "lists it; cancel_background_job {kind: \"motion-video\"} "
                "stops it)");
  auto run = std::make_shared<VideoRun>();
  if (!PlanMotionVideo(state, req, &run->plan, error))
    return false;
  if (planOut)
    *planOut = run->plan;
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

  run->studyId = req.studyId;
  run->ranAgainst = state.assembly.get();
  run->saved = SavePose(*state.assembly);
  run->started = std::chrono::steady_clock::now();
  // Playback draws a stored run's poses over the document's: a video of the
  // posed document must not be a video of that instead.
  StopMotionPlayback(state);

  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = "motion-video";
  bg->label = "Motion video '" + run->plan.studyName + "' to " +
              fs::path(run->plan.path).filename().string();
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
    const bool stopping = e.cancel.load() || e.failed.load();

    // Capture the next frame, while there is one and room for it.
    if (!stopping && run->next < run->plan.frames) {
      {
        std::unique_lock lock(e.m);
        if (e.queue.size() >= kQueueDepth) {
          // A short wait, not a spin: the synchronous RPC calls this in a
          // loop, and the frame loop can afford a few milliseconds.
          e.cv.wait_for(lock, std::chrono::milliseconds(5));
          return false;
        }
      }
      const int k = run->next;
      const double t = double(k) / double(run->plan.frames - 1);
      std::string err;
      if (!ApplyMotionPose(s, run->studyId, t, &err)) {
        stop("frame " + std::to_string(k) + ": " + err);
        return false;
      }
      SyncRenderersToDocument(s);
      AppState::RasterStill req;
      req.width = run->plan.width;
      req.height = run->plan.height;
      req.supersample = kSupersample;
      RasterFrame captured;
      std::string warning;
      if (!s.captureRaster(req, &captured, &err, &warning)) {
        stop("frame " + std::to_string(k) + " could not be captured: " + err);
        return false;
      }
      if (!warning.empty() &&
          std::find(run->warnings.begin(), run->warnings.end(), warning) ==
              run->warnings.end())
        run->warnings.push_back(warning);
      {
        std::lock_guard lock(e.m);
        e.queue.push_back(Frame{std::move(captured.rgb), captured.width,
                                captured.height, captured.factor, k});
      }
      e.cv.notify_all();
      run->next++;
      raw->fraction.store(0.5 * (double(run->next) + double(e.written.load())) /
                          double(run->plan.frames));
      return false;
    }

    // Every frame captured (or the export stopped): the pose goes back
    // first — the encoder does not need it — and no more frames come.
    if (!run->restored) {
      run->restored = true;
      if (ours)
        RestorePose(s, run->saved);
      std::lock_guard lock(e.m);
      e.closing = true;
    }
    e.cv.notify_all();
    raw->fraction.store(0.5 + 0.5 * double(e.written.load()) /
                                  double(run->plan.frames));
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
    result->ok =
        result->error.empty() && result->framesWritten == run->plan.frames;
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
      LOG_INFO("Motion video written to {} ({} frames, {}x{}, {} fps, {} "
               "bytes)",
               run->plan.path, result->framesWritten, run->plan.width,
               run->plan.height, run->plan.fps, result->bytes);
    else
      LOG_WARN("Motion video to {} failed: {}", run->plan.path, result->error);
    s.motionVideoLast = std::move(result);
    return true;
  };
  return true;
}

} // namespace ui
