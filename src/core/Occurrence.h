#pragma once

#include "core/Configuration.h"
#include "core/ElementMap.h"
#include "core/Units.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// One placed instance of a part within an assembly. Stores an *explicit*
// transform — its last solved placement — so an occurrence is always loadable
// on its own; A3 mates will re-solve over non-`fixed` occurrences on load
// (the literal-plus-`_expr` pattern applied to placement). See
// docs/assemblies/a2-design.md.
// What an occurrence places (A5). The field it qualifies is `partId`, whose
// SHAPE does not change — a slot id is a slot id — but whose resolution
// widens from "a part document" to "a part or an assembly document". Stored
// explicitly rather than inferred from whatever the id currently resolves to,
// so a DANGLING reference still knows what it was looking for and can say so.
enum class OccurrenceRef { Part, Assembly };

struct Occurrence {
  uint64_t id = 0;     // stable, unique within the assembly, never reused
  std::string name;    // display label (cosmetic)
  uint64_t partId = 0; // stable id of the referenced part or assembly document
  OccurrenceRef refKind = OccurrenceRef::Part;
  std::optional<std::string> partVersion; // reserved: git-commit pin (F5)

  // Placement: translation + orientation (quaternion). Diffable, shear-free,
  // and how mate frames will think — not a raw mat4.
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; // (w,x,y,z) identity

  bool visible = true;
  bool fixed = false; // grounded flag; the anchor for the A3 solve

  // What this occurrence RIDES ON (docs/parts/f1-loose-ends.md): the id of
  // another occurrence in the same assembly, with `attachOffset` /
  // `attachRotation` its placement in that occurrence's frame. 0 = free.
  //
  // Not a mate, deliberately. A mate is for kinematics and a fastener has
  // none — it is a zero-DOF leaf — so putting one in the mate graph costs up
  // to four rows per joint in the list you read the MECHANISM from, and mate
  // placement is frame alignment, which could relocate the fastener away from
  // where the fit computed it. An attachment is a composition, not a
  // constraint: no solver sees it.
  //
  // `position`/`orientation` keep their one meaning — the placement in the
  // ASSEMBLY — and are RECOMPUTED from the parent by ApplyAttachments. That
  // is the trade: one derived field pair instead of overloading what
  // `position` means for some occurrences, which is the kind of double duty
  // that has bitten this codebase before.
  uint64_t attachedTo = 0;
  glm::vec3 attachOffset{0.0f};
  glm::quat attachRotation{1.0f, 0.0f, 0.0f, 0.0f};

  // Which CONFIGURATION of the referenced document this placement uses
  // (A5, v56); 0 = whatever that document's active configuration is, which
  // is the behaviour every existing file means.
  //
  // This is what makes a sub-assembly reusable rather than merely tidy: one
  // Gangway placed "short" beside another placed "long". Scoped to ASSEMBLY
  // configurations, which are a suppression set the walk can simply consult
  // — a PART configuration changes geometry, so honouring it per occurrence
  // would mean regenerating that part once per configuration, and stays
  // deferred (it was already deferred in C0-C4).
  uint64_t configurationId = 0;
};

// Where on its part a connector frame is anchored (A3 mates,
// docs/assemblies/a3-design.md). Either the part's coordinate origin (an
// identity frame) or a face named by a toponaming ElementRef — planar (frame
// = centroid + normal, CADShape::FacePlane) or cylindrical (frame = axis,
// CADShape::FaceAxis). The frame itself is *derived* at solve time, never
// stored, so a part edit moves the connector with the geometry.
struct ConnectorAnchor {
  bool origin = false; // true → the part origin; `face` is ignored
  ElementRef face;     // valid when !origin
  // Inference snap (A2.5 phase 4): when set, the frame ORIGIN is this edge's
  // characteristic point (a circular edge's center, else its midpoint) instead
  // of the face centroid — e.g. a mate snapped to a hole center. The face still
  // gives the Z normal. Additive; face-centroid when unset.
  ElementRef snapEdge;
  // Corner snap (phase 4c, v65): with BOTH edges set the origin is the vertex
  // where they meet. A corner has no stable name of its own — the ElementMap
  // names faces and edges, not vertices — but two named edges do, and so
  // does the point they share. When they share no vertex or more than one,
  // the anchor falls back to `snapEdge`'s own point rather than guessing.
  ElementRef snapEdge2;
};

// One side of a mate: a frame on some occurrence of this assembly.
struct Connector {
  uint64_t occurrenceId = 0;
  ConnectorAnchor anchor;

  // A5/v57: when `occurrenceId` names a SUB-ASSEMBLY, the chain of
  // occurrence ids from inside it down to the part whose face the anchor
  // means. Empty for the ordinary case.
  //
  // A placed assembly has no faces of its own, so before this the only way
  // to mate onto one was to open that assembly — which cannot express "bolt
  // this bracket to the flange of the pump inside the pump unit". The mate
  // still positions the TOP-LEVEL occurrence: the anchor only reads
  // geometry, and the unit is rigid, so a face inside it is at a known place
  // in the unit's frame.
  std::vector<uint64_t> innerPath;
};

