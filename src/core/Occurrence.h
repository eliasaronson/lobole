#pragma once

#include "core/Configuration.h"
#include "core/ElementMap.h"

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
struct Occurrence {
  uint64_t id = 0;         // stable, unique within the assembly, never reused
  std::string name;        // display label (cosmetic)
  uint64_t partId = 0;     // stable id of the referenced part document
  std::optional<std::string> partVersion; // reserved: git-commit pin (F5)

  // Placement: translation + orientation (quaternion). Diffable, shear-free,
  // and how mate frames will think — not a raw mat4.
  glm::vec3 position{0.0f};
  glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f}; // (w,x,y,z) identity

  bool visible = true;
  bool fixed = false; // Onshape-style grounded flag; anchor for the A3 solve
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
};

// One side of a mate: a frame on some occurrence of this assembly.
struct Connector {
  uint64_t occurrenceId = 0;
  ConnectorAnchor anchor;
};

// The DOF a mate leaves open between its two connector frames. The classical
// lower kinematic pairs, shipped incrementally — the type is the *normal
// form* a constraint stack would converge to.
enum class MateType {
  Fastened,    // nothing free
  Revolute,    // rotation about Z free (`rotation` is the DOF value)
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
  float offset = 0.0f;   // translation along Z (slider/cylindrical DOF; planar
                         // plane gap)
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
  uint64_t id = 0;  // stable, shares the assembly's id counter
  std::string name; // display label (cosmetic)
  std::vector<ExplodeStep> steps; // at most one per occurrence
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

  // Directly set the next-id counter (used by the loader so restored ids are
  // never reissued). No-op if `next` is not greater than the current counter.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId) m_nextId = next;
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
  void RestoreConfiguration(Configuration cfg);  // load/undo, keeps id

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

  // Monotonic change counter — bumped on every add/remove/mutate — so the
  // renderer can skip resyncing an unchanged assembly (mirrors Document).
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

private:
  std::vector<Occurrence> m_occurrences;
  std::vector<Mate> m_mates;
  std::vector<ExplodedView> m_explodedViews;
  std::vector<Configuration> m_configurations;
  uint64_t m_activeConfiguration = 0;
  uint64_t m_nextId = 1; // stable ids grow from 1, never reused; one id
                         // space shared by occurrences, mates, and views
  uint64_t m_version = 1;
};
