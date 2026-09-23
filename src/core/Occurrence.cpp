#include "core/Occurrence.h"
#include "core/Angles.h"
#include "core/fem/FemSolver.h" // StudyRuntime, StudyRuntimeCache

#include <algorithm>
#include <cmath>

uint64_t AssemblyDocument::AddOccurrence(Occurrence occ) {
  occ.id = m_nextId++;
  m_occurrences.push_back(std::move(occ));
  m_version++;
  return m_occurrences.back().id;
}

bool AssemblyDocument::RemoveOccurrence(uint64_t id) {
  const auto it =
      std::find_if(m_occurrences.begin(), m_occurrences.end(),
                   [id](const Occurrence& o) { return o.id == id; });
  if (it == m_occurrences.end())
    return false;
  m_occurrences.erase(it);
  m_version++;
  return true;
}

Occurrence* AssemblyDocument::FindById(uint64_t id) {
  for (Occurrence& o : m_occurrences)
    if (o.id == id)
      return &o;
  return nullptr;
}

const Occurrence* AssemblyDocument::FindById(uint64_t id) const {
  for (const Occurrence& o : m_occurrences)
    if (o.id == id)
      return &o;
  return nullptr;
}

uint64_t AssemblyDocument::AddMate(Mate mate) {
  mate.id = m_nextId++;
  m_mates.push_back(std::move(mate));
  m_version++;
  return m_mates.back().id;
}

bool AssemblyDocument::RemoveMate(uint64_t id) {
  const auto it = std::find_if(m_mates.begin(), m_mates.end(),
                               [id](const Mate& m) { return m.id == id; });
  if (it == m_mates.end())
    return false;
  m_mates.erase(it);
  m_version++;
  return true;
}

Mate* AssemblyDocument::FindMateById(uint64_t id) {
  for (Mate& m : m_mates)
    if (m.id == id)
      return &m;
  return nullptr;
}

const Mate* AssemblyDocument::FindMateById(uint64_t id) const {
  for (const Mate& m : m_mates)
    if (m.id == id)
      return &m;
  return nullptr;
}

uint64_t AssemblyDocument::AddExplodedView(ExplodedView view) {
  view.id = m_nextId++;
  m_explodedViews.push_back(std::move(view));
  m_version++;
  return m_explodedViews.back().id;
}

bool AssemblyDocument::RemoveExplodedView(uint64_t id) {
  const auto it =
      std::find_if(m_explodedViews.begin(), m_explodedViews.end(),
                   [id](const ExplodedView& v) { return v.id == id; });
  if (it == m_explodedViews.end())
    return false;
  m_explodedViews.erase(it);
  m_version++;
  return true;
}

ExplodedView* AssemblyDocument::FindExplodedViewById(uint64_t id) {
  for (ExplodedView& v : m_explodedViews)
    if (v.id == id)
      return &v;
  return nullptr;
}

const ExplodedView* AssemblyDocument::FindExplodedViewById(uint64_t id) const {
  for (const ExplodedView& v : m_explodedViews)
    if (v.id == id)
      return &v;
  return nullptr;
}

uint64_t AssemblyDocument::AddRelation(Relation relation) {
  relation.id = m_nextId++;
  m_relations.push_back(std::move(relation));
  m_version++;
  return m_relations.back().id;
}

bool AssemblyDocument::RemoveRelation(uint64_t id) {
  const auto it = std::find_if(m_relations.begin(), m_relations.end(),
                               [id](const Relation& r) { return r.id == id; });
  if (it == m_relations.end())
    return false;
  m_relations.erase(it);
  m_version++;
  return true;
}

Relation* AssemblyDocument::FindRelationById(uint64_t id) {
  for (Relation& r : m_relations)
    if (r.id == id)
      return &r;
  return nullptr;
}

const Relation* AssemblyDocument::FindRelationById(uint64_t id) const {
  for (const Relation& r : m_relations)
    if (r.id == id)
      return &r;
  return nullptr;
}

