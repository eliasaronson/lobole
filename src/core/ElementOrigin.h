#pragma once

// What a MappedName says about the element it names: which feature made it,
// in what role, and what happened to it since (docs/modeling/toponaming.md).
// The one reader of the grammar -- drawing provenance, tapping and the
// `list_elements` filters all go through it.

#include <cstdint>
#include <string>
#include <vector>

namespace topo {

enum class ElementRole {
  StartCap,         // an extrude's or revolve's first end (cap0)
  EndCap,           // its far end (cap1) -- a cut's floor
  Side,             // swept from a sketch entity (g<entity>)
  Generated,        // made from another element by a G step: a fillet's or
                    // chamfer's face, or one a boolean made along a seam
  Pole,             // a revolve's degenerate edge at the axis
  Seam,             // a boolean section edge history did not name
  Positional,       // named by index by a feature that rebuilds wholesale
  Boundary,         // the L fallback: named from its edges, role unknown
  Bore,             // a hole's
  Bottom,           // a hole's
  Counterbore,      // a hole's
  CounterboreFloor, // a hole's
  Countersink,      // a hole's
  DrillPoint,       // a hole's: the cone a drill leaves at a blind floor
  // A generated bearing's (docs/parts/README.md#bearings); its bore is Bore.
  Outside,   // the outer ring's outside diameter
  InnerSide, // an inner-ring side face (a shaft shoulder bears on it)
  OuterSide, // an outer-ring side face (a housing shoulder bears on it)
  Shield,    // a recessed shield face
  // A generated retaining ring's flat side (its bore and outside are Bore
  // and Outside), and the groove one sits in
  // (docs/parts/README.md#retaining-rings).
  RingSide,
  GrooveFloor,
  GrooveFlank,
  Unknown, // a seed this decoder does not know
};

// "start_cap", "end_cap", "side", ... -- the spelling the RPC uses.
const char* ElementRoleName(ElementRole role);
// Parses a spelling ElementRoleName produces. Unknown is not a spelling.
[[nodiscard]] bool ElementRoleFromString(const std::string& s,
                                         ElementRole& out);
// Every spelling, for a refusal to name.
std::string ElementRoleList();

struct DerivedStep {
  uint64_t derivedId = 0;       // the Derived feature in this document
  uint64_t sourceCreatedBy = 0; // the source body's created_by
};

struct ElementOrigin {
  bool ok = false; // the name parsed
  // Derived copies the element came in through, outermost first; `madeBy`
  // and every id below are then ids in the innermost SOURCE document.
  std::vector<DerivedStep> derived;
  uint64_t seedTag = 0; // the feature whose seed the name starts from
  // The feature that made the element: the seed's, or the last G step's (a
  // fillet's face is the fillet's, not the edge's). 0 for a Boundary face.
  uint64_t madeBy = 0;
  ElementRole role = ElementRole::Unknown;
  uint32_t entity = 0;                // the sketch entity (Side, hole roles)
  std::vector<std::string> instances; // pattern copies ('#2', '#m')
  std::vector<uint64_t> modifiedBy;   // the M (and S) steps, in order
};

ElementOrigin DecodeElementName(const std::string& name);

// Every feature id the name mentions, in this document: each comma part's
// leading id (the outermost Derived feature or the seed) and every M and G
// step. What drawing provenance matches a hole or a tolerance by.
std::vector<uint64_t> FeatureIdsMentioned(const std::string& name);

} // namespace topo
