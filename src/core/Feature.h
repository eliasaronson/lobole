#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Expression.h"
#include "core/SheetMetal.h"
#include "core/Units.h"

#include <functional>
#include <glm/glm.hpp>
#include <map>
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
struct ThreadRegion {
  ElementRef face;
  std::string designation;
  double length = 0.0, minimumEngagement = 0.0;
  bool external = false;
};

// A bend a sheet feature MADE, written down rather than measured back off the
// cylindrical faces afterwards (docs/sheetmetal/plan.md). Unfolding is then
// arithmetic over a graph of these — BA = angle * (R + K*t) — instead of
// surface development, which is what makes refold(unfold(x)) == x a property
// a test can assert rather than a tolerance to hope about.
//
// Transient like the shape and the element map: the producing feature stamps
// it on every regeneration, and nothing about it is serialized.
struct BendRegion {
  uint64_t featureId = 0; // the feature that made this bend
  glm::dvec3 axisOrigin{0.0};
  glm::dvec3 axisDir{0.0, 0.0, 1.0};
  double radius = 0.0; // INSIDE radius, mm
  double width = 0.0;  // how far the bend runs along its axis, mm
  double angle = 0.0;  // radians, always > 0; `up` carries the sense
  bool up = false; // sense about axisDir; the DXF layer a bend line lands on
  double kFactor = 0.0;
  // Where that K came from ("table:steel R/t<2", "rules", "override"). A bend
  // whose K nobody can trace is a bend nobody can argue with when the part
  // comes back the wrong length.
  std::string kSource;
  ElementRef parentFace, childFace; // the flat panels it joins
  ElementRef innerFace, outerFace;  // the cylindrical faces themselves

  // The frame the bend was built in, which is what makes this record
  // self-contained: `parentDir` is the in-plane direction the PARENT panel
  // runs toward the bend, `upDir` the parent face's outward normal. The
  // child's direction is cos(angle)*parentDir + sin(angle)*upDir, and
  // unfolding is a rotation about parentDir x upDir. Without them the
  // unfolder would have to re-derive a frame from the cylinders, which is
  // exactly the measuring-it-back-afterwards this design avoids.
  glm::dvec3 parentDir{1.0, 0.0, 0.0};
  glm::dvec3 upDir{0.0, 0.0, 1.0};

