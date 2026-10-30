// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/Document.h"
#include "cad/OccSignals.h"

#include "core/ElementOrigin.h"
#include "core/FitBands.h"
#include "core/HeavyBuild.h"
#include "core/features/CradleFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/RetainingRingGrooveFeature.h"
#include "core/features/SketchFeature.h"

#include "common/Logger.h"
#include "core/io/Serialization.h" // FeatureToJson: the regen-cache hash input

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

static bool PrintDrivesGeometry(const Feature& f);
// The part's print direction (printset::Settings::direction, unset = +Z),
// unit: what a printed hole's roof points along.
static glm::dvec3 PrintUp(const printset::PartSettings& s) {
  const glm::dvec3 d = s.part.direction.value_or(glm::dvec3(0.0, 0.0, 1.0));
  const double len = glm::length(d);
  return len > 1e-12 ? d / len : glm::dvec3(0.0, 0.0, 1.0);
}
// A Hole printed as a teardrop reads the print direction.
static bool PrintUpDrivesGeometry(const Feature& f) {
  const auto* hole = dynamic_cast<const HoleFeature*>(&f);
  return hole != nullptr && hole->printed != HolePrinted::Round;
}

// A face a later feature MODIFIES keeps its name with ";M<tag>" appended
// (core/ElementHistory.h), and a thread recorded on it before -- a Thread, a
// tapped Hole -- still named it the old way: a chamfer on the pan/tilt base
// plate's tripod hole left the thread mill finding "no thread" on its bore.
// So after each feature, a mark whose face no longer resolves follows it to
// the one face whose name extends it. Two (a face split in two) is not one:
// the mark is left as it is, and its reader says the thread is gone.
void FollowModifiedFaces(std::vector<Body>& bodies) {
  const auto follow = [](const ElementMap& map, ElementRef& face) {
    if (map.Resolve(face.name))
      return;
    const std::string stem = face.name + ";M";
    const std::string* only = nullptr;
    for (const auto& [index, name] : map.names) {
      if (name.compare(0, stem.size(), stem) != 0)
        continue;
      if (only != nullptr)
        return;
      only = &name;
    }
    if (only != nullptr)
      face.name = *only;
  };
  for (Body& b : bodies) {
    for (ThreadMark& t : b.threads)
      follow(b.elementMap, t.face);
    for (ThreadRegion& t : b.threadRegions)
      follow(b.elementMap, t.face);
    for (RingGrip& g : b.ringGrips)
      follow(b.elementMap, g.face);
    for (HoleMark& h : b.holes)
      follow(b.elementMap, h.bore);
  }
}

// Does `later` name what became of the face `name` through later features
// alone — `name` plus whole `;M<tag>` steps, each possibly split
// (topo::DescentOf)? A face appearance follows every piece a cut made of it,
// where a seat (AssemblySolve) must tell a split apart.
static bool DescendsByModification(const std::string& name,
                                   const std::string& later) {
  const topo::Descent d = topo::DescentOf(name, later);
  return d == topo::Descent::Modified || d == topo::Descent::Split;
}

// Injected slowdowns, for verifying the performance harness and never for
// anything else (docs/performance/README.md#injected-slowdowns): a slowdown
// planted on purpose is how `tools/bench.py compare` is shown to see one.
// LOBOLE_TEST_REGEN_DELAY_MS sleeps that long per executed feature;
// LOBOLE_TEST_NO_REGEN_CACHE=1 caps the regen cache at zero, so only the
// current chain survives an eviction and an undo re-executes. Read once; the
// log says when either is on, and the harness records it.
static int TestRegenDelayMs() {
  static const int ms = [] {
    const char* env = std::getenv("LOBOLE_TEST_REGEN_DELAY_MS");
    const int v = env ? std::max(0, std::atoi(env)) : 0;
    if (v > 0)
      LOG_WARN("LOBOLE_TEST_REGEN_DELAY_MS: every executed feature sleeps "
               "{} ms (a planted slowdown)",
               v);
    return v;
  }();
  return ms;
}

static bool TestNoRegenCache() {
  static const bool on = [] {
    const char* env = std::getenv("LOBOLE_TEST_NO_REGEN_CACHE");
    const bool v = env != nullptr && env[0] != '\0' && std::string(env) != "0";
    if (v)
      LOG_WARN("LOBOLE_TEST_NO_REGEN_CACHE: the regen cache is capped at zero "
               "(a planted slowdown)");
    return v;
  }();
  return on;
}

