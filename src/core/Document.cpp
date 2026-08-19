#include "core/Document.h"

#include "common/Logger.h"
#include "core/io/Serialization.h" // FeatureToJson: the regen-cache hash input

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

// Monotonic for the life of the process: identity must never be reused, or
// the consumers of InstanceId() are back to guessing.
uint64_t Document::NextInstanceId() {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
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
  m_features[index] = std::move(feature);
  Regenerate();
  return true;
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

  // Rollback bar: only the prefix regenerates; the tail keeps its params but
  // contributes nothing (and holds no stale errors).
  const size_t upTo =
      m_rollback < 0
          ? m_features.size()
          : std::min(m_features.size(), static_cast<size_t>(m_rollback));
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
    // Appearance is cosmetic (the stamp pass reapplies it after any cache
    // hit): keep it out of the hash so a recolor never rebuilds geometry.
    fj.erase("appearance");
    chain = std::hash<std::string>{}(
        std::to_string(chain) + fj.dump() +
        (EffectiveSuppressed(i) ? "|cfg-suppressed" : ""));
    hashes[i] = chain;
  }
  m_tipHash = upTo > 0 ? hashes[upTo - 1] : 0;

  // Baked geometry: if the blob the loader adopted describes exactly this
  // feature list, install it and execute nothing. The prefix cache stays
  // empty, so the first edit rebuilds from the start — the trade recorded in
  // docs/versioning/baked-geometry.md. Either way the hand-off is consumed
  // here, so a later edit can never resurrect it.
  if (!m_bakedBodies.empty()) {
    std::vector<Body> baked = std::move(m_bakedBodies);
    const size_t bakedHash = m_bakedHash;
    std::map<uint64_t, std::string> bakedWarnings = std::move(m_bakedWarnings);
    m_bakedBodies.clear();
    m_bakedHash = 0;
    m_bakedWarnings.clear();
    if (bakedHash != 0 && bakedHash == m_tipHash && m_cache.empty()) {
      m_bodies = std::move(baked);
      m_lastStats.cached = upTo;
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
      LOG_INFO("Loaded baked geometry ({} bodies): {} features executed 0, "
               "{} advisories restored",
               m_bodies.size(), upTo, bakedWarnings.size());
      stampAppearance();
      return true;
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
    m_cache.resize(divergence); // stale suffix; rebuilt below
    for (size_t i = divergence; i < upTo; i++) {
      m_features[i]->ClearError();
      m_features[i]->ClearWarning();
      if (!EffectiveSuppressed(i)) {
        RegenContext ctx{m_bodies, m_features, i, &m_symbols};
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

fem::Study* Document::FindStudy(uint64_t id) {
  for (fem::Study& s : m_studies) {
    if (s.id == id)
      return &s;
  }
  return nullptr;
}

fem::Study& Document::AddStudy(const std::string& name) {
  fem::Study study;
  study.id = m_nextId++;
  study.name =
      name.empty() ? ("Study " + std::to_string(m_studies.size() + 1)) : name;
  m_studies.push_back(std::move(study));
  return m_studies.back();
}

bool Document::RemoveStudy(uint64_t id) {
  for (auto it = m_studies.begin(); it != m_studies.end(); ++it) {
    if (it->id == id) {
      m_studies.erase(it);
      m_studyRuntime.erase(id); // drop derived mesh/results too
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
}

bool Document::ReplaceStudy(fem::Study study) {
  for (fem::Study& s : m_studies) {
    if (s.id == study.id) {
      for (const fem::Load& l : study.loads)
        EnsureNextIdAtLeast(l.id + 1);
      s = std::move(study);
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

Configuration* Document::FindConfiguration(uint64_t id) {
  for (Configuration& c : m_configurations)
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
  return m_configurations.back();
}

bool Document::RemoveConfiguration(uint64_t id) {
  for (auto it = m_configurations.begin(); it != m_configurations.end(); ++it) {
    if (it->id == id) {
      m_configurations.erase(it);
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
}

bool Document::ReplaceConfiguration(Configuration cfg) {
  for (Configuration& c : m_configurations) {
    if (c.id == cfg.id) {
      const bool active = m_activeConfiguration == cfg.id;
      c = std::move(cfg);
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
  if (m_features[index]->IsSuppressed())
    return true;
  const Configuration* c = ActiveConfig();
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

void Document::EvaluateParameters() {
  // Project variables (Phase V) seed the table as the outer scope, shadowed by
  // same-named local parameters.
  const Configuration* cfg = ActiveConfig();
  if (cfg == nullptr) {
    EvaluateParameterTable(m_parameters, m_symbols, m_projectSymbols, {});
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
      augmented, m_symbols, m_projectSymbols,
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
