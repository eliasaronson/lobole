// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ui/FeatureBuildJob.h"

#include "cad/CADKernel.h"
#include "common/Logger.h"
#include "core/Command.h"
#include "core/Document.h"
#include "core/HeavyBuild.h"
#include "ui/AppActionsInternal.h"
#include "ui/AppState.h"
#include "ui/BackgroundJobs.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <thread>

namespace ui {

namespace {

// What one `feature` job builds and how it ended. Shared by the worker,
// the finish and the poll.
struct Build {
  uint64_t slotId = 0;
  size_t hash = 0;
  std::string featureName;
  std::shared_ptr<heavy::Request> request;
  // The request this build answers (Document::PendingSerial). An edit
  // whose execution asked for it carries the same serial
  // (Command::CausedBuild); a user cancel rolls that edit back when it is
  // still the top of the part's undo stack.
  uint64_t serial = 0;
  bool superseded = false;             // cancelled by the poll: the key moved
  bool cancelApplied = false;          // a user cancel's document side is done
  std::optional<heavy::Result> result; // written by the worker before done
  std::string error;
};

// A part slot's document and undo stack. A document with no tabs yet (no
// EnsureParts) is the lone part, slot id 0.
struct Part {
  uint64_t slotId = 0;
  Document* doc = nullptr;
  CommandStack* commands = nullptr;
};

std::vector<Part> PartsOf(AppState& state) {
  std::vector<Part> out;
  if (state.parts.empty()) {
    if (state.document)
      out.push_back({0, state.document.get(), state.commands.get()});
    return out;
  }
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Part)
      continue;
    Document* doc = SlotDoc(state, i);
    if (doc == nullptr)
      continue;
    CommandStack* commands = static_cast<int>(i) == state.activePart
                                 ? state.commands.get()
                                 : state.parts[i].commands.get();
    out.push_back({state.parts[i].id, doc, commands});
  }
  return out;
}

std::optional<Part> PartBySlot(AppState& state, uint64_t slotId) {
  for (const Part& p : PartsOf(state))
    if (p.slotId == slotId)
      return p;
  return std::nullopt;
}

// The build a job runs, when `bg` is a `feature` job this module started.
std::map<const BackgroundJob*, std::shared_ptr<Build>>& Payloads() {
  static std::map<const BackgroundJob*, std::shared_ptr<Build>> m;
  return m;
}

std::shared_ptr<Build> PayloadOf(const BackgroundJob* bg) {
  const auto it = Payloads().find(bg);
  return it == Payloads().end() ? nullptr : it->second;
}

BackgroundJob* RunningJob(AppState& state, uint64_t jobId) {
  if (jobId == 0)
    return nullptr;
  BackgroundJob* bg = FindBackgroundJobById(state, jobId);
  return bg != nullptr && !bg->reaped ? bg : nullptr;
}

// The document side of a user's cancel: the edit that made the build is
// rolled back and discarded when it is still the last thing done to the
// part -- no undo step, the features and bytes as before, as a refused call
// leaves them; otherwise the feature is left not built (cancelled), with
// everything after it waiting, until something changes its inputs or a
// build is asked for (Document::RequestBuild).
void ApplyCancel(AppState& state, Build& build) {
  if (build.cancelApplied || build.superseded)
    return;
  build.cancelApplied = true;
  const std::optional<Part> part = PartBySlot(state, build.slotId);
  if (!part)
    return;
  Document& doc = *part->doc;
  auto& track = state.featureBuilds[build.slotId];
  if (Command* top =
          part->commands != nullptr ? part->commands->Top() : nullptr;
      top != nullptr && build.serial != 0 &&
      top->CausedBuild() == build.serial) {
    const std::string what = part->commands->UndoDescription();
    track.revertedStamp = top->Stamp();
    part->commands->RollbackSince(part->commands->UndoDepth() - 1, doc);
    LOG_INFO("Build of '{}' cancelled: the edit that made it ({}) is rolled "
             "back",
             build.featureName, what);
  } else {
    doc.CancelBuild(build.hash);
    LOG_INFO("Build of '{}' cancelled: it is left not built",
             build.featureName);
  }
  track.serialSeen = doc.PendingSerial();
}

bool Finish(AppState& state, BackgroundJob& bg,
            const std::shared_ptr<Build>& build) {
  Payloads().erase(&bg);
  auto& track = state.featureBuilds[build->slotId];
  if (track.jobId == bg.id)
    track.jobId = 0;
  // Paid for, whoever still wants it: the store keeps it for an undo, a
  // copy or a reopen whose key it is.
  if (build->result)
    heavy::Put(*build->result);
  if (build->superseded)
    return true;
  if (bg.cancel.load()) {
    ApplyCancel(state, *build);
    return true;
  }
  if (!build->error.empty())
    LOG_ERROR("Build of '{}' failed off the frame: {}", build->featureName,
              build->error);
  const std::optional<Part> part = PartBySlot(state, build->slotId);
  if (!part || !build->result)
    return true;
  Document& doc = *part->doc;
  const heavy::Request* pending = doc.PendingBuild();
  if (pending == nullptr || pending->hash != build->hash)
    return true; // the part moved on; the store has it for when it comes back
  doc.AdoptBuilt(*build->result);
  doc.Regenerate();
  track.serialSeen = doc.PendingSerial();
  LOG_INFO("Build of '{}' landed after {:.1f} s", build->featureName,
           std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         bg.started)
               .count());
  return true;
}

