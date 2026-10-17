// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "cad/CADKernel.h"
#include "core/ElementMap.h"
#include "core/Expression.h"
#include "core/SheetMetal.h"
#include "core/Units.h"

#include <cstdint>
#include <functional>
#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// A visual override SEPARATE from the physical material
// (docs/rendering/materials.md#appearance-override): every unset field inherits
// from the material, so physics (density, FEM, BOM) never sees this. Persisted
// on the creating feature (v54), stamped onto its bodies each regeneration, and
// STRIPPED from the regen chain hash — recoloring never rebuilds B-Rep.
struct Appearance {
  // LINEAR base colour: the shaders light it as linear and the STEP/GLB
  // writers are handed it as linear. A mid-grey written as sRGB 0.5 renders as
  // about 0.73 — so a casting that should read grey wants ~0.2. The tone
  // curve (render/ToneCurve.h) displays a lit linear colour about as its sRGB
  // encode, so a photograph's colour converted with c^2.2 renders as picked.
  std::optional<glm::vec3> color;
  std::optional<float> roughness; // 0..1
  std::optional<float> metallic;  // 0..1
  bool Any() const {
    return color.has_value() || roughness.has_value() || metallic.has_value();
  }
};

// An appearance override on ONE face of a body (v164,
// docs/rendering/materials.md#a-face-of-its-own): a wall brick outside and
// plaster inside. Held on the body's CREATING feature beside the body-level
// override and keyed by the face's element name (topological naming), so it
// survives upstream edits that renumber faces — and a face a later cut made
// is still a face of the body, named in its ElementMap. Resolved to a face
// index at every regeneration (Document::Regenerate's appearance stamp); a
// name that no longer resolves is REPORTED (Feature::UnresolvedFaceLooks),
// never dropped. Unset fields inherit from the body's own look. Like the
// body override it is stripped from the regen chain hash.
struct FaceAppearance {
  std::string element; // a MappedName in the body's ElementMap
  Appearance look;
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

// A retaining ring's GRIP, stamped on both sides of it by the feature that
// knows it (docs/parts/README.md#a-ring-in-its-groove): the groove a
// RetainingRingGroove cut (`face` its floor) and the ring a RetainingRing is
// (`face` the face that grips: a DIN 471 ring's bore, a 472 ring's outside).
// A ring is drawn at its FREE diameter, so seated in its groove it overlaps
// the floor by (d2 - d3)/2 — the grip that holds it, not a clash. A contact
// check relieves exactly that annulus, and only where a ring and a groove of
// the same standard and size are coaxial and the ring lies within the
// groove's width; anything else either overlaps is still a clash. The
// ThreadRegion idea, for a ring. Transient: stamped every regeneration, and
// carried by the geometry cache.
struct RingGrip {
  ElementRef face;
  uint64_t featureId = 0; // the RetainingRing / RetainingRingGroove
  std::string standard;   // "din471" | "din472"
  double size = 0.0;      // the nominal shaft or bore diameter, mm
  bool groove = false;    // the groove's floor (else the ring's grip face)
};

// A thread a feature put on a face of this body, whatever it is for: a
// `Thread` feature, or a Hole with a tapped fit, on each bore it drilled.
// Written down by the feature that knows it, as a bend is, so a consumer
// that has only the bodies -- the CAM thread mill
// (docs/cam/operations.md#thread-mill) -- reads the thread rather than
// guessing it back from a diameter. Unlike `ThreadRegion` it claims nothing
// about engagement. Transient: stamped every regeneration, never saved.
struct ThreadMark {
  ElementRef face;         // the cylindrical face
  uint64_t featureId = 0;  // the Thread (or Hole) that says so
  std::string designation; // canonical when the tables know it
  double majorDia = 0.0;   // D, mm (0 = custom: from the face)
  double pitch = 0.0;      // P, mm
  std::string threadClass; // "2B", "6H"; empty = not stated
  bool rightHand = true;
  bool external = false;
  double length = 0.0; // threaded length from the face's start; 0 = all
  bool modeled = false;
  double flankDeg = 30.0; // half the thread angle; 30 = the ISO/UTS profile
};

// A hole a Hole feature drilled into this body, written by the feature that
// knows it (docs/modeling/holes-and-threads.md#hole), as a thread is: what a
// consumer that has only the bodies -- the CAM drill derivation, the job's
// list of what it does not cut
// (docs/cam/simulation.md#what-the-job-does-not-cut)
// -- reads rather than guessing a hole back from a cylinder. Transient.
struct HoleMark {
  ElementRef bore;        // the cylindrical bore face
  uint64_t featureId = 0; // the Hole
  std::string feature;    // its name, for a finding to say
  double diameter = 0.0;  // mm
  double depth = 0.0;     // mm, the full-diameter depth; 0 when through
  bool through = false;
  // The included angle of the drill point at a blind floor, degrees; 180 is
  // a flat floor. Read as modelled: the cone below the bore is the part's.
  double pointAngleDeg = 180.0;
  std::string thread; // a tapped fit's thread ("M4"); empty otherwise
};

// A tolerance on this body's faces, as a CAM job reads it
// (docs/cam/operations.md#tolerance-bands): a SIZE band on a round face --
// its deviations on the DIAMETER, mm, at the face's own size -- or, with
// `reference` set, a DISTANCE band between two level faces, its deviations
// on that distance; and how a person names it ("K7", "+0.05/-0.02").
// Stamped by the job when it assembles its bodies (ui::JobBodies) from the
// part's tolerances, which live outside the regeneration chain; empty
// everywhere else. Transient.
struct FaceBand {
  ElementRef face;
  ElementRef reference; // set: a distance from `face` to it
  double lower = 0.0, upper = 0.0;
  std::string label;
};

// A SIZE BAND on a round face, as the contact checks read it
// (docs/tolerances/README.md#press-fits): how far the face's diameter may come
// out from the size it is made at, from a size tolerance (an ISO 286 class or
// explicit deviations) or a generated bearing ring's ISO 492 band. Two
// coaxial faces of placed parts, one inside the other, both banded, whose fit
// allows interference, are a PRESS FIT: Measure::ContactBetween relieves
// their overlap up to the fit's maximum interference, and anything past it is
// still a clash. Stamped on a part's bodies by the Document from what it
// already stores (core/FitBands.h): tolerances live outside the regeneration
// chain, so no feature can stamp them. Transient, never saved.
struct FitBand {
  ElementRef face;
  // Signed deviations on the DIAMETER, mm, from the size the face is MADE at
  // (the modelled diameter, less `allowance` on a printed part).
  double lower = 0.0, upper = 0.0;
  // A printed part's process allowance on the diameter
  // (docs/tolerances/README.md#printed-parts): the model carries it, so the
  // print comes out at the modelled diameter less this. 0 otherwise.
  double allowance = 0.0;
  // Which side the band itself says it is: an uppercase class or a bearing's
  // bore is a hole, a lowercase class or a bearing's outside a shaft.
  // Explicit deviations say neither, and the geometry decides.
  enum class Side : uint8_t { Either, Hole, Shaft };
  Side side = Side::Either;
  // v179: a printed press's band (Tolerance::printedPress), read from the
  // part's print profile. It pairs only with another, and `label` is then
  // the profile it came from ("'K5 PLA on P1S' (fdm pla)").
  bool printedPress = false;
  std::string label; // "K7", "+0.010/-0.020", "6005 outside (ISO 492 normal)"
};

// A bend a sheet feature MADE, written down rather than measured back off the
// cylindrical faces afterwards
// (docs/sheetmetal/README.md#bends-are-recorded-never-recognised). Unfolding is
// then arithmetic over a graph of these — BA = angle * (R + K*t) — instead of
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
  // Radians, always > 0. Whether a bend is UP or DOWN is not stored: it is
  // only meaningful from one side of the blank, and sheet::FoldsToward
  // answers it for the side asked about. A sense stored against axisDir
  // flipped with whichever way the picked edge happened to run.
  double angle = 0.0;
  // A hem: folded flat on itself rather than bent round a die, so its radius
  // is the gap the return leaves and is deliberately far under what the
  // material takes. Nothing downstream can tell that from the radius alone,
  // and check_sheet_metal fired the minimum-radius violation on every hem
  // until this said so.
  bool crushed = false;
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

