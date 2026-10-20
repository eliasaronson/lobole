// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// An assembly sequence played and filmed
// (docs/assemblies/README.md#playing-and-filming-a-sequence).
//
// The timeline is laid out from the sequence CHECK's own answer: the check
// is read on the frame (remote::ReadSequenceCheck, which resolves every
// step's path) and run on a worker (remote::RunSequenceCheck); each step's
// way home, its line and its distance from clear are the row it answers.
// So what is shown is what was checked, and nothing is re-derived here.
// What a step MOVES and what it SETS are read through one accessor each
// (StepOccurrences, StepMateValues), the two places a group step and a pose
// step join in.

#include "ui/SequencePlayback.h"

#include "cad/CADKernel.h" // ArmOccSignalHandlers
#include "core/Feature.h"  // Body, ThreadMark
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "remote/RemoteInspection.h" // the sequence check
#include "ui/AppState.h"
#include "ui/AssemblyActions.h"
#include "ui/PanelLayout.h"
#include "ui/TextFit.h"
#include "ui/Widgets.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <limits>

namespace ui {
namespace {

using nlohmann::json;

// --- What a step moves and sets --------------------------------------------

// The occurrences a step moves, rigidly together: one today.
std::vector<uint64_t> StepOccurrences(const SequenceStep& st) {
  return {st.occurrenceId};
}

// The mate values a pose step sets (mate, value in the mate's own unit):
// none, until a step can be a pose.
struct StepMateValue {
  uint64_t mate = 0;
  double value = 0.0;
};
std::vector<StepMateValue> StepMateValues(const SequenceStep&) { return {}; }

// --- Resolving ---------------------------------------------------------------

// The assembly's content as playback depends on it: its own version and
// every part's, the bill of materials' key. A change ends playback and
// makes the next Play check again.
uint64_t ContentKey(const AppState& state) {
  uint64_t key = state.assembly ? state.assembly->Version() : 0;
  for (const PartSlot& s : state.parts)
    if (s.kind == DocKind::Part && s.doc)
      key = key * 1315423911ull + s.doc->Version();
  return key;
}

// What a step needs that the check's answer does not carry: what it moves,
// the turn it was given, and the thread it goes on by.
struct StepExtra {
  std::vector<uint64_t> occurrences;
  double turnDeg = 0.0;
  bool thread = false;
  double pitch = 0.0; // the moving side's thread, mm; 0: none found
  bool rightHand = true;
};

// The pitch and hand of the thread a step goes on by: a modelled thread
// of the moving side first (the one the check turns at its pitch), else
// any thread it carries.
void ThreadOf(const std::vector<Body>& bodies, StepExtra& x) {
  const ThreadMark* best = nullptr;
  for (const Body& b : bodies)
    for (const ThreadMark& m : b.threads)
      if (m.pitch > 0.0 && (best == nullptr || (m.modeled && !best->modeled)))
        best = &m;
  if (best) {
    x.pitch = best->pitch;
    x.rightHand = best->rightHand;
  }
}

std::string StepLabel(const json& row) {
  return "step " + std::to_string(row.value("step", 0)) + " ('" +
         row.value("name", std::string()) + "')";
}

// Why a checked sequence cannot be shown: its first failing step, by what
// the check said of it, or what no step places. Empty when it passes.
std::string FirstFailure(const json& r) {
  for (const json& row : r.at("steps")) {
    if (row.value("pass", false))
      continue;
    if (row.contains("error"))
      return StepLabel(row) +
             " cannot go home: " + row["error"].get<std::string>();
    char buf[256];
    std::snprintf(buf, sizeof buf,
                  " does not go home: forced %.3g mm against a press of "
                  "%.3g mm",
                  row.value("interfering_mm", 0.0), row.value("press_mm", 0.0));
    std::string why = StepLabel(row) + buf;
    if (row.contains("worst_against"))
      why +=
          ", worst against '" + row["worst_against"].get<std::string>() + "'";
    return why;
  }
  if (!r.at("missing").empty()) {
    std::string names;
    for (const json& n : r.at("missing"))
      names += (names.empty() ? "'" : ", '") + n.get<std::string>() + "'";
    return "no step places " + names;
  }
  if (!r.value("pass", false))
    return "the check does not pass";
  return {};
}

void PushAll(AppState& state) {
  if (!state.assembly)
    return;
  for (const Occurrence& o : state.assembly->GetOccurrences())
    PushOccurrenceLive(state, o.id);
}

SequenceShow& ShowOf(AppState& state) {
  if (!state.sequenceShow)
    state.sequenceShow = std::make_shared<SequenceShow>();
  return *state.sequenceShow;
}

bool Fresh(const AppState& state, const SequencePoses& p) {
  return state.assembly && p.assembly == state.assembly.get() &&
         p.key == ContentKey(state);
}

} // namespace

std::shared_ptr<SequencePoses> ResolveSequence(AppState& state,
                                               std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return nullptr;
  };
  if (!state.assembly)
    return fail("an assembly sequence is played on an assembly tab "
                "(set_active_part)");
  const AssemblyDocument& asmDoc = *state.assembly;
  const AssemblySequence& seq = asmDoc.GetSequence();
  std::shared_ptr<const remote::SequenceCheck> check;
  try {
    check = remote::ReadSequenceCheck(state);
  } catch (const std::exception& e) {
    return fail(e.what());
  }
  // What each step moves and turns, read on the frame.
  std::vector<StepExtra> extras;
  for (size_t i = 0; i < seq.steps.size(); i++) {
    const SequenceStep& st = seq.steps[i];
    if (!StepMateValues(st).empty())
      return fail("step " + std::to_string(i + 1) +
                  " sets mate values, and a pose step is not played yet");
    StepExtra x;
    x.occurrences = StepOccurrences(st);
    x.turnDeg = st.turnDeg;
    x.thread = st.thread;
    if (st.thread && !x.occurrences.empty())
      ThreadOf(OccurrenceSubtreeBodies(state, x.occurrences.front()), x);
    extras.push_back(std::move(x));
  }
  // The assembly's size, which sets the one speed everything moves at.
  glm::dvec3 lo(std::numeric_limits<double>::infinity()), hi(-lo);
  for (const Occurrence& o : asmDoc.GetOccurrences()) {
    if (!o.visible)
      continue;
    glm::dvec3 a, b;
    if (Measure::WorldBounds(OccurrenceSubtreeBodies(state, o.id),
                             glm::dmat4(1.0), a, b)) {
      lo = glm::min(lo, a);
      hi = glm::max(hi, b);
    }
  }
  const double size = hi.x >= lo.x ? glm::length(hi - lo) : 0.0;
  if (!(size > 0.0))
    return fail("the assembly has no extent to set the film's speed by");