void Start(AppState& state, const Part& part) {
  std::shared_ptr<heavy::Request> request = part.doc->PendingBuildShared();
  // The worker's inputs are its own: OpenCASCADE writes to a builder's
  // arguments and the frame meshes the document's shapes.
  heavy::Detach(*request);
  auto build = std::make_shared<Build>();
  build->slotId = part.slotId;
  build->hash = request->hash;
  build->featureName = request->featureName;
  build->request = request;
  build->serial = part.doc->PendingSerial();
  auto bg = std::make_unique<BackgroundJob>();
  bg->kind = kFeatureBuildKind;
  bg->label = "Building '" + request->featureName + "'";
  bg->cancellable = true;
  bg->owner = 0; // an edit's build outlives the client that made it
  BackgroundJob* raw = bg.get();
  bg->thread = std::thread([build, raw] {
    // OCC's signal handler is per thread.
    ArmOccSignalHandlers();
    try {
      build->result = heavy::Run(*build->request, &raw->cancel, &raw->fraction);
    } catch (const std::exception& e) {
      build->error = e.what();
    } catch (...) {
      build->error = "the build threw";
    }
    raw->done.store(true); // last: everything above is visible first
  });
  bg->finish = [build, raw](AppState& s) { return Finish(s, *raw, build); };
  Payloads()[raw] = build;
  BackgroundJob& started = StartBackgroundJob(state, std::move(bg));
  state.featureBuilds[part.slotId].jobId = started.id;
  LOG_INFO("Building '{}' off the frame (estimated {:.1f} s)",
           request->featureName, request->estimateS);
}

} // namespace

void OptPartsIntoOffFrameBuilds(AppState& state) {
  for (const Part& p : PartsOf(state))
    p.doc->SetBuildsOffFrame(true);
}

void PollFeatureBuilds(AppState& state) {
  OptPartsIntoOffFrameBuilds(state);
  for (const Part& part : PartsOf(state)) {
    auto& track = state.featureBuilds[part.slotId];
    const uint64_t serial = part.doc->PendingSerial();
    track.serialSeen = serial;
    const heavy::Request* pending = part.doc->PendingBuild();
    BackgroundJob* bg = RunningJob(state, track.jobId);
    std::shared_ptr<Build> build = bg != nullptr ? PayloadOf(bg) : nullptr;
    if (build != nullptr) {
      if (bg->cancel.load() && !build->superseded) {
        ApplyCancel(state, *build);
        continue; // what the part asks for next is the next poll's
      }
      if (pending == nullptr || pending->hash != build->hash) {
        // Restart: the edit changed what the feature builds from, so this
        // build could only land as a cache entry nobody reads.
        build->superseded = true;
        bg->cancelReason = "superseded: an edit changed what it builds from";
        bg->cancel.store(true);
        track.jobId = 0;
        build = nullptr;
      } else {
        // The same evaluation, asked for again (a preview accepted while it
        // builds: removed and added back): it answers the newer request,
        // and so belongs to the edit that made that one.
        build->serial = serial;
        continue;
      }
    }
    if (pending != nullptr)
      Start(state, part);
  }
}

uint64_t FeatureBuildSlotOf(AppState& state, const Document* doc) {
  for (const Part& p : PartsOf(state))
    if (p.doc == doc)
      return p.slotId;
  return 0;
}

Document* FeatureBuildDoc(AppState& state, uint64_t slotId) {
  const std::optional<Part> p = PartBySlot(state, slotId);
  return p ? p->doc : nullptr;
}

CommandStack* FeatureBuildCommands(AppState& state, uint64_t slotId) {
  const std::optional<Part> p = PartBySlot(state, slotId);
  return p ? p->commands : nullptr;
}

bool FeatureBuildRunning(AppState& state, uint64_t slotId) {
  const auto it = state.featureBuilds.find(slotId);
  return it != state.featureBuilds.end() &&
         RunningJob(state, it->second.jobId) != nullptr;
}

bool CancelFeatureBuild(AppState& state, uint64_t slotId) {
  const auto it = state.featureBuilds.find(slotId);
  if (it == state.featureBuilds.end())
    return false;
  BackgroundJob* bg = RunningJob(state, it->second.jobId);
  if (bg == nullptr)
    return false;
  bg->cancelReason = "cancelled";
  bg->cancel.store(true);
  return true;
}

bool SettleFeatureBuilds(AppState& state, double seconds) {
  const auto end =
      std::chrono::steady_clock::now() +
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(seconds));
  for (;;) {
    PollBackgroundJobs(state);
    bool any = false;
    for (const Part& p : PartsOf(state))
      if (p.doc->PendingBuild() != nullptr)
        any = true;
    if (!any)
      return true;
    if (std::chrono::steady_clock::now() > end)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

std::string FeatureBuildStatus(AppState& state) {
  if (!state.document)
    return {};
  const uint64_t slot = FeatureBuildSlotOf(state, state.document.get());
  const auto it = state.featureBuilds.find(slot);
  if (it == state.featureBuilds.end())
    return {};
  BackgroundJob* bg = RunningJob(state, it->second.jobId);
  if (bg == nullptr)
    return {};
  const double s = std::chrono::duration<double>(
                       std::chrono::steady_clock::now() - bg->started)
                       .count();
  std::string out =
      bg->label + " (" + std::to_string(static_cast<int>(s)) + " s";
  if (const double f = bg->fraction.load(); f >= 0.0)
    out +=
        ", " + std::to_string(static_cast<int>(std::lround(100.0 * f))) + "%";
  return out + ")";
}

} // namespace ui
