#include "ui/JobActions.h"

#include "core/cam/CamToolpath.h"

#include "ui/AppState.h"
#include "ui/DocumentSlots.h"
#include "ui/DrawingActions.h" // ConfiguredPartDocument

#include "cad/CADKernel.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

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
  // Editing a configuration the job cuts regenerates it and moves no
  // Version.
  return doc ? doc->Version() ^
                   (doc->ConfigurationsRevision() * 0x9E3779B97F4A7C15ull)
             : 0;
}

// What the assembled bodies depend on: which parts the job lists, and what
// version each of those parts is at. Mixed rather than summed so that a part
// gaining a version while another loses one is not a collision.
uint64_t JobBodiesKey(AppState& state, const cam::JobDocument& job) {
  uint64_t key = 1469598103934665603ull ^ job.Version();
  for (const cam::JobPart& part : job.GetParts()) {
    key = (key ^ part.sourceDocId) * 1099511628211ull;
    key = (key ^ part.configuration) * 1099511628211ull;
    key = (key ^ JobSourceVersion(state, part.sourceDocId)) * 1099511628211ull;
  }
  return key;
}

// Ask each body for its TIGHT bounding box once, here, where the answer
// survives.
//
// `CADShape` memoizes that box -- it is expensive, `BRepBndLib::AddOptimal`
// evaluates the surfaces -- but the memo is a member of the SHAPE, and
// `cam::BodiesInSetupFrame` returns its bodies BY VALUE. So every caller that
// frames bodies on its way to a bound warmed the memo on a temporary and then
// destroyed it, leaving the originals here cold; the next frame did it all
// again. On a plain part nobody noticed. On a six-flute B-spline twist it was
// 514 ms of a 520 ms frame, every frame, with the app otherwise IDLE --
// `AddOptimal` at 28% of the profile, reached from `StockBoundsFramed`,
// `ModelBoundsFramed` and the panel's machine lines.
//
// Warming once when the bodies are built costs what one frame used to and
// then costs nothing, because a copy of a shape carries the memo with it.
void WarmTightBounds(const std::vector<Body>& bodies) {
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 lo(0.0f), hi(0.0f);
    b.shape.GetBoundingBox(lo, hi, /*tight=*/true);
  }
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
  // Framed bodies are NEW shapes when the setup is not square, so their memo
  // is cold even though the ones they came from are warm.
  WarmTightBounds(state.jobPlacedBodies);
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
    // A seated fixture follows the STOCK, and editing the stock bumps no
    // version: without its definition here, a clamp would stay drawn where
    // the old stock put it (docs/cam/setup.md#seats).
    key = (key ^ static_cast<uint64_t>(setup.stock.mode)) * 1099511628211ull;
    mix(setup.stock.margin.x);
    mix(setup.stock.margin.y);
    mix(setup.stock.margin.z);
    mix(setup.stock.bottomMargin);
    mix(setup.stock.size.x);
    mix(setup.stock.size.y);
    mix(setup.stock.size.z);
    mix(setup.stock.place.x);
    mix(setup.stock.place.y);
    mix(setup.stock.place.z);
    // Fixtures are drawn too, and their parts can be edited like any other.
    for (const cam::Fixture& f : setup.fixtures) {
      key = (key ^ f.id) * 1099511628211ull;
      if (f.seat) {
        key = (key ^ static_cast<uint64_t>(f.seat->edge) ^ 0x5ea7u) *
              1099511628211ull;
        key = (key ^ f.seat->face.featureId) * 1099511628211ull;
        key = (key ^ std::hash<std::string>{}(f.seat->face.name)) *
              1099511628211ull;
        mix(f.seat->alongMm);
        mix(f.seat->overMm);
        mix(f.seat->spinDeg);
      }
      key = (key ^ JobSourceVersion(state, f.sourceDocId)) * 1099511628211ull;
      mix(f.pos.x);
      mix(f.pos.y);
      mix(f.pos.z);
      mix(f.rotZdeg);
      key = (key ^ (f.enabled ? 1u : 2u)) * 1099511628211ull;
    }
  }
  return key;
}

std::vector<Body> FixtureModelBodies(AppState& state, const cam::Fixture& f) {
  std::vector<Body> out;
  if (f.IsBox()) {
    if (f.box.x <= 0.0 || f.box.y <= 0.0 || f.box.z <= 0.0)
      return out;
    // One shape per size, kept: the renderer's tessellation cache is keyed on
    // the SHAPE, so a fresh box every resync would be meshed every resync.
    static std::map<std::tuple<double, double, double>, CADShape> boxes;
    const auto key = std::make_tuple(f.box.x, f.box.y, f.box.z);
    auto it = boxes.find(key);
    if (it == boxes.end())
      it = boxes
               .emplace(key, CADKernel::Instance().CreateBox(glm::vec3(0.0f),
                                                             glm::vec3(f.box)))
               .first;
    Body b;
    b.name = f.name;
    b.shape = it->second;
    out.push_back(std::move(b));
    return out;
  }
  if (const Document* doc = JobSourceDocument(state, f.sourceDocId))
    for (const Body& b : doc->GetBodies())
      if (!b.reference && b.shape.IsValid())
        out.push_back(b);
  return out;
}

