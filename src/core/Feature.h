#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Expression.h"

#include <functional>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A visual override SEPARATE from the physical material
// (docs/rendering/appearance-plan.md): every unset field inherits from the
// material, so physics (density, FEM, BOM) never sees this. Persisted on the
// creating feature (v54), stamped onto its bodies each regeneration, and
// STRIPPED from the regen chain hash — recoloring never rebuilds B-Rep.
struct Appearance {
  std::optional<glm::vec3> color; // sRGB base color
  std::optional<float> roughness; // 0..1
  std::optional<float> metallic;  // 0..1
  bool Any() const {
    return color.has_value() || roughness.has_value() || metallic.has_value();
  }
};

// A named solid produced by regenerating the feature list. `material` is a slug
// into the render-layer material library (empty = default); it is transient
// like the shape — the feature that creates the body stamps its own material
// here on every regeneration.
struct Body {
  std::string name;
  CADShape shape;
  std::string material;
  // Stable id of the feature that *created* this body (a primitive, or an
  // extrude/revolve in NewBody mode). Add/Cut modify a body without changing
  // it, so it survives fusing. Transient like the shape — stamped every
  // regeneration; lets features reference a whole body by its producer
  // (BooleanFeature). 0 = unstamped.
  uint64_t createdBy = 0;

  // Stable names for this body's faces (topological naming, T2). Transient,
  // rebuilt each regeneration. Empty when the producing feature doesn't name
  // its output yet (only extrude NewBody does so far).
  ElementMap elementMap;

  // Optional display color carried in from interchange (XCAF import) — a
  // viewport appearance, NOT a physical material (FEM still requires an
  // explicit material). Transient like the shape; the importing feature
  // stamps it every regeneration. Renderers prefer material > this > palette.
  // Last on purpose: existing aggregate inits stop at createdBy.
  bool hasDisplayColor = false;
  glm::vec3 displayColor{0.0f};

  // User appearance override, stamped from the creating feature after each
  // regeneration (Document::Regenerate post-pass keyed on createdBy).
  // Renderers prefer: appearance > material > displayColor > palette.
  Appearance appearance;
};

class Feature;

// What a feature sees while executing during regeneration: the bodies built
// so far, and read access to the features strictly BEFORE it in the history
// (a feature can only reference what already exists — e.g. an extrude can
// only consume an earlier sketch).
struct RegenContext {
  std::vector<Body>& bodies;
  const std::vector<std::unique_ptr<Feature>>& features;
  size_t currentIndex;
  // The document's evaluated parameter values (never null during a normal
  // regeneration); what expression-driven fields were evaluated against.
  const SymbolTable* symbols = nullptr;

  // Searches features[0, currentIndex) by stable id; nullptr if absent.
  const Feature* FindById(uint64_t id) const;
};

// One step in the parametric history. Features are executed in order by
// Document::Regenerate() to rebuild the model from scratch, so they must be
// deterministic and self-contained (all inputs are parameters, never live
// geometry).
class Feature {
public:
  virtual ~Feature() = default;

  const std::string& GetName() const { return m_name; }
  void SetName(const std::string& name) { m_name = name; }

  // Material slug (into the render-layer material library) applied to any body
  // this feature creates. Empty = default. Persistent + serialized; a feature
  // that only modifies existing bodies (extrude add/cut) leaves their material
  // untouched.
  const std::string& GetMaterial() const { return m_material; }
  void SetMaterial(const std::string& material) { m_material = material; }

  // Visual override applied to this feature's bodies on top of the material
  // (persistent + serialized, v54). Empty (no field set) = pure material.
  const Appearance& GetAppearance() const { return m_appearance; }
  void SetAppearance(const Appearance& appearance) {
    m_appearance = appearance;
  }

  // Stable identity for feature->feature references (an extrude names its
  // sketch by id, which survives reordering and undo). 0 = unassigned; the
  // Document assigns ids on insert and serialization round-trips them.
  uint64_t GetId() const { return m_id; }
  void SetId(uint64_t id) { m_id = id; }

  virtual std::string TypeName() const = 0;