namespace {
// A periodic MONOTONE cubic (Fritsch-Carlson) through the table's points.
//
// Continuous in slope, which the loop solver's numeric Jacobian needs, AND
// incapable of overshooting the data — which a plain Catmull-Rom is not. The
// single-cylinder engine caught that: a 9 mm lobe with a long dwell made the
// spline dip 1.86 mm below zero between the lobe's close and its reopening,
// so a CLOSED valve travelled backwards into the head, and the motion study
// reported a 10.86 mm lift on a 9 mm cam
// (generators/README.md#single-cylinder-and-air-cooled-engines). A lift table
// is data, not a curve to be smoothed past its own values.
double CamAt(const std::vector<std::pair<double, double>>& t, double deg,
             double* slope) {
  const size_t n = t.size();
  const auto wrapDeg = [](double d) {
    d = std::fmod(d, 360.0);
    return d < 0.0 ? d + 360.0 : d;
  };
  deg = wrapDeg(deg);
  size_t i = 0;
  if (deg < t[0].first) {
    // BELOW the table's first angle, which is not "before the start" — the
    // profile is periodic, so it is inside the span that wraps past the end
    // of the turn. Leaving i at 0 here made u negative and the Hermite
    // cubic extrapolate backwards out of the interval its endpoints define:
    // a lobe whose table began at 74 degrees answered 60 mm of lift at 0,
    // on a cam whose whole rise is 8.
    i = n - 1;
    deg += 360.0;
  } else {
    while (i + 1 < n && t[i + 1].first <= deg)
      i++;
  }
  // Span i runs from point i to point i+1, wrapping past the end of the turn.
  const auto span = [&](size_t k) {
    const double a0 = t[k].first;
    const double a1 = (k + 1 < n) ? t[k + 1].first : t[0].first + 360.0;
    return a1 - a0;
  };
  const auto value = [&](long k) {
    const long m = static_cast<long>(n);
    return t[static_cast<size_t>(((k % m) + m) % m)].second;
  };
  const auto delta = [&](long k) {
    const long m = static_cast<long>(n);
    const size_t kk = static_cast<size_t>(((k % m) + m) % m);
    const double h = span(kk);
    return h <= 1e-12 ? 0.0 : (value(k + 1) - value(k)) / h;
  };
  // Tangents: the average of the neighbouring secants, then limited so the
  // interpolant cannot leave the interval its endpoints define.
  const auto tangent = [&](long k) {
    const double dPrev = delta(k - 1), dNext = delta(k);
    if (dPrev * dNext <= 0.0)
      return 0.0; // a turning point: flat, so no overshoot past it
    const double m = 0.5 * (dPrev + dNext);
    // Fritsch-Carlson: no more than three times either secant.
    const double lim = 3.0 * std::min(std::abs(dPrev), std::abs(dNext));
    return std::abs(m) > lim ? (m > 0 ? lim : -lim) : m;
  };
  const double h = span(i);
  if (h <= 1e-12) {
    if (slope)
      *slope = 0.0;
    return t[i].second;
  }
  const double u = (deg - t[i].first) / h;
  const double p0 = value(static_cast<long>(i));
  const double p1 = value(static_cast<long>(i) + 1);
  const double m0 = tangent(static_cast<long>(i)) * h;
  const double m1 = tangent(static_cast<long>(i) + 1) * h;
  const double u2 = u * u, u3 = u2 * u;
  const double out = (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 +
                     (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1;
  if (slope) {
    const double dvdu = (6 * u2 - 6 * u) * p0 + (3 * u2 - 4 * u + 1) * m0 +
                        (-6 * u2 + 6 * u) * p1 + (3 * u2 - 2 * u) * m1;
    // mm per radian. `dvdu / h` is mm per DEGREE (the table's angles are
    // degrees), so this scales a RATE rather than converting an angle —
    // hence the bare factor instead of angles::ToDegrees.
    *slope = dvdu / h * angles::kDegPerRad;
  }
  return out;
}

} // namespace

double CamLift(const std::vector<std::pair<double, double>>& table,
               double angleRad, double* slopeOut) {
  if (table.empty()) {
    if (slopeOut)
      *slopeOut = 0.0;
    return 0.0;
  }
  if (table.size() == 1) {
    if (slopeOut)
      *slopeOut = 0.0;
    return table[0].second;
  }
  return CamAt(table, angles::ToDegrees(angleRad), slopeOut);
}

uint64_t AssemblyDocument::AddAssertion(ClearanceAssertion a) {
  a.id = m_nextId++;
  m_assertions.push_back(std::move(a));
  m_version++;
  return m_assertions.back().id;
}

bool AssemblyDocument::RemoveAssertion(uint64_t id) {
  const auto it =
      std::find_if(m_assertions.begin(), m_assertions.end(),
                   [id](const ClearanceAssertion& a) { return a.id == id; });
  if (it == m_assertions.end())
    return false;
  m_assertions.erase(it);
  m_version++;
  return true;
}

ClearanceAssertion* AssemblyDocument::FindAssertionById(uint64_t id) {
  for (ClearanceAssertion& a : m_assertions)
    if (a.id == id)
      return &a;
  return nullptr;
}

uint64_t AssemblyDocument::AddMotionStudy(MotionStudy study) {
  study.id = m_nextId++;
  m_motionStudies.push_back(std::move(study));
  m_version++;
  return m_motionStudies.back().id;
}

bool AssemblyDocument::RemoveMotionStudy(uint64_t id) {
  const auto it =
      std::find_if(m_motionStudies.begin(), m_motionStudies.end(),
                   [id](const MotionStudy& s) { return s.id == id; });
  if (it == m_motionStudies.end())
    return false;
  m_motionStudies.erase(it);
  m_version++;
  return true;
}

MotionStudy* AssemblyDocument::FindMotionStudyById(uint64_t id) {
  for (MotionStudy& s : m_motionStudies)
    if (s.id == id)
      return &s;
  return nullptr;
}

const MotionStudy* AssemblyDocument::FindMotionStudyById(uint64_t id) const {
  for (const MotionStudy& s : m_motionStudies)
    if (s.id == id)
      return &s;
  return nullptr;
}

// ---- FEM studies (S1) -----------------------------------------------------

const fem::Study* AssemblyDocument::FindStudy(uint64_t id) const {
  for (const fem::Study& s : m_studies)
    if (s.id == id)
      return &s;
  return nullptr;
}

fem::Study* AssemblyDocument::FindStudy(uint64_t id) {
  for (fem::Study& s : m_studies)
    if (s.id == id)
      return &s;
  return nullptr;
}

fem::Study& AssemblyDocument::AddStudy(const std::string& name) {
  fem::Study study;
  study.id = m_nextId++;
  study.name =
      name.empty() ? ("Study " + std::to_string(m_studies.size() + 1)) : name;
  m_studies.push_back(std::move(study));
  m_version++;
  return m_studies.back();
}

bool AssemblyDocument::RemoveStudy(uint64_t id) {
  const auto it =
      std::find_if(m_studies.begin(), m_studies.end(),
                   [id](const fem::Study& s) { return s.id == id; });
  if (it == m_studies.end())
    return false;
  m_studies.erase(it);
  m_version++;
  return true;
}

void AssemblyDocument::RestoreStudy(fem::Study study) {
  EnsureNextIdAtLeast(study.id + 1);
  for (const fem::Load& l : study.loads)
    EnsureNextIdAtLeast(l.id + 1);
  m_studies.push_back(std::move(study));
}

const fem::StudyRuntime*
AssemblyDocument::StudyResults(uint64_t studyId) const {
  if (!m_studyRuntime)
    return nullptr;
  const auto it = m_studyRuntime->byStudy.find(studyId);
  return it == m_studyRuntime->byStudy.end() ? nullptr : &it->second;
}

void AssemblyDocument::StoreStudyResults(uint64_t studyId,
                                         fem::StudyRuntime rt) {
  if (!m_studyRuntime)
    m_studyRuntime = std::make_shared<fem::StudyRuntimeCache>();
  m_studyRuntime->byStudy[studyId] = std::move(rt);
  ++m_studyRuntime->epoch;
}

uint64_t AssemblyDocument::StudyResultsEpoch() const {
  return m_studyRuntime ? m_studyRuntime->epoch : 0;
}

bool AssemblyDocument::StudyResultsStale(
    uint64_t studyId,
    const std::function<uint64_t(uint64_t partId)>& partVersion) const {
  const fem::StudyRuntime* rt = StudyResults(studyId);
  if (!rt || rt->solvedAtVersion != m_version)
    return true;
  for (const auto& [partId, version] : rt->solvedPartVersions)
    if (!partVersion || partVersion(partId) != version)
      return true;
  return false;
}

// ---- Configurations (C4) --------------------------------------------------

const Configuration* AssemblyDocument::FindConfiguration(uint64_t id) const {
  for (const Configuration& c : m_configurations)
    if (c.id == id)
      return &c;
  return nullptr;
}

Configuration* AssemblyDocument::FindConfiguration(uint64_t id) {
  for (Configuration& c : m_configurations)
    if (c.id == id)
      return &c;
  return nullptr;
}

Configuration& AssemblyDocument::AddConfiguration(const std::string& name) {
  Configuration c;
  c.id = m_nextId++;
  c.name =
      name.empty()
          ? ("Configuration " + std::to_string(m_configurations.size() + 1))
          : name;
  m_configurations.push_back(std::move(c));
  m_version++;
  return m_configurations.back();
}

bool AssemblyDocument::RemoveConfiguration(uint64_t id) {
  const auto it =
      std::find_if(m_configurations.begin(), m_configurations.end(),
                   [id](const Configuration& c) { return c.id == id; });
  if (it == m_configurations.end())
    return false;
  m_configurations.erase(it);
  if (m_activeConfiguration == id)
    m_activeConfiguration = 0;
  m_version++;
  return true;
}

void AssemblyDocument::RestoreConfiguration(Configuration cfg) {
  EnsureNextIdAtLeast(cfg.id + 1);
  m_configurations.push_back(std::move(cfg));
}

bool AssemblyDocument::SetActiveConfiguration(uint64_t id) {
  if (id != 0 && FindConfiguration(id) == nullptr)
    return false;
  m_activeConfiguration = id;
  m_version++; // renderers/solve resync against the new variant
  return true;
}

const Configuration* AssemblyDocument::ActiveConfig() const {
  return m_activeConfiguration == 0 ? nullptr
                                    : FindConfiguration(m_activeConfiguration);
}

bool AssemblyDocument::OccurrenceSuppressed(uint64_t occurrenceId) const {
  const Configuration* c = ActiveConfig();
  return c != nullptr && c->Suppresses(occurrenceId);
}

bool AssemblyDocument::MateSuppressed(uint64_t mateId) const {
  const Configuration* c = ActiveConfig();
  if (c == nullptr)
    return false;
  if (c->SuppressesMate(mateId))
    return true;
  for (const Mate& m : m_mates)
    if (m.id == mateId)
      return c->Suppresses(m.a.occurrenceId) || c->Suppresses(m.b.occurrenceId);
  return false;
}

bool AssemblyDocument::RelationSuppressed(uint64_t relationId) const {
  const Configuration* c = ActiveConfig();
  if (c == nullptr)
    return false;
  if (c->SuppressesRelation(relationId))
    return true;
  for (const Relation& r : m_relations)
    if (r.id == relationId)
      return MateSuppressed(r.mateA) || MateSuppressed(r.mateB);
  return false;
}

const MateOverride* AssemblyDocument::MateOverrideFor(uint64_t mateId) const {
  const Configuration* c = ActiveConfig();
  return c != nullptr ? c->OverrideForMate(mateId) : nullptr;
}

bool AssemblyDocument::OccurrenceSuppressedIn(uint64_t configurationId,
                                              uint64_t occurrenceId) const {
  if (configurationId == 0)
    return OccurrenceSuppressed(occurrenceId);
  const Configuration* c = FindConfiguration(configurationId);
  if (c == nullptr)
    return OccurrenceSuppressed(
        occurrenceId); // deleted: fall back, don't break
  return c->Suppresses(occurrenceId);
}