  // Filled by the unfolder, zero on a folded body: how much flat stock the
  // bend eats (the neutral arc), and the crease centreline in the FLAT
  // pattern — what a bend line is drawn from, and what the DXF puts on its
  // BEND_UP / BEND_DOWN layer.
  double allowance = 0.0;
  glm::dvec3 flatA{0.0}, flatB{0.0};
  // Set on a body that has ALREADY been flattened. The angle and radius stay
  // (a bend note says "UP 90 R2.25"), so without this a second flatten would
  // cheerfully unfold a blank a second time and hand back a longer one.
  bool flat = false;
  // On a flat crease: the Unfold (or FlatPattern) feature that flattened it,
  // and where its PARENT panel went — the transform from the coordinates the
  // bend record above is written in to where that panel sits now. Together
  // with the record, that is everything Refold needs to put the bend back, so
  // nothing about the fold has to be measured off the flat geometry. Kept
  // current: a later Unfold that moves this crease composes onto it.
  uint64_t unfoldedBy = 0;
  glm::dmat4 parentPlace{1.0};
};

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

  // Which part of a CAM JOB this body came from (v73), stamped when the job
  // assembles its body list (ui::JobBodies). Transient like the shape, and 0
  // everywhere else -- a job is the only thing that puts several parts'
  // bodies in one vector, which is exactly when an ElementRef alone stops
  // naming one face (docs/cam/job-document.md).
  uint64_t jobPartId = 0;

  // User appearance override, stamped from the creating feature after each
  // regeneration (Document::Regenerate post-pass keyed on createdBy).
  // Renderers prefer: appearance > material > displayColor > palette.
  Appearance appearance;

  // A ghost, not this part (a Derived feature with `reference: true`,
  // docs/modeling/derived-plan.md): here to be drawn on — sketch-on-face,
  // external geometry, hole centres, a mate connector — and absent from
  // everything that treats a body as matter: mass and BOM, export, CAM,
  // the FEM mesh, Add/Cut and Boolean operands, the assembly's placed
  // bodies. Transient like the shape, stamped by the creating feature.
  bool reference = false;
  std::vector<ThreadRegion> threadRegions; // derived opt-in contact semantics
  // The bends this body carries, in the order the features made them.
  std::vector<BendRegion> bends;
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
  // The document's asset store (v49 image pixels, a Derived feature's
  // snapshot B-Reps), read-only: what a feature whose geometry comes from
  // bytes rather than parameters loads from. Null only in a hand-built
  // context.
  const std::map<std::string, std::vector<unsigned char>>* assets = nullptr;

  // The document's sheet-metal rules, or null when the part is not sheet
  // metal. A flange REFUSES rather than guessing a thickness, so this being
  // null is a reportable state and not a default to fall back on.
  const sheet::SheetMetalRules* sheetMetal = nullptr;

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
  // REFUSES an unrecognised slug, naming what is accepted (empty stays legal
  // and means default). This is the one funnel every ingress runs through —
  // the set_material RPC, and FeatureFromJson, which is what add_feature,
  // modify_feature and document load all use. Before it validated,
  // add_feature(feature={..., "material":"unobtanium"}) was accepted and the
  // bad string sat on the feature until a renderer or a mass query tripped
  // over it.
  void SetMaterial(const std::string& material);

  // Does this feature work out its body's material itself, rather than just
  // stamping m_material? Derived inherits the SOURCE body's when its own is
  // empty, and a Fastener defaults to steel — so neither can be re-stamped
  // from m_material after a cached regeneration. Everything else can, which
  // is what lets Document strip `material` from the chain hash and makes a
  // recolour a cache hit instead of a full rebuild of the part.
  virtual bool StampsOwnMaterial() const { return false; }

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

  // Every OTHER feature of THIS history that this one names by stable id —
  // the edges of the dependency graph, which is how "what depends on this
  // feature?" gets answered (sketch edit rolls the model back to the first
  // consumer of the sketch, so the solid built FROM the sketch is not sitting
  // in front of it while you edit).
  //
  // PURE virtual for the reason Clone() is. A reference a feature forgets to
  // report is an edge the graph does not have, and the failure is silent: the
  // rollback simply leaves the offending solid on screen, looking like the
  // feature never worked. A runtime table could be forgotten; this cannot.
  //
  // Report ids unconditionally — 0, dangling and duplicate ids are all fine,
  // callers filter, and over-reporting only moves a rollback earlier while
  // UNDER-reporting is the silent failure above. The two exceptions:
  //   - an id a mode switch left stale (an AxisRef moved back to X keeps its
  //     sketchId) is not a dependency; VisitRefs(AxisRef) switches on kind.
  //   - an id naming something in ANOTHER document (a Derived feature's
  //     source part and the bodies it picks there) is not an edge of this
  //     history at all.
  // A body id IS an edge: Body::createdBy is the id of the feature that made
  // it, so a Shell naming a body names the feature that produced it.
  using RefVisitor = std::function<void(uint64_t)>;
  virtual void VisitFeatureRefs(const RefVisitor& visit) const = 0;

  // Why this feature cannot be COMMITTED as it stands (empty = fine).
  // Distinct from GetError(), which reports a regeneration failure: a fillet
  // with no edges regenerates perfectly happily and does nothing, so only the
  // feature itself can say that its inputs are not there yet. Checked when a
  // preview is accepted and when `add_feature` arrives over the RPC — NOT
  // during regeneration, because an empty tool must still be able to open.
  [[nodiscard]] virtual std::string WhyIncomplete() const { return {}; }

  // A DATUM: a feature whose output is not geometry — a plane, an axis, a
  // frame, a curve — but state computed in Execute and read afterwards (by
  // the overlays, by a mate connector, by the `frame` query).
  //
  // Baked geometry (docs/versioning/baked-geometry.md) replaces the BODY
  // list and executes nothing, which leaves that state at its default: a
  // datum plane draws at the origin, and a mate anchored to a Frame silently
  // resolves to the identity — the model loads, looks plausible and is
  // wrong. So Regenerate executes these on a baked open too. They are pure
  // arithmetic plus at most a face lookup, which is why this costs nothing
  // against the rebuild the bake avoids. A feature that CONTRIBUTES A BODY
  // must never return true: its geometry is what the bake carries.
  [[nodiscard]] virtual bool IsDatum() const { return false; }

  // The tool solid a feature that CUTS or ADDS swept on its last Execute, for
  // a Pattern referencing it: neither leaves a body of its own behind to
  // replicate (a cut removes, an add fuses into a body another feature made),
  // so the pattern replicates the TOOL instead, in the source's own mode —
  // R14 for cuts, and for adds since the watch's contrate teeth needed one.
  // Null for everything else. Valid only within the same regeneration pass —
  // features regen in order and a pattern may only reference an earlier
  // feature, so it always reads the tool its source just built.
  virtual const CADShape* ToolShape() const { return nullptr; }
  // The names of that tool's faces/edges, so each patterned instance can carry
  // suffixed copies of them (…#2) instead of arriving nameless.
  virtual const ElementMap* ToolMap() const { return nullptr; }
  // Whether the tool is fused into the bodies (an add) rather than subtracted
  // (a cut). Meaningful only with a ToolShape.
  virtual bool ToolAdds() const { return false; }

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