  auto p = std::make_shared<SequencePoses>();
  p->assembly = state.assembly.get();
  p->key = ContentKey(state);
  p->steps = seq.steps.size();
  SequencePoses* raw = p.get();
  p->worker = std::thread([raw, check, extras = std::move(extras), size] {
    ArmOccSignalHandlers(); // the check's kernel calls, as its job's are
    std::string error;
    std::shared_ptr<sequence::Timeline> tl;
    try {
      const json r =
          remote::RunSequenceCheck(*check, raw->cancel, raw->fraction);
      error = FirstFailure(r);
      if (error.empty()) {
        const json& rows = r.at("steps");
        if (rows.size() != extras.size())
          throw std::runtime_error("the check answered " +
                                   std::to_string(rows.size()) + " steps for " +
                                   std::to_string(extras.size()));
        std::vector<sequence::TimelineStep> steps;
        for (size_t i = 0; i < rows.size(); i++) {
          const json& row = rows[i];
          const StepExtra& x = extras[i];
          sequence::TimelineStep s;
          s.occurrences = x.occurrences;
          s.base = row.value("base", false);
          if (!s.base && row.contains("path") &&
              row["path"].contains("moves")) {
            const json& o = row["path"]["origin_mm"];
            const json& m = row["path"]["moves"];
            s.point = {o[0].get<double>(), o[1].get<double>(),
                       o[2].get<double>()};
            s.moves = glm::normalize(glm::dvec3(
                m[0].get<double>(), m[1].get<double>(), m[2].get<double>()));
            s.travelMm = std::max(0.0, row.value("travel_mm", 0.0));
            // The turn it was given; else a thread's, a turn a pitch.
            s.turnDeg =
                x.turnDeg != 0.0 ? x.turnDeg
                : x.thread && x.pitch > 0.0
                    ? (x.rightHand ? 1.0 : -1.0) * 360.0 * s.travelMm / x.pitch
                    : 0.0;
          } else {
            s.base = true; // nothing to go on along: it is put in place
          }
          steps.push_back(std::move(s));
        }
        tl = std::make_shared<sequence::Timeline>(
            sequence::Time(std::move(steps), size));
      } else {
        error = "the sequence does not check: " + error;
      }
    } catch (const std::exception& e) {
      error = e.what();
    }
    raw->timeline = std::move(tl);
    raw->error = std::move(error);
    raw->done.store(true); // last
  });
  return p;
}

const sequence::Timeline* ShownSequence(const AppState& state) {
  const SequenceShow* sh = state.sequenceShow.get();
  if (sh == nullptr || !sh->showing || !sh->poses || !sh->poses->done.load() ||
      !sh->poses->error.empty() ||
      sh->poses->assembly != state.assembly.get()) // another tab's
    return nullptr;
  return sh->poses->timeline.get();
}

