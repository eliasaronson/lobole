#pragma once

#include "core/catalog/Catalog.h"

#include "core/Configuration.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/Units.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
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

// Evaluates an ordered parameter list into `out` (topo-sorted by inter-list
// dependency, in table order for determinism; a parameter whose expression
// fails — parse error, unknown name, cycle — keeps its last good `value` and
// records the reason in `error`). `base`, when non-null, seeds `out` first so a
// parameter can reference an OUTER scope (Phase V: a part-local expression
// resolving a project variable); same-named local parameters then shadow it.
// `overrideExpr`, when set, supplies a replacement expression per parameter
// name (configuration overrides) — return nullptr to use the stored expression.
// Shared by Document (local scope) and AppState (project scope).
void EvaluateParameterTable(
    std::vector<Parameter>& params, SymbolTable& out, const SymbolTable* base,
    const std::function<const std::string*(const std::string&)>& overrideExpr);

// The parametric model: an ordered feature list plus the bodies produced by
// regenerating it. This is the single source of truth for model state — the
// UI, scripts, and (later) files all act on the Document through Commands,
// never on geometry directly.
class Document {
public:
  Document() = default;
  // Deep copy: every feature is cloned (Feature::Clone), and so is everything
  // else the document IS — parameters, studies, CAM setups, configurations,
  // assets, units, rollback. What it merely CACHED (bodies, baked geometry,
  // the regen cache) is not copied; the copy regenerates itself, so the only
  // cost of duplicating a heavy part is one rebuild.
  //
  // The copy is a NEW document, not the same one twice: it gets a fresh
  // InstanceId() and starts at version 0. Sharing an instance id would put us
  // straight back into the autosave bug this file's InstanceId() comment
  // describes. Copy-ASSIGNMENT stays deleted (that const member) — a document
  // is replaced by swapping the pointer to it, never overwritten in place.
  Document(const Document& other);
  Document& operator=(const Document&) = delete;

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

  // Moves the feature at `from` so its new index becomes `to`, then
  // regenerates once. Returns false (document unchanged) if either index is
  // out of range or from == to. Reordering can dangle a downstream reference
  // (an extrude dragged before its sketch); that surfaces as a per-feature
  // regen error, which MoveFeatureCommand reads to accept or reject the move.
  bool MoveFeature(size_t from, size_t to);

  // Swaps the feature at index and regenerates. Returns false (and leaves
  // the document unchanged) if index is out of range or feature is null.
  bool ReplaceFeature(size_t index, std::unique_ptr<Feature> feature);

  const catalog::Json& GetCatalogs() const { return m_catalogs; }
  void SetCatalogsRaw(const catalog::Json& entries) {
    m_catalogs = catalog::Deserialize(entries);
  }
  catalog::Json TakeCatalogs() {
    return std::exchange(m_catalogs, catalog::Json::array());
  }

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
  // (and UI previews) evaluate against. This is the EFFECTIVE scope: the
  // project symbols (if any) overlaid with this document's local parameters
  // (local shadows project), so downstream reads are unchanged by Phase V.
  const SymbolTable& Symbols() const { return m_symbols; }

  // Project-wide variables (Phase V, docs/dogfooding.md, caster round). A
  // Document keeps its own m_parameters as the PART-LOCAL scope and borrows a
  // non-owning pointer to the project scope's evaluated symbols; regeneration
  // resolves expressions local-first, then project. AppState owns the project
  // table and points every slot's Document here (on load, part add, activate).
  // Headless and core tests inject the pointer directly, so the core stays
  // self- contained. Null = no project scope (a bare Document evaluates
  // local-only).
  void SetProjectSymbols(const SymbolTable* projectSymbols) {
    m_projectSymbols = projectSymbols;
  }
  const SymbolTable* ProjectSymbols() const { return m_projectSymbols; }

