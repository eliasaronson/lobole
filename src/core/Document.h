// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "core/catalog/Catalog.h"

#include "core/Configuration.h"
#include "core/Expression.h"
#include "core/Feature.h"
#include "core/LookupTable.h"
#include "core/PrintAllowance.h"
#include "core/PrintSettings.h"
#include "core/SheetMetal.h"
#include "core/Template.h"
#include "core/Units.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/tube/Rules.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// A permitted variation of one measured feature of the part: "this bore is
// H7". Design intent, not geometry — see docs/tolerances/README.md.
//
// It stores the CLASS, not the deviations it implies, for the reason the hole
// fit stores "M6 normal" rather than 6.6: one source of truth, so a correction
// to the table reaches every document. `lower`/`upper` are for the case a
// class cannot express — a deviation pair straight off a drawing — and are
// read only when `isoClass` is empty.
//
// One per element: the same face cannot be two sizes at once, so setting a
// tolerance on a face that has one REPLACES it.
struct Tolerance {
  uint64_t id = 0; // document-unique, from the shared id counter
  ElementRef ref;  // the face this is a tolerance ON
  // Set (v99) makes this a tolerance on the DISTANCE from `ref` to `ref2`
  // rather than on `ref`'s own size — a shoulder-to-seat length, a slot
  // width, a boss height. Diameters are the easy half: a part with three
  // perfectly toleranced bores and forty untoleranced lengths is still not
  // a drawing anybody can make from.
  ElementRef ref2;
  std::string isoClass; // "H7"; empty means the pair below is authoritative
  double lower = 0.0;   // mm, signed deviation from nominal
  double upper = 0.0;   // mm, signed deviation from nominal
  // v179: a PRINTED PRESS (docs/tolerances/README.md#printed-press-fits),
  // set on both faces of a printed cap pressed on a printed pin. Its band is
  // not stored: it is the part's print profile's `press` allowance, give or
  // take a tenth of its gap (printfit::PrintedPress), so a corrected profile
  // reaches every press, as a corrected ISO table reaches every class. With
  // it set, `isoClass` is empty and `lower`/`upper` are not read.
  bool printedPress = false;
  std::string comment;

