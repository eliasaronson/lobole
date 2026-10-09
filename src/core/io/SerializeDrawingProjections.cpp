// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// A drawing view's saved projection (v162,
// docs/drawings/README.md#saved-projections): the curves ProjectView
// returned, their sources in index space, and a section's cuts, in the
// result-array form a study's result uses (SerializationInternal.h). The
// points are float, because they are derived bulk data; a conic's centre
// and radius are double, because a dimension reads its value off them.

#include "core/InputHash.h"
#include "core/drawing/ViewProjection.h"
#include "core/io/SerializationInternal.h"

#include <stdexcept>

using json = nlohmann::json;

namespace {

json Vec3(const glm::dvec3& v) { return json::array({v.x, v.y, v.z}); }

glm::dvec3 Vec3From(const json& j, const char* name) {
  if (!j.is_array() || j.size() != 3)
    throw std::runtime_error(std::string("`") + name +
                             "` is not three numbers");
  return {j.at(0).get<double>(), j.at(1).get<double>(), j.at(2).get<double>()};
}

const char* ModeName(drawing::ProjectionMode m) {
  return m == drawing::ProjectionMode::Poly ? "fast" : "exact";
}

} // namespace

json ViewProjectionToJson(const drawing::ViewProjection& p) {
  ResultArrayWriter w;
  const auto& curves = p.result.curves;
  std::vector<int> kind, flags, type, count, source, edge, face;
  std::vector<double> points, conics;
  for (const drawing::ProjectedCurve& c : curves) {
    kind.push_back(static_cast<int>(c.kind));
    flags.push_back((c.visible ? 1 : 0) | (c.closed ? 2 : 0));
    type.push_back(c.curveType);
    count.push_back(static_cast<int>(c.points.size()));
    source.push_back(static_cast<int>(c.source.sourceId));
    edge.push_back(c.source.edgeIndex);
    face.push_back(c.source.faceIndex);
    for (const glm::dvec2& q : c.points) {
      points.push_back(q.x);
      points.push_back(q.y);
    }
    conics.push_back(c.conicCenter.x);
    conics.push_back(c.conicCenter.y);
    conics.push_back(c.conicRadius);
  }
  json out{{"kind", "drawing-projection"},
           {"revision", drawing::kProjectionRevision},
           {"input_hash", inputhash::Hex(p.inputHash)},
           {"mode", ModeName(p.result.mode)},
           {"view_dir", Vec3(p.viewDir)},
           {"up", Vec3(p.up)},
           {"sectioned", p.sectioned},
           {"ds_edges", p.result.stats.dsEdges},
           {"unresolved", p.result.stats.unresolvedSources}};
  json c{{"count", curves.size()},       {"kind", w.Index(kind)},
         {"flags", w.Index(flags)},      {"type", w.Index(type)},
         {"points_per", w.Index(count)}, {"source", w.Index(source)},
         {"edge", w.Index(edge)},        {"face", w.Index(face)},
         {"points", w.F32(points)},      {"conics", w.F64(conics)}};
  out["curves"] = std::move(c);
  if (p.sectioned) {
    json cuts = json::array();
    for (const drawing::ViewProjection::Cut& cut : p.cuts) {
      std::vector<int> loopSizes;
      std::vector<glm::dvec3> loopPoints;
      for (const auto& loop : cut.cutLoops) {
        loopSizes.push_back(static_cast<int>(loop.size()));
        loopPoints.insert(loopPoints.end(), loop.begin(), loop.end());
      }
      cuts.push_back({{"body", cut.body},
                      {"edge_origin", w.Index(cut.edgeOrigin)},
                      {"edge_from_face", w.Index(cut.edgeFromFace)},
                      {"face_origin", w.Index(cut.faceOrigin)},
                      {"loop_sizes", w.Index(loopSizes)},
                      {"loop_points", w.F32(loopPoints)}});
    }
    out["cuts"] = std::move(cuts);
  }
  out[kResultData] = w.Take();
  out["bin_bytes"] = out[kResultData].get_binary().size();
  return out;
}

