#include "core/Document.h"

#include "Logger.h"
#include "core/io/Serialization.h" // FeatureToJson: the regen-cache hash input

#include <algorithm>
#include <functional>
#include <string>
#include <deque>
#include <unordered_map>

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
  Regenerate();
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
  Regenerate();
  return removed;
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
  EvaluateParameters();

  m_version++;
  bool allOk = true;
  m_lastStats = {};

  // Rollback bar: only the prefix regenerates; the tail keeps its params but
  // contributes nothing (and holds no stale errors).
  const size_t upTo = m_rollback < 0
                          ? m_features.size()
                          : std::min(m_features.size(),
                                     static_cast<size_t>(m_rollback));
  for (size_t i = upTo; i < m_features.size(); i++) {
    m_features[i]->ClearError();
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
      ExprError err;
      const auto v =
          ExpressionEngine::Default().EvaluateText(text, m_symbols, &err);
      if (v.has_value()) {
        value = *v;
      } else if (exprErr[i].empty()) {
        // The ExprError message distinguishes the case ("unknown variable
        // 'bore'" vs. a parse / division error); first failure per feature wins.
        exprErr[i] = err.message;
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
    chain = std::hash<std::string>{}(
        std::to_string(chain) + DocumentIO::FeatureToJson(*m_features[i]).dump() +
        (EffectiveSuppressed(i) ? "|cfg-suppressed" : ""));
    hashes[i] = chain;
  }
  size_t divergence = 0;
  while (divergence < upTo && divergence < m_cache.size() &&
         m_cache[divergence].hash == hashes[divergence]) {
    divergence++;
  }
  m_lastStats.cached = divergence;
  m_bodies = divergence > 0 ? m_cache[divergence - 1].bodiesAfter
                            : std::vector<Body>{};
  // Errors in the cached prefix persist from their last (deterministic)
  // execution; they still count against the result.
  for (size_t i = 0; i < divergence; i++) {
    if (!m_features[i]->GetError().empty()) {
      allOk = false;
    }
  }

  if (divergence < upTo) {
    m_cache.resize(divergence); // stale suffix; rebuilt below
    for (size_t i = divergence; i < upTo; i++) {
      m_features[i]->ClearError();
      if (!EffectiveSuppressed(i)) {
        RegenContext ctx{m_bodies, m_features, i, &m_symbols};
        m_lastStats.executed++;
        if (!m_features[i]->Execute(ctx)) {
          if (m_features[i]->GetError().empty()) {
            m_features[i]->SetError("regeneration failed");
          }
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
    if (s.id == id) return &s;
  }
  return nullptr;
}

fem::Study* Document::FindStudy(uint64_t id) {
  for (fem::Study& s : m_studies) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

fem::Study& Document::AddStudy(const std::string& name) {
  fem::Study study;
  study.id = m_nextId++;
  study.name = name.empty() ? ("Study " + std::to_string(m_studies.size() + 1)) : name;
  m_studies.push_back(std::move(study));
  return m_studies.back();
}

bool Document::RemoveStudy(uint64_t id) {
  for (auto it = m_studies.begin(); it != m_studies.end(); ++it) {
    if (it->id == id) {
      m_studies.erase(it);
      m_studyRuntime.erase(id);  // drop derived mesh/results too
      return true;
    }
  }
  return false;
}

void Document::RestoreStudy(fem::Study study) {
  EnsureNextIdAtLeast(study.id + 1);
  for (const fem::Load& l : study.loads) EnsureNextIdAtLeast(l.id + 1);
  m_studies.push_back(std::move(study));
}

bool Document::ReplaceStudy(fem::Study study) {
  for (fem::Study& s : m_studies) {
    if (s.id == study.id) {
      for (const fem::Load& l : study.loads) EnsureNextIdAtLeast(l.id + 1);
      s = std::move(study);
      return true;
    }
  }
  return false;
}

// ---- Configurations -------------------------------------------------------

const Configuration* Document::FindConfiguration(uint64_t id) const {
  for (const Configuration& c : m_configurations)
    if (c.id == id) return &c;
  return nullptr;
}

Configuration* Document::FindConfiguration(uint64_t id) {
  for (Configuration& c : m_configurations)
    if (c.id == id) return &c;
  return nullptr;
}

Configuration& Document::AddConfiguration(const std::string& name) {
  Configuration cfg;
  cfg.id = m_nextId++;
  cfg.name = name.empty()
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
        m_activeConfiguration = 0;  // fall back to base
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
      if (active) Regenerate();  // its overrides changed the applied model
      return true;
    }
  }
  return false;
}

bool Document::SetActiveConfiguration(uint64_t id) {
  if (id != 0 && FindConfiguration(id) == nullptr) return false;
  m_activeConfiguration = id;
  Regenerate();
  return true;
}

const Configuration* Document::ActiveConfig() const {
  return m_activeConfiguration == 0 ? nullptr
                                    : FindConfiguration(m_activeConfiguration);
}

bool Document::EffectiveSuppressed(size_t index) const {
  if (index >= m_features.size()) return false;
  if (m_features[index]->IsSuppressed()) return true;
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

std::vector<std::string> Document::FindParameterReferences(
    const std::string& name) {
  const auto& engine = ExpressionEngine::Default();
  const auto references = [&](const std::string& text) {
    if (text.empty()) {
      return false;
    }
    const auto compiled = engine.Compile(text, nullptr);
    return compiled != nullptr && compiled->Dependencies().contains(name);
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
  for (const auto& [name, expr] : cfg->parameterOverrides) {
    const bool isLocal =
        std::any_of(m_parameters.begin(), m_parameters.end(),
                    [&](const Parameter& p) { return p.name == name; });
    const bool isProject =
        m_projectSymbols != nullptr && m_projectSymbols->count(name) != 0;
    if (!isLocal && isProject) augmented.push_back({name, expr, "", 0.0, {}});
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
  return true;  // caller regenerates
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
    ExprError err;
    compiled[i] = engine.Compile(expression, &err);
    if (compiled[i] == nullptr) {
      p.error = err.message;
      continue;
    }
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
      ExprError err;
      const auto v = compiled[i]->Evaluate(out, &err);
      if (v.has_value()) {
        p.value = *v;
        out[p.name] = *v;
        p.error.clear();
      } else {
        p.error = err.message;  // keep last good value
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