bool SequencePlaybackWorld(const AppState& state, uint64_t occurrenceId,
                           glm::dmat4& world) {
  const sequence::Timeline* tl = ShownSequence(state);
  if (tl == nullptr || !state.assembly)
    return false;
  const Occurrence* o = state.assembly->FindById(occurrenceId);
  if (o == nullptr)
    return false;
  const glm::dmat4 seat = OccurrenceMatrix(*o);
  world = sequence::PoseAt(*tl, occurrenceId, seat, state.sequenceShow->t)
              .value_or(seat);
  return true;
}

bool SequencePlaybackHides(const AppState& state, uint64_t occurrenceId) {
  const sequence::Timeline* tl = ShownSequence(state);
  if (tl == nullptr)
    return false;
  const auto it = tl->stepOf.find(occurrenceId);
  return it != tl->stepOf.end() &&
         state.sequenceShow->t < tl->steps[it->second].appear;
}

bool StartSequencePlayback(AppState& state, std::string* why) {
  StopMotionPlayback(state); // both place the same occurrences
  SequenceShow& sh = ShowOf(state);
  sh.refusal.clear();
  // The timeline at hand when the assembly is as it was checked, else a
  // check for one.
  if (!sh.poses || !Fresh(state, *sh.poses)) {
    std::string err;
    sh.poses = ResolveSequence(state, &err);
    if (!sh.poses) {
      sh.refusal = err;
      if (why)
        *why = err;
      return false;
    }
  } else if (sh.poses->done.load() && !sh.poses->error.empty()) {
    sh.refusal = sh.poses->error; // the same assembly checks the same way
    if (why)
      *why = sh.refusal;
    return false;
  }
  sh.showing = true;
  sh.playing = true;
  sh.pinned = false;
  sh.synced = false;
  sh.t = 0.0;
  sh.lastTick = -1.0;
  return true;
}

void StopSequencePlayback(AppState& state) {
  SequenceShow* sh = state.sequenceShow.get();
  if (sh == nullptr)
    return;
  if (sh->showing)
    state.syncedDocumentVersion = UINT64_MAX; // back to the placements
  sh->showing = false;
  sh->playing = false;
  sh->pinned = false;
  sh->t = 0.0;
  // A check still running for it is no longer wanted.
  if (sh->poses && !sh->poses->done.load())
    sh->poses.reset();
}

void SeekSequencePlayback(AppState& state, double t) {
  SequenceShow* sh = state.sequenceShow.get();
  const sequence::Timeline* tl = ShownSequence(state);
  if (sh == nullptr || tl == nullptr)
    return;
  sh->playing = false;
  sh->t = std::clamp(t, 0.0, tl->seconds);
  PushAll(state);
}

void TickSequencePlayback(AppState& state, double now) {
  SequenceShow* sh = state.sequenceShow.get();
  if (sh == nullptr || !sh->showing || sh->pinned)
    return; // a video moves a pinned one itself
  if (!sh->poses || !Fresh(state, *sh->poses)) {
    // An edit, or a tab switch: not the assembly it was made of.
    StopSequencePlayback(state);
    return;
  }
  if (!sh->poses->done.load())
    return; // still checking: the viewport shows the model meanwhile
  if (!sh->poses->error.empty()) {
    sh->refusal = sh->poses->error;
    StopSequencePlayback(state);
    return;
  }
  if (!sh->synced) {
    // Once: pickers and the parts not put in yet follow it.
    sh->synced = true;
    state.syncedDocumentVersion = UINT64_MAX;
    sh->lastTick = -1.0;
  }
  const double seconds = sh->poses->timeline->seconds;
  if (sh->playing && sh->lastTick >= 0.0) {
    const double dt = std::clamp(now - sh->lastTick, 0.0, 0.25);
    sh->t += dt;
    if (sh->t > seconds) // round again, from the base
      sh->t = seconds > 0.0 ? std::fmod(sh->t, seconds) : 0.0;
  }
  sh->lastTick = sh->playing ? now : -1.0;
  PushAll(state);
}

// --- The panel ------------------------------------------------------------