struct AxisRef; // core/AxisRef.h

// The feature ids the shared reference structs name, in ONE place so that a
// new kind of reference cannot reach only two of an AxisRef's three holders.
void VisitRefs(const AxisRef& ref, const Feature::RefVisitor& visit);
void VisitRefs(const ElementRef& ref, const Feature::RefVisitor& visit);
void VisitRefs(const std::vector<ElementRef>& refs,
               const Feature::RefVisitor& visit);

class BoxFeature : public Feature {
public:
  BoxFeature(const std::string& name, const glm::vec3& position, double width,
             double height, double depth);

  std::string TypeName() const override { return "Box"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BoxFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: a primitive is built from numbers alone.
  void VisitFeatureRefs(const RefVisitor&) const override {}

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

  // Names no other feature: a primitive is built from numbers alone.
  void VisitFeatureRefs(const RefVisitor&) const override {}

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

  // Names no other feature: a primitive is built from numbers alone.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
    visit(heightExpr, height);
  }

  glm::vec3 center;
  glm::vec3 direction;
  double radius, height;
  std::string radiusExpr, heightExpr;
};

// References an external STEP/STL/BREP file. By DEFAULT the shapes are
// translated so their combined bounding-box centre sits at the origin, which
// keeps regeneration deterministic for a given file and puts an import where
// the camera is looking.
//
// That is the wrong answer whenever the file's own coordinates mean
// something — an assembly's neighbour part, a fixture baked into the same
// frame, a re-import of geometry this app exported. Nothing said it was
// happening, so a check that placed such an import by its file coordinates
// silently compared the wrong things (a 0.00 mm gap that read like an
// interference). `keepOrigin` turns it off, and `Shift()` reports what the
// last regeneration applied either way.
class ImportFeature : public Feature {
public:
  ImportFeature(const std::string& name, const std::string& path);
  // Everything but the parse seed below: that is a one-shot hand-off the next
  // Execute drops, so a copy re-reads `path` exactly as a reloaded document
  // does. The compiler cannot write this one — m_seed is an owning pointer.
  ImportFeature(const ImportFeature& other)
      : Feature(other), path(other.path), keepOrigin(other.keepOrigin),
        lengthUnit(other.lengthUnit), hasColor(other.hasColor),
        color(other.color) {}

  std::string TypeName() const override { return "Import"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ImportFeature>(*this);
  }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: an import's input is a file on disk.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  // .step/.stp (structured), .brep (exact sidecar), or .stl (triangle mesh,
  // sewn to a shell/solid).
  std::string path;
  // Keep the file's own coordinates instead of centring the import (v97).
  bool keepOrigin = false;
  // The length unit the FILE was drawn in (v103, serialized as `units`), for
  // STL and BREP only: an STL has no unit field and a BREP is OpenCASCADE's
  // unitless native form, so a mesh exported in metres arrived 1000x too
  // small. STEP declares its own unit and REFUSES this. Storage stays mm: the
  // shape is scaled once, as it is read.
  units::Length lengthUnit = units::Length::Millimetre;
  // Whether `lengthUnit` means anything for this path: .stl or .brep, any
  // case.
  static bool TakesUnits(const std::string& path);
  // What the last Execute translated the shapes by — zero when keepOrigin.
  // Reported by the RPC so a caller never has to discover the move by
  // measuring a bounding box.
  glm::vec3 Shift() const { return m_shift; }
  // A hash of the file's BYTES, cached by path + size + mtime, so the chain
  // hash can key an import on what it imports rather than where the file
  // sits: saving a project adopts staged imports into the project directory
  // and rewrites `path`, and with the path in the hash every Derived copy of
  // an imported part read stale after the first save (the SO-101 gripper
  // dogfood). 0 when the file cannot be read.
  size_t ContentHash() const;
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
  glm::vec3 m_shift{0.0f}; // what the last Execute centred by

  std::unique_ptr<CADKernel::ImportedModel> m_seed;

  mutable std::string m_hashPath;
  mutable uintmax_t m_hashSize = 0;
  mutable long long m_hashMtime = 0;
  mutable size_t m_hashValue = 0;
};