  // Where a mitred corner cut this bend short: planes (a point, and the
  // normal of the side the bend KEEPS), in the same coordinates as the record
  // above. Two flanges meeting at a corner occupy overlapping space, so
  // without these the unfolder would sweep the neighbour's bend into this
  // one's and call it material added.
  struct Trim {
    glm::dvec3 point{0.0};
    glm::dvec3 normal{0.0, 0.0, 1.0};
  };
  std::vector<Trim> trims;
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

  // Stable names for this body's faces (topological naming). Transient,
  // rebuilt each regeneration by the feature that produced or last modified
  // the body — every solid feature names its output.
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
  // naming one face (docs/cam/README.md#the-model).
  uint64_t jobPartId = 0;

  // User appearance override, stamped from the creating feature after each
  // regeneration (Document::Regenerate post-pass keyed on createdBy).
  // Renderers prefer: appearance > material > displayColor > palette.
  Appearance appearance;
  // Per-face overrides (FaceAppearance, above), stamped beside `appearance`:
  // face index (TopExp_Explorer order, as triangleFaces and picking count
  // them) -> the override that resolved to it. Transient.
  std::map<int, FaceAppearance> faceAppearances;

  // A ghost, not this part (a Derived feature with `reference: true`,
  // docs/modeling/derived.md#reference-bodies-ghosts): here to be drawn on —
  // sketch-on-face, external geometry, hole centres, a mate connector — and
  // absent from everything that treats a body as matter: mass and BOM, export,
  // CAM, the FEM mesh, Add/Cut and Boolean operands, the assembly's placed
  // bodies. Transient like the shape, stamped by the creating feature.
  bool reference = false;
  std::vector<ThreadRegion> threadRegions; // derived opt-in contact semantics
  // A retaining ring or its groove (RingGrip, above): the same opt-in.
  std::vector<RingGrip> ringGrips;
  // The bends this body carries, in the order the features made them.
  std::vector<BendRegion> bends;
  // Every thread on this body's faces (ThreadMark, above).
  std::vector<ThreadMark> threads;
  // Every hole a Hole feature drilled into it (HoleMark, above).
  std::vector<HoleMark> holes;
  // The size tolerances a CAM job read off its part (FaceBand, above).
  std::vector<FaceBand> bands;
  // Every size band its round faces carry, for the contact checks' press
  // fits (FitBand, above).
  std::vector<FitBand> fitBands;
};