void DrawSequenceSection(AppState& state) {
  if (!state.assembly || state.assembly->GetSequence().Empty())
    return;
  const size_t n = state.assembly->GetSequence().steps.size();
  ImGui::Separator();
  ImGui::TextUnformatted("Assembly sequence");
  ImGui::SameLine();
  ImGui::TextDisabled("%zu steps", n);
  SequenceShow* sh = state.sequenceShow.get();
  const bool showing = sh && sh->showing;
  const bool checking = showing && sh->poses && !sh->poses->done.load();
  if (checking) {
    JobProgress bar;
    bar.status = "Checking the sequence to play it";
    bar.fraction = sh->poses->fraction.load();
    bar.cancelId = "###sequence_cancel";
    bar.cancelTip = "Stop checking; nothing is played.";
    if (DrawJobProgress(bar))
      StopSequencePlayback(state);
    return;
  }
  const sequence::Timeline* tl = ShownSequence(state);
  WrapRow row;
  const std::string playLabel =
      std::string(tl && sh->playing ? "Pause" : "Play") + "###sequence_play";
  row.Next(ButtonWidth(playLabel.c_str()));
  if (ImGui::SmallButton(playLabel.c_str())) {
    if (tl && sh->playing) {
      sh->playing = false;
    } else if (tl) {
      sh->playing = true;
      sh->lastTick = -1.0;
    } else {
      std::string why;
      StartSequencePlayback(state, &why);
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Put the assembly together in the viewport, step by "
                      "step, each part along the path the sequence check "
                      "took: nothing is solved or moved.");
  if (showing) {
    row.Next(ButtonWidth("Stop"));
    if (ImGui::SmallButton("Stop###sequence_stop"))
      StopSequencePlayback(state);
  }
  if (tl) {
    float t = static_cast<float>(sh->t);
    LeftLabel("Time");
    char fmt[48];
    std::snprintf(fmt, sizeof fmt, "%%.1f of %.1f s", tl->seconds);
    if (ImGui::SliderFloat("###sequence_t", &t, 0.0f,
                           static_cast<float>(tl->seconds), fmt))
      SeekSequencePlayback(state, t);
    // The step on show: the last one whose parts are out.
    size_t at = 0;
    for (size_t i = 0; i < tl->steps.size(); i++)
      if (tl->steps[i].appear <= sh->t)
        at = i;
    TextDisabledWrapped(("Step " + std::to_string(at + 1) + " of " +
                         std::to_string(tl->steps.size()))
                            .c_str());
  }
  if (sh && !sh->refusal.empty())
    TextDisabledWrapped(("Not played: " + sh->refusal).c_str());
}

// --- The video -------------------------------------------------------------

namespace {

// The sequence's timeline as the poses a video films.
class SequenceVideoPoses final : public VideoPoses {
public:
  explicit SequenceVideoPoses(std::shared_ptr<SequencePoses> p)
      : m_poses(std::move(p)) {}
  bool Ready() const override { return m_poses->done.load(); }
  std::string Refusal() const override {
    return m_poses->error.empty()
               ? std::string()
               : "the assembly sequence cannot be filmed: " + m_poses->error;
  }
  double Seconds() const override { return m_poses->timeline->seconds; }
  void Show(AppState& state, double fraction) override {
    SequenceShow& sh = ShowOf(state);
    if (sh.poses != m_poses || !sh.showing || !sh.pinned) {
      sh.poses = m_poses;
      sh.showing = true;
      sh.pinned = true;
      sh.playing = false;
      sh.synced = true;
      state.syncedDocumentVersion = UINT64_MAX; // a full resync reads it too
    }
    sh.t = std::clamp(fraction, 0.0, 1.0) * m_poses->timeline->seconds;
    PushAll(state);
  }
  bool Release(AppState& state) override {
    m_poses->cancel.store(true);
    SequenceShow* sh = state.sequenceShow.get();
    if (sh && sh->poses == m_poses && sh->pinned)
      StopSequencePlayback(state);
    return m_poses->done.load(); // the check stops at its next step
  }

private:
  std::shared_ptr<SequencePoses> m_poses;
};

} // namespace

bool StartAssemblyVideo(AppState& state, const MotionVideoRequest& req,
                        std::string* error, MotionVideoPlan* planOut) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (const std::string no = VideoJobRunningRefusal(state); !no.empty())
    return fail(no);
  if (!state.assembly)
    return fail("the active document is not an assembly");
  MotionVideoPlan plan;
  plan.studyName =
      state.activePart >= 0 &&
              static_cast<size_t>(state.activePart) < state.parts.size()
          ? state.parts[static_cast<size_t>(state.activePart)].name
          : std::string("assembly");
  // The length is the timeline's, known once the check has run.
  if (!PlanVideoFile(state, req, "export_assembly_video", -1.0, &plan, error))
    return false;
  // The timeline Play made, when the assembly is as it was checked; else
  // a check of its own, inside the job.
  std::shared_ptr<SequencePoses> poses;
  if (const SequenceShow* sh = state.sequenceShow.get();
      sh && sh->poses && sh->poses->done.load() && sh->poses->error.empty() &&
      Fresh(state, *sh->poses))
    poses = sh->poses;
  else
    poses = ResolveSequence(state, error);
  if (!poses)
    return false;
  if (planOut)
    *planOut = plan;
  return StartVideoJob(state, plan, req.frames,
                       std::make_shared<SequenceVideoPoses>(std::move(poses)),
                       "Assembly video '" + plan.studyName + "' to " +
                           std::filesystem::path(plan.path).filename().string(),
                       error);
}

} // namespace ui
