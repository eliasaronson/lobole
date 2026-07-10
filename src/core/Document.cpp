#include "core/Document.h"

#include "Logger.h"

#include <algorithm>
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

  m_bodies.clear();
  m_version++;

  for (size_t i = 0; i < m_features.size(); i++) {
    // Refresh expression-driven fields first, so the feature executes (and a
    // sketch solves) against current parameter values. Failed evaluations
    // keep the field's last good value.
    m_features[i]->VisitExprs([this](std::string& text, double& value) {
      if (text.empty()) {
        return;
      }
      const auto v =
          ExpressionEngine::Default().EvaluateText(text, m_symbols, nullptr);
      if (v.has_value()) {
        value = *v;
      }
    });

    RegenContext ctx{m_bodies, m_features, i, &m_symbols};
    if (!m_features[i]->Execute(ctx)) {
      LOG_ERROR("Regeneration stopped at feature '{}'",
                m_features[i]->GetName());
      return false;
    }
  }
  return true;
}

const Parameter* Document::FindParameter(const std::string& name) const {
  for (const Parameter& p : m_parameters) {
    if (p.name == name) {
      return &p;
    }
  }
  return nullptr;
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
  const auto& engine = ExpressionEngine::Default();
  const size_t n = m_parameters.size();

  // Publish every parameter's last good value up front, so parameters whose
  // evaluation fails below still resolve for their dependents.
  m_symbols.clear();
  for (const Parameter& p : m_parameters) {
    m_symbols[p.name] = p.value;
  }

  // Compile everything, collect dependency edges between parameters (deps on
  // non-parameters surface later as evaluation errors).
  std::unordered_map<std::string, size_t> indexByName;
  for (size_t i = 0; i < n; i++) {
    indexByName[m_parameters[i].name] = i;
  }
  std::vector<std::unique_ptr<CompiledExpr>> compiled(n);
  std::vector<std::vector<size_t>> dependents(n);
  std::vector<size_t> pendingDeps(n, 0);
  for (size_t i = 0; i < n; i++) {
    Parameter& p = m_parameters[i];
    ExprError err;
    compiled[i] = engine.Compile(p.expression, &err);
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
    Parameter& p = m_parameters[i];
    if (compiled[i] != nullptr) {
      ExprError err;
      const auto v = compiled[i]->Evaluate(m_symbols, &err);
      if (v.has_value()) {
        p.value = *v;
        m_symbols[p.name] = *v;
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
      m_parameters[i].error = "circular reference";
      LOG_ERROR("Parameter '{}' has a circular reference",
                m_parameters[i].name);
    }
  }
}
