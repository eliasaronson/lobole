#pragma once

#include "cad/CADKernel.h"
#include "core/ElementHistory.h"          // RefineResult
#include "core/Feature.h"                 // Body
#include "core/features/BooleanFeature.h" // BooleanOp
#include "core/features/ExtrudeFeature.h" // ExtrudeMode

#include <functional>
#include <string>
#include <utility>
#include <vector>

// Applies a freshly built solid to the body list according to `mode`, shared
// by the solid-producing features (extrude, revolve):
//   NewBody: append `shape` as its own body, stamped with `ownerId` as its
//            createdBy and carrying `shapeMap` as its element map.
//   Add:     fuse into the last body whose bbox overlaps (else the last),
//            leaving that body's createdBy (and material) untouched; element
//            names propagate through the fuse (toponaming T5) — the target's
//            names plus `shapeMap`'s, rewritten onto the fused shape. Every
//            OTHER body the shape touches joins that one too (a neck between
//            two turned pieces makes one body of them), and is removed from
//            the list.
//   Cut:     subtract from every body, dropping any the cut fully consumes;
//            each surviving body's names propagate through its cut.
// Returns false only on an operation that genuinely failed (a fuse/cut error,
// or Cut with no bodies). `shape` and `shapeMap` are moved from.
//
// `warning` (optional out-param): set to a human-readable message when the
// operation SUCCEEDED but did nothing useful — a Cut whose tool intersected no
// body (total volume unchanged), or an Add whose tool lay entirely inside its
// target. These are not errors (the result is valid), but almost always signal
// a mis-placed sketch/plane, so callers surface them via Feature::SetWarning.
//
// `error` (optional out-param): the reason for a false return, in user-grade
// prose, so callers can surface it via Feature::SetError instead of the
// generic "regeneration failed". Without it the reason is only logged.
bool ApplyBodyMode(std::vector<Body>& bodies, CADShape shape,
                   ElementMap shapeMap, ExtrudeMode mode,
                   const std::string& name, const std::string& material,
                   uint64_t ownerId, std::string* warning = nullptr,
                   std::string* error = nullptr);

// What a body boolean must keep, checked on its RESULT before it is committed
// (docs/modeling/README.md#a-boolean-is-checked-before-it-is-kept). A kernel
// boolean can return a shape that is non-null, passes BRepCheck, and is still
// wrong: a slot whose round ends were the same cylinders as two bores came
// back with the bores' floors kept inside the slot and its far end standing
// in the empty bores — the bores INSIDE OUT, a section solid where the holes
// are, and the part 13,800 mm3 lighter than any cut could have made it.
// Checked:
//   - the result's volume is not negative, and no closed shell of it is the
//     wrong way round (a solid's outer shell inward, a void outward);
//   - a Subtract is no larger than `target`, an Intersect no larger than
//     either operand, a Union no smaller than `target`;
//   - no face of a cut or a union lies strictly INSIDE the tool, and none of
//     an intersection outside it — judged for every face the boolean made
//     and every face of the target it kept whole that reaches into the
//     tool's box, at one point inside each, against the tool (a sound solid,
//     and usually a small one).
// The volumes are the ones the callers need anyway; the rest cost the
// OM615's build about 2%. Tolerances are relative to the sizes compared.
// Returns the refusal in user-grade prose naming `feature`, the operation,
// `body` and both volumes; empty when the result is sound.
[[nodiscard]] std::string BooleanRefusal(const std::string& feature,
                                         BooleanOp op, const std::string& body,
                                         const CADShape& target,
                                         const CADShape& tool,
                                         const CADShape& result);