drawing::ViewProjection ViewProjectionFromJson(const json& j,
                                               const std::string& what) {
  if (j.value("kind", std::string()) != "drawing-projection")
    throw std::runtime_error(what + " is not a drawing projection");
  drawing::ViewProjection p;
  if (!inputhash::FromHex(j.value("input_hash", std::string()), p.inputHash))
    throw std::runtime_error(what + " has no input_hash");
  const std::string mode = j.value("mode", std::string("exact"));
  if (mode == "fast")
    p.result.mode = drawing::ProjectionMode::Poly;
  else if (mode == "exact")
    p.result.mode = drawing::ProjectionMode::Exact;
  else
    throw std::runtime_error(what + ": unknown mode '" + mode +
                             "' (exact, fast)");
  p.viewDir = Vec3From(j.at("view_dir"), "view_dir");
  p.up = Vec3From(j.at("up"), "up");
  p.sectioned = j.value("sectioned", false);
  p.result.stats.dsEdges = j.value("ds_edges", 0);
  p.result.stats.unresolvedSources = j.value("unresolved", 0);
  const ResultArrayReader bin(j, what);
  const json& c = j.at("curves");
  const size_t n = c.at("count").get<size_t>();
  const auto kind = bin.Indices(c.at("kind"), "kind");
  const auto flags = bin.Indices(c.at("flags"), "flags");
  const auto type = bin.Indices(c.at("type"), "type");
  const auto per = bin.Indices(c.at("points_per"), "points_per");
  const auto source = bin.Indices(c.at("source"), "source");
  const auto edge = bin.Indices(c.at("edge"), "edge");
  const auto face = bin.Indices(c.at("face"), "face");
  const auto points = bin.Reals(c.at("points"), "points");
  const auto conics = bin.Reals(c.at("conics"), "conics");
  for (const auto* v : {&kind, &flags, &type, &per, &source, &edge, &face})
    if (v->size() != n)
      throw std::runtime_error(what + ": a curve array is not " +
                               std::to_string(n) + " long");
  if (conics.size() != 3 * n)
    throw std::runtime_error(what + ": the conics are not three per curve");
  size_t at = 0;
  p.result.curves.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    if (kind[i] < 0 || kind[i] > static_cast<int>(drawing::CurveKind::Bend) ||
        per[i] < 0 || source[i] < 0)
      throw std::runtime_error(what + ": curve " + std::to_string(i) +
                               " is malformed");
    drawing::ProjectedCurve pc;
    pc.kind = static_cast<drawing::CurveKind>(kind[i]);
    pc.visible = (flags[i] & 1) != 0;
    pc.closed = (flags[i] & 2) != 0;
    pc.curveType = type[i];
    pc.source.sourceId = static_cast<uint64_t>(source[i]);
    pc.source.edgeIndex = edge[i];
    pc.source.faceIndex = face[i];
    if (at + 2 * static_cast<size_t>(per[i]) > points.size())
      throw std::runtime_error(what + ": the points run out");
    pc.points.reserve(per[i]);
    for (int k = 0; k < per[i]; ++k, at += 2)
      pc.points.emplace_back(points[at], points[at + 1]);
    pc.conicCenter = {conics[3 * i], conics[3 * i + 1]};
    pc.conicRadius = conics[3 * i + 2];
    p.result.curves.push_back(std::move(pc));
  }
  if (at != points.size())
    throw std::runtime_error(what + ": points left over");
  if (p.sectioned)
    for (const json& cj : j.value("cuts", json::array())) {
      drawing::ViewProjection::Cut cut;
      cut.body = cj.at("body").get<int>();
      cut.edgeOrigin = bin.Indices(cj.at("edge_origin"), "edge_origin");
      cut.edgeFromFace = bin.Indices(cj.at("edge_from_face"), "edge_from_face");
      cut.faceOrigin = bin.Indices(cj.at("face_origin"), "face_origin");
      const auto sizes = bin.Indices(cj.at("loop_sizes"), "loop_sizes");
      const auto pts = bin.Vec3s(cj.at("loop_points"), "loop_points");
      size_t k = 0;
      for (int size : sizes) {
        if (size < 0 || k + size > pts.size())
          throw std::runtime_error(what + ": a cut loop runs out");
        cut.cutLoops.emplace_back(pts.begin() + k, pts.begin() + k + size);
        k += size;
      }
      p.cuts.push_back(std::move(cut));
    }
  return p;
}