  // A deep copy of this feature — id, name, material and all. The one
  // deep-copy path there is: duplicating a tab clones the document, which
  // clones every feature through here. PURE virtual on purpose, so a new
  // feature type cannot arrive without one; a runtime table could be
  // forgotten, and the copy would then silently lose that feature.
  // Every concrete feature is a plain value (no owning pointers), so each
  // override is `make_unique<T>(*this)`. Cached geometry (a cut tool's
  // shape/element map) comes along and is overwritten by the clone's first
  // regeneration — harmless either way.
  [[nodiscard]] virtual std::unique_ptr<Feature> Clone() const = 0;

  // Applies this feature to the model. Returns false on failure, which
  // aborts regeneration of subsequent features.
  virtual bool Execute(RegenContext& ctx) = 0;

  // Expression-driven numeric fields follow the pattern of a plain double
  // (what geometry code reads) paired with an expression-text string (empty
  // = plain literal; see ExtrudeFeature::distanceExpr). Features expose each
  // such pair here so the Document can evaluate them during regeneration and
  // parameter rename/delete can rewrite/inspect the texts — one hook instead
  // of three per-feature code paths.
  using ExprVisitor = std::function<void(std::string& text, double& value)>;
  virtual void VisitExprs(const ExprVisitor& visit) { (void)visit; }

  // Why this feature cannot be COMMITTED as it stands (empty = fine).
  // Distinct from GetError(), which reports a regeneration failure: a fillet
  // with no edges regenerates perfectly happily and does nothing, so only the
  // feature itself can say that its inputs are not there yet. Checked when a
  // preview is accepted and when `add_feature` arrives over the RPC — NOT
  // during regeneration, because an empty tool must still be able to open.
  [[nodiscard]] virtual std::string WhyIncomplete() const { return {}; }

  // The tool solid a CUT feature swept on its last Execute, for a Pattern
  // referencing it: a cut leaves no body behind to replicate, so the pattern
  // replicates the CUTTER instead (R14). Null for everything else. Valid
  // only within the same regeneration pass — features regen in order and a
  // pattern may only reference an earlier feature, so it always reads the
  // tool its source just built.
  virtual const CADShape* CutToolShape() const { return nullptr; }
  // The names of that tool's faces/edges, so each patterned instance can carry
  // suffixed copies of them (…#2) instead of arriving nameless.
  virtual const ElementMap* CutToolMap() const { return nullptr; }

  // Why this feature's last regeneration failed (empty = healthy). Transient
  // (never serialized), like Parameter::error: Document::Regenerate clears it
  // before each Execute and sets a generic message if Execute fails silently;
  // a feature can SetError a specific message (e.g. a dangling element ref) for
  // the UI to surface. Set on the object even though regeneration aborts, so
  // the tree can show which feature broke and why.
  [[nodiscard]] const std::string& GetError() const { return m_error; }
  void SetError(const std::string& error) { m_error = error; }
  void ClearError() {
    m_error.clear();
    m_errorAfterEarlierFailure = false;
  }

  // Whether this feature's failure happened while an EARLIER feature in the
  // same regeneration had already failed. Deliberately non-causal ("after",
  // not "because of"): with continue-past-failure regen semantics true
  // causality is a heuristic, but downstream failures are usually collateral,
  // and clients want to fix the first red feature first. Transient, stamped
  // at execution time (cached-prefix errors keep the flag from their last
  // execution, exactly like m_error).
  [[nodiscard]] bool ErrorAfterEarlierFailure() const {
    return m_errorAfterEarlierFailure;
  }
  void SetErrorAfterEarlierFailure(bool v) { m_errorAfterEarlierFailure = v; }

  // A non-fatal advisory from the last regeneration (empty = none). Unlike
  // m_error, a warning does NOT mark the feature broken or count against
  // regeneration success: the feature built a valid result, but it is likely
  // not what the user intended — e.g. a cut whose tool profile missed every
  // body, so it removed nothing (the exact "silent no-op cut" trap). Transient
  // (never serialized), cleared each regen exactly like m_error.
  [[nodiscard]] const std::string& GetWarning() const { return m_warning; }
  void SetWarning(const std::string& warning) { m_warning = warning; }
  void ClearWarning() { m_warning.clear(); }