class Feature;

// What a feature sees while executing during regeneration: the bodies built
// so far, and read access to the features strictly BEFORE it in the history
// (a feature can only reference what already exists — e.g. an extrude can
// only consume an earlier sketch).
struct Configuration;
namespace printfit {
struct Process;
}

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

  // The configuration the document is regenerating under, or null for the
  // authored model. A reference resolves through the SAME suppression the
  // regeneration loop uses (Document::EffectiveSuppressed): reading only a
  // feature's authored flag made a sketch a configuration turns ON invisible
  // to the revolve built on it, and one it turns OFF still referable.
  const Configuration* configuration = nullptr;

  // How the part is made, when it is printed (Document::PrintProcess), else
  // null. A Hole's clearance fit is modelled at its size plus the process's
  // hole allowance (docs/tolerances/README.md#printed-parts); the regen hash
  // folds the process into exactly those holes, so nothing else rebuilds.
  const printfit::Process* print = nullptr;

  // A Template instance executes its expanded features against this same
  // context (docs/modeling/templates.md#expansion): `inner` lists them and
  // `innerIndex` is the one executing, so a reference inside the template
  // finds the template's own earlier features first, then the document's.
  // Null everywhere else.
  const std::vector<std::unique_ptr<Feature>>* inner = nullptr;
  size_t innerIndex = 0;

  // Searches inner[0, innerIndex), then features[0, currentIndex), by
  // stable id; nullptr if absent or suppressed.
  const Feature* FindById(uint64_t id) const;
};

