#include "ui/JobActions.h"

#include "core/cam/CamToolpath.h"

#include "ui/AppActions.h"
#include "ui/AppState.h"

#include <algorithm>

namespace ui {

cam::JobDocument* ActiveJob(AppState& state) { return state.job.get(); }

const cam::JobDocument* ActiveJob(const AppState& state) {
  return state.job.get();
}

Document* JobSourceDocument(AppState& state, uint64_t sourceDocId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].id != sourceDocId ||
        state.parts[i].kind != DocKind::Part)
      continue;
    return static_cast<int>(i) == state.activePart ? state.document.get()
                                                   : state.parts[i].doc.get();
  }
  return nullptr;
}

uint64_t JobSourceVersion(AppState& state, uint64_t sourceDocId) {
  const Document* doc = JobSourceDocument(state, sourceDocId);
  return doc ? doc->Version() : 0;
}

// What the assembled bodies depend on: which parts the job lists, and what
// version each of those parts is at. Mixed rather than summed so that a part
// gaining a version while another loses one is not a collision.
uint64_t JobBodiesKey(AppState& state, const cam::JobDocument& job) {
  uint64_t key = 1469598103934665603ull ^ job.Version();
  for (const cam::JobPart& part : job.GetParts()) {
    key = (key ^ part.sourceDocId) * 1099511628211ull;
    key = (key ^ JobSourceVersion(state, part.sourceDocId)) * 1099511628211ull;
  }
  return key;
}

const cam::Setup* ActiveJobSetup(AppState& state) {
  const cam::JobDocument* job = ActiveJob(state);
  if (job == nullptr || job->GetSetups().empty())
    return nullptr;
  for (const cam::Setup& setup : job->GetSetups())
    if (setup.id == state.camActiveSetup)
      return &setup;
  return &job->GetSetups().front();
}

const std::vector<Body>& JobPlacedBodies(AppState& state) {
  static const std::vector<Body> kNone;
  if (ActiveJob(state) == nullptr) {
    state.jobPlacedBodies.clear();
    state.jobPlacedKey = 0;
    return kNone;
  }
  const uint64_t key = JobRenderVersion(state);
  if (key == state.jobPlacedKey && !state.jobPlacedBodies.empty())
    return state.jobPlacedBodies;
  const cam::Setup* setup = ActiveJobSetup(state);
  const std::vector<Body>& bodies = JobBodies(state);
  state.jobPlacedBodies =
      setup != nullptr ? cam::BodiesInSetupFrame(*setup, bodies) : bodies;
  state.jobPlacedKey = key;
  return state.jobPlacedBodies;
}

uint64_t JobRenderVersion(AppState& state) {
  const cam::JobDocument* job = ActiveJob(state);
  if (job == nullptr)
    return 0;
  // The same mix JobBodies keys its cache on, plus everything that decides
  // where a body LANDS: the active setup, how it is fixtured, and every
  // placement. None of those bump the job's version -- the panel and the
  // picks write them in place -- so leaving them out meant laying a face up
  // redrew nothing at all, and the backplot was the only thing that moved.
  uint64_t key = JobBodiesKey(state, *job);
  const auto mix = [&key](double v) {
    key = (key ^ static_cast<uint64_t>(v * 4096.0)) * 1099511628211ull;
  };
  key = (key ^ state.camActiveSetup) * 1099511628211ull;
  for (const cam::Setup& setup : job->GetSetups()) {
    mix(setup.frame.x);
    mix(setup.frame.y);
    mix(setup.frame.z);
    mix(setup.frame.w);
    mix(setup.rotationA);
    for (const cam::Placement& p : setup.placements) {
      if (p.frame) {
        mix(p.frame->x);
        mix(p.frame->y);
        mix(p.frame->z);
        mix(p.frame->w);
      }
      key = (key ^ p.id) * 1099511628211ull;
      mix(p.pos.x);
      mix(p.pos.y);
      mix(p.rotZdeg);
      key = (key ^ (p.enabled ? 1u : 2u)) * 1099511628211ull;
    }
  }
  return key;
}