  // Suppressed features are skipped by regeneration (contributing nothing)
  // but keep their parameters; anything referencing their outputs fails with
  // its normal missing-reference error — same rule as deletion. Serialized
  // (v19), unlike the transient m_error.
  [[nodiscard]] bool IsSuppressed() const { return m_suppressed; }
  void SetSuppressed(bool suppressed) { m_suppressed = suppressed; }

protected:
  std::string m_name;
  std::string m_material;
  Appearance m_appearance;
  uint64_t m_id = 0;
  std::string m_error;
  std::string m_warning;
  bool m_errorAfterEarlierFailure = false;
  bool m_suppressed = false;
};

class BoxFeature : public Feature {
public:
  BoxFeature(const std::string& name, const glm::vec3& position, double width,
             double height, double depth);

  std::string TypeName() const override { return "Box"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BoxFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // Expression-driven dimensions, like every other feature that carries a
  // length. Without these a primitive could not be driven by a variable —
  // and since a configuration works by overriding variables, a part built
  // from primitives could not be configured at all. That was true until
  // v61, which is a strange thing for a parametric modeller to have shipped.
  void VisitExprs(const ExprVisitor& visit) override {
    visit(widthExpr, width);
    visit(heightExpr, height);
    visit(depthExpr, depth);
  }

  glm::vec3 position;
  double width, height, depth;
  std::string widthExpr, heightExpr, depthExpr;
};

class SphereFeature : public Feature {
public:
  SphereFeature(const std::string& name, const glm::vec3& center,
                double radius);

  std::string TypeName() const override { return "Sphere"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<SphereFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
  }

  glm::vec3 center;
  double radius;
  std::string radiusExpr;
};

class CylinderFeature : public Feature {
public:
  CylinderFeature(const std::string& name, const glm::vec3& center,
                  const glm::vec3& direction, double radius, double height);

  std::string TypeName() const override { return "Cylinder"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CylinderFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
    visit(heightExpr, height);
  }

  glm::vec3 center;
  glm::vec3 direction;
  double radius, height;
  std::string radiusExpr, heightExpr;
};

// References an external STEP file. Shapes are translated so their combined
// bounding-box center sits at the origin, keeping regeneration deterministic
// for a given file.
class ImportFeature : public Feature {
public:
  ImportFeature(const std::string& name, const std::string& path);
  // Everything but the parse seed below: that is a one-shot hand-off the next
  // Execute drops, so a copy re-reads `path` exactly as a reloaded document
  // does. The compiler cannot write this one — m_seed is an owning pointer.
  ImportFeature(const ImportFeature& other)
      : Feature(other), path(other.path), hasColor(other.hasColor),
        color(other.color) {}

  std::string TypeName() const override { return "Import"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ImportFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // .step/.stp (structured), .brep (exact sidecar), or .stl (triangle mesh,
  // sewn to a shell/solid).
  std::string path;
  // Display color for the imported bodies. Assembly import (I2) extracts
  // per-part .brep sidecars, which carry no color — the import plan passes
  // the product's STEP color through here instead. Per-product colors in a
  // directly-imported STEP don't need this (they ride the XCAF read).
  bool hasColor = false;
  glm::vec3 color{0.0f};

  // Geometry the CALLER already parsed, used by the next Execute instead of
  // re-reading `path`, then dropped.
  //
  // The GUI import worker parses the file to decide part-vs-assembly
  // (ui/ImportJob.cpp) — on a single-product STEP the answer is "feature", and
  // the feature's first Execute would then parse the very same file again.
  // That is the whole read twice: ~12 s each on a 48 MB file.
  //
  // Deliberately NOT serialized and NOT sticky. A later regen re-reads the
  // path exactly as before, so editing the source file still updates the part
  // — the seed removes a duplicate parse, it does not change what an import
  // means.
  void SeedGeometry(CADKernel::ImportedModel model) {
    m_seed = std::make_unique<CADKernel::ImportedModel>(std::move(model));
  }

private:
  std::unique_ptr<CADKernel::ImportedModel> m_seed;
};