  // The project scope's parameter EXPRESSIONS (same owner as the symbols).
  // Needed when a configuration overrides a project variable: derived project
  // variables (`height = opening + 2*sec`) must re-evaluate under the
  // override for THIS document, which requires their expressions, not just
  // their project-level values. Null = overrides shadow the named variable
  // only (dependents keep their project values — the pre-wiring behavior).
  void SetProjectParameters(const std::vector<Parameter>* projectParameters) {
    m_projectParameters = projectParameters;
  }

  // Removes and returns the local parameter table without regenerating. Used
  // when a single loaded part is promoted to be the whole project (its params
  // become the project scope): the caller re-wires + regenerates.
  std::vector<Parameter> TakeParameters() {
    return std::exchange(m_parameters, {});
  }

  // Replaces the local parameter table without regenerating — serializer/loader
  // scaffolding (e.g. injecting the project scope into a single-part file
  // write, then restoring). Not for model edits; use SetParameter for those.
  void SetParametersRaw(std::vector<Parameter> params) {
    m_parameters = std::move(params);
  }

  // Rewrites references to `oldName` into `newName` in this document's feature
  // expressions and local-parameter expressions, WITHOUT renaming any local
  // parameter DECLARATION (the declaration lives in the project scope). Backs a
  // project-variable rename, which must rewrite refs across every part. Returns
  // false and touches nothing if a local parameter named `oldName` shadows the
  // project variable (references then resolve locally and must not move) — or
  // if newName is invalid. Does NOT regenerate; the caller regenerates all.
  bool RewriteProjectVarRefs(const std::string& oldName,
                             const std::string& newName);

  // ---- Studies (FEM) ----------------------------------------------------
  // Document-level analysis definitions, a sibling to the parameter table.
  // A study consumes the finished bodies and produces no geometry, so it is
  // not a Feature and does not participate in regeneration. Mesh and results
  // are derived data held elsewhere (never serialized); only this
  // configuration persists. Edits do NOT auto-regenerate (a study is inert
  // until explicitly meshed/solved).

  // CAM setups (v58, docs/cam/plan.md). Document-level objects like studies:
  // they describe how to cut the model, and change nothing about it — so they
  // are deliberately OUTSIDE the regen chain hash (a new setup must not
  // rebuild the geometry).

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