glm::dmat4 FixtureBedTransform(AppState& state, const cam::Setup& setup,
                               const cam::Fixture& f, std::string* why) {
  if (!f.seat)
    return cam::FixtureTransform(f);
  // Re-solved against the stock as it is NOW, on every read: that is the
  // point of a seat. A clamp placed on the board stays on the board when a
  // margin or the parts change the stock (docs/cam/setup.md#seats).
  glm::dvec3 lo(0.0), hi(0.0);
  std::string reason = "there is no stock to rest it on";
  cam::Fixture solved = f;
  if (cam::StockBounds(setup, JobBodies(state), lo, hi) &&
      cam::SolveFixtureSeat(lo, hi, FixtureModelBodies(state, f), *f.seat,
                            solved.pos, solved.rotZdeg, &reason))
    return cam::FixtureTransform(solved);
  if (why != nullptr)
    *why = reason;
  return cam::FixtureTransform(f); // the pose it last solved to
}

void RefreshFixtureSeats(AppState& state, cam::Setup& setup) {
  for (cam::Fixture& f : setup.fixtures) {
    state.camSeatRefusals.erase(f.id);
    if (!f.seat)
      continue;
    glm::dvec3 lo(0.0), hi(0.0), pos(0.0);
    double rot = 0.0;
    std::string why = "there is no stock to rest it on";
    if (cam::StockBounds(setup, JobBodies(state), lo, hi) &&
        cam::SolveFixtureSeat(lo, hi, FixtureModelBodies(state, f), *f.seat,
                              pos, rot, &why)) {
      f.pos = pos;
      f.rotZdeg = rot;
    } else {
      state.camSeatRefusals[f.id] = why;
    }
  }
}

std::optional<cam::VerdictLine> FixtureTravelLine(AppState& state,
                                                  const cam::Setup& setup,
                                                  const cam::Verdict& v) {
  if (v.outsideTravel == 0 || v.outsideAxis != 'Z')
    return std::nullopt;
  glm::dvec3 lo(0.0), hi(0.0);
  if (!cam::StockBounds(setup, JobBodies(state), lo, hi))
    return std::nullopt;
  // The tallest fixture standing above the stock is the one safe Z cleared.
  std::string name;
  double tallest = hi.z;
  for (const cam::FixtureSolid& f : JobFixtureSolids(state, setup))
    for (const Body& b : f.bodies) {
      glm::vec3 blo, bhi;
      b.shape.GetBoundingBox(blo, bhi, /*tight=*/true);
      if (bhi.z > tallest) {
        tallest = bhi.z;
        name = f.name;
      }
    }
  if (name.empty())
    return std::nullopt;
  return cam::VerdictLine{
      cam::VerdictLine::Level::Warn,
      "Safe Z is " +
          std::to_string(static_cast<int>(std::lround(setup.safeZ))) +
          " mm because it clears fixture \"" + name + "\" (its top is " +
          std::to_string(static_cast<int>(std::lround(tallest - hi.z))) +
          " mm above the stock): that is what asks for more Z than the "
          "machine has. Lower the fixture, or hold the work lower in it."};
}

std::vector<cam::FixtureSolid>
JobFixtureSolids(AppState& state, const cam::Setup& setup, bool enabledOnly,
                 std::vector<std::string>* missing) {
  std::vector<cam::FixtureSolid> out;
  for (const cam::Fixture& f : setup.fixtures) {
    if (enabledOnly && !f.enabled)
      continue;
    std::vector<Body> bodies = FixtureModelBodies(state, f);
    if (bodies.empty()) {
      if (missing != nullptr)
        missing->push_back(f.name);
      continue;
    }
    // Bed space straight from the part's own coordinates: never through
    // BodiesInSetupFrame, whose frame turns the PART and not the vise.
    const glm::mat4 m(FixtureBedTransform(state, setup, f));
    for (Body& b : bodies)
      b.shape = b.shape.Transformed(m);
    out.push_back({f.id, f.name, std::move(bodies)});
  }
  return out;
}

double DeriveJobSafeZ(AppState& state, const cam::Setup& setup) {
#ifdef HAVE_CAM
  std::vector<Body> fixtureBodies;
  for (cam::FixtureSolid& f : JobFixtureSolids(state, setup))
    for (Body& b : f.bodies)
      fixtureBodies.push_back(std::move(b));
  return cam::DeriveSafeZ(setup, JobBodies(state), fixtureBodies);
#else
  // The derivation is CAM's (CamDerive.cpp), and only CAM calls this; a
  // build without it keeps what the job file says.
  (void)state;
  return setup.safeZ;
#endif
}

const Document* JobPartDocument(AppState& state, const cam::JobPart& part,
                                std::string* why) {
  if (part.configuration == 0) {
    const Document* doc = JobSourceDocument(state, part.sourceDocId);
    if (!doc && why)
      *why = "its source part is gone";
    return doc;
  }
  return ConfiguredPartDocument(state, part.sourceDocId, part.configuration,
                                why);
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
    const Document* doc = JobPartDocument(state, part);
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
  WarmTightBounds(state.jobBodies);
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

// The active setup of the active job.
//
// It lived in CamPanel.cpp, inside that file's `#ifdef HAVE_CAM`, which
// made a job tab's placement gizmo unbuildable without a toolpath
// generator. It reads the job document and nothing else, and a job
// document is a DocKind in every build.
cam::Setup* ActiveCamSetup(AppState& state) {
  if (!state.job)
    return nullptr;
  std::vector<cam::Setup>& setups = state.job->GetSetups();
  if (setups.empty())
    return nullptr;
  for (cam::Setup& s : setups)
    if (s.id == state.camActiveSetup)
      return &s;
  state.camActiveSetup = setups.front().id;
  return &setups.front();
}

} // namespace ui