const std::vector<Body>& JobBodies(AppState& state) {
  static const std::vector<Body> kNone;
  const cam::JobDocument* job = ActiveJob(state);
  if (!job) {
    state.jobBodies.clear();
    state.jobBodiesKey = 0;
    return kNone;
  }
  const uint64_t key = JobBodiesKey(state, *job);
  if (key == state.jobBodiesKey && !state.jobBodies.empty())
    return state.jobBodies;
  state.jobBodies.clear();
  for (const cam::JobPart& part : job->GetParts()) {
    const Document* doc = JobSourceDocument(state, part.sourceDocId);
    if (!doc)
      continue; // a source that is gone is reported, never substituted
    for (const Body& b : doc->GetBodies()) {
      if (b.reference)
        continue; // a derived ghost is not stock to cut
      state.jobBodies.push_back(b);
      state.jobBodies.back().jobPartId = part.id;
    }
  }
  state.jobBodiesKey = key;
  return state.jobBodies;
}

cam::Target TargetFor(AppState& state, const ElementRef& ref, uint64_t partHint,
                      std::string* why) {
  cam::Target t;
  t.ref = ref;
  std::vector<uint64_t> owners;
  for (const Body& b : JobBodies(state)) {
    if (b.createdBy != ref.featureId || !b.elementMap.Resolve(ref.name))
      continue;
    if (std::find(owners.begin(), owners.end(), b.jobPartId) == owners.end())
      owners.push_back(b.jobPartId);
  }
  if (partHint != 0) {
    if (std::find(owners.begin(), owners.end(), partHint) != owners.end()) {
      t.jobPartId = partHint;
      return t;
    }
    if (why)
      *why = "that element is not on job part " + std::to_string(partHint);
    return t;
  }
  if (owners.size() == 1) {
    t.jobPartId = owners.front();
    return t;
  }
  if (why) {
    if (owners.empty()) {
      *why = "no part of this job owns '" + ref.name + "'";
    } else {
      // Named, not picked between: every candidate is plausible and cutting
      // the wrong one is scrap.
      std::string names;
      const cam::JobDocument* job = ActiveJob(state);
      for (uint64_t id : owners) {
        const cam::JobPart* p = job ? job->FindPart(id) : nullptr;
        names += (names.empty() ? "" : ", ") + std::to_string(id);
        if (p && !p->name.empty())
          names += " (" + p->name + ")";
      }
      *why = "'" + ref.name +
             "' is on more than one part of this job — say "
             "which with `part`: " +
             names;
    }
  }
  return t;
}

int FindJobForPart(AppState& state, uint64_t sourcePartId) {
  for (size_t i = 0; i < state.parts.size(); i++) {
    if (state.parts[i].kind != DocKind::Job)
      continue;
    const cam::JobDocument* job = static_cast<int>(i) == state.activePart
                                      ? state.job.get()
                                      : state.parts[i].job.get();
    if (job == nullptr)
      continue;
    for (const cam::JobPart& part : job->GetParts())
      if (part.sourceDocId == sourcePartId)
        return static_cast<int>(i);
  }
  return -1;
}

size_t OpenJobForPart(AppState& state, uint64_t sourcePartId) {
  const int existing = FindJobForPart(state, sourcePartId);
  if (existing >= 0) {
    SetActivePart(state, static_cast<size_t>(existing));
    return static_cast<size_t>(existing);
  }
  return CreateJobForPart(state, sourcePartId);
}

size_t CreateJobForPart(AppState& state, uint64_t sourcePartId,
                        const std::string& name) {
  std::string partName;
  for (const PartSlot& slot : state.parts)
    if (slot.id == sourcePartId)
      partName = slot.name;
  const size_t index =
      AddJob(state, name.empty() ? (partName.empty() ? std::string("Job")
                                                     : partName + " Job")
                                 : name);
  if (state.job)
    state.job->AddPart(sourcePartId, partName);
  return index;
}

} // namespace ui
