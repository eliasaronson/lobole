#include "core/Document.h"
#include "core/features/DerivedFeature.h"
#include "core/features/HoleFeature.h"

#include "common/Logger.h"
#include "core/io/Serialization.h" // FeatureToJson: the regen-cache hash input

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

static bool PrintDrivesGeometry(const Feature& f);

namespace {

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
    for (HoleMark& h : b.holes)
      follow(b.elementMap, h.bore);
  }
}

} // namespace

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

// The one copy implementation, behind both public forms. A list of members,
// not a `= default`, for two reasons: the features are owning pointers and
// must be CLONED, and m_instanceId must NOT come along (it is left to its
// initializer, which mints a fresh one).
//
// Anything absent below is absent on purpose — it is cache, and the
// Regenerate() at the end rebuilds it: m_bodies, the prefix cache,
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
  m_generator = other.m_generator;
  m_sheetMetal = other.m_sheetMetal; // every flange's thickness comes from it
  m_tube = other.m_tube;             // what check_frame and the cut list read
  m_print = other.m_print;           // what the fit checks model a print at
  m_tables = other.m_tables;         // a lone part's project tables
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
  for (size_t i = own + 1; i < m_features.size(); i++) {
    bool consumes = false;
    m_features[i]->VisitFeatureRefs([&](uint64_t id) {
      if (id == featureId)
        consumes = true;
    });
    if (consumes)
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

  // Appearance stamp (v54): each body carries its creating feature's visual
  // override. A POST-pass keyed on createdBy (rather than plumbing through
  // every Execute signature) — run at every successful exit below, so cached
  // and baked bodies re-stamp too. This is what lets the chain hash STRIP
  // appearance: a recolor re-stamps here without rebuilding any B-Rep.
  const auto stampAppearance = [this] {
    for (Body& b : m_bodies)
      if (const Feature* f = FindFeatureById(b.createdBy))
        b.appearance = f->GetAppearance();
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
    for (Body& b : m_bodies) {
      const Feature* f = FindFeatureById(b.createdBy);
      if (f == nullptr)
        continue;
      if (!f->StampsOwnMaterial() || !f->GetMaterial().empty()) {
        b.material = f->GetMaterial();
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
  // everything after it — editing feature k re-runs [k, N) only. Precise
  // dependency tracking is deliberately v2; chaining is what makes retained
  // feature-internal state (solved sketches, cached datum planes) safe: a
  // matching prefix implies identical inputs, hence identical state.
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
  for (size_t i = 0; i < upTo; i++) {
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
    // hashes whatever the process.
    if (m_print && PrintDrivesGeometry(*m_features[i]))
      fj["print"] = m_print->process + "/" + m_print->material;
    chain = std::hash<std::string>{}(
        std::to_string(chain) + fj.dump() +
        (EffectiveSuppressed(i) ? "|cfg-suppressed" : ""));
    hashes[i] = chain;
  }
  m_tipHash = upTo > 0 ? hashes[upTo - 1] : 0;

  // Baked geometry: if the blob the loader adopted describes exactly this
  // feature list, install it and execute nothing. The prefix cache stays
  // empty, so the first edit rebuilds from the start — the trade recorded in
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
      // Seed the prefix cache so a LATER regeneration (the UI syncs, the RPC
      // layer, an unrelated document-version bump) is free too — without it,
      // the next Regenerate found an empty cache and rebuilt everything the
      // blob had just saved us. Only the tip entry carries bodies; the rest
      // are hashes with bodiesValid=false, which is what makes an edit
      // anywhere in the history fall back to a full rebuild.
      m_cache.clear();
      m_cache.reserve(upTo);
      for (size_t i = 0; i + 1 < upTo; i++)
        m_cache.push_back({hashes[i], {}, /*bodiesValid=*/false});
      if (upTo > 0)
        m_cache.push_back({hashes[upTo - 1], m_bodies, /*bodiesValid=*/true});
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
        datums++;
        if (!m_features[i]->Execute(ctx)) {
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
      return datumsOk;
    }
    LOG_INFO("Baked geometry does not match the recipe; rebuilding");
  }

  size_t divergence = 0;
  while (divergence < upTo && divergence < m_cache.size() &&
         m_cache[divergence].hash == hashes[divergence]) {
    divergence++;
  }
  // A divergence that resumes from a placeholder entry (baked geometry) has
  // no bodies to resume FROM: rebuild the whole list instead.
  if (divergence > 0 && !m_cache[divergence - 1].bodiesValid) {
    divergence = 0;
    m_cache.clear();
  }
  m_lastStats.cached = divergence;
  m_bodies = divergence > 0 ? m_cache[divergence - 1].bodiesAfter
                            : std::vector<Body>{};
  // Errors in the cached prefix persist from their last (deterministic)
  // execution; they still count against the result.
  bool anyFailedYet = false;
  for (size_t i = 0; i < divergence; i++) {
    if (!m_features[i]->GetError().empty()) {
      allOk = false;
      anyFailedYet = true;
    }
  }

  if (divergence < upTo) {
    m_bodiesFromBlob = 0; // executed: the bodies are this build's now
    m_bodiesBlobSource.clear();
    m_cache.resize(divergence); // stale suffix; rebuilt below
    for (size_t i = divergence; i < upTo; i++) {
      m_features[i]->ClearError();
      m_features[i]->ClearWarning();
      if (!EffectiveSuppressed(i)) {
        RegenContext ctx{m_bodies,  m_features,
                         i,         &m_symbols,
                         &m_assets, m_sheetMetal ? &*m_sheetMetal : nullptr};
        ctx.configuration = ActiveConfig();
        ctx.print = m_print ? &*m_print : nullptr;
        m_lastStats.executed++;
        if (!m_features[i]->Execute(ctx)) {
          if (m_features[i]->GetError().empty()) {
            m_features[i]->SetError("regeneration failed");
          }
          // A failure downstream of another failure is usually collateral —
          // tag it so clients can point at the first red feature.
          m_features[i]->SetErrorAfterEarlierFailure(anyFailedYet);
          anyFailedYet = true;
          // Continue past the failure (R1): the broken feature contributes
          // nothing; downstream features see the bodies as of this point
          // and fail on their own if they referenced its outputs.
          LOG_ERROR("Feature '{}' failed to regenerate: {}",
                    m_features[i]->GetName(), m_features[i]->GetError());
          allOk = false;
        }
        FollowModifiedFaces(m_bodies);
      }
      m_cache.push_back({hashes[i], m_bodies});
    }
  }
  // divergence == upTo: fully cached — keep any entries beyond upTo so
  // rolling the bar forward again is instant (their hashes re-verify then).

  // Fail-loud (Phase V): stamp the expression-resolution failures collected in
  // the refresh pass. Applied here — after Execute — so a rebuilt feature's
  // ClearError()/regen error takes precedence and this only fills in features
  // that regenerated cleanly on their stale literal (the visible-but-rendering
  // case). The feature error is transient (not serialized, not hashed), so it
  // never perturbs the cache.
  for (size_t i = 0; i < upTo; i++) {
    if (!exprErr[i].empty() && m_features[i]->GetError().empty()) {
      m_features[i]->SetError(exprErr[i]);
      allOk = false;
    }
  }
  stampAppearance();
  stampMaterial();
  return allOk;
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
  for (Tolerance& existing : m_tolerances) {
    if (&existing == ToleranceForSubject(t.ref, t.ref2)) {
      existing = std::move(t);
      return existing;
    }
  }
  m_tolerances.push_back(std::move(t));
  return m_tolerances.back();
}

bool Document::RemoveTolerance(uint64_t id) {
  for (auto it = m_tolerances.begin(); it != m_tolerances.end(); ++it) {
    if (it->id == id) {
      m_tolerances.erase(it);
      MarkEdited();
      return true;
    }
  }
  return false;
}

void Document::RestoreTolerance(Tolerance t) {
  EnsureNextIdAtLeast(t.id + 1);
  m_tolerances.push_back(std::move(t));
  MarkEdited();
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
  const auto* hole = dynamic_cast<const HoleFeature*>(&f);
  return hole != nullptr && !hole->fitThread.empty() &&
         hole->fitKind != "tapped";
}

void Document::SetPrintProcess(std::optional<printfit::Process> p) {
  m_print = std::move(p);
  MarkEdited();
  if (std::any_of(m_features.begin(), m_features.end(),
                  [](const auto& f) { return PrintDrivesGeometry(*f); }))
    Regenerate();
}

void Document::EvaluateParameters() {
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
  const Configuration* cfg = ActiveConfig();
  if (cfg == nullptr) {
    EvaluateParameterTable(m_parameters, m_symbols, outer, {});
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
  std::vector<Parameter> augmented = m_parameters;
  const auto isLocal = [&](const std::string& name) {
    return std::any_of(m_parameters.begin(), m_parameters.end(),
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
  EvaluateParameterTable(
      augmented, m_symbols, outer,
      [cfg](const std::string& name) { return cfg->OverrideFor(name); });
  // Copy the evaluated value/error back into the real local table (the
  // synthetic project-override entries only needed to reach m_symbols).
  for (Parameter& p : m_parameters) {
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