// The DOF a mate leaves open between its two connector frames. The classical
// lower kinematic pairs, shipped incrementally — the type is the *normal
// form* a constraint stack would converge to.
enum class MateType {
  Fastened,    // nothing free
  Revolute,    // rotation about Z free (`rotation` is the DOF value); `offset`
               //   is a fixed axial slide along Z (the pose parameter that
               //   places B up/down the shaft — a hinge pin's seat depth), not
               //   a free DOF
  Slider,      // translation along Z free (`offset` is the DOF value)
  Cylindrical, // rotation about Z AND translation along Z free (both DOF)
  Planar,      // faces coincident; free to slide in-plane (`dx`,`dy`) + spin
               //   about Z (`rotation`); `offset` = the fixed plane gap
  Ball,        // origins coincident; free 3-axis rotation (`ballRotation`)
};

// A mate: one connection between two connector frames, with one type. The
// solve places `b`'s occurrence so that b's world frame equals a's world
// frame composed with Flip?·Rz(rotation)·Tz(offset). Every mate carries the
// same three adjustment fields; the type decides which are fixed parameters
// and which is the live DOF — so a mate always stores its DOF pose and a
// file re-solves to identical placements (the A2 explicit-transform
// principle, one level up).
struct Mate {
  uint64_t id = 0;  // stable, shares the assembly's id counter with occurrences
  std::string name; // display label (cosmetic)
  MateType type = MateType::Fastened;
  Connector a;
  Connector b; // the side the solve places (relative to a)

  bool flipped = true;   // reverse b's Z against a's Z (two touching planar
                         // faces have opposing normals, hence default true)
  float rotation = 0.0f; // twist about Z, radians (revolute/cylindrical DOF)
  float offset = 0.0f;   // translation along +Z of a's connector frame (a's
                         // outward face normal): slider/cylindrical DOF, the
                         // revolute axial seat, the planar plane gap. A
                         // POSITIVE offset moves b along a's +Z — see the
                         // MateAdjust composition (T is applied in a's frame).
  float dx = 0.0f;       // in-plane X translation (planar DOF)
  float dy = 0.0f;       // in-plane Y translation (planar DOF)
  glm::quat ballRotation{1.0f, 0.0f, 0.0f, 0.0f}; // free rotation (ball DOF)

  // Transient solve status (mirrors Feature::m_error): stamped by the
  // placement pass, cleared on every solve, never serialized. Empty = ok.
  std::string error;
};

// One occurrence's displacement within an exploded view (A4): a world-space
// offset, scaled by the transient explode factor at render time.
struct ExplodeStep {
  uint64_t occurrenceId = 0;
  glm::vec3 offset{0.0f}; // world space, at factor 1.0
};

// A named, persistent exploded view (A4). PURE VIEW STATE: applying it never
// touches occurrence transforms, mates, or the solve — the offset composes
// into the rendered instance transform only, so a saved exploded view cannot
// corrupt placements by construction. The active view + factor are transient
// (AppState), not stored.
struct ExplodedView {
  uint64_t id = 0;                // stable, shares the assembly's id counter
  std::string name;               // display label (cosmetic)
  std::vector<ExplodeStep> steps; // at most one per occurrence
};

// A relation couples two mates' degrees of freedom
// (docs/assemblies/relations-plan.md): dofB = ratio * dofA + phase. The kind
// is a label and a unit convention on that one equation — a gear is
// rotation-to-rotation with ratio -zA/zB, a rack is the pinion's rotation to
// the rack's slide with the pitch radius as ratio (mm per radian), a screw
// is one cylindrical mate's own rotation to its own offset (pitch / 2pi),
// and linear is slide-to-slide. Enforced by propagation in a mate tree and
// as a residual row inside a loop.
enum class RelationKind { Gear, Rack, Screw, Linear };
enum class MateDof { Rotation, Offset };

struct Relation {
  uint64_t id = 0;  // stable, shares the assembly's id counter
  std::string name; // display label (cosmetic)
  RelationKind kind = RelationKind::Gear;
  uint64_t mateA = 0;
  uint64_t mateB = 0; // may equal mateA (a screw)
  MateDof dofA = MateDof::Rotation;
  MateDof dofB = MateDof::Rotation;
  double ratio = -1.0;
  double phase = 0.0; // in dofB's unit (radians or mm)
  std::string error;  // set by the solve: a mate gone, a DOF missing
};

// An assembly document: an ordered list of occurrences that *place* the bodies
// of other parts by explicit transform, plus the mates that constrain them
// (A3 — re-solved over non-`fixed` occurrences by a placement pass). A
// distinct document kind from a part's feature-list Document — an occurrence
// references another part, which Feature/RegenContext (same-document only)
// cannot reach, so an assembly regenerates by a placement pass, not by
// executing features.
class AssemblyDocument {
public:
  // The units this assembly is READ in — same rule and same storage
  // invariant as Document::Units (core/Units.h). The BOM's masses are the
  // reason an assembly needs its own: it is where they are shown.
  const units::System& Units() const { return m_units; }
  void SetUnits(const units::System& u) { m_units = u; }

