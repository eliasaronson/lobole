// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A primitive fitted to a body's triangles from a picked point
// (docs/modeling/README.md#fitting-a-primitive-to-a-mesh): the region a
// plane, cylinder, cone or sphere grows over from the point, fitted
// robustly against a scan's outliers. What `fit_primitive` answers, what a
// DatumPlane or DatumAxis `from_fit` refits on every regeneration, and what
// the GUI Fit tool previews. Works on a mesh body's own triangles and on a
// B-Rep's tessellation alike.

#include "core/Feature.h"
#include "core/mesh/Segment.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace meshfit {

// "plane, cylinder, cone, sphere, auto": what a refusal names.
std::string AcceptedKinds();
// A kind by name; `auto` is nullopt in `kind` with true returned. False for
// any other string.
[[nodiscard]] bool KindFromString(const std::string& s,
                                  std::optional<mesh::Kind>& kind);

inline constexpr int kDefaultClipSigma = 3;
// With hints, how many surfaces near the point are grown before the fit is
// refused.
inline constexpr size_t kMostSeeds = 8;

// "1, 2, 3 or \"all\"": what a refusal of clip_sigma names.
std::string AcceptedClipSigma();
// clip_sigma as JSON: 1, 2 or 3, or "all" for 0. False for anything else.
[[nodiscard]] bool ClipSigmaFromJson(const nlohmann::json& j, int& out);
nlohmann::json ClipSigmaJson(int clipSigma);
// The hints of a request or a datum's `fit` read from `j` (side, radius_mm,
// axis + axis_tolerance_deg, within {min_mm, max_mm}), each checked:
// throws naming the key and what it accepts. `what` prefixes the message.
mesh::FitHints HintsFromJson(const nlohmann::json& j, const std::string& what);
// The keys HintsFromJson reads.
const std::vector<std::string>& HintKeys();
// The hints written back, only those given.
void HintsToJson(const mesh::FitHints& h, nlohmann::json& j);

struct Request {
  glm::dvec3 pointMm{0.0}; // on (or near) the surface, world
  // Unset = auto: each kind is grown and the one covering the most area
  // wins, a simpler kind when it covers nearly as much (kAutoShare).
  std::optional<mesh::Kind> kind;
  // Unset = the scan's own noise sets it (mesh::ScanTolerance).
  std::optional<double> toleranceMm;
  // How many sigma of the residuals a used point stays within: 1, 2, 3;
  // 0 = every vertex of the region ("all").
  int clipSigma = kDefaultClipSigma;
  // Steering, all optional (docs/modeling/README.md#hints). With hints, the
  // triangles within the pick's reach of the point are tried as seeds,
  // nearest first, until one grows a fit that meets them.
  mesh::FitHints hints;
};

// auto takes a simpler kind (plane, sphere, cylinder, cone, in that order)
// when it covers at least this share of the best one's area: a cone fits a
// cylinder's triangles as well as the cylinder does, with a half angle of
// nearly nothing.
inline constexpr double kAutoShare = 0.95;

struct Candidate {
  mesh::Kind kind = mesh::Kind::Plane;
  bool ok = false;
  double areaMm2 = 0.0;
  double rmsMm = 0.0;
  std::string why; // when not ok
};

struct Result {
  mesh::Region region; // the fit and the triangles it grew over
  std::string body;    // the body's name
  double toleranceMm = 0.0;
  double seedDistanceMm = 0.0; // the point's distance from the triangles
  int clipSigma = kDefaultClipSigma;
  size_t seedsTried = 1; // with hints, the surfaces tried before this one
  bool chosen = false;   // the kind came from auto
  std::vector<Candidate> tried;
};

// Refuses a request that is wrong before anything is read: a point that is
// not three finite numbers, a tolerance that is not a number of mm > 0, a
// clip_sigma that is not 0-3, a hint the kind cannot take (side on a plane,
// radius_mm on a plane or cone, axis on a sphere), a point outside `within`.
void CheckRequest(const Request& request);

// The fit on whichever of `bodies` the point is nearest. False with `error`
// naming why: no triangles, the point off every body (farther than a
// fiftieth of their diagonal), no kind fitting there.
bool Fit(const std::vector<const Body*>& bodies, const Request& request,
         Result& out, std::string& error);

// The bodies feature `featureId` made, of `bodies` (references skipped).
std::vector<const Body*> BodiesOf(const std::vector<Body>& bodies,
                                  uint64_t featureId);

// The reply's {kind, origin_mm | centre_mm | apex_mm, normal | axis,
// radius_mm?, half_angle_deg?, rms_mm, max_mm, sigma_mm, form_mm, used,
// clip_sigma, diameter_min_mm?, diameter_max_mm?, length_mm?, arc_deg?,
// extent_mm?, warnings?, tolerance_mm, triangles, area_mm2, body,
// seed_distance_mm, tried?}.
nlohmann::json ResultJson(const Result& r);
// A cylinder or sphere whose used points cover less arc than this says so
// in the reply's `warnings`: a partial arc is where a radius is least
// certain.
inline constexpr double kPartialArcDeg = 60.0;

// A cylinder's axis has no sense of its own: the one whose largest
// component is positive, so a refit on a re-scan does not flip it. A
// cone's points from the apex into the cone.
glm::dvec3 CanonicalAxis(const mesh::FitResult& f);

// What a datum stores to refit itself (DatumPlane / DatumAxis `from_fit`):
// the body's feature, the seed point and the kind -- never `auto`, so a
// re-scan cannot turn the datum into another kind -- and the tolerance if
// one was given.
struct FitRef {
  uint64_t bodyFeatureId = 0;
  glm::dvec3 pointMm{0.0};
  mesh::Kind kind = mesh::Kind::Plane;
  std::optional<double> toleranceMm;
  // Written only when given: a datum refits clipped and steered as it was
  // fitted.
  std::optional<int> clipSigma;
  mesh::FitHints hints;
};

// Fits `ref` on the context's bodies. False with `error` when the body is
// gone, makes nothing before this feature, or nothing fits.
bool Refit(const RegenContext& ctx, const FitRef& ref, Result& out,
           std::string& error);

} // namespace meshfit
