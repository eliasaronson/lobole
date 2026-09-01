#include "ui/JobActions.h"

#include "core/cam/CamToolpath.h"

#include "ui/AppActions.h"
#include "ui/AppState.h"

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

std::vector<Body> JobPlacedBodies(AppState& state) {
  const cam::Setup* setup = ActiveJobSetup(state);
  const std::vector<Body>& bodies = JobBodies(state);
  return setup != nullptr ? cam::BodiesInSetupFrame(*setup, bodies) : bodies;
}

uint64_t JobRenderVersion(AppState& state) {
  const cam::JobDocument* job = ActiveJob(state);
  if (job == nullptr)
    return 0;
  // The same mix JobBodies keys its cache on, plus the ACTIVE setup and its
  // placements: moving a part must redraw it, and a placement is not part of
  // the job's version.
  uint64_t key = JobBodiesKey(state, *job);
  for (const cam::Setup& setup : job->GetSetups())
    for (const cam::Placement& p : setup.placements) {
      key = (key ^ p.id) * 1099511628211ull;
      key = (key ^ static_cast<uint64_t>(p.pos.x * 1024.0)) * 1099511628211ull;
      key = (key ^ static_cast<uint64_t>(p.pos.y * 1024.0)) * 1099511628211ull;
      key =
          (key ^ static_cast<uint64_t>(p.rotZdeg * 1024.0)) * 1099511628211ull;
      key = (key ^ (p.enabled ? 1u : 2u)) * 1099511628211ull;
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
      state.jobBodies.push_back(b);
      state.jobBodies.back().jobPartId = part.id;
    }
  }
  state.jobBodiesKey = key;
  return state.jobBodies;
}

cam::Target TargetFor(AppState& state, const ElementRef& ref) {
  cam::Target t;
  t.ref = ref;
  for (const Body& b : JobBodies(state)) {
    if (b.createdBy != ref.featureId)
      continue;
    if (!b.elementMap.Resolve(ref.name))
      continue;
    t.jobPartId = b.jobPartId;
    break;
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
