// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What the four direct edits share -- Delete, Offset, Move and Replace Face
// (docs/modeling/README.md#direct-edit): resolving the faces they take,
// the fingerprint that keeps a positional name honest, the checks every
// result passes before it is kept, and naming through the builder's history.
// Each edit changes ONE body in place, named by its createdBy as Shell's is.

#include "core/ElementMap.h"
#include "core/Feature.h"

#include <Message_ProgressIndicator.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <glm/glm.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class BRepTools_History;

namespace directedit {

// A face as it was picked: what a positional name is checked against when
// the body is rebuilt (an Import's "<tag>/face<i>" is an index, and a
// changed file read again renumbers it).
struct FaceFingerprint {
  std::string surface; // Measure's FaceInfo::kind: plane, cylinder, ...
  double areaMm2 = 0.0;
  glm::dvec3 centroid{0.0}; // mm
};
// Keyed by the face's element name.
using Fingerprints = std::map<std::string, FaceFingerprint>;

// Whether `name` is an index into a file's faces -- a positional name whose
// seed is an Import -- so that the face it names can silently become another
// one. A primitive's "<tag>/face<i>" is positional too, but a box keeps its
// numbering whatever its size, so its faces are not fingerprinted.
bool IsFilePositional(const RegenContext& ctx, const std::string& name);

std::optional<FaceFingerprint> FingerprintAt(const Body& body, int faceIndex);

// "planar face `7/face12`, 120 mm²" -- how every refusal names a face.
std::string DescribeFace(const Body& body, int faceIndex,
                         const std::string& name);
// "`a`, `b` and `c`".
std::string NameList(const std::vector<std::string>& names);

// The body `createdBy` made that holds the face `name`: bodies can share a
// createdBy (a multi-body import's "<tag>/b<k>/...", a split's pieces), and
// the name says which. The first such body when none holds it; null when
// there is none at all.
const Body* BodyHolding(const std::vector<Body>& bodies, uint64_t createdBy,
                        const std::string& name);

// The faces a direct edit was given, resolved on its body.
struct Picked {
  Body* body = nullptr;
  std::vector<int> indices;       // face indices, in the order given
  std::vector<std::string> names; // their names, the same order
};

// Resolves `faces` on the body `bodyId` created: every ref on that one body
// (refused otherwise, as Shell's are), every name resolving (a face gone
// upstream is an error naming why, never skipped), and every file-positional
// name still the face its fingerprint says was picked. A missing fingerprint
// is taken now -- the first time the edit sees the face is the pick -- and
// one for a name no longer among `faces` is dropped. Empty on success, else
// the error.
// `keep`, when given, lists every name whose print stays (an edit with faces
// on two bodies resolves each in turn).
std::string ResolvePicked(RegenContext& ctx, uint64_t bodyId,
                          const std::vector<ElementRef>& faces,
                          Fingerprints& prints, const std::string& what,
                          Picked& out,
                          const std::vector<std::string>* keep = nullptr);

// The body's shape copied (topology only: an OCC builder writes to its input,
// and the body is a cached result) and the copy's faces at `indices`. The
// copy's explorer order is the body's, so the body's names apply to it.
struct Copied {
  TopoDS_Shape shape;
  TopTools_ListOfShape faces;
};
Copied CopyFor(const Body& body, const std::vector<int>& indices);

// What a result must satisfy to be kept (the second rule of
// docs/modeling/README.md#direct-edit).
struct Expect {
  enum class Volume { Any, Changes, Grows, Shrinks, Same };
  Volume volume = Volume::Changes;
  double volumeTolerance = 1e-9; // relative
  double bboxGrowth = 0.0;       // mm the tight box may grow along any axis
  enum class Faces { Gone, Modified };
  Faces faces = Faces::Modified;
};
// Empty when `result` (named `map`) passes, else why not, the faces named.
// `op` is the edit's name in the sentence ("Offset Face").
std::string CheckResult(const Body& before, const Picked& picked,
                        const CADShape& result, const ElementMap& map,
                        uint64_t tag, const Expect& expect,
                        const std::string& op);

// Names for a direct edit's result through its builder's history
// (topo::NamesThroughHistory without the whole-face rule, so what it moved
// reads ";M<tag>").
ElementMap NameThrough(const TopoDS_Shape& input, const ElementMap& map,
                       const BRepTools_History& history,
                       const TopoDS_Shape& result, uint64_t tag);

// The faces an edit left alone, named as they were: a result face named
// "<name>;M<tag>" that is the input's face `<name>`, unchanged -- the same
// surface kind, area and centroid to 1e-9 relative -- and not one of
// `picked`, takes `<name>` back. Builders report faces they merely rebuilt
// (a boolean of two bodies sharing every untouched face reports each one
// modified), and a reference to a face nobody edited must keep resolving.
void RestoreUntouched(const Body& before, const CADShape& result,
                      ElementMap& map, uint64_t tag,
                      const std::vector<std::string>& picked);

// `name` with each run of repeated ";M<tag>" steps collapsed to one: an edit
// built from several named steps, all under its own tag, modified a face
// once.
std::string CollapseSteps(const std::string& name, uint64_t tag);

// Runs an OCC builder: a fault (cad/OccSignals.h) or a Standard_Failure
// becomes the returned reason, else empty.
std::string Guarded(const std::string& op, const std::function<void()>& f);

// "Add the feature" (docs/modeling/README.md#delete-face): the faces of the
// one feature `seeds` belong to -- grown across edges to every face reachable
// without stepping onto a HOST, a face whose inner loop (a hole in it) the
// edge lies on. A through bore stays itself, a blind hole's bore takes its
// floor, a boss's side takes the boss. nullopt, with `why`, when no inner
// loop bounds the set (a fillet runs on into the body) or it would take
// every face. Indices in ElementMap order, sorted.
std::optional<std::vector<int>> FeatureFaces(const Body& body,
                                             const std::vector<int>& seeds,
                                             std::string* why = nullptr);

// The outward normal of the host face of `faces` -- the largest planar face
// that is not one of them and shares an edge with one: the face a hole or a
// boss sits in. nullopt when none is planar.
std::optional<glm::dvec3> HostNormal(const Body& body,
                                     const std::vector<int>& faces);

// How long a direct edit's builders may run before it is given up, s of the
// builder thread's CPU time: 30 s and half a second more for each face it
// was given (a wall-clock limit of four times that stays behind it). The
// builders stop at their progress checks
// (Message_ProgressIndicator::UserBreak); without one, deleting a face of a
// modelled thread ran Defeaturing's edge-face intersections for more than
// fifteen minutes. It is a backstop against a frozen app, not a test of speed
// -- an answer that depends on the machine's load is not one to rely on -- so
// it sits far past any edit that builds (the 100 bores of plate_100_holes.step
// take 17 s of CPU on a fast core and 62 s on a slow one in a parallel suite;
// they get 80 s), while a refusal it forces on one face, overshoot and all,
// still answers inside the action matrix's minute (random design 33's face
// `100/g4;U1;G110;M120` stopped at 67 s under a 60 s budget, 50 s under
// 40 s).
inline double BuilderBudgetS(size_t faces) { return 30.0 + 0.5 * faces; }

// A progress indicator that asks its builder to stop once it has used
// `seconds` of CPU (`Expired` then says so). Handed to a builder as `Start()`.
Handle(Message_ProgressIndicator) MakeDeadline(double seconds);
bool Expired(const Handle(Message_ProgressIndicator) & deadline);

// A direct edit extends or intersects the faces around the ones it is given.
// A freeform face there (Measure's kind "other": a modelled thread's flank,
// a spline) is one the builders extend and intersect at a cost past any
// budget -- deleting one face of a thread ran for more than fifteen minutes
// -- so the edit is refused before it runs, naming both faces. Empty when
// every neighbour is analytic. `does` says what the edit does to them
// ("heals by extending").
std::string FreeformNeighbour(const Picked& picked, const std::string& op,
                              const std::string& does);

// The body's thinnest bounding-box extent, mm.
double ThinnestExtent(const Body& body);

} // namespace directedit