// Moves the marks a face carries (a hole's, a thread's) onto its renamed
// self after a feature modified it: what the regeneration loop runs after
// every feature, and a Template instance after each feature it expands.
void FollowModifiedFaces(std::vector<Body>& bodies);

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

  // The feature whose body this one's body CONTINUES, when it re-owns one: a
  // Boolean's target, a body pattern's or mirror's seed, the body a Split
  // cut. Such a feature erases the body it was given and makes a new one
  // owned by itself, so the material stamp (keyed on the creating feature)
  // would otherwise hand the result this feature's own material, empty unless
  // someone set one. With one, an empty material here means the source's,
  // followed back through a chain of them
  // (docs/rendering/materials.md#the-feature-that-holds-a-material). 0 = none.
  virtual uint64_t MaterialSource() const { return 0; }

  // Visual override applied to this feature's bodies on top of the material
  // (persistent + serialized, v54). Empty (no field set) = pure material.
  const Appearance& GetAppearance() const { return m_appearance; }
  void SetAppearance(const Appearance& appearance) {
    m_appearance = appearance;
  }
  // Per-face overrides on this feature's bodies (FaceAppearance; persistent,
  // v164), in the order they were set.
  const std::vector<FaceAppearance>& GetFaceAppearances() const {
    return m_faceAppearances;
  }
  void SetFaceAppearances(std::vector<FaceAppearance> faces) {
    m_faceAppearances = std::move(faces);
  }
  // Sets the override on `element`, replacing one already there; an EMPTY
  // look removes it. Returns whether `element` carried one before.
  bool SetFaceAppearance(const std::string& element, const Appearance& look);
  // The face overrides whose element name resolved to no face of any body
  // this feature created, at the last regeneration. Transient: stamped by
  // Document::Regenerate with the appearance, and reported in every
  // regeneration summary (remote::RegenSummaryJson) and in get_state.
  const std::vector<std::string>& UnresolvedFaceLooks() const {
    return m_unresolvedFaceLooks;
  }
  void SetUnresolvedFaceLooks(std::vector<std::string> names) {
    m_unresolvedFaceLooks = std::move(names);
  }

  // Stable identity for feature->feature references (an extrude names its
  // sketch by id, which survives reordering and undo). 0 = unassigned; the
  // Document assigns ids on insert and serialization round-trips them.
  uint64_t GetId() const { return m_id; }
  void SetId(uint64_t id) { m_id = id; }

  virtual std::string TypeName() const = 0;

  // A deep copy of this feature — id, name, material and all, and the state
  // its last Execute left (a cut tool's shape and element map). Duplicating a
  // tab clones the document through here, and the regeneration cache keeps a
  // clone of every evaluation, which CopyDerivedState restores from. PURE
  // virtual on purpose, so a new feature type cannot arrive without one; a
  // runtime table could be forgotten, and the copy would then silently lose
  // that feature. Each override is `make_unique<T>(*this)`; Import and
  // Template write their copy constructors, since they hold owning pointers.
  [[nodiscard]] virtual std::unique_ptr<Feature> Clone() const = 0;

  // Makes this feature what its Execute left it as, from `executed`: a
  // Clone() of this same feature (same type, id and chain hash) taken right
  // after it executed. The regeneration cache calls it on a hit, because the
  // hit may come from ANOTHER history than the one this object last executed
  // in — undo, a value set back — and everything Execute leaves on the
  // object (a cut's tool, a frame's placement, a sketch's resolved plane, a
  // hole's thread-fit diameter, the error and warning it raised) is then
  // that other history's
  // (docs/architecture.md#the-document-model-and-regeneration).
  //
  // PURE virtual for the reason Clone() is: a new feature type has to say
  // what its Execute retains. Nearly every one says RestoreExecuted<Self>,
  // which copies the whole object except what the chain hash leaves out
  // (appearance, material), so a member added later cannot be forgotten;
  // test_conventions.cpp holds the overrides to that.
  virtual void CopyDerivedState(const Feature& executed) = 0;

  // The regeneration status `executed` was left with: error, warning and
  // the after-an-earlier-failure flag. Part of every CopyDerivedState, and
  // applied on its own by a hit whose object already holds that evaluation
  // (an expression error stamped after the last one is not its).
  void CopyRegenStatus(const Feature& executed) {
    m_error = executed.m_error;
    m_warning = executed.m_warning;
    m_errorAfterEarlierFailure = executed.m_errorAfterEarlierFailure;
  }

  // The chain hash of the evaluation this object's derived state is from —
  // the last Execute, or the last CopyDerivedState — stamped by
  // Document::Regenerate; 0 = none (a feature fresh from JSON, or one an
  // undo put back). Lets a cache hit skip the restore when the object is
  // already that evaluation. Transient, never serialized.
  [[nodiscard]] size_t DerivedFor() const { return m_derivedFor; }
  void SetDerivedFor(size_t chainHash) { m_derivedFor = chainHash; }

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

  // Ids this feature OWNS besides its own: a Template instance's expanded
  // features carry document ids of their own, minted once and stored on the
  // instance, so the names they give faces are unique to it
  // (docs/modeling/templates.md#names). The document keeps its id counter
  // ahead of them, and a feature naming one of them depends on this one.
  virtual void VisitOwnedIds(const RefVisitor& visit) const { (void)visit; }

  // Why this feature cannot be COMMITTED as it stands (empty = fine): an
  // input not there yet, or (a Thread) one the tables refuse. Distinct from
  // GetError(), which reports a regeneration failure: a fillet with no edges
  // regenerates perfectly happily and does nothing, so only the feature
  // itself can say that its inputs are not there yet. Checked when a
  // preview is accepted and when `add_feature` arrives over the RPC — not
  // by Regenerate, because an empty tool must still be able to open.
  [[nodiscard]] virtual std::string WhyIncomplete() const { return {}; }

  // A DATUM: a feature whose output is not geometry — a plane, an axis, a
  // frame, a curve — but state computed in Execute and read afterwards (by
  // the overlays, by a mate connector, by the `frame` query).
  //
  // Baked geometry (docs/versioning/README.md#baked-geometry) replaces the BODY
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
  // Null for everything else. The tool of the source's CURRENT evaluation —
  // features regen in order and a pattern may only reference an earlier
  // feature, which either just executed or was restored from the cache.
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
  // The usual CopyDerivedState: becomes `executed` whole, keeping its own
  // appearance, material and the face looks Regenerate stamps — what the
  // chain hash leaves out, so they may differ between two objects with one
  // evaluation. Everything else is either hashed (and so equal) or derived.
  template <class T> void RestoreExecuted(const Feature& executed) {
    Appearance appearance = std::move(m_appearance);
    std::vector<FaceAppearance> faces = std::move(m_faceAppearances);
    std::vector<std::string> unresolved = std::move(m_unresolvedFaceLooks);
    std::string material = std::move(m_material);
    static_cast<T&>(*this) = static_cast<const T&>(executed);
    m_appearance = std::move(appearance);
    m_faceAppearances = std::move(faces);
    m_unresolvedFaceLooks = std::move(unresolved);
    m_material = std::move(material);
  }

  std::string m_name;
  std::string m_material;
  Appearance m_appearance;
  std::vector<FaceAppearance> m_faceAppearances;
  std::vector<std::string> m_unresolvedFaceLooks;
  uint64_t m_id = 0;
  std::string m_error;
  std::string m_warning;
  bool m_errorAfterEarlierFailure = false;
  bool m_suppressed = false;
  size_t m_derivedFor = 0; // see DerivedFor()
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
  BoxFeature(const std::string& name, const glm::dvec3& position, double width,
             double height, double depth);

  std::string TypeName() const override { return "Box"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<BoxFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<BoxFeature>(executed);
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

  glm::dvec3 position;
  double width, height, depth;
  std::string widthExpr, heightExpr, depthExpr;
};