  // A distance tolerance never carries an ISO class: a class states how a
  // SHAFT or a BORE may vary, and says nothing about the gap between two
  // arbitrary faces. The asymmetry is real, so it is visible here rather
  // than papered over with a class that would quietly mean something else.
  [[nodiscard]] bool IsDistance() const { return ref2.IsSet(); }
  // Explicit deviations: neither a class nor a printed press.
  [[nodiscard]] bool IsExplicit() const {
    return isoClass.empty() && !printedPress;
  }
};

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
  // the regen cache) is not copied; this copy regenerates itself, so the cost
  // of duplicating a heavy part is one full rebuild — which is exactly what
  // CopyWithGeometry below exists to avoid for a caller that only wants to
  // READ the geometry.
  //
  // The copy is a NEW document, not the same one twice: it gets a fresh
  // InstanceId() and starts at version 0. Sharing an instance id would put us
  // straight back into the autosave bug this file's InstanceId() comment
  // describes. Copy-ASSIGNMENT stays deleted (that const member) — a document
  // is replaced by swapping the pointer to it, never overwritten in place.
  Document(const Document& other);
  Document& operator=(const Document&) = delete;

  // A copy that REUSES `other`'s bodies instead of re-deriving them.
  //
  // The plain copy above ends in a Regenerate(), which executes the whole
  // feature list — real OCCT booleans, lofts, fillets. For a copy taken to
  // READ geometry (a motion run's snapshot of the assembly) that is pure
  // waste: the bodies it rebuilds are the ones it was just handed. On the
  // SO-101 gripper it was 54% of a profile, all of it BooleanFeature::Execute
  // re-cutting cuts that were already cut.
  //
  // It works by handing the copy `other`'s bodies as baked geometry, so the
  // same tip-hash comparison an opened project goes through decides whether
  // they still describe the feature list — see AdoptBakedGeometry. That makes
  // the failure mode SLOW, not WRONG: if `other` is mid-edit, or its hash
  // does not match for any reason, the copy rebuilds exactly as the plain one
  // does. Datums still execute (their output is not a body), which is the
  // same rule a baked open follows.
  //
  // Returns a POINTER on purpose. Returned by value, the natural call site
  // `make_unique<Document>(CopyWithGeometry(d))` binds the rvalue to the COPY
  // constructor above — Document declares one, so it has no implicit move —
  // and pays a second, plain copy: a full regeneration, exactly the cost this
  // exists to avoid. That is not a small tax, it is a net loss; it took the
  // gripper's motion prep from 3.4 s to 10.7 s, and it is invisible at the
  // call site.
  static std::unique_ptr<Document> CopyWithGeometry(const Document& other);

  size_t FeatureCount() const { return m_features.size(); }
  Feature* GetFeature(size_t index);
  [[nodiscard]] const Feature* GetFeature(size_t index) const {
    return const_cast<Document*>(this)->GetFeature(index);
  }
  const std::vector<std::unique_ptr<Feature>>& GetFeatures() const {
    return m_features;
  }
  // Lookup by stable id (see Feature::GetId); nullptr if absent. Returns the
  // feature's current index through outIndex if given.
  Feature* FindFeatureById(uint64_t id, size_t* outIndex = nullptr);
  // The const half, for the readers: a question about a document has no
  // business needing a mutable one.
  [[nodiscard]] const Feature*
  FindFeatureById(uint64_t id, size_t* outIndex = nullptr) const {
    return const_cast<Document*>(this)->FindFeatureById(id, outIndex);
  }

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

  const catalog::Json& Generator() const { return m_generator; }
  void SetGeneratorRaw(catalog::Json stamp) {
    if (!stamp.is_null()) {
      if (!stamp.is_object() || stamp.size() != 4)
        throw std::runtime_error("invalid generated component stamp");
      for (const char* field :
           {"namespace", "key", "fingerprint", "recipe_hash"})
        if (!stamp.contains(field) || !stamp[field].is_string() ||
            stamp[field].get<std::string>().empty())
          throw std::runtime_error("invalid generated component stamp field: " +
                                   std::string(field));
    }
    m_generator = std::move(stamp);
    MarkEdited();
  }

  const catalog::Json& GetCatalogs() const { return m_catalogs; }
  // Save scaffolding, like SetParametersRaw below: the project's catalogs go
  // in for a write and come straight back out, so neither stamps.
  void SetCatalogsRaw(const catalog::Json& entries) {
    m_catalogs = catalog::Deserialize(entries);
  }
  catalog::Json TakeCatalogs() {
    return std::exchange(m_catalogs, catalog::Json::array());
  }

  // Project tables (docs/variables/README.md#tables) riding this document:
  // a single-part file is the whole project, so the project's tables are
  // written into it and taken back out on load (TakeTables), exactly like
  // the project variables. A document that keeps them (a core test, a file
  // read without an app) evaluates its own lookups against them.
  const std::vector<lookuptable::Table>& GetTables() const { return m_tables; }
  void SetTablesRaw(std::vector<lookuptable::Table> tables) {
    m_tables = std::move(tables);
  }
  std::vector<lookuptable::Table> TakeTables() {
    return std::exchange(m_tables, {});
  }

  // The project's requirements (docs/requirements/README.md) riding this
  // document, as their saved JSON list: a single-part file is the whole
  // project, so they are written into it and taken back out on load
  // (TakeRequirementsRaw), like the tables above. Validated when read
  // (req::ListFromJson); the app holds the parsed list.
  const nlohmann::json& GetRequirementsRaw() const { return m_requirements; }
  void SetRequirementsRaw(nlohmann::json list) {
    m_requirements = std::move(list);
  }
  nlohmann::json TakeRequirementsRaw() {
    return std::exchange(m_requirements, nlohmann::json::array());
  }

  // The project's template library (docs/modeling/templates.md#the-library)
  // riding this document the way the tables do: a single-part file carries
  // it (format v161), and the open takes it back into the project
  // (TakeTemplates). A Template instance never READS it -- it expands the
  // definition it carries -- so a document without it regenerates the same.
  const std::vector<tmpl::Template>& GetTemplates() const {
    return m_templates;
  }
  void SetTemplatesRaw(std::vector<tmpl::Template> templates) {
    m_templates = std::move(templates);
  }
  std::vector<tmpl::Template> TakeTemplates() {
    return std::exchange(m_templates, {});
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
  // (local shadows project), so a downstream read sees one table.
  const SymbolTable& Symbols() const { return m_symbols; }

  // The effective scope as it would be in `configuration` (0 = base),
  // whichever is active, without regenerating: the variables only, which is
  // all an expression requirement reads
  // (docs/requirements/README.md#expression). A configuration the document
  // does not have reads as base; the caller checks FindConfiguration first.
  SymbolTable SymbolsIn(uint64_t configuration) const;

  // Project-wide variables (docs/variables/README.md#concepts). A
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
  // are derived data held elsewhere (StudyResults, saved beside the study
  // with the input hash that produced them). Edits do NOT auto-regenerate (a
  // study is inert until explicitly meshed/solved).

  // CAM setups (v58, docs/cam/README.md#invariants). Document-level objects
  // like studies: they describe how to cut the model, and change nothing about
  // it — so they are deliberately OUTSIDE the regen chain hash (a new setup
  // must not rebuild the geometry).

  // Read-only on purpose: a study is saved and does not move Version(), so an
  // edit through a mutable reference would never reach SaveStamp(). Edit one
  // with ReplaceStudy.
  const std::vector<fem::Study>& GetStudies() const { return m_studies; }
  const fem::Study* FindStudy(uint64_t id) const;
  // Write access for a RUN, not an edit: meshing and solving refresh a
  // study's transient fields (its error, its evaluated expressions), and a
  // load drag previews on it until its ModifyStudyCommand lands. None of that
  // is a saved change, so this does not stamp.
  fem::Study* StudyForRun(uint64_t id);

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

  // The mesh+results cache for a study, or nullptr if it has never been run.
  // Derived data, saved with the project and read back on load (v153;
  // docs/simulation/README.md#studies-as-jobs); stale when its
  // solvedAtVersion differs from Version(). Mutable so const RPC reads can
  // populate/query it.
  const fem::StudyRuntime* StudyResults(uint64_t studyId) const {
    auto it = m_studyRuntime.find(studyId);
    return it == m_studyRuntime.end() ? nullptr : &it->second;
  }
  // Kept as it is read back and saved (fem::KeepWhatIsRead).
  void StoreStudyResults(uint64_t studyId, fem::StudyRuntime rt) {
    fem::KeepWhatIsRead(rt);
    m_studyRuntime[studyId] = std::move(rt);
    ++m_studyResultsEpoch;
  }
  // Monotonic counter bumped on every StoreStudyResults; lets the field
  // overlay detect a re-solve (study edits don't bump Version()).
  uint64_t StudyResultsEpoch() const { return m_studyResultsEpoch; }
  // Stale when solved at another version, and a static study of a topology
  // result (fem::ShapeSource) also when that result is stale, gone, or no
  // longer the one it was cut from.
  bool StudyResultsStale(uint64_t studyId) const;
  // Stamps a stored result as solved at this version: one whose inputs hash
  // as they did when it was made (fem::RestampByInputs). False when absent.
  bool MarkStudyResultsCurrent(uint64_t studyId);

  // ---- Tolerances (docs/tolerances/README.md) ------------------------------
  // Permitted variation of measured features, keyed by element. Like studies
  // and CAM setups these are document-level objects OUTSIDE the regen chain
  // hash, and here that is not merely an optimisation but the whole claim: a
  // size tolerance does not move a surface, so a tolerance-only edit must
  // rebuild exactly zero features. tests/test_tolerances.cpp asserts it.

  const std::vector<Tolerance>& GetTolerances() const { return m_tolerances; }
  const Tolerance* FindTolerance(uint64_t id) const;
  // The SIZE tolerance on `ref`, or nullptr — never a distance tolerance that
  // merely starts there, which is a statement about a gap and not about this
  // face. The lookup check_fits runs per mate.
  const Tolerance* ToleranceFor(const ElementRef& ref) const;
  // The tolerance on the same SUBJECT — `ref` alone for a size, the ordered
  // pair for a distance. This is the key SetTolerance upserts on, and so the
  // one an undo has to snapshot: ToleranceFor answers a different question
  // ("how big is this face"), and using it to snapshot a DISTANCE edit either
  // finds nothing and undoes by deleting, or finds the face's unrelated size
  // tolerance and undoes nothing at all.
  const Tolerance* ToleranceForSubject(const ElementRef& ref,
                                       const ElementRef& ref2) const;

  // Adds or REPLACES the tolerance on the same SUBJECT — `t.ref` for a size,
  // the ordered pair (`t.ref`, `t.ref2`) for a distance, so a face can carry
  // its own size and any number of distances measured from it. Assigns a
  // fresh id when `t.id` is 0 and keeps list position when it replaces.
  // Returns the stored tolerance.
  const Tolerance& SetTolerance(Tolerance t);
  // Removes the tolerance with `id`; false if absent.
  bool RemoveTolerance(uint64_t id);
  // Appends a fully-formed tolerance keeping its id, raising the id
  // high-water mark past it. For deserialization and redo.
  void RestoreTolerance(Tolerance t);

  // ---- Units ------------------------------------------------------------
  // The units this document is READ in. Presentation only: storage stays
  // millimetres, grams and degrees, so changing this cannot move a vertex.
  // Document-level rather than a preference because a part authored in
  // inches has to display in inches for whoever opens it (core/Units.h).
  const units::System& Units() const { return m_units; }
  void SetUnits(const units::System& u) {
    m_units = u;
    MarkEdited();
  }

  // ---- Sheet metal (docs/sheetmetal/README.md) -----------------------------
  // The stock this part is made from and how it bends: one per document,
  // read by every sheet feature during regeneration. Unset until a sheet
  // feature needs it, and then required — a flange cannot guess its own
  // thickness. Document-level for the reason units and tolerances are: a
  // part is cut from ONE sheet, so a per-feature gauge would let a body be
  // two thicknesses at once and nothing could refuse it.
  //
  // Unlike tolerances, this one IS geometry: changing the gauge rebuilds
  // every sheet feature, which is why it participates in the regen chain
  // hash (Document::ChainHash).
  const std::optional<sheet::SheetMetalRules>& SheetMetal() const {
    return m_sheetMetal;
  }
  // Regenerates, unlike SetUnits and the tolerance mutators: the gauge is an
  // input to geometry, so changing it has to rebuild the flanges that read it.
  void SetSheetMetal(std::optional<sheet::SheetMetalRules> rules) {
    m_sheetMetal = std::move(rules);
    Regenerate();
  }

  // The stock a tube part is BENT from — the material, the size and the
  // bender, read by check_tube and the cut list. Document-level for the same
  // reason the sheet's gauge is: a frame member is one length of tube, and a
  // per-feature size would let a body be two walls at once.
  //
  // Unlike the sheet's rules this drives NO geometry — a Pipe carries its own
  // diameter and wall, and a former is a number on the path. Changing it can
  // therefore not rebuild anything, and does not: it changes what the checks
  // say, which is why it sits outside the regen chain like a tolerance.
  const std::optional<tube::TubeRules>& Tube() const { return m_tube; }
  void SetTube(std::optional<tube::TubeRules> rules) {
    m_tube = std::move(rules);
    MarkEdited();
  }

  // How this part is PRINTED — the process and the material it is printed in
  // (docs/tolerances/README.md#printed-parts). Unset for a part that is not
  // printed. It lives on the part, not on a body's material or a preference:
  // a part is made one way, by one process, whichever machine or tab opens it,
  // and the same PETG is printed or moulded with different allowances.
  //
  // It drives ONE kind of geometry: a Hole with a clearance fit is modelled
  // at the fit's size plus the hole allowance, because the fit names the
  // size it must come out at. Everything else it changes is what check_fit,
  // check_fits, print_allowance and the Thread editor say should be
  // modelled. The regen hash folds the process into fitted clearance holes
  // only, so setting it rebuilds those (and what follows them), and on a
  // part without one it does not regenerate at all — the version stays, so
  // nothing reads a solved study as stale over it.
  const std::optional<printfit::Process>& PrintProcess() const {
    return m_print;
  }
  void SetPrintProcess(std::optional<printfit::Process> p);
  // The part's SLICER settings and its bodies' overrides (v181,
  // core/PrintSettings.h): walls, shells, infill, supports, brim. What
  // export_print_job hands the slicer per object and what the printed mass
  // weighs. Only the part's `direction` drives geometry -- a printed hole's
  // teardrop points up it -- so a new one rebuilds those holes and nothing
  // else regenerates.
  const printset::PartSettings& PrintSettings() const {
    return m_printSettings;
  }
  void SetPrintSettings(printset::PartSettings s);

  // ---- Configurations ---------------------------------------------------
  // Named variants (parameter overrides + feature suppression), a sibling to
  // the parameter table like studies — not Features. The ACTIVE configuration
  // (0 = base, the model as authored) is applied during regeneration by
  // overlaying its overrides onto the symbol table and composing its
  // suppression set; no stored parameter/feature state is mutated. See
  // docs/configurations/README.md.

  const std::vector<Configuration>& GetConfigurations() const {
    return m_configurations;
  }
  // Read-only for the reason GetStudies is: edit one with
  // ReplaceConfiguration, which stamps.
  const Configuration* FindConfiguration(uint64_t id) const;

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

  // Bumped whenever the CONFIGURATION LIST changes — added, removed, edited.
  // Editing a configuration the document does not have active changes no
  // geometry, so Version() does not move; anything that evaluates this
  // document in another configuration (a drawing view naming one) would go on
  // showing the variant as it was. Session-local like Version(), and not
  // copied: a copy starts its own count.
  uint64_t ConfigurationsRevision() const { return m_configurationsRevision; }

  // Which configuration regeneration applies (0 = base). Persisted like the
  // rollback bar. SetActiveConfiguration regenerates; returns false for an
  // unknown non-zero id.
  uint64_t ActiveConfiguration() const { return m_activeConfiguration; }
  bool SetActiveConfiguration(uint64_t id);
  // Loader hook: set the active id without regenerating (the load's own regen
  // rebuilds bodies once). Does not stamp: a loader sets its baseline after,
  // and a motion run sets it on a throwaway copy.
  void SetActiveConfigurationRaw(uint64_t id) { m_activeConfiguration = id; }
  // The active configuration, or nullptr when base (0) or unknown.
  const Configuration* ActiveConfig() const;

  // Whether feature `index` is suppressed under the active configuration: its
  // authored `suppressed` flag OR the active config suppresses its id. This is
  // what regeneration honors (and folds into the regen cache key).
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
  // Every InsertFeature regenerates, which the regen cache keeps cheap in
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

  // Baked geometry (docs/versioning/README.md#baked-geometry): bodies read from
  // the saved project, to be used INSTEAD of executing the feature list — but
  // only if they still describe it. The next Regenerate compares `tipHash`
  // against the chain hash it computes and either installs these bodies
  // (executing nothing) or ignores them and rebuilds. Loaders adopt inside a
  // BulkEdit, so that comparison happens exactly once.
  // Whether saving bakes the geometry beside the recipe (default true).
  // Persisted with the project; turning it off also writes a .gitignore, for
  // people who would rather keep the repo small and pay the rebuild.
  bool CommitGeometry() const { return m_commitGeometry; }
  void SetCommitGeometry(bool on) {
    m_commitGeometry = on;
    MarkEdited();
  }

  // `warnings` maps feature id -> the advisory that feature raised when the
  // geometry was built. Warnings are a product of EXECUTION, and a baked open
  // executes nothing — without carrying them the model would silently lose
  // its advice ("this draft changes the body by 46%") on every open.
  // `source` names where the blob was read (see BodiesAreBlob); empty for
  // bodies handed over from another document.
  void AdoptBakedGeometry(std::vector<Body> bodies, size_t tipHash,
                          std::map<uint64_t, std::string> warnings = {},
                          std::string source = {}) {
    m_bakedBodies = std::move(bodies);
    m_bakedHash = tipHash;
    m_bakedWarnings = std::move(warnings);
    m_bakedSource = std::move(source);
  }

  // ---- Reference-image assets (v49) -------------------------------------
  // Raw pixel bytes for sketch reference images (see
  // docs/sketching/README.md#reference-images-underlays), keyed by asset name.
  // Kept at document scope — NOT on the sketch feature — so they survive
  // undo/redo (which rebuilds features from JSON, and JSON never carries the
  // bytes) and so the renderer decodes each asset into a texture once no matter
  // how many images reference it. Serialization persists only the assets an
  // image actually references.
  void SetAsset(const std::string& name, std::vector<unsigned char> bytes) {
    m_assets[name] = std::move(bytes);
    MarkEdited();
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

  // Whether the document has changed since it was saved, as one number: it
  // moves on every regeneration AND on every edit to saved state that
  // regenerates nothing — a tolerance, the units, the tube or print process,
  // a study, a configuration that is not the active one, an asset. Dirty
  // tracking and autosave compare THIS against the stamp taken at the save,
  // never Version(): those edits leave Version() where it was, and a project
  // that reads clean is closed without a prompt.
  //
  // Both counters only grow, so the sum moves whenever either does. An undo
  // is an edit too — undoing back to the saved state reads dirty, which is
  // the safe direction. Every non-const mutator of saved state either
  // regenerates or calls MarkEdited(); tests/test_conventions.cpp holds new
  // ones to that (docs/versioning/README.md#unsaved-changes).
  uint64_t SaveStamp() const { return m_version + m_edits; }

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
  // from the regeneration cache vs. actually executed. Observable, not vibes.
  struct RegenStats {
    size_t executed = 0;
    size_t cached = 0;
  };
  const RegenStats& LastRegenStats() const { return m_lastStats; }

  // The chain hash of the whole feature list as of the last regeneration —
  // the same value the regen cache keys its last entry on, exposed so the
  // saved geometry can be tagged with it (and a load can ask "is this blob
  // still the answer?"). 0 before the first regeneration or with no features.
  size_t TipChainHash() const { return m_tipHash; }
  // Whether the bodies in memory were INSTALLED from the geometry blob at
  // `source` (the directory a baked open read) carrying `hash`, with nothing
  // executed since. A save may skip rewriting a blob that carries the
  // recipe's hash only when this holds: the hash names the recipe, not the
  // code that ran it, and a build that changed what a recipe produces
  // regenerated a part, saved it, and left the previous build's bodies on
  // disk under the same hash. The source matters as much as the hash: a
  // project opened from one file and saved as another must write the second
  // file's blob, whatever hash an older save left there.
  bool BodiesAreBlob(size_t hash, const std::string& source) const {
    return m_bodiesFromBlob != 0 && m_bodiesFromBlob == hash &&
           !source.empty() && m_bodiesBlobSource == source;
  }
  void ClearRegenCache() {
    m_cache.clear();
    m_chain.clear();
  }
  // How many evaluations the regeneration cache holds, across every history
  // it remembers, and the most it may (RegenCacheCap). For the cap test.
  size_t RegenCacheSize() const { return m_cache.size(); }
  size_t RegenCacheCap() const;

  // Whether the feature's last execution ended in a fault OpenCASCADE's
  // signal scope caught (occsig::FaultCount moved during it): its error is
  // an ordinary one, and `regen` marks it `kernel_fault`. The regeneration
  // that saw it dropped the regen cache, and does not cache the result, so
  // nothing built on a heap the fault may have written is served again.
  bool KernelFaulted(uint64_t featureId) const {
    return m_kernelFaulted.count(featureId) != 0;
  }

  // The body list as it stood after feature `index` executed, from the regen
  // cache — i.e. feature `index + 1`'s input. Valid for the chain the last
  // Regenerate covered; null when the cache doesn't reach `index`. Used by
  // edge picking to resolve refs against a blend's input.
  const std::vector<Body>* CachedBodiesAfter(size_t index) const {
    if (index >= m_chain.size())
      return nullptr;
    const auto it = m_cache.find(m_chain[index]);
    return it != m_cache.end() ? &it->second.bodiesAfter : nullptr;
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
  // A feature naming an id the feature at `featureId` OWNS (a Template
  // instance's expanded features, Feature::VisitOwnedIds) consumes it too.
  int FirstConsumerIndex(uint64_t featureId) const;

  // The index of the feature that owns `id` -- the feature itself, or the
  // Template instance whose expanded feature it is. -1 when nothing does.
  int OwnerIndex(uint64_t id) const;

private:
  // The one copy implementation; both public forms delegate to it.
  Document(const Document& other, bool reuseGeometry);

  // Records an edit to saved state that does not regenerate (SaveStamp()).
  // CommandStack calls it too: whatever a command, its undo or its redo
  // did, it was an edit — the backstop for a command that changes saved
  // state some way the mutators above do not see.
  void MarkEdited() { ++m_edits; }
  friend class CommandStack;

  // Topo-sorts m_parameters by expression dependency and evaluates them into
  // m_symbols. Failures (parse error, unknown name, cycle) set the affected
  // parameters' `error` and keep their last good values.
  void EvaluateParameters();
  // EvaluateParameters' body: `params` evaluated into `out` under `cfg`
  // (null = base), against this document's project scope and tables.
  void EvaluateInto(const Configuration* cfg, std::vector<Parameter>& params,
                    SymbolTable& out) const;

  std::vector<std::unique_ptr<Feature>> m_features;
  std::map<std::string, std::vector<unsigned char>>
      m_assets; // v49 image pixels
  std::vector<Body> m_bodies;
  catalog::Json m_generator = nullptr;
  catalog::Json m_catalogs = catalog::Json::array();
  std::vector<lookuptable::Table> m_tables;
  nlohmann::json m_requirements = nlohmann::json::array();
  std::vector<tmpl::Template> m_templates;
  std::vector<Parameter> m_parameters;
  std::vector<Tolerance> m_tolerances;
  std::vector<fem::Study> m_studies;
  // Per-study mesh+results (StudyResults); keyed by study id.
  std::map<uint64_t, fem::StudyRuntime> m_studyRuntime;
  uint64_t m_studyResultsEpoch = 0;
  static uint64_t NextInstanceId();

  std::vector<Configuration> m_configurations;
  units::System m_units;
  std::optional<sheet::SheetMetalRules> m_sheetMetal;
  std::optional<tube::TubeRules> m_tube;
  std::optional<printfit::Process> m_print;
  printset::PartSettings m_printSettings;
  uint64_t m_activeConfiguration = 0; // 0 = base; persisted like m_rollback
  SymbolTable m_symbols;
  // Non-owning; owned by AppState (Phase V). Null for a bare Document.
  const SymbolTable* m_projectSymbols = nullptr;
  const std::vector<Parameter>* m_projectParameters = nullptr;
  uint64_t m_version = 0;
  uint64_t m_configurationsRevision = 0;
  uint64_t m_edits = 0; // saved-state edits that did not regenerate
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
  std::string m_bakedSource;
  size_t m_bodiesFromBlob = 0;    // see BodiesAreBlob()
  std::string m_bodiesBlobSource; // see BodiesAreBlob()
  bool m_commitGeometry = true;
  std::set<uint64_t> m_kernelFaulted; // see KernelFaulted()

  // Regeneration cache (R3): one entry per feature EVALUATION, keyed on the
  // chain hash of everything up to and including that feature, so an entry
  // means "identical inputs all the way down" wherever it sits. It remembers
  // more than the current history: setting a value back, undo and redo find
  // the evaluations an earlier history made and execute nothing
  // (docs/architecture.md#the-document-model-and-regeneration). Bounded by
  // RegenCacheCap(), least recently used first, never an entry of the
  // current chain. Body snapshots are cheap (TopoDS_Shape is refcounted).
  struct RegenCacheEntry {
    std::vector<Body> bodiesAfter;
    // False for the placeholder entries baked geometry installs: the blob
    // carries only the TIP bodies, so intermediate entries know their hash
    // but not the body list at that point. Execution cannot resume from
    // one — Regenerate falls back to a full rebuild.
    bool bodiesValid = true;
    // The feature as its Execute left it (a Clone taken right after), which
    // a hit restores through Feature::CopyDerivedState: the live object may
    // last have executed in ANOTHER history. Null for a suppressed feature
    // (nothing executed) and for the bake's entries, which are usable only
    // while the live object still holds that evaluation
    // (Feature::DerivedFor).
    std::shared_ptr<const Feature> executed;
    uint64_t lastUse = 0; // m_regenClock of the last regeneration it served
  };
  std::unordered_map<size_t, RegenCacheEntry> m_cache;
  // The chain hashes of the current feature list, by index: what
  // CachedBodiesAfter reads. Entries beyond the rollback bar stay, so rolling
  // forward again is instant (their hashes re-verify then).
  std::vector<size_t> m_chain;
  uint64_t m_regenClock = 0;
  void EvictRegenCache();
  RegenStats m_lastStats;
};
