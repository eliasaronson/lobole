#pragma once

#include "core/Configuration.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/FemSolver.h"
#include "core/FemStudy.h"

#include <cstdint>
#include <map>
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

  // ---- Studies (FEM) ----------------------------------------------------
  // Document-level analysis definitions, a sibling to the parameter table.
  // A study consumes the finished bodies and produces no geometry, so it is
  // not a Feature and does not participate in regeneration. Mesh and results
  // are derived data held elsewhere (never serialized); only this
  // configuration persists. Edits do NOT auto-regenerate (a study is inert
  // until explicitly meshed/solved).

  const std::vector<fem::Study>& GetStudies() const { return m_studies; }
  std::vector<fem::Study>& GetStudies() { return m_studies; }
  const fem::Study* FindStudy(uint64_t id) const;
  fem::Study* FindStudy(uint64_t id);

  // Appends a new study with a document-unique id and returns it. `name`
  // empty means an auto-generated "Study N".
  fem::Study& AddStudy(const std::string& name = "");
  // Removes the study with `id`; returns false if absent.
  bool RemoveStudy(uint64_t id);

  // Appends a fully-formed study (keeping its ids) and raises the id
  // high-water mark past the study and all its loads. For deserialization.
  void RestoreStudy(fem::Study study);

  // Replaces the study with `study.id` in place (preserving list order),
  // raising the id high-water mark past its loads. Returns false if absent.
  // Backs the study modify Command.
  bool ReplaceStudy(fem::Study study);

  // The transient mesh+results cache for a study, or nullptr if it has never
  // been run. Derived data, never serialized; stale when its solvedAtVersion
  // differs from Version(). Mutable so const RPC reads can populate/query it.
  const fem::StudyRuntime* StudyResults(uint64_t studyId) const {
    auto it = m_studyRuntime.find(studyId);
    return it == m_studyRuntime.end() ? nullptr : &it->second;
  }
  void StoreStudyResults(uint64_t studyId, fem::StudyRuntime rt) {
    m_studyRuntime[studyId] = std::move(rt);
    ++m_studyResultsEpoch;
  }
  // Monotonic counter bumped on every StoreStudyResults; lets the field
  // overlay detect a re-solve (study edits don't bump Version()).
  uint64_t StudyResultsEpoch() const { return m_studyResultsEpoch; }
  bool StudyResultsStale(uint64_t studyId) const {
    auto it = m_studyRuntime.find(studyId);
    return it == m_studyRuntime.end() ? true
                                      : it->second.solvedAtVersion != m_version;
  }

  // ---- Configurations ---------------------------------------------------
  // Named variants (parameter overrides + feature suppression), a sibling to
  // the parameter table like studies — not Features. The ACTIVE configuration
  // (0 = base, the model as authored) is applied during regeneration by
  // overlaying its overrides onto the symbol table and composing its
  // suppression set; no stored parameter/feature state is mutated. See
  // docs/configurations/plan.md.

  const std::vector<Configuration>& GetConfigurations() const {
    return m_configurations;
  }
  std::vector<Configuration>& GetConfigurations() { return m_configurations; }
  const Configuration* FindConfiguration(uint64_t id) const;
  Configuration* FindConfiguration(uint64_t id);

  // Appends a configuration with a doc-unique id (auto-named "Configuration N"
  // when empty) and returns it. Does not change the active configuration.
  Configuration& AddConfiguration(const std::string& name = "");
  // Removes the configuration with `id`; if it was active, falls back to base
  // and regenerates. Returns false if absent.
  bool RemoveConfiguration(uint64_t id);
  // Appends a fully-formed configuration (keeping its id) and raises the id
  // high-water mark. For deserialization / undo.
  void RestoreConfiguration(Configuration cfg);
  // Replaces the configuration with `cfg.id` in place. Returns false if absent.
  // Regenerates when it is the active one. Backs the modify Command.
  bool ReplaceConfiguration(Configuration cfg);

  // Which configuration regeneration applies (0 = base). Persisted like the
  // rollback bar. SetActiveConfiguration regenerates; returns false for an
  // unknown non-zero id.
  uint64_t ActiveConfiguration() const { return m_activeConfiguration; }
  bool SetActiveConfiguration(uint64_t id);
  // Loader hook: set the active id without regenerating (the load's own regen
  // rebuilds bodies once).
  void SetActiveConfigurationRaw(uint64_t id) { m_activeConfiguration = id; }
  // The active configuration, or nullptr when base (0) or unknown.
  const Configuration* ActiveConfig() const;

  // Whether feature `index` is suppressed under the active configuration: its
  // authored `suppressed` flag OR the active config suppresses its id. This is
  // what regeneration honors (and folds into the prefix-cache key).
  bool EffectiveSuppressed(size_t index) const;

  // Allocates the next document-unique id (studies and their loads draw from
  // the same counter as features, so ids never collide across kinds).
  uint64_t AllocateId() { return m_nextId++; }
  // Raises the id high-water mark on load so freshly allocated ids never
  // collide with deserialized ones.
  void EnsureNextIdAtLeast(uint64_t id) {
    if (id > m_nextId) m_nextId = id;
  }

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

  // The body list as it stood after feature `index` executed, from the regen
  // prefix cache — i.e. feature `index + 1`'s input. Valid for the prefix the
  // last successful Regenerate covered; null when the cache doesn't reach
  // `index`. Used by edge picking to resolve refs against a blend's input.
  const std::vector<Body>* CachedBodiesAfter(size_t index) const {
    return index < m_cache.size() ? &m_cache[index].bodiesAfter : nullptr;
  }

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
  std::vector<fem::Study> m_studies;
  // Transient per-study mesh+results (never serialized); keyed by study id.
  std::map<uint64_t, fem::StudyRuntime> m_studyRuntime;
  uint64_t m_studyResultsEpoch = 0;
  std::vector<Configuration> m_configurations;
  uint64_t m_activeConfiguration = 0;  // 0 = base; persisted like m_rollback
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
