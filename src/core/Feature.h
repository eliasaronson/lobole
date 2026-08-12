#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Expression.h"

#include <functional>
#include <glm/glm.hpp>
#include <string>
#include <vector>

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

  // Stable identity for feature->feature references (an extrude names its
  // sketch by id, which survives reordering and undo). 0 = unassigned; the
  // Document assigns ids on insert and serialization round-trips them.
  uint64_t GetId() const { return m_id; }
  void SetId(uint64_t id) { m_id = id; }

  virtual std::string TypeName() const = 0;

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
  bool Execute(RegenContext& ctx) override;

  glm::vec3 position;
  double width, height, depth;
};

class SphereFeature : public Feature {
public:
  SphereFeature(const std::string& name, const glm::vec3& center,
                double radius);

  std::string TypeName() const override { return "Sphere"; }
  bool Execute(RegenContext& ctx) override;

  glm::vec3 center;
  double radius;
};

class CylinderFeature : public Feature {
public:
  CylinderFeature(const std::string& name, const glm::vec3& center,
                  const glm::vec3& direction, double radius, double height);

  std::string TypeName() const override { return "Cylinder"; }
  bool Execute(RegenContext& ctx) override;

  glm::vec3 center;
  glm::vec3 direction;
  double radius, height;
};

// References an external STEP file. Shapes are translated so their combined
// bounding-box center sits at the origin, keeping regeneration deterministic
// for a given file.
class ImportFeature : public Feature {
public:
  ImportFeature(const std::string& name, const std::string& path);

  std::string TypeName() const override { return "Import"; }
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
};