// Runs a feature, and warns on it when a reference it holds resolved BY
// POSITION: a split piece's ".<i>" name saved before pieces were named by
// what bounds them (docs/modeling/toponaming.md#resolving-a-name). It still
// means what it meant, but a reorder of the pieces would move it.
static bool ExecuteNoting(Feature& feature, RegenContext& ctx) {
  PositionalLookups positional;
  if (const int ms = TestRegenDelayMs(); ms > 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  const bool ok = feature.Execute(ctx);
  if (ok && !positional.Names().empty()) {
    std::set<std::string> seen;
    std::string names;
    for (const std::string& n : positional.Names())
      if (seen.insert(n).second)
        names += (names.empty() ? "'" : ", '") + n + "'";
    const std::string text =
        "references " + names +
        " by position (a split piece named before pieces were named by what "
        "bounds them): re-pick it, or a change to the cut that split it can "
        "move it to another piece";
    const std::string& had = feature.GetWarning();
    feature.SetWarning(had.empty() ? text : had + "; " + text);
  }
  return ok;
}

// Monotonic for the life of the process: identity must never be reused, or
// the consumers of InstanceId() are back to guessing.
uint64_t Document::NextInstanceId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

std::unique_ptr<Document> Document::CopyWithGeometry(const Document& other) {
  return std::unique_ptr<Document>(new Document(other, /*reuseGeometry=*/true));
}

Document::Document(const Document& other) : Document(other, false) {}

void Document::AdoptEvaluations(const Document& other) {
  for (const auto& [hash, entry] : other.m_cache)
    if (entry.bodiesValid && entry.executed != nullptr &&
        m_cache.count(hash) == 0) {
      RegenCacheEntry adopted = entry;
      adopted.lastUse = ++m_regenClock;
      m_cache.emplace(hash, std::move(adopted));
    }
}

namespace {
thread_local bool t_buildOffFrameScope = false;
} // namespace

Document::BuildOffFrameScope::BuildOffFrameScope()
    : m_saved(t_buildOffFrameScope) {
  t_buildOffFrameScope = true;
}
Document::BuildOffFrameScope::~BuildOffFrameScope() {
  t_buildOffFrameScope = m_saved;
}
bool Document::BuildOffFrameScopeHeld() { return t_buildOffFrameScope; }

void Document::AdoptBuilt(const heavy::Result& result) {
  if (result.hash == 0 || result.executed == nullptr)
    return;
  m_cache.insert_or_assign(result.hash,
                           RegenCacheEntry{result.bodiesAfter, true,
                                           result.executed, ++m_regenClock});
}

void Document::CancelBuild(size_t hash) {
  if (hash == 0)
    return;
  m_cancelledBuilds.insert(hash);
  Regenerate();
}

bool Document::RequestBuild(uint64_t featureId) {
  if (BuildStateOf(featureId) != BuildState::Cancelled)
    return false;
  // The mark is on the evaluation, which this regeneration is about to ask
  // for again: drop every mark, since only the one it stops at matters and
  // any other is an evaluation no history reaches now.
  m_cancelledBuilds.clear();
  Regenerate();
  return true;
}

// The one copy implementation, behind both public forms. A list of members,
// not a `= default`, for two reasons: the features are owning pointers and
// must be CLONED, and m_instanceId must NOT come along (it is left to its
// initializer, which mints a fresh one).
//
// Anything absent below is absent on purpose — it is cache, and the
// Regenerate() at the end rebuilds it: m_bodies, the regen cache,
// m_tipHash, m_lastStats. m_version starts at 0 like any other new document.
// The baked-geometry hand-off is the one exception, and only under
// `reuseGeometry`: it is set precisely so that Regenerate has something to
// install instead of rebuilding (see CopyWithGeometry).
//
// A new PERSISTENT member added to this class must be added here too, and
// tests/test_document_copy.cpp is what says so: it saves an original and its
// copy and diffs the two files, so a field that copying forgets shows up as a
// diff rather than as a duplicated tab quietly missing something. It runs
// that same diff over BOTH copy forms.
Document::Document(const Document& other, bool reuseGeometry)
    : m_assets(other.m_assets), m_parameters(other.m_parameters),
      m_tolerances(other.m_tolerances), m_studies(other.m_studies),
      // Solved results cost minutes and are not derivable from the feature
      // list, so they travel with the copy (re-stamped below).
      m_studyRuntime(other.m_studyRuntime),
      m_configurations(other.m_configurations), m_units(other.m_units),
      m_activeConfiguration(other.m_activeConfiguration),
      m_symbols(other.m_symbols),
      // Non-owning scope pointers into the project that owns both documents.
      // A copy taken outside a project simply carries the same nulls.
      m_projectSymbols(other.m_projectSymbols),
      m_projectParameters(other.m_projectParameters), m_nextId(other.m_nextId),
      m_rollback(other.m_rollback), m_commitGeometry(other.m_commitGeometry) {
  // A copy is built whole, inline: it is a trial, a snapshot or a
  // configured view, and nobody starts a job for it
  // (docs/ui/long-operations.md#heavy-features).
  m_buildsOffFrame = false;
  m_generator = other.m_generator;
  m_sheetMetal = other.m_sheetMetal; // every flange's thickness comes from it
  m_tube = other.m_tube;             // what check_frame and the cut list read
  m_print = other.m_print;           // what the fit checks model a print at
  m_printSettings = other.m_printSettings; // what the slicer is handed
  m_tables = other.m_tables;         // a lone part's project tables
  m_templates = other.m_templates;   // ...and its template library
  m_features.reserve(other.m_features.size());
  for (const std::unique_ptr<Feature>& f : other.m_features)
    m_features.push_back(f->Clone());
  // Hand the source's own bodies to Regenerate as baked geometry (see
  // CopyWithGeometry). The features are clones of the ones those bodies were
  // built from, so the chain hash matches and nothing executes; if it does
  // not match, Regenerate ignores the blob and rebuilds. Warnings come along
  // because nothing runs to raise them again — the same reason a baked open
  // carries them.
  if (reuseGeometry && other.m_tipHash != 0) {
    m_bakedBodies = other.m_bodies;
    m_bakedHash = other.m_tipHash;
    for (const std::unique_ptr<Feature>& f : other.m_features)
      if (!f->GetWarning().empty())
        m_bakedWarnings[f->GetId()] = f->GetWarning();
  }
  Regenerate();
  // Study results are stamped with the version they were solved at, and this
  // document's versions are its own — so the numbers have to be re-stamped or
  // every copied result reads stale. Only the ones that were CURRENT on the
  // original are stamped current here: the copy's geometry is the original's
  // geometry, so a result that matched then matches now. A stale one keeps a
  // version this document will never hold (0, never a post-regen version) and
  // goes on reading stale, which is the honest answer — laundering it into a
  // fresh-looking result is exactly what this codebase refuses to do.
  for (auto& [id, rt] : m_studyRuntime)
    rt.solvedAtVersion = other.StudyResultsStale(id) ? 0 : m_version;
}

bool Document::StudyResultsStale(uint64_t studyId) const {
  auto it = m_studyRuntime.find(studyId);
  if (it == m_studyRuntime.end() || it->second.solvedAtVersion != m_version)
    return true;
  const fem::Study* s = FindStudy(studyId);
  if (s == nullptr || s->shapeFrom.study == 0 || s->shapeFrom.study == studyId)
    return false;
  const auto src = m_studyRuntime.find(s->shapeFrom.study);
  return src == m_studyRuntime.end() ||
         src->second.solvedAtVersion != m_version ||
         src->second.densityDigest != it->second.sourceDigest;
}

bool Document::MarkStudyResultsCurrent(uint64_t studyId) {
  const auto it = m_studyRuntime.find(studyId);
  if (it == m_studyRuntime.end())
    return false;
  it->second.solvedAtVersion = m_version;
  return true;
}

Feature* Document::GetFeature(size_t index) {
  return index < m_features.size() ? m_features[index].get() : nullptr;
}

void Document::AddFeature(std::unique_ptr<Feature> feature) {
  InsertFeature(std::move(feature), m_features.size());
}

void Document::InsertFeature(std::unique_ptr<Feature> feature, size_t index) {
  if (index > m_features.size()) {
    index = m_features.size();
  }
  // Assign a stable id, or respect one carried in (file load, RPC, undo
  // re-insertion) while keeping the counter ahead of it.
  if (feature->GetId() == 0) {
    feature->SetId(m_nextId++);
  } else {
    m_nextId = std::max(m_nextId, feature->GetId() + 1);
  }
  // ...and ahead of the ids it owns (a Template instance's features), so
  // nothing added later can be given one of them.
  feature->VisitOwnedIds(
      [this](uint64_t id) { m_nextId = std::max(m_nextId, id + 1); });
  m_features.insert(m_features.begin() + index, std::move(feature));
  Regenerate(); // deferred while a BulkEdit guard is held
}

Feature* Document::FindFeatureById(uint64_t id, size_t* outIndex) {
  if (id == 0)
    return nullptr;
  for (size_t i = 0; i < m_features.size(); i++) {
    if (m_features[i]->GetId() == id) {
      if (outIndex)
        *outIndex = i;
      return m_features[i].get();
    }
  }
  return nullptr;
}

std::unique_ptr<Feature> Document::RemoveFeature(size_t index) {
  if (index >= m_features.size()) {
    return nullptr;
  }
  std::unique_ptr<Feature> removed = std::move(m_features[index]);
  m_features.erase(m_features.begin() + index);
  Regenerate(); // deferred while a BulkEdit guard is held
  return removed;
}

bool Document::MoveFeature(size_t from, size_t to) {
  if (from >= m_features.size() || to >= m_features.size() || from == to) {
    return false;
  }
  // Erase-then-insert-at-`to` gives the natural drag behavior in both
  // directions: dragging down lands the feature after the drop target,
  // dragging up lands it before (like ImGui's reorderable-list example).
  std::unique_ptr<Feature> moved = std::move(m_features[from]);
  m_features.erase(m_features.begin() + from);
  m_features.insert(m_features.begin() + to, std::move(moved));
  Regenerate();
  return true;
}

bool Document::ReplaceFeature(size_t index, std::unique_ptr<Feature> feature) {
  if (index >= m_features.size() || !feature) {
    return false;
  }
  // Replacing a feature changes what it IS, never which one it is. A
  // replacement built from JSON that carries no "id" (the modify_feature RPC
  // sends parameters, not identity) would otherwise land with id 0 — writing
  // `0-box.json`, listing 0 in the index, and dangling every reference that
  // named the old id. Stable, never-reused ids are the contract the whole
  // directory format and merge story rest on (F1/F3/F7).
  if (feature->GetId() == 0) {
    feature->SetId(m_features[index]->GetId());
  }
  m_features[index] = std::move(feature);
  Regenerate();
  return true;
}

int Document::FirstConsumerIndex(uint64_t featureId) const {
  if (featureId == 0)
    return -1;
  size_t own = m_features.size();
  for (size_t i = 0; i < m_features.size(); i++) {
    if (m_features[i]->GetId() == featureId) {
      own = i;
      break;
    }
  }
  if (own == m_features.size())
    return -1; // no such feature
  std::set<uint64_t> ids = {featureId};
  m_features[own]->VisitOwnedIds([&](uint64_t id) { ids.insert(id); });
  for (size_t i = own + 1; i < m_features.size(); i++) {
    bool consumes = false;
    m_features[i]->VisitFeatureRefs([&](uint64_t id) {
      if (ids.contains(id))
        consumes = true;
    });
    if (consumes)
      return static_cast<int>(i);
  }
  return -1;
}

int Document::OwnerIndex(uint64_t id) const {
  if (id == 0)
    return -1;
  for (size_t i = 0; i < m_features.size(); i++) {
    bool owns = m_features[i]->GetId() == id;
    if (!owns)
      m_features[i]->VisitOwnedIds([&](uint64_t o) { owns = owns || o == id; });
    if (owns)
      return static_cast<int>(i);
  }
  return -1;
}

bool Document::Regenerate() {
  // Deferred while a BulkEdit guard is held: a loader adds N features (and
  // sets rollback, parameters, the active configuration) and gets exactly one
  // regeneration when the guard releases, instead of one per mutation.
  if (m_bulkDepth > 0)
    return true;

  EvaluateParameters();

  m_version++;
  bool allOk = true;
  m_lastStats = {};
  // What this regeneration leaves not built (a heavy feature it stopped at,
  // and what follows it) is decided afresh; the request survives only if it
  // asks for the same evaluation again.
  m_unbuilt.clear();
  const size_t pendingBefore = m_pending ? m_pending->hash : 0;
  std::shared_ptr<heavy::Request> pendingKept = std::move(m_pending);
  m_pending.reset();
  bool stopped = false;
  const auto settlePending = [&] {
    const size_t now = m_pending ? m_pending->hash : 0;
    if (now != pendingBefore || (now == 0) != (pendingBefore == 0))
      m_pendingSerial++;
  };

  // Appearance stamp (v54): each body carries its creating feature's visual
  // override. A POST-pass keyed on createdBy (rather than plumbing through
  // every Execute signature) — run at every successful exit below, so cached
  // and baked bodies re-stamp too. This is what lets the chain hash STRIP
  // appearance: a recolor re-stamps here without rebuilding any B-Rep.
  //
  // Face overrides (v164) resolve here too, by element name against each
  // body the feature created — including faces a later cut made, which the
  // body's ElementMap names — and follow a face a later feature modified or
  // split (DescendsByModification) to what it became. A name that resolves on
  // none of them is recorded on the feature and reported, never dropped. A
  // feature with no body this time (suppressed, rolled back, failed) reports
  // nothing: its faces are not stale, just absent, and a failure says so
  // itself.
  const auto stampAppearance = [this] {
    std::map<uint64_t, std::set<std::string>> resolved;
    std::set<uint64_t> hasBody;
    for (Body& b : m_bodies) {
      b.faceAppearances.clear();
      const Feature* f = FindFeatureById(b.createdBy);
      if (!f)
        continue;
      b.appearance = f->GetAppearance();
      hasBody.insert(b.createdBy);
      for (const FaceAppearance& fa : f->GetFaceAppearances()) {
        if (const std::optional<int> face = b.elementMap.Resolve(fa.element)) {
          b.faceAppearances[*face] = fa;
          resolved[b.createdBy].insert(fa.element);
          continue;
        }
        // A later feature modified the face (a pocket in a painted top
        // renames it "…;M7") or cut it in pieces: the paint follows it.
        for (const auto& [index, name] : b.elementMap.names)
          if (DescendsByModification(fa.element, name)) {
            b.faceAppearances.try_emplace(index, fa);
            resolved[b.createdBy].insert(fa.element);
          }
      }
    }
    for (const auto& f : m_features) {
      std::vector<std::string> stale;
      if (hasBody.count(f->GetId()) != 0)
        for (const FaceAppearance& fa : f->GetFaceAppearances())
          if (resolved[f->GetId()].count(fa.element) == 0)
            stale.push_back(fa.element);
      f->SetUnresolvedFaceLooks(std::move(stale));
    }
  };
  // Material, for the same reason and by the same key. It used to be stamped
  // only where a body is CREATED, which meant a recolour changed the feature
  // JSON, changed the chain hash, and re-ran every boolean after it: setting
  // one material on the intake manifold cost 42.7 s, and on the cylinder
  // head 17.5 s, for a change its own handler describes as "PBR/albedo but
  // not geometry". Features that work their material out from something
  // other than m_material say so and are left alone.
  const auto stampMaterial = [this] {
    // A feature that stamps its own material does so in Execute, and a
    // material edit is a CACHE HIT (material is out of the chain hash), so
    // Execute does not run: a walnut chess piece derived from the piece part
    // kept the source's empty material and weighed nothing. Its own
    // material, when it has one, is stamped here like everyone else's; a
    // Derived with none takes its source body's back.
    //
    // A feature that RE-OWNS a body (Feature::MaterialSource: a Boolean, a
    // body pattern or mirror, a Split) and sets no material of its own
    // continues its source's, followed back through a chain of them: a steel
    // block unioned with a boss and then subtracted from stays steel, and so
    // does it when the steel is set after the booleans exist, which is a
    // cache hit no Boolean executes for. nullopt where the chain ends at a
    // feature that works its material out itself (a Derived, a Fastener) or
    // at a feature this document does not hold: what Execute stamped, which
    // took the source body's, stands.
    const auto materialOf =
        [this](const Feature* f) -> std::optional<std::string> {
      for (size_t hops = 0; f != nullptr && hops <= m_features.size(); hops++) {
        if (!f->GetMaterial().empty())
          return f->GetMaterial();
        const uint64_t source = f->MaterialSource();
        if (source == 0)
          return f->StampsOwnMaterial() ? std::nullopt
                                        : std::optional<std::string>("");
        f = FindFeatureById(source);
      }
      return std::nullopt;
    };
    for (Body& b : m_bodies) {
      const Feature* f = FindFeatureById(b.createdBy);
      if (f == nullptr) {
        // A body a Template instance's feature made: the instance's own
        // material, when it has one, over what the template saved.
        const int owner = OwnerIndex(b.createdBy);
        if (owner >= 0 && !m_features[owner]->GetMaterial().empty())
          b.material = m_features[owner]->GetMaterial();
        continue;
      }
      if (!f->StampsOwnMaterial() || !f->GetMaterial().empty()) {
        if (const std::optional<std::string> m = materialOf(f))
          b.material = *m;
        continue;
      }
      // The copy carries the source body's name (DerivedFeature::Execute).
      if (const auto* d = dynamic_cast<const DerivedFeature*>(f))
        for (const DerivedBody& sb : d->snapshot)
          if (sb.name == b.name) {
            b.material = sb.material;
            break;
          }
    }
  };

  // Rollback bar: only the prefix regenerates; the tail keeps its params but
  // contributes nothing (and holds no stale errors).
  const int marker = EffectiveRollback();
  const size_t upTo =
      marker < 0 ? m_features.size()
                 : std::min(m_features.size(), static_cast<size_t>(marker));
  for (size_t i = upTo; i < m_features.size(); i++) {
    m_features[i]->ClearError();
    m_features[i]->ClearWarning();
    // Its object no longer holds an evaluation: rolling forward restores
    // the cached one, errors and all.
    m_features[i]->SetDerivedFor(0);
  }

  // Refresh expression-driven fields before hashing: the serialized feature
  // (with evaluated values) is exactly the state that determines outputs
  // (the F1 contract), so it is what the cache keys on. Failed evaluations
  // keep the field's last good value as the RENDERED fallback, but — Phase V
  // fail-loud — surface the failure as a feature error instead of silently
  // drilling the literal twin (the "unknown variable" 1 mm-hole bug). Applied
  // after the execute loop below (a rebuilt feature ClearError()s first), so
  // the message survives; it is also why it must cover cached features whose
  // unchanged literal never re-executes.
  std::vector<std::string> exprErr(upTo);
  for (size_t i = 0; i < upTo; i++) {
    m_features[i]->VisitExprs([&, i](std::string& text, double& value) {
      if (text.empty()) {
        return;
      }
      const auto v = ExpressionEngine::Default().EvaluateText(text, m_symbols);
      if (v.has_value()) {
        value = *v;
      } else if (exprErr[i].empty()) {
        // The ExprError message distinguishes the case ("unknown variable
        // 'bore'" vs. a parse / division error); first failure per feature
        // wins.
        exprErr[i] = v.error().message;
      }
    });
  }

  // Incremental regeneration (R3): chain hashes make any change invalidate
  // everything after it — editing feature k re-runs [k, N) at most. Precise
  // dependency tracking is deliberately v2: a matching hash implies identical
  // inputs all the way down, hence an identical evaluation, which a hit
  // restores (feature state included, Feature::CopyDerivedState).
  std::vector<size_t> hashes(upTo);
  size_t chain = 0x9e3779b97f4a7c15ull;
  // The sheet-metal rules seed the chain rather than sitting in any one
  // feature's JSON: every sheet feature reads the gauge, so changing it
  // rebuilds all of them. Seeding is how "invalidate everything" is spelled
  // in a chained cache — and the absent case must hash as absent, or a part
  // that never was sheet metal would pick up a different tip hash and rebuild
  // once for nothing.
  if (m_sheetMetal) {
    const sheet::SheetMetalRules& r = *m_sheetMetal;
    chain = std::hash<std::string>{}(
        std::to_string(chain) + "|sheet|" + r.material + "|" + r.gauge + "|" +
        std::to_string(r.thickness) + "|" + std::to_string(r.bendRadius) + "|" +
        std::to_string(r.kFactor) + "|" +
        sheet::ReliefStyleName(r.reliefStyle) + "|" +
        std::to_string(r.reliefWidth) + "|" + std::to_string(r.reliefDepth));
  }
  const size_t seed = chain;
  // Feature i's link of the chain, from the link before it.
  const auto link = [&](size_t before, size_t i) {
    // Fold in the active configuration's effective suppression: the feature
    // JSON encodes only the authored `suppressed` flag, so a config that
    // suppresses feature i (without changing any field value) must still move
    // the cache divergence to i. Parameter overrides invalidate for free —
    // they change the evaluated field values already baked into the JSON.
    // Reference images (v49) are cosmetic sketch underlays contributing no
    // geometry — strip them from the hash input so toggling opacity / moving an
    // image never invalidates the cache and rebuilds downstream features.
    nlohmann::json fj = DocumentIO::FeatureToJson(*m_features[i]);
    fj.erase("images");
    fj.erase("next_image_id");
    // Appearance and material are cosmetic to the GEOMETRY (both stamp
    // passes reapply them after any cache hit): keep them out of the hash so
    // a recolour never rebuilds. Material still reaches physics — the BOM and
    // FEM read Body::material, which the stamp sets.
    fj.erase("appearance");
    fj.erase("face_appearance");
    fj.erase("material");
    // An import is WHAT it imports, not where the file sits: a save adopts
    // staged files into the project directory and rewrites the path, which
    // must not read as a changed part (every Derived copy of an imported
    // part went stale after the first save). A file that cannot be read
    // keeps its path in the hash, so it still differs from one that can.
    if (const auto* imp =
            dynamic_cast<const ImportFeature*>(m_features[i].get()))
      if (const size_t content = imp->ContentHash(); content != 0) {
        fj.erase("path");
        fj["content_hash"] = content;
      }
    // A clearance-fitted Hole on a printed part is modelled at the fit plus
    // the process's allowance (HoleFeature::Execute), so the process is one
    // of ITS inputs — and only its: a part with no such hole keeps its
    // hashes whatever the process. The rows' CONTENT, not the names: a
    // profile's gap edited from a coupon (v169) is the same name with a
    // different number, and must rebuild what was modelled from the old one.
    // A sketch standing on a face, a datum plane, another sketch or a frame
    // works its plane out in Execute and writes it back: an OUTPUT, so it is
    // not keyed on. Kept in, the key carried the plane of whichever history
    // last executed the sketch, and a value set back missed from that sketch
    // on (Execute refuses rather than fall back to it when the reference
    // fails to resolve, so leaving it out cannot make two inputs one).
    if (const auto* sk =
            dynamic_cast<const SketchFeature*>(m_features[i].get())) {
      if (sk->faceRef.IsSet() || sk->planeRef != 0)
        fj.erase("plane");
      // A section's drawing is an OUTPUT too: the cut of the bodies before
      // it, refitted every regeneration
      // (docs/sketching/README.md#a-section-into-a-sketch). Keyed on, an
      // upstream edit set back missed, because the entities still held the
      // edited cut.
      std::set<uint32_t> drawn;
      for (const sketch::ExternalGeom& e : sk->external)
        if (e.kind == sketch::ExternalKind::Section)
          drawn.insert(e.genIds.begin(), e.genIds.end());
      if (!drawn.empty()) {
        nlohmann::json kept = nlohmann::json::array();
        for (const nlohmann::json& e :
             fj.value("entities", nlohmann::json::array()))
          if (!e.contains("id") || !e.at("id").is_number_unsigned() ||
              drawn.count(e.at("id").get<uint32_t>()) == 0)
            kept.push_back(e);
        fj["entities"] = std::move(kept);
        for (nlohmann::json& e : fj["external"])
          if (e.value("kind", std::string()) == "section")
            e.erase("gen_ids");
      }
    }
    if (m_print && PrintDrivesGeometry(*m_features[i]))
      fj["print"] = printfit::Fingerprint(*m_print);
    // A teardrop's roof points up the print direction: an input of the
    // printed holes alone.
    if (PrintUpDrivesGeometry(*m_features[i])) {
      const glm::dvec3 up = PrintUp(m_printSettings);
      fj["print_up"] = {up.x, up.y, up.z};
    }
    return std::hash<std::string>{}(
        std::to_string(before) + fj.dump() +
        (EffectiveSuppressed(i) ? "|cfg-suppressed" : ""));
  };
  for (size_t i = 0; i < upTo; i++) {
    chain = link(chain, i);
    hashes[i] = chain;
  }
  m_tipHash = upTo > 0 ? hashes[upTo - 1] : 0;
  // A feature whose evaluation writes into what it is keyed on what its
  // inputs fix (Feature::RecordsWhatItIsKeyedOn: a direct edit records the
  // fingerprint of an imported face the first time it sees it,
  // docs/modeling/README.md#direct-edit) is keyed afresh on what it now
  // says, and so is every link after it, the evaluation filed under both
  // keys. Keyed only on what it said before, the next regeneration missed
  // and built it, and everything after it, again: an Offset Face on a
  // 291-face import, twice.
  const auto rekey = [&](size_t i) {
    if (!m_features[i]->RecordsWhatItIsKeyedOn())
      return;
    const size_t now = link(i == 0 ? seed : hashes[i - 1], i);
    if (now == hashes[i])
      return;
    if (const auto it = m_cache.find(hashes[i]); it != m_cache.end())
      m_cache.insert_or_assign(now, it->second);
    m_features[i]->SetDerivedFor(now);
    hashes[i] = now;
    for (size_t j = i + 1; j < upTo; j++)
      hashes[j] = link(hashes[j - 1], j);
  };

  // Baked geometry: if the blob the loader adopted describes exactly this
  // feature list, install it and execute nothing. The regen cache holds no
  // evaluation of it, so the first edit rebuilds from the start — the trade in
  // docs/versioning/README.md#baked-geometry. Either way the hand-off is
  // consumed here, so a later edit can never resurrect it.
  if (!m_bakedBodies.empty()) {
    std::vector<Body> baked = std::move(m_bakedBodies);
    const size_t bakedHash = m_bakedHash;
    std::map<uint64_t, std::string> bakedWarnings = std::move(m_bakedWarnings);
    std::string bakedSource = std::move(m_bakedSource);
    m_bakedSource.clear();
    m_bakedBodies.clear();
    m_bakedHash = 0;
    m_bakedWarnings.clear();
    if (bakedHash != 0 && bakedHash == m_tipHash && m_cache.empty()) {
      m_bodies = std::move(baked);
      m_bodiesFromBlob = bakedHash;
      m_bodiesBlobSource = std::move(bakedSource);
      m_lastStats.cached = upTo;
      // A bake from before the marks followed their faces holds the old
      // names.
      FollowModifiedFaces(m_bodies);
      // Seed the regen cache so a LATER regeneration (the UI syncs, the RPC
      // layer, an unrelated document-version bump) is free too — without it,
      // the next Regenerate found an empty cache and rebuilt everything the
      // blob had just saved us. Only the tip entry carries bodies; the rest
      // are hashes with bodiesValid=false, which is what makes an edit
      // anywhere in the history fall back to a full rebuild.
      //
      // None of them holds a clone: nothing executed, so each object's
      // derived state is what a bake leaves (none, a datum's below), and
      // DerivedFor says the entry may be used only while that stays so.
      m_cache.clear();
      m_chain = hashes;
      for (size_t i = 0; i + 1 < upTo; i++)
        m_cache[hashes[i]] = {{}, /*bodiesValid=*/false, nullptr, 0};
      if (upTo > 0)
        m_cache[hashes[upTo - 1]] = {m_bodies, /*bodiesValid=*/true, nullptr,
                                     0};
      for (size_t i = 0; i < upTo; i++)
        m_features[i]->SetDerivedFor(hashes[i]);
      // Restore the advice the features gave when this geometry was built:
      // nothing executed, so nothing would have raised it again.
      for (const auto& [id, text] : bakedWarnings) {
        if (Feature* f = FindFeatureById(id))
          f->SetWarning(text);
      }
      // ...but a DATUM's output is not a body, so the bake does not carry it
      // (Feature::IsDatum). Execute those against the bodies just installed —
      // in list order, so a frame offset from another frame reads a resolved
      // parent. Skipping this left every datum at its default: a mate
      // anchored to a Frame resolved to the identity and placed the part at
      // the origin, on a document that had just been saved correct.
      size_t datums = 0;
      bool datumsOk = true;
      for (size_t i = 0; i < upTo; i++) {
        if (!m_features[i]->IsDatum() || EffectiveSuppressed(i))
          continue;
        m_features[i]->ClearError();
        RegenContext ctx{m_bodies,  m_features,
                         i,         &m_symbols,
                         &m_assets, m_sheetMetal ? &*m_sheetMetal : nullptr};
        ctx.configuration = ActiveConfig();
        ctx.print = m_print ? &*m_print : nullptr;
        ctx.printUp = PrintUp(m_printSettings);
        datums++;
        if (!ExecuteNoting(*m_features[i], ctx)) {
          if (m_features[i]->GetError().empty())
            m_features[i]->SetError("regeneration failed");
          datumsOk = false;
        }
      }
      m_lastStats.executed = datums;
      m_lastStats.cached = upTo - datums;
      LOG_INFO("Loaded baked geometry ({} bodies): {} features, {} datums "
               "executed, {} advisories restored",
               m_bodies.size(), upTo, datums, bakedWarnings.size());
      stampAppearance();
      stampMaterial();
      fitband::Stamp(*this, m_bodies);
      settlePending();
      return datumsOk;
    }
    LOG_INFO("Baked geometry does not match the recipe; rebuilding");
  }

  // Each feature is a lookup of its chain hash, so a hit is a hit wherever
  // the entry came from: the current history, or one an undo or a value set
  // back has just returned to. Once the chain diverges it cannot re-converge
  // (the hash folds everything before it), so a run is hits up to the edit,
  // then all hits (a state seen before) or all misses.
  //
  // `input` is feature i's input: m_bodies, or the entry that last hit,
  // copied out only when something executes or at the end (a copy per hit
  // would be a copy of every element map per feature). Null after a bake's
  // placeholder, which knows its hash but not its bodies: a feature that
  // must execute there cannot, and the pass restarts with every feature
  // executing, as a bake's first edit always has.
  bool chainOk = true;
  for (bool executeAll : {false, true}) {
    chainOk = true;
    m_lastStats = {};
    m_bodies.clear();
    const std::vector<Body>* input = &m_bodies;
    bool anyFailedYet = false;
    bool restart = false;
    for (size_t i = 0; i < upTo && !restart; i++) {
      Feature& f = *m_features[i];
      const size_t h = hashes[i];
      const bool suppressed = EffectiveSuppressed(i);
      const auto it = executeAll ? m_cache.end() : m_cache.find(h);
      // A clone restores whatever the object last executed; without one the
      // object itself must already be this evaluation (the bake's entries).
      if (it != m_cache.end() &&
          (suppressed || it->second.executed != nullptr ||
           f.DerivedFor() == h)) {
        RegenCacheEntry& e = it->second;
        bool restored = false;
        e.lastUse = ++m_regenClock;
        if (suppressed) {
          f.ClearError();
          f.ClearWarning();
        } else if (e.executed != nullptr && f.DerivedFor() != h) {
          f.CopyDerivedState(*e.executed);
          restored = true;
        } else if (e.executed != nullptr) {
          f.CopyRegenStatus(*e.executed);
        }
        f.SetDerivedFor(h);
        input = e.bodiesValid ? &e.bodiesAfter : nullptr;
        // The evaluation restored may be one that recorded what the object
        // had not (another document's, AdoptEvaluations).
        if (restored)
          rekey(i);
        m_lastStats.cached++;
        // Errors from the evaluation that made the entry still count
        // against the result.
        if (!f.GetError().empty()) {
          chainOk = false;
          anyFailedYet = true;
        }
        continue;
      }
      if (input == nullptr) {
        restart = true;
        break;
      }
      if (input != &m_bodies) {
        m_bodies = *input;
        input = &m_bodies;
      }
      // A heavy feature (Feature::MayBuildOffFrame): a build another
      // document, a job or an earlier history already paid for is installed
      // like a hit; a document that builds off the frame stops here and asks
      // for one (docs/ui/long-operations.md#heavy-features).
      const bool heavyHere =
          !suppressed &&
          heavy::BuildsOffFrame(f, m_bodies, m_print ? &*m_print : nullptr);
      if (heavyHere) {
        if (std::optional<heavy::Result> built = heavy::Find(h)) {
          m_bodiesFromBlob = 0;
          m_bodiesBlobSource.clear();
          m_bodies = built->bodiesAfter;
          f.CopyDerivedState(*built->executed);
          f.SetDerivedFor(h);
          m_cache.insert_or_assign(
              h,
              RegenCacheEntry{m_bodies, true, built->executed, ++m_regenClock});
          rekey(i);
          m_lastStats.cached++;
          if (!f.GetError().empty()) {
            f.SetErrorAfterEarlierFailure(anyFailedYet);
            chainOk = false;
            anyFailedYet = true;
          }
          continue;
        }
        if (m_buildsOffFrame) {
          f.ClearError();
          f.ClearWarning();
          f.SetDerivedFor(0);
          const bool cancelled = m_cancelledBuilds.count(h) != 0;
          m_unbuilt[f.GetId()] =
              cancelled ? BuildState::Cancelled : BuildState::Building;
          for (size_t j = i + 1; j < upTo; j++) {
            m_features[j]->ClearError();
            m_features[j]->ClearWarning();
            m_features[j]->SetDerivedFor(0);
            m_unbuilt[m_features[j]->GetId()] = BuildState::Waiting;
          }
          if (!cancelled) {
            if (pendingKept && pendingKept->hash == h &&
                pendingKept->featureId == f.GetId()) {
              pendingKept->index = i;
              m_pending = std::move(pendingKept);
            } else {
              auto req = std::make_shared<heavy::Request>();
              req->index = i;
              req->featureId = f.GetId();
              req->featureName = f.GetName();
              req->hash = h;
              req->estimateS =
                  f.BuildCostS(m_bodies, m_print ? &*m_print : nullptr);
              req->feature = f.Clone();
              req->input = m_bodies;
              req->symbols = m_symbols;
              if (const Configuration* cfg = ActiveConfig())
                req->configuration = *cfg;
              req->sheetMetal = m_sheetMetal;
              req->print = m_print;
              req->printUp = PrintUp(m_printSettings);
              // The features it names, as RegenContext::FindById would
              // resolve them here, in history order.
              RegenContext refCtx{m_bodies,   m_features, i,
                                  &m_symbols, &m_assets,  nullptr};
              refCtx.configuration = ActiveConfig();
              std::set<uint64_t> ids;
              f.VisitFeatureRefs([&](uint64_t id) { ids.insert(id); });
              for (size_t k = 0; k < i; k++)
                if (ids.count(m_features[k]->GetId()) != 0 &&
                    refCtx.FindById(m_features[k]->GetId()) != nullptr)
                  req->refs.push_back(m_features[k]->Clone());
              m_pending = std::move(req);
            }
          }
          LOG_INFO("Feature '{}' {} off the frame; {} feature(s) after it "
                   "wait",
                   f.GetName(),
                   cancelled ? "is not built (its build was cancelled)"
                             : "builds",
                   upTo - i - 1);
          stopped = true;
          break;
        }
      }
      m_bodiesFromBlob = 0; // executed: the bodies are this build's now
      m_bodiesBlobSource.clear();
      f.ClearError();
      f.ClearWarning();
      bool faulted = false;
      if (!suppressed) {
        RegenContext ctx{m_bodies,  m_features,
                         i,         &m_symbols,
                         &m_assets, m_sheetMetal ? &*m_sheetMetal : nullptr};
        ctx.configuration = ActiveConfig();
        ctx.print = m_print ? &*m_print : nullptr;
        ctx.printUp = PrintUp(m_printSettings);
        m_lastStats.executed++;
        const unsigned faultsBefore = occsig::FaultCount();
        const bool ok = ExecuteNoting(f, ctx);
        faulted = occsig::FaultCount() != faultsBefore;
        if (faulted) {
          // A fault long-jumped out of the kernel's frames: what the cache
          // holds was built on the same heap. Drop it all, and do not cache
          // this evaluation either.
          m_kernelFaulted.insert(f.GetId());
          m_cache.clear();
          LOG_WARN("Feature '{}': a kernel fault was caught ({} this "
                   "session); the regen cache is dropped",
                   f.GetName(), occsig::FaultCount());
        } else {
          m_kernelFaulted.erase(f.GetId());
        }
        if (!ok) {
          if (f.GetError().empty()) {
            f.SetError("regeneration failed");
          }
          // A failure downstream of another failure is usually collateral —
          // tag it so clients can point at the first red feature.
          f.SetErrorAfterEarlierFailure(anyFailedYet);
          anyFailedYet = true;
          // Continue past the failure (R1): the broken feature contributes
          // nothing; downstream features see the bodies as of this point
          // and fail on their own if they referenced its outputs.
          LOG_ERROR("Feature '{}' failed to regenerate: {}", f.GetName(),
                    f.GetError());
          chainOk = false;
        }
        FollowModifiedFaces(m_bodies);
      }
      f.SetDerivedFor(h);
      if (!faulted) {
        m_cache.insert_or_assign(
            h, RegenCacheEntry{m_bodies, true,
                               suppressed
                                   ? nullptr
                                   : std::shared_ptr<const Feature>(f.Clone()),
                               ++m_regenClock});
        if (!suppressed)
          rekey(i);
        // A heavy feature built inline (a copy, a test, a worker's load):
        // filed where every other document of the session finds it.
        if (heavyHere) {
          heavy::Put({h, m_bodies, m_cache.at(h).executed});
          if (hashes[i] != h)
            heavy::Put({hashes[i], m_bodies, m_cache.at(h).executed});
        }
      }
    }
    if (!restart && input != nullptr) {
      if (input != &m_bodies)
        m_bodies = *input;
      break;
    }
    // Only a bake's placeholders can get here, and a full rebuild replaces
    // every one of them.
    LOG_INFO("Regeneration resumes from baked geometry; rebuilding");
  }
  if (!chainOk)
    allOk = false;
  // A list that stopped at a heavy feature has no tip: nothing may bake it
  // or hand its bodies to a copy as the recipe's answer.
  m_tipHash = upTo > 0 && !stopped ? hashes[upTo - 1] : 0;
  settlePending();
  // The current chain. Entries past the rollback bar stay in it while the
  // features before the bar are unchanged, so rolling forward is instant.
  if (m_chain.size() < upTo ||
      !std::equal(hashes.begin(), hashes.end(), m_chain.begin()))
    m_chain = hashes;
  EvictRegenCache();

  // Fail-loud (Phase V): stamp the expression-resolution failures collected in
  // the refresh pass. Applied here — after Execute — so a rebuilt feature's
  // ClearError()/regen error takes precedence and this only fills in features
  // that regenerated cleanly on their stale literal (the visible-but-rendering
  // case). The feature error is transient (not serialized, not hashed), so it
  // never perturbs the cache.
  for (size_t i = 0; i < upTo; i++) {
    if (!exprErr[i].empty() && m_features[i]->GetError().empty() &&
        m_unbuilt.count(m_features[i]->GetId()) == 0) {
      m_features[i]->SetError(exprErr[i]);
      allOk = false;
    }
  }
  stampAppearance();
  stampMaterial();
  // The size bands its round faces carry (core/FitBands.h): read from the
  // tolerances and the print process, which are outside the chain hash, so
  // like the appearance they are stamped here and not by a feature.
  fitband::Stamp(*this, m_bodies);
  return allOk;
}

// The current chain plus about two alternatives for the whole list, more
// for edits low in it. An entry costs what its feature made and nothing it
// passed through (bodies are shared shapes), so this bounds memory at a few
// times one full chain.
size_t Document::RegenCacheCap() const {
  if (TestNoRegenCache())
    return 0;
  return std::max<size_t>(256, 3 * m_features.size());
}

void Document::EvictRegenCache() {
  const size_t cap = RegenCacheCap();
  if (m_cache.size() <= cap)
    return;
  const std::unordered_set<size_t> current(m_chain.begin(), m_chain.end());
  std::vector<std::pair<uint64_t, size_t>> others; // lastUse, hash
  for (const auto& [hash, entry] : m_cache)
    if (current.count(hash) == 0)
      others.emplace_back(entry.lastUse, hash);
  std::sort(others.begin(), others.end());
  const size_t excess = std::min(m_cache.size() - cap, others.size());
  for (size_t i = 0; i < excess; i++)
    m_cache.erase(others[i].second);
}

const Parameter* Document::FindParameter(const std::string& name) const {
  for (const Parameter& p : m_parameters) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
}

const fem::Study* Document::FindStudy(uint64_t id) const {
  for (const fem::Study& s : m_studies) {
    if (s.id == id)
      return &s;
  }
  return nullptr;
}

fem::Study* Document::StudyForRun(uint64_t id) {
  for (fem::Study& s : m_studies) {
    if (s.id == id)
      return &s;
  }
  return nullptr;
}

const Tolerance* Document::FindTolerance(uint64_t id) const {
  for (const Tolerance& t : m_tolerances)
    if (t.id == id)
      return &t;
  return nullptr;
}

const Tolerance* Document::ToleranceFor(const ElementRef& ref) const {
  if (!ref.IsSet())
    return nullptr;
  for (const Tolerance& t : m_tolerances)
    if (!t.IsDistance() && t.ref.featureId == ref.featureId &&
        t.ref.name == ref.name)
      return &t;
  return nullptr;
}

const Tolerance* Document::ToleranceForSubject(const ElementRef& ref,
                                               const ElementRef& ref2) const {
  for (const Tolerance& t : m_tolerances)
    if (t.ref.featureId == ref.featureId && t.ref.name == ref.name &&
        t.ref2.featureId == ref2.featureId && t.ref2.name == ref2.name)
      return &t;
  return nullptr;
}

const Tolerance& Document::SetTolerance(Tolerance t) {
  MarkEdited();
  if (t.id == 0)
    t.id = m_nextId++;
  else
    EnsureNextIdAtLeast(t.id + 1);
  // Upsert by SUBJECT, not by id: the same face cannot carry two sizes, and
  // the same ordered pair of faces cannot be two distances apart. A face's own
  // size and a distance measured from it are different subjects, so they
  // coexist. Replacing in place keeps list order stable for a UI listing them.
  Tolerance* stored = nullptr;
  for (Tolerance& existing : m_tolerances) {
    if (&existing == ToleranceForSubject(t.ref, t.ref2)) {
      existing = std::move(t);
      stored = &existing;
      break;
    }
  }
  if (stored == nullptr) {
    m_tolerances.push_back(std::move(t));
    stored = &m_tolerances.back();
  }
  // A tolerance rebuilds nothing, but it is a size band the contact checks
  // read off the bodies (core/FitBands.h).
  fitband::Stamp(*this, m_bodies);
  return *stored;
}

bool Document::RemoveTolerance(uint64_t id) {
  for (auto it = m_tolerances.begin(); it != m_tolerances.end(); ++it) {
    if (it->id == id) {
      m_tolerances.erase(it);
      MarkEdited();
      fitband::Stamp(*this, m_bodies);
      return true;
    }
  }
  return false;
}

void Document::RestoreTolerance(Tolerance t) {
  EnsureNextIdAtLeast(t.id + 1);
  m_tolerances.push_back(std::move(t));
  MarkEdited();
  fitband::Stamp(*this, m_bodies);
}

fem::Study& Document::AddStudy(const std::string& name) {
  fem::Study study;
  study.id = m_nextId++;
  study.name =
      name.empty() ? ("Study " + std::to_string(m_studies.size() + 1)) : name;
  m_studies.push_back(std::move(study));
  MarkEdited();
  return m_studies.back();
}

bool Document::RemoveStudy(uint64_t id) {
  for (auto it = m_studies.begin(); it != m_studies.end(); ++it) {
    if (it->id == id) {
      m_studies.erase(it);
      m_studyRuntime.erase(id); // drop derived mesh/results too
      MarkEdited();
      return true;
    }
  }
  return false;
}

void Document::RestoreStudy(fem::Study study) {
  EnsureNextIdAtLeast(study.id + 1);
  for (const fem::Load& l : study.loads)
    EnsureNextIdAtLeast(l.id + 1);
  m_studies.push_back(std::move(study));
  MarkEdited();
}

bool Document::ReplaceStudy(fem::Study study) {
  for (fem::Study& s : m_studies) {
    if (s.id == study.id) {
      for (const fem::Load& l : study.loads)
        EnsureNextIdAtLeast(l.id + 1);
      s = std::move(study);
      MarkEdited();
      return true;
    }
  }
  return false;
}

// ---- Configurations -------------------------------------------------------

const Configuration* Document::FindConfiguration(uint64_t id) const {
  for (const Configuration& c : m_configurations)
    if (c.id == id)
      return &c;
  return nullptr;
}

Configuration& Document::AddConfiguration(const std::string& name) {
  Configuration cfg;
  cfg.id = m_nextId++;
  cfg.name =
      name.empty()
          ? ("Configuration " + std::to_string(m_configurations.size() + 1))
          : name;
  m_configurations.push_back(std::move(cfg));
  m_configurationsRevision++;
  MarkEdited();
  return m_configurations.back();
}

bool Document::RemoveConfiguration(uint64_t id) {
  for (auto it = m_configurations.begin(); it != m_configurations.end(); ++it) {
    if (it->id == id) {
      m_configurations.erase(it);
      m_configurationsRevision++;
      MarkEdited();
      if (m_activeConfiguration == id) {
        m_activeConfiguration = 0; // fall back to base
        Regenerate();
      }
      return true;
    }
  }
  return false;
}

void Document::RestoreConfiguration(Configuration cfg) {
  EnsureNextIdAtLeast(cfg.id + 1);
  m_configurations.push_back(std::move(cfg));
  m_configurationsRevision++;
  MarkEdited();
}

bool Document::ReplaceConfiguration(Configuration cfg) {
  for (Configuration& c : m_configurations) {
    if (c.id == cfg.id) {
      const bool active = m_activeConfiguration == cfg.id;
      c = std::move(cfg);
      m_configurationsRevision++;
      MarkEdited();
      if (active)
        Regenerate(); // its overrides changed the applied model
      return true;
    }
  }
  return false;
}

bool Document::SetActiveConfiguration(uint64_t id) {
  if (id != 0 && FindConfiguration(id) == nullptr)
    return false;
  m_activeConfiguration = id;
  Regenerate();
  return true;
}

const Configuration* Document::ActiveConfig() const {
  return m_activeConfiguration == 0 ? nullptr
                                    : FindConfiguration(m_activeConfiguration);
}

bool Document::EffectiveSuppressed(size_t index) const {
  if (index >= m_features.size())
    return false;
  const Configuration* c = ActiveConfig();
  // Unsuppression is the last word, so a configuration can turn ON a feature
  // the authored model keeps off — which is what a flat pattern is.
  if (c != nullptr && c->Unsuppresses(m_features[index]->GetId()))
    return false;
  if (m_features[index]->IsSuppressed())
    return true;
  return c != nullptr && c->Suppresses(m_features[index]->GetId());
}

bool Document::SetParameter(const std::string& name,
                            const std::string& expression,
                            const std::string& comment) {
  if (!ExpressionEngine::Default().IsValidParameterName(name)) {
    return false;
  }
  const auto it =
      std::find_if(m_parameters.begin(), m_parameters.end(),
                   [&](const Parameter& p) { return p.name == name; });
  if (it != m_parameters.end()) {
    it->expression = expression;
    it->comment = comment;
  } else {
    m_parameters.push_back({name, expression, comment, 0.0, {}});
  }
  Regenerate();
  return true;
}

bool Document::RemoveParameter(const std::string& name,
                               std::vector<std::string>* outReferences) {
  const auto it =
      std::find_if(m_parameters.begin(), m_parameters.end(),
                   [&](const Parameter& p) { return p.name == name; });
  if (it == m_parameters.end()) {
    return false;
  }
  std::vector<std::string> refs = FindParameterReferences(name);
  if (!refs.empty()) {
    if (outReferences != nullptr) {
      *outReferences = std::move(refs);
    }
    return false;
  }
  m_parameters.erase(it);
  Regenerate();
  return true;
}

bool Document::RenameParameter(const std::string& oldName,
                               const std::string& newName) {
  if (FindParameter(oldName) == nullptr || FindParameter(newName) != nullptr ||
      !ExpressionEngine::Default().IsValidParameterName(newName)) {
    return false;
  }
  const auto& engine = ExpressionEngine::Default();
  for (Parameter& p : m_parameters) {
    if (p.name == oldName) {
      p.name = newName;
    }
    p.expression = engine.RenameIdentifier(p.expression, oldName, newName);
  }
  for (const auto& feature : m_features) {
    feature->VisitExprs([&](std::string& text, double& value) {
      (void)value;
      text = engine.RenameIdentifier(text, oldName, newName);
    });
  }
  Regenerate();
  return true;
}

std::vector<std::string>
Document::FindParameterReferences(const std::string& name) {
  const auto& engine = ExpressionEngine::Default();
  const auto references = [&](const std::string& text) {
    if (text.empty()) {
      return false;
    }
    const auto compiled = engine.Compile(text);
    return compiled.has_value() && (*compiled)->Dependencies().contains(name);
  };

  std::vector<std::string> refs;
  for (const Parameter& p : m_parameters) {
    if (references(p.expression)) {
      refs.push_back("parameter '" + p.name + "'");
    }
  }
  for (const auto& feature : m_features) {
    bool hit = false;
    feature->VisitExprs([&](std::string& text, double& value) {
      (void)value;
      hit = hit || references(text);
    });
    if (hit) {
      refs.push_back("feature '" + feature->GetName() + "'");
    }
  }
  return refs;
}

// A Hole whose fit is a clearance (close/normal/loose) is modelled at the
// print allowance when the part is printed (HoleFeature::Execute).
static bool PrintDrivesGeometry(const Feature& f) {
  // A retaining ring groove reads its face at the size it prints and cuts
  // its floor to print at d2 (docs/parts/README.md#the-groove).
  if (dynamic_cast<const RetainingRingGrooveFeature*>(&f) != nullptr)
    return true;
  // A Cradle with no clearance of its own takes the process's gap.
  if (const auto* cr = dynamic_cast<const CradleFeature*>(&f))
    return !cr->hasClearance;
  const auto* hole = dynamic_cast<const HoleFeature*>(&f);
  // A printed hole is refused without a process, and its flat is the
  // process's bridge.
  return hole != nullptr &&
         ((!hole->fitThread.empty() && hole->fitKind != "tapped") ||
          hole->printed != HolePrinted::Round);
}

void Document::SetPrintProcess(std::optional<printfit::Process> p) {
  m_print = std::move(p);
  MarkEdited();
  if (std::any_of(m_features.begin(), m_features.end(),
                  [](const auto& f) { return PrintDrivesGeometry(*f); }))
    Regenerate();
  // A printed face's band is read at the size it prints (core/FitBands.h).
  fitband::Stamp(*this, m_bodies);
}

void Document::SetPrintSettings(printset::PartSettings s) {
  const bool turned = PrintUp(s) != PrintUp(m_printSettings);
  m_printSettings = std::move(s);
  MarkEdited();
  // A printed hole's roof follows the print direction.
  if (turned &&
      std::any_of(m_features.begin(), m_features.end(),
                  [](const auto& f) { return PrintUpDrivesGeometry(*f); }))
    Regenerate();
}

void Document::EvaluateParameters() {
  EvaluateInto(ActiveConfig(), m_parameters, m_symbols);
}

SymbolTable Document::SymbolsIn(uint64_t configuration) const {
  std::vector<Parameter> params = m_parameters;
  SymbolTable out;
  EvaluateInto(configuration == 0 ? nullptr : FindConfiguration(configuration),
               params, out);
  return out;
}

void Document::EvaluateInto(const Configuration* cfg,
                            std::vector<Parameter>& params,
                            SymbolTable& out) const {
  // Project variables (Phase V) seed the table as the outer scope, shadowed by
  // same-named local parameters. Tables this document carries itself (no app
  // took them into the project) join that scope as cells.
  SymbolTable withTables;
  const SymbolTable* outer = m_projectSymbols;
  if (!m_tables.empty()) {
    if (m_projectSymbols != nullptr)
      withTables = *m_projectSymbols;
    lookuptable::AddCellSymbols(m_tables, withTables);
    outer = &withTables;
  }
  if (cfg == nullptr) {
    EvaluateParameterTable(params, out, outer, {});
    return;
  }
  // The active configuration overrides parameter VALUES for THIS document's
  // regeneration only (never mutating the stored expression), so dependents
  // propagate: overriding `width` re-evaluates `depth = width*3`. A local
  // parameter takes the override through the callback below. An override that
  // targets a PROJECT variable (Phase V — most variables live there now) is
  // injected as a synthetic local parameter so it shadows the project value in
  // this part alone, leaving the shared project scope and every other part
  // untouched.
  std::vector<Parameter> augmented = params;
  const auto isLocal = [&](const std::string& name) {
    return std::any_of(params.begin(), params.end(),
                       [&](const Parameter& p) { return p.name == name; });
  };
  bool overridesProject = false;
  for (const auto& [name, expr] : cfg->parameterOverrides) {
    const bool isProject =
        m_projectSymbols != nullptr && m_projectSymbols->count(name) != 0;
    if (!isLocal(name) && isProject) {
      overridesProject = true;
      augmented.push_back({name, expr, "", 0.0, {}});
    }
  }
  // Overriding a project variable must also re-derive project variables
  // computed FROM it (`height = opening + 2*sec`) — otherwise direct
  // references see the override while derived ones keep the project value,
  // splitting the document's geometry (the C-clamp configuration bug).
  // Inject the whole project table as synthetic locals: the topological
  // evaluation below re-derives exactly the affected subset, and unaffected
  // entries evaluate to their project values anyway. Requires the project
  // parameter EXPRESSIONS (wired by the app; without them, fall back to
  // shadowing just the named variables).
  if (overridesProject && m_projectParameters != nullptr) {
    for (const Parameter& pp : *m_projectParameters) {
      const bool alreadyIn =
          std::any_of(augmented.begin(), augmented.end(),
                      [&](const Parameter& a) { return a.name == pp.name; });
      if (!alreadyIn && !isLocal(pp.name))
        augmented.push_back({pp.name, pp.expression, "", pp.value, {}});
    }
  }
  EvaluateParameterTable(augmented, out, outer, [cfg](const std::string& name) {
    return cfg->OverrideFor(name);
  });
  // Copy the evaluated value/error back into the real local table (the
  // synthetic project-override entries only needed to reach `out`).
  for (Parameter& p : params) {
    for (const Parameter& a : augmented) {
      if (a.name == p.name) {
        p.value = a.value;
        p.error = a.error;
        break;
      }
    }
  }
}

bool Document::RewriteProjectVarRefs(const std::string& oldName,
                                     const std::string& newName) {
  if (!ExpressionEngine::Default().IsValidParameterName(newName)) {
    return false;
  }
  // A local parameter of the same name shadows the project variable: references
  // in this document resolve locally, so a project rename must not touch them.
  if (FindParameter(oldName) != nullptr) {
    return false;
  }
  const auto& engine = ExpressionEngine::Default();
  for (Parameter& p : m_parameters) {
    p.expression = engine.RenameIdentifier(p.expression, oldName, newName);
  }
  for (const auto& feature : m_features) {
    feature->VisitExprs([&](std::string& text, double& value) {
      (void)value;
      text = engine.RenameIdentifier(text, oldName, newName);
    });
  }
  return true; // caller regenerates
}

void EvaluateParameterTable(
    std::vector<Parameter>& params, SymbolTable& out, const SymbolTable* base,
    const std::function<const std::string*(const std::string&)>& overrideExpr) {
  const auto& engine = ExpressionEngine::Default();
  const size_t n = params.size();

  // Seed with the outer scope (project vars), then publish every parameter's
  // last good value so dependents still resolve when a sibling fails; the local
  // value shadows any same-named outer symbol.
  out.clear();
  if (base != nullptr) {
    out = *base;
  }
  for (const Parameter& p : params) {
    out[p.name] = p.value;
  }

  // Compile everything, collect dependency edges between parameters (deps on
  // non-parameters — including outer-scope symbols — surface as constants, not
  // edges, since they carry no in-list producer).
  std::unordered_map<std::string, size_t> indexByName;
  for (size_t i = 0; i < n; i++) {
    indexByName[params[i].name] = i;
  }

  std::vector<std::unique_ptr<CompiledExpr>> compiled(n);
  std::vector<std::vector<size_t>> dependents(n);
  std::vector<size_t> pendingDeps(n, 0);
  for (size_t i = 0; i < n; i++) {
    Parameter& p = params[i];
    const std::string* ov = overrideExpr ? overrideExpr(p.name) : nullptr;
    const std::string& expression = ov ? *ov : p.expression;
    auto result = engine.Compile(expression);
    if (!result) {
      p.error = result.error().message;
      continue;
    }
    compiled[i] = std::move(*result);
    p.error.clear();
    for (const std::string& dep : compiled[i]->Dependencies()) {
      const auto it = indexByName.find(dep);
      if (it != indexByName.end() && it->second != i) {
        dependents[it->second].push_back(i);
        pendingDeps[i]++;
      }
    }
  }

  // Kahn's algorithm, in table order for determinism. Parameters that never
  // become ready are in (or downstream of) a cycle.
  std::deque<size_t> ready;
  std::vector<bool> evaluated(n, false);
  for (size_t i = 0; i < n; i++) {
    if (pendingDeps[i] == 0) {
      ready.push_back(i);
    }
  }
  while (!ready.empty()) {
    const size_t i = ready.front();
    ready.pop_front();
    evaluated[i] = true;
    Parameter& p = params[i];
    if (compiled[i] != nullptr) {
      const auto v = compiled[i]->Evaluate(out);
      if (v.has_value()) {
        p.value = *v;
        out[p.name] = *v;
        p.error.clear();
      } else {
        p.error = v.error().message; // keep last good value
      }
    }
    for (const size_t dep : dependents[i]) {
      if (--pendingDeps[dep] == 0) {
        ready.push_back(dep);
      }
    }
  }
  for (size_t i = 0; i < n; i++) {
    if (!evaluated[i]) {
      params[i].error = "circular reference";
      LOG_ERROR("Parameter '{}' has a circular reference", params[i].name);
    }
  }
}