class SphereFeature : public Feature {
public:
  SphereFeature(const std::string& name, const glm::dvec3& center,
                double radius);

  std::string TypeName() const override { return "Sphere"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<SphereFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<SphereFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: a primitive is built from numbers alone.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
  }

  glm::dvec3 center;
  double radius;
  std::string radiusExpr;
};

class CylinderFeature : public Feature {
public:
  CylinderFeature(const std::string& name, const glm::dvec3& center,
                  const glm::dvec3& direction, double radius, double height);

  std::string TypeName() const override { return "Cylinder"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<CylinderFeature>(*this);
  }

  void CopyDerivedState(const Feature& executed) override {
    RestoreExecuted<CylinderFeature>(executed);
  }

  bool Execute(RegenContext& ctx) override;

  // Names no other feature: a primitive is built from numbers alone.
  void VisitFeatureRefs(const RefVisitor&) const override {}

  void VisitExprs(const ExprVisitor& visit) override {
    visit(radiusExpr, radius);
    visit(heightExpr, height);
  }

  glm::dvec3 center;
  glm::dvec3 direction;
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
        color(other.color), m_shift(other.m_shift) {}

  std::string TypeName() const override { return "Import"; }

  std::unique_ptr<Feature> Clone() const override {
    return std::make_unique<ImportFeature>(*this);
  }

  // Its one derived member is the shift; the parse seed is a hand-off.
  void CopyDerivedState(const Feature& executed) override {
    CopyRegenStatus(executed);
    m_shift = static_cast<const ImportFeature&>(executed).m_shift;
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
  glm::dvec3 Shift() const { return m_shift; }
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
  glm::dvec3 m_shift{0.0}; // what the last Execute centred by

  std::unique_ptr<CADKernel::ImportedModel> m_seed;

  mutable std::string m_hashPath;
  mutable uintmax_t m_hashSize = 0;
  mutable long long m_hashMtime = 0;
  mutable size_t m_hashValue = 0;
};