  // ---- Units ------------------------------------------------------------
  // The units this document is READ in. Presentation only: storage stays
  // millimetres, grams and degrees, so changing this cannot move a vertex.
  // Document-level rather than a preference because a part authored in
  // inches has to display in inches for whoever opens it (core/Units.h).
  const units::System& Units() const { return m_units; }
  void SetUnits(const units::System& u) { m_units = u; }

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
    if (id > m_nextId)
      m_nextId = id;
  }

  // Rebuilds all bodies by executing the feature list in order, after
  // re-evaluating the parameter table and every expression-driven feature
  // field. Returns false if any feature failed; bodies then reflect the
  // successful prefix.
  bool Regenerate();

  // Suspends regeneration for a batch of edits (loading a file, importing an
  // assembly), running exactly one Regenerate when the last guard releases.
  //
  // Every InsertFeature regenerates, which the prefix cache keeps cheap in
  // EXECUTION — but not in bookkeeping: each pass re-serializes and re-hashes
  // every feature so far, so loading N features costs N^2/2 feature-JSON
  // dumps (about 11k of them for a 148-feature part, tens of seconds before
  // any geometry is built). Adding the features first and regenerating once
  // is the same result for a fraction of the work.
  class BulkEdit {
  public:
    explicit BulkEdit(Document& doc) : m_doc(doc) { m_doc.m_bulkDepth++; }
    ~BulkEdit() {
      if (--m_doc.m_bulkDepth == 0)
        m_doc.Regenerate();
    }
    BulkEdit(const BulkEdit&) = delete;
    BulkEdit& operator=(const BulkEdit&) = delete;

  private:
    Document& m_doc;
  };
  bool InBulkEdit() const { return m_bulkDepth > 0; }

  const std::vector<Body>& GetBodies() const { return m_bodies; }

  // Baked geometry (docs/versioning/baked-geometry.md): bodies read from the
  // saved project, to be used INSTEAD of executing the feature list — but
  // only if they still describe it. The next Regenerate compares `tipHash`
  // against the chain hash it computes and either installs these bodies
  // (executing nothing) or ignores them and rebuilds. Loaders adopt inside a
  // BulkEdit, so that comparison happens exactly once.
  // Whether saving bakes the geometry beside the recipe (default true).
  // Persisted with the project; turning it off also writes a .gitignore, for
  // people who would rather keep the repo small and pay the rebuild.
  bool CommitGeometry() const { return m_commitGeometry; }
  void SetCommitGeometry(bool on) { m_commitGeometry = on; }

  // `warnings` maps feature id -> the advisory that feature raised when the
  // geometry was built. Warnings are a product of EXECUTION, and a baked open
  // executes nothing — without carrying them the model would silently lose
  // its advice ("this draft changes the body by 46%") on every open.
  void AdoptBakedGeometry(std::vector<Body> bodies, size_t tipHash,
                          std::map<uint64_t, std::string> warnings = {}) {
    m_bakedBodies = std::move(bodies);
    m_bakedHash = tipHash;
    m_bakedWarnings = std::move(warnings);
  }

  // ---- Reference-image assets (v49) -------------------------------------
  // Raw pixel bytes for sketch reference images (see
  // docs/sketching/reference-image-plan.md), keyed by asset name. Kept at
  // document scope — NOT on the sketch feature — so they survive undo/redo
  // (which rebuilds features from JSON, and JSON never carries the bytes) and
  // so the renderer decodes each asset into a texture once no matter how many
  // images reference it. Serialization persists only the assets an image
  // actually references.
  void SetAsset(const std::string& name, std::vector<unsigned char> bytes) {
    m_assets[name] = std::move(bytes);
  }
  const std::vector<unsigned char>* GetAsset(const std::string& name) const {
    auto it = m_assets.find(name);
    return it == m_assets.end() ? nullptr : &it->second;
  }
  const std::map<std::string, std::vector<unsigned char>>& GetAssets() const {
    return m_assets;
  }

  // Incremented on every regeneration; lets the UI detect when to re-upload
  // render meshes without observing individual edits.
  uint64_t Version() const { return m_version; }

  // Which document this is, as opposed to how far it has been edited. Unique
  // per instance for the life of the process and never reused.
  //
  // Anyone who remembers "I already handled version N" needs it, because
  // Version() restarts at 0 every time the document is replaced (new project,
  // open, history restore). Autosaver learned that the hard way: it skipped
  // saving a freshly opened document whose version happened to match the
  // version it had autosaved from the PREVIOUS one — silently, for the rest
  // of the session.
  uint64_t InstanceId() const { return m_instanceId; }

  // Incremental regeneration (R3): how much of the last Regenerate came
  // from the prefix cache vs. actually executed. Observable, not vibes.
  struct RegenStats {
    size_t executed = 0;
    size_t cached = 0;
  };
  const RegenStats& LastRegenStats() const { return m_lastStats; }

  // The chain hash of the whole feature list as of the last regeneration —
  // the same value the prefix cache keys its last entry on, exposed so the
  // saved geometry can be tagged with it (and a load can ask "is this blob
  // still the answer?"). 0 before the first regeneration or with no features.
  size_t TipChainHash() const { return m_tipHash; }
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
  // Persisted with the document, like the rollback marker in commercial CAD.
  int Rollback() const { return m_rollback; }
  void SetRollback(int index) { SetMarker(m_rollback, index); }

  // A SECOND rollback marker, NOT persisted: where the VIEW is stopping, as
  // opposed to where the user parked the bar. Sketch edit sets it to the
  // sketch's first consumer, so the solid built FROM the sketch is not in
  // front of the sketch while you edit it — the rollback every commercial CAD
  // does on "edit sketch". Kept apart from m_rollback because the document
  // you save must be the document you had: this one never reaches JSON, so no
  // format version, no golden, and a save during sketch edit is unaffected.
  int ViewRollback() const { return m_viewRollback; }
  void SetViewRollback(int index) { SetMarker(m_viewRollback, index); }

  // The marker regeneration actually stops at: the EARLIER of the two, since
  // each independently says "do not execute past here". -1 = the end.
  int EffectiveRollback() const {
    if (m_rollback < 0)
      return m_viewRollback;
    if (m_viewRollback < 0)
      return m_rollback;
    return std::min(m_rollback, m_viewRollback);
  }

  // The index of the earliest feature that NAMES `featureId` (Feature::
  // VisitFeatureRefs), searching only after that feature's own position — a
  // reference may only point backwards, so nothing at or before it consumes
  // it. -1 when the id is unknown or nothing consumes it.
  int FirstConsumerIndex(uint64_t featureId) const;

