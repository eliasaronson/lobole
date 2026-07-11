#pragma once

#include "core/Expression.h"
#include "core/Feature.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// A named document-level value, definable as an expression over other
// parameters ("width * 2"). `value` is the last successful evaluation and is
// kept when evaluation fails, so dependent expressions and geometry stay
// stable while the user fixes the input; `error` is transient (never
// serialized) and empty when healthy.
struct Parameter {
  std::string name;
  std::string expression;
  std::string comment;
  double value = 0.0;
  std::string error;
};

// The parametric model: an ordered feature list plus the bodies produced by
// regenerating it. This is the single source of truth for model state — the
// UI, scripts, and (later) files all act on the Document through Commands,
// never on geometry directly.
class Document {
public:
  size_t FeatureCount() const { return m_features.size(); }
  Feature* GetFeature(size_t index);
  const std::vector<std::unique_ptr<Feature>>& GetFeatures() const {
    return m_features;
  }
  // Lookup by stable id (see Feature::GetId); nullptr if absent. Returns the
  // feature's current index through outIndex if given.
  Feature* FindFeatureById(uint64_t id, size_t* outIndex = nullptr);

  // Mutators regenerate automatically so bodies always match the features.
  void AddFeature(std::unique_ptr<Feature> feature);
  void InsertFeature(std::unique_ptr<Feature> feature, size_t index);
  std::unique_ptr<Feature> RemoveFeature(size_t index);

  // Swaps the feature at index and regenerates. Returns false (and leaves
  // the document unchanged) if index is out of range or feature is null.
  bool ReplaceFeature(size_t index, std::unique_ptr<Feature> feature);

  // ---- Parameters (variables) -------------------------------------------
  // The ordered parameter table expressions evaluate against. All mutators
  // regenerate automatically, like the feature mutators above.

  const std::vector<Parameter>& GetParameters() const { return m_parameters; }
  const Parameter* FindParameter(const std::string& name) const;

  // Adds or updates a parameter (updates keep table order). Returns false
  // without touching the document if name is not a valid identifier.
  bool SetParameter(const std::string& name, const std::string& expression,
                    const std::string& comment = "");

  // Refuses to delete a parameter that other expressions reference — the
  // referencing parameters/features are reported through outReferences.
  // Returns false if absent or referenced.
  bool RemoveParameter(const std::string& name,
                       std::vector<std::string>* outReferences = nullptr);

  // Renames a parameter and rewrites every expression referencing it (other
  // parameters and feature fields), so references never dangle silently.
  // Returns false if oldName is absent or newName is invalid/taken.
  bool RenameParameter(const std::string& oldName, const std::string& newName);

  // Everything that references `name`, as human-readable labels ("parameter
  // 'depth'", "feature 'Extrude 1'"). Backs delete refusal and UI hints.
  // Non-const because it walks features via VisitExprs (read-only here).
  std::vector<std::string> FindParameterReferences(const std::string& name);

  // Parameter values from the last regeneration; what feature expressions
  // (and UI previews) evaluate against.
  const SymbolTable& Symbols() const { return m_symbols; }

  // Rebuilds all bodies by executing the feature list in order, after
  // re-evaluating the parameter table and every expression-driven feature
  // field. Returns false if any feature failed; bodies then reflect the
  // successful prefix.
  bool Regenerate();

  const std::vector<Body>& GetBodies() const { return m_bodies; }

  // Incremented on every regeneration; lets the UI detect when to re-upload
  // render meshes without observing individual edits.
  uint64_t Version() const { return m_version; }

  // Incremental regeneration (R3): how much of the last Regenerate came
  // from the prefix cache vs. actually executed. Observable, not vibes.
  struct RegenStats {
    size_t executed = 0;
    size_t cached = 0;
  };
  const RegenStats& LastRegenStats() const { return m_lastStats; }
  void ClearRegenCache() { m_cache.clear(); }

  // Rollback bar (R2): regeneration executes features [0, rollback) only;
  // the rest are skipped (grayed in the tree, params untouched). -1 = end.
  // Persisted with the document, like SolidWorks' marker position.
  int Rollback() const { return m_rollback; }
  void SetRollback(int index) {
    if (index < 0 || index >= static_cast<int>(m_features.size()))
      index = -1; // at or past the end = no marker
    if (index == m_rollback)
      return;
    m_rollback = index;
    Regenerate();
  }

private:
  // Topo-sorts m_parameters by expression dependency and evaluates them into
  // m_symbols. Failures (parse error, unknown name, cycle) set the affected
  // parameters' `error` and keep their last good values.
  void EvaluateParameters();

  std::vector<std::unique_ptr<Feature>> m_features;
  std::vector<Body> m_bodies;
  std::vector<Parameter> m_parameters;
  SymbolTable m_symbols;
  uint64_t m_version = 0;
  uint64_t m_nextId = 1;
  int m_rollback = -1;

  // Prefix cache (R3): per feature, the chain hash of everything up to and
  // including it, plus a snapshot of the body list after it executed. Body
  // snapshots are cheap (TopoDS_Shape is refcounted). Entries beyond the
  // current rollback stay — rolling forward again is then instant.
  struct RegenCacheEntry {
    size_t hash = 0;
    std::vector<Body> bodiesAfter;
  };
  std::vector<RegenCacheEntry> m_cache;
  RegenStats m_lastStats;
};