// A body boolean run through BooleanRefusal, with ONE retry when it is
// refused (docs/modeling/README.md#a-refused-boolean-gets-one-fuzzy-retry).
// Coincident geometry — a slot exactly as wide as the seat it opens, two equal
// spokes whose axes cross microns apart — is what OpenCASCADE's exact booleans
// get wrong, and its fuzzy option is what they have for it: the retry runs the
// same boolean with SetFuzzyValue at FuzzyTolerance(target, tool),
// non-destructively, and keeps its result only if it passes the same check.
struct CheckedBoolean {
  CADShape shape;      // the kept result; invalid on a failure or a refusal
  ElementMap map;      // its names (toponaming T5)
  std::string error;   // the kernel could not build it (shape invalid)
  std::string refusal; // refused, retry included (shape invalid)
  double fuzzy = 0.0;  // > 0: kept only through the retry, at this tolerance
  std::string warning; // set with `fuzzy`: what the retry saved
};
// `extra` (optional) is a check of the caller's own that a result must also
// pass, returning why not (the Boolean feature's BRepCheck); it runs before
// BooleanRefusal on either attempt. `refine` (optional) cleans each attempt's
// result up before it is named and checked (topo::RefineResult).
CheckedBoolean CheckedBooleanNamed(
    const std::string& feature, BooleanOp op, const std::string& body,
    const CADShape& target, const ElementMap& targetMap, const CADShape& tool,
    const ElementMap& toolMap, uint64_t featureTag,
    const std::function<std::string(const CADShape&)>& extra = {},
    const topo::RefineResult& refine = {});

// The fuzzy retry's tolerance: 1e-5 of the SMALLER operand's bounding-box
// diagonal, so it scales with the feature and never swallows a dimension of
// it (a 2 mm spoke 290 mm long gets 0.0029 mm). The fraction is measured:
// too small or too large a value each gave a result that passed the check
// and was wrong (BodyMode.cpp, kFuzzyRel).
[[nodiscard]] double FuzzyTolerance(const CADShape& target,
                                    const CADShape& tool);

// How far a cut (or add) tool drawn on a sketch plane must start BEHIND that
// plane, so that a body face the plane was meant to lie on is opened (or
// fused to) rather than missed by a hair. A sketch plane is exact (double),
// and a face made from one lies on it; but a face can come from a value that
// went through float -- a primitive's position is a glm::vec3, so a box
// placed at z = 24.7 sits at 24.70000076 -- and a cut from a plane typed at
// that face's height then keeps a sub-micron skin over the pocket (a sealed
// counterbore that passes BRepCheck), or an add leaves two solids. Returns 0
// unless some planar face of `bodies` lies within single-precision rounding
// of the plane through `origin` -- but not ON it, to double precision: that
// coplanar boolean needs no help, and a moved tool would leave the feature's
// own face off its plane -- square to `sweep`, and faces the way the tool
// meets it: AGAINST the sweep for a cut (the tool goes into the
// material behind it), WITH it for an add (the tool grows out of it).
// Otherwise twice that rounding, scaled to the coordinates.
[[nodiscard]] double CoplanarStartSlack(const std::vector<Body>& bodies,
                                        const glm::dvec3& origin,
                                        const glm::dvec3& sweep, bool cut);

// Convenience for the solid-producing features: run ApplyBodyMode and forward
// any advisory onto `owner` (Feature::SetWarning) and any failure reason onto
// Feature::SetError, so every cut/add feature reports the same way without
// repeating the plumbing.
bool ApplyBodyModeReporting(std::vector<Body>& bodies, CADShape shape,
                            ElementMap shapeMap, ExtrudeMode mode,
                            Feature& owner);

// Fuses `instances` (a pattern/mirror's seed + transformed copies, each with
// its ElementMap), splits the result back into its connected solids, erases
// every body created by `sourceId`, and appends each solid as a body owned by
// `ownerId`. Instances that touch merge into one body; disjoint instances
// become separate bodies. Each instance's names travel with it through the
// fuse, so a patterned body's faces can be picked, threaded and fitted like
// the seed's — the caller suffixes them per instance ("4/g7" -> "4/g7#2"),
// the convention patterned CUTS already use. Returns false if no source body
// was found or the fuse failed; `error` (optional) receives the reason for
// callers to SetError.
bool FuseInstancesReplacingSource(
    std::vector<Body>& bodies, uint64_t sourceId,
    std::vector<std::pair<CADShape, ElementMap>> instances,
    const std::string& name, const std::string& material, uint64_t ownerId,
    std::string* error = nullptr);
