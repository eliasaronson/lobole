#pragma once

#include "cad/SketchPlane.h"
#include "core/ElementMap.h"
#include "core/Expression.h"
#include "core/features/ExtrudeFeature.h" // ExtrudeMode

#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

class CommandStack;
class Document;
class Feature;
class SketchFeature;

// The primitive tools (docs/modeling/README.md#primitives): a Box, Cylinder
// or Sphere asked for in one call, stored as the features a person would
// draw — a fully constrained, dimensioned Sketch on what it is placed on,
// and an Extrude (Box, Cylinder) or a Revolve (Sphere) of it.
//
// Every primitive names what it is ON: a planar face, a datum plane, another
// sketch's plane, a Frame (a mate connector), or one of the named world
// planes. There is no bare world position. The sketch attaches the way any
// sketch does (face_ref, plane_ref, or a base plane), so the primitive
// follows its reference at every regeneration, and a lost reference is the
// sketch's error, never a fall-back to the world.
//
// The stored BoxFeature / CylinderFeature / SphereFeature classes are NOT
// this: they remain only so older documents load and so core tests have a
// one-line solid. Nothing an agent or a person reaches creates them.
namespace primitive {

enum class Kind { Box, Cylinder, Sphere };
[[nodiscard]] const char* KindName(Kind kind);
// "Box" / "Cylinder" / "Sphere", exactly; anything else is not a primitive.
[[nodiscard]] std::optional<Kind> KindFromName(const std::string& name);

// A world plane (by name), a planar face (by element name), or a feature
// that IS a plane: a DatumPlane, a Sketch (its plane) or a Frame (its XY
// plane, X as u). Which of the three is the document's to say.
enum class OnKind { WorldPlane, Face, PlaneFeature };

// What a primitive stands on.
struct On {
  OnKind kind = OnKind::WorldPlane;
  PlaneType world = PlaneType::XY; // WorldPlane
  ElementRef face;                 // Face
  uint64_t featureId = 0;          // PlaneFeature
};

// A dimension: the number, the expression that drives it (empty = a literal)
// and whether the caller gave it at all (unset ones take derived defaults).
struct Dim {
  double value = 0.0;
  std::string expr;
  bool set = false;
};

struct Spec {
  Kind kind = Kind::Box;
  std::string name;
  On on;
  // Box: width along the frame's u, depth along v, height along its normal.
  // Cylinder: diameter and height. Sphere: diameter.
  Dim width, depth, height, diameter;
  // Where on the frame: the rectangle's centre (or, with centered = false,
  // its low-u low-v corner), the circle's centre, the sphere's centre.
  Dim offsetU, offsetV;
  bool centered = true; // Box only
  ExtrudeMode mode = ExtrudeMode::NewBody;
  bool reversed = false; // Box, Cylinder: extrude against the normal
  std::string material;
};

// The frame a primitive is drawn in, resolved against the document as it
// stands, and the size its defaults derive from.
struct Placement {
  SketchPlane plane{PlaneType::XY};
  // A face: the UV box its outline spans in `plane` (origin at its
  // centroid). Absent for a plane or a frame, which have no outline.
  bool hasExtent = false;
  glm::dvec2 lo{0.0}, hi{0.0};
  // The length defaults scale from: the face's smaller span, or the part's
  // largest bounding-box dimension when the reference has no outline of its
  // own. 0 when there is nothing to measure (an empty part on a plane).
  double size = 0.0;
};

// Every key a primitive accepts, and what `on` may be, as the refusal text
// names them.
[[nodiscard]] const char* AcceptedOn();
[[nodiscard]] const char* AcceptedKeys(Kind kind);

// The add_feature dialect. Throws std::runtime_error naming what IS
// accepted for an unknown or missing `on`, an unknown key, a world position
// from the old dialect, or a size given twice (diameter and radius).
[[nodiscard]] Spec SpecFromJson(const nlohmann::json& j);
// The same spec back as that dialect (what a test or the GUI round-trips).
[[nodiscard]] nlohmann::json SpecToJson(const Spec& spec);

// Resolves `on` against the document's current bodies and features. The
// error names the reference and what was wrong with it.
[[nodiscard]] std::expected<Placement, std::string> Resolve(const Document& doc,
                                                            const On& on);

// Fills every unset dimension from the placement: a primitive half the
// face's smaller span on a face, a quarter of the part's size on a plane or
// a frame. `fallbackSize` stands in when the placement measured nothing
// (the GUI passes the viewport's reach); with neither, the refusal asks for
// the sizes. Returns "" or the refusal.
[[nodiscard]] std::string ApplyDefaults(Spec& spec, const Placement& placement,
                                        double fallbackSize = 0.0);

// Evaluates every dimension's expression against `symbols` into its value.
// A size must come out positive. Returns "" or the refusal.
[[nodiscard]] std::string EvaluateDims(Spec& spec, const SymbolTable& symbols);

// The features, unattached to any document: the sketch first, the solid
// second. The solid names the sketch by id, so a caller adds the sketch,
// then calls BindSketch with the id the document gave it, then adds the
// solid. Requires ApplyDefaults/EvaluateDims to have run.
struct Expansion {
  std::unique_ptr<SketchFeature> sketch;
  std::unique_ptr<Feature> solid;
};
[[nodiscard]] Expansion Expand(const Spec& spec, const Placement& placement);
void BindSketch(Feature& solid, uint64_t sketchId);

// The whole tool: resolves `on`, fills the defaults, evaluates the
// expressions, expands, and adds the sketch and the solid to `doc` through
// `commands` as ONE undo step. Throws std::runtime_error with the refusal
// ("Box: ..."), leaving the document as it was.
struct Added {
  uint64_t solidId = 0;
  uint64_t sketchId = 0;
};
Added Add(Document& doc, CommandStack& commands, Spec spec,
          double fallbackSize = 0.0);

// Rewrites an expanded sketch's geometry and dimensions, and the solid's
// height, from `spec` — the live edit a tool session makes while the
// primitive is a preview. Entity ids are kept.
void Refill(SketchFeature& sketch, Feature& solid, const Spec& spec);

} // namespace primitive