private:
  // Topo-sorts m_parameters by expression dependency and evaluates them into
  // m_symbols. Failures (parse error, unknown name, cycle) set the affected
  // parameters' `error` and keep their last good values.
  void EvaluateParameters();

  std::vector<std::unique_ptr<Feature>> m_features;
  std::map<std::string, std::vector<unsigned char>>
      m_assets; // v49 image pixels
  std::vector<Body> m_bodies;
  catalog::Json m_catalogs = catalog::Json::array();
  std::vector<Parameter> m_parameters;
  std::vector<fem::Study> m_studies;
  // Transient per-study mesh+results (never serialized); keyed by study id.
  std::map<uint64_t, fem::StudyRuntime> m_studyRuntime;
  uint64_t m_studyResultsEpoch = 0;
  static uint64_t NextInstanceId();

  std::vector<Configuration> m_configurations;
  units::System m_units;
  uint64_t m_activeConfiguration = 0; // 0 = base; persisted like m_rollback
  SymbolTable m_symbols;
  // Non-owning; owned by AppState (Phase V). Null for a bare Document.
  const SymbolTable* m_projectSymbols = nullptr;
  const std::vector<Parameter>* m_projectParameters = nullptr;
  uint64_t m_version = 0;
  // Handed out once per instance by NextInstanceId(); see InstanceId().
  const uint64_t m_instanceId = NextInstanceId();
  uint64_t m_nextId = 1;
  // Sets one of the two rollback markers, clamping "at or past the end" to
  // -1, and regenerates when it actually moved. Shared so the transient
  // marker cannot drift from the persisted one's rules.
  void SetMarker(int& marker, int index) {
    if (index < 0 || index >= static_cast<int>(m_features.size()))
      index = -1;
    if (index == marker)
      return;
    marker = index;
    Regenerate();
  }

  int m_rollback = -1;
  int m_viewRollback = -1; // transient; see ViewRollback()
  int m_bulkDepth = 0;     // >0 = a BulkEdit guard is deferring regeneration
  size_t m_tipHash = 0;    // chain hash of the last regenerated feature list
  // Bodies loaded from the project's geometry blob, awaiting the hash check
  // in the next Regenerate. Cleared once used or rejected — this is a
  // hand-off, not a second source of truth.
  std::vector<Body> m_bakedBodies;
  size_t m_bakedHash = 0;
  std::map<uint64_t, std::string> m_bakedWarnings;
  bool m_commitGeometry = true;

  // Prefix cache (R3): per feature, the chain hash of everything up to and
  // including it, plus a snapshot of the body list after it executed. Body
  // snapshots are cheap (TopoDS_Shape is refcounted). Entries beyond the
  // current rollback stay — rolling forward again is then instant.
  struct RegenCacheEntry {
    size_t hash = 0;
    std::vector<Body> bodiesAfter;
    // False for the placeholder entries baked geometry installs: the blob
    // carries only the TIP bodies, so intermediate entries know their hash
    // but not the body list at that point. A divergence landing on one of
    // those cannot resume from it — Regenerate falls back to a full rebuild.
    bool bodiesValid = true;
  };
  std::vector<RegenCacheEntry> m_cache;
  RegenStats m_lastStats;
};