  const std::vector<Occurrence>& GetOccurrences() const {
    return m_occurrences;
  }
  std::vector<Occurrence>& GetOccurrences() { return m_occurrences; }
  size_t OccurrenceCount() const { return m_occurrences.size(); }

  // Appends an occurrence, assigning a fresh stable id (the caller-supplied
  // `id` is ignored/overwritten). Returns the assigned id.
  uint64_t AddOccurrence(Occurrence occ);

  // Removes the occurrence with this id; returns false if absent.
  bool RemoveOccurrence(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  Occurrence* FindById(uint64_t id);
  const Occurrence* FindById(uint64_t id) const;

  const std::vector<Mate>& GetMates() const { return m_mates; }
  std::vector<Mate>& GetMates() { return m_mates; }
  size_t MateCount() const { return m_mates.size(); }

  // Appends a mate, assigning a fresh stable id from the SAME counter as
  // occurrences — ids are unique across the whole assembly, so an RPC/file/
  // error message naming "id 7" is unambiguous. Returns the assigned id.
  uint64_t AddMate(Mate mate);

  // Removes the mate with this id; returns false if absent.
  bool RemoveMate(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  Mate* FindMateById(uint64_t id);
  const Mate* FindMateById(uint64_t id) const;

  const std::vector<ExplodedView>& GetExplodedViews() const {
    return m_explodedViews;
  }
  std::vector<ExplodedView>& GetExplodedViews() { return m_explodedViews; }

  // Appends an exploded view, assigning a fresh stable id from the shared
  // counter. Returns the assigned id.
  uint64_t AddExplodedView(ExplodedView view);

  // Removes the exploded view with this id; returns false if absent.
  bool RemoveExplodedView(uint64_t id);

  // Lookup by stable id; nullptr if absent.
  ExplodedView* FindExplodedViewById(uint64_t id);
  const ExplodedView* FindExplodedViewById(uint64_t id) const;

  // Relations (v83, docs/assemblies/relations-plan.md): same id space and
  // the same add/remove/find discipline as mates and exploded views.
  const std::vector<Relation>& GetRelations() const { return m_relations; }
  std::vector<Relation>& GetRelations() { return m_relations; }
  size_t RelationCount() const { return m_relations.size(); }
  uint64_t AddRelation(Relation relation);
  bool RemoveRelation(uint64_t id);
  Relation* FindRelationById(uint64_t id);
  const Relation* FindRelationById(uint64_t id) const;

  // Directly set the next-id counter (used by the loader so restored ids are
  // never reissued). No-op if `next` is not greater than the current counter.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId)
      m_nextId = next;
  }

  // Configurations (workstream 13, C4): named assembly variants. Reuses the
  // Configuration type — for an assembly, `suppressedFeatures` holds suppressed
  // OCCURRENCE ids (a suppressed occurrence is hidden from render / solve /
  // BOM). Parameter overrides are unused (an assembly has no parameters).
  // Per-occurrence part-config selection is a planned follow-on. The active
  // configuration (0 = base) is persisted.
  const std::vector<Configuration>& GetConfigurations() const {
    return m_configurations;
  }
  std::vector<Configuration>& GetConfigurations() { return m_configurations; }
  const Configuration* FindConfiguration(uint64_t id) const;
  Configuration* FindConfiguration(uint64_t id);
  Configuration& AddConfiguration(const std::string& name = "");
  bool RemoveConfiguration(uint64_t id);
  void RestoreConfiguration(Configuration cfg); // load/undo, keeps id

  uint64_t ActiveConfiguration() const { return m_activeConfiguration; }
  // Sets the active id (0 = base) and bumps the version so renderers resync;
  // returns false for an unknown non-zero id.
  bool SetActiveConfiguration(uint64_t id);
  void SetActiveConfigurationRaw(uint64_t id) { m_activeConfiguration = id; }
  const Configuration* ActiveConfig() const;

  // Whether an occurrence is hidden by the active configuration (its base
  // `visible` flag is separate; this is the config layer). Render/solve/BOM
  // skip an occurrence that is suppressed OR not visible.
  bool OccurrenceSuppressed(uint64_t occurrenceId) const;

  // The same question against a NAMED configuration (A5): 0 means the active
  // one, and an id that no longer resolves also falls back to it — a
  // configuration deleted out from under a placement should not silently
  // un-suppress half of it, but it must not break the model either.
  bool OccurrenceSuppressedIn(uint64_t configurationId,
                              uint64_t occurrenceId) const;

  // Monotonic change counter — bumped on every add/remove/mutate — so the
  // renderer can skip resyncing an unchanged assembly (mirrors Document).
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

private:
  std::vector<Occurrence> m_occurrences;
  units::System m_units;
  std::vector<Mate> m_mates;
  std::vector<ExplodedView> m_explodedViews;
  std::vector<Relation> m_relations;
  std::vector<Configuration> m_configurations;
  uint64_t m_activeConfiguration = 0;
  uint64_t m_nextId = 1; // stable ids grow from 1, never reused; one id
                         // space shared by occurrences, mates, and views
  uint64_t m_version = 1;
};
