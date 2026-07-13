#pragma once

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

// An assembly document: an ordered list of occurrences that *place* the bodies
// of other parts by explicit transform. A distinct document kind from a part's
// feature-list Document — an occurrence references another part, which
// Feature/RegenContext (same-document only) cannot reach, so an assembly
// regenerates by a placement pass, not by executing features.
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

  // Directly set the next-id counter (used by the loader so restored ids are
  // never reissued). No-op if `next` is not greater than the current counter.
  void EnsureNextIdAtLeast(uint64_t next) {
    if (next > m_nextId) m_nextId = next;
  }

  // Monotonic change counter — bumped on every add/remove/mutate — so the
  // renderer can skip resyncing an unchanged assembly (mirrors Document).
  uint64_t Version() const { return m_version; }
  void BumpVersion() { m_version++; }

private:
  std::vector<Occurrence> m_occurrences;
  uint64_t m_nextId = 1; // stable ids grow from 1, never reused
  uint64_t m_version = 1;
};
