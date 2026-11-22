// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/ScanCompare.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/MeshOperand.h"
#include "core/mesh/Segment.h" // ScanTolerance

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace scancompare {

namespace {

std::string Mm(double v) {
  char buf[48];
  std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

nlohmann::json Json(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

// Several meshes as one.
std::shared_ptr<const mesh::TriMesh>
Joined(const std::vector<std::shared_ptr<const mesh::TriMesh>>& meshes) {
  if (meshes.size() == 1)
    return meshes.front();
  auto out = std::make_shared<mesh::TriMesh>();
  for (const auto& m : meshes) {
    const uint32_t base = static_cast<uint32_t>(out->positions.size());
    out->positions.insert(out->positions.end(), m->positions.begin(),
                          m->positions.end());
    out->quantum = std::max(out->quantum, m->quantum);
    for (const auto& t : m->triangles)
      out->triangles.push_back({t[0] + base, t[1] + base, t[2] + base});
  }
  mesh::BuildAdjacency(*out);
  return out;
}

} // namespace

void AddBodies(const std::vector<std::pair<const Body*, int>>& bodies,
               double tol, Input& out) {
  // A B-Rep is tessellated finer than the tolerance: a quarter of it, never
  // coarser than a thousandth of its size, never finer than a
  // hundred-thousandth.
  std::vector<std::shared_ptr<const mesh::TriMesh>> parts;
  std::vector<std::vector<uint32_t>> partFaces;
  for (size_t k = 0; k < bodies.size(); k++) {
    const Body& b = *bodies[k].first;
    if (b.IsMesh()) {
      double used = 0.0;
      auto m = meshop::Triangles(b, 0.0, used);
      if (m == nullptr || m->triangles.empty())
        continue;
      const uint32_t face = static_cast<uint32_t>(out.faces.size());
      out.faces.push_back({bodies[k].second, -1, ""});
      parts.push_back(std::move(m));
      partFaces.emplace_back(parts.back()->triangles.size(), face);
      continue;
    }
    if (!b.shape.IsValid())
      continue;
    glm::dvec3 lo, hi;
    if (!BodyBounds(b, lo, hi))
      continue;
    const double diag = glm::length(hi - lo);
    const double defl =
        std::clamp(tol / 4.0, std::max(diag * 1e-5, 1e-6),
                   std::max(meshop::DerivedDeflectionMm(b), 1e-6));
    out.chordMm = std::max(out.chordMm, defl);
    const CADShape::Mesh cm = b.shape.GenerateMeshDetached(defl);
    std::vector<glm::dvec3> soup;
    soup.reserve(cm.indices.size());
    for (uint32_t i : cm.indices)
      soup.emplace_back(cm.vertices[i].position);
    std::vector<uint32_t> kept;
    auto m = std::make_shared<const mesh::TriMesh>(
        mesh::FromSoup(soup, mesh::Float32Quantum(soup), kept));
    if (m->triangles.empty())
      continue;
    // Each B-Rep face of this body, once.
    const uint32_t base = static_cast<uint32_t>(out.faces.size());
    uint32_t most = 0;
    for (uint32_t f : cm.triangleFaces)
      most = std::max(most, f + 1);
    for (uint32_t f = 0; f < most; f++) {
      FaceRef ref{bodies[k].second, static_cast<int>(f), ""};
      if (const std::string* n = b.elementMap.NameOf(static_cast<int>(f)))
        ref.element = *n;
      out.faces.push_back(std::move(ref));
    }
    std::vector<uint32_t> faces(m->triangles.size(), mesh::kNone);
    for (size_t t = 0; t < kept.size(); t++)
      if (kept[t] < cm.triangleFaces.size())
        faces[t] = base + cm.triangleFaces[kept[t]];
    parts.push_back(std::move(m));
    partFaces.push_back(std::move(faces));
  }
  if (parts.empty())
    return;
  out.body = Joined(parts);
  out.bodyFaces.clear();
  for (const auto& f : partFaces)
    out.bodyFaces.insert(out.bodyFaces.end(), f.begin(), f.end());
}

std::string Gather(const Document& doc, const Request& r, Input& out) {
  out = Input{};
  out.request = r;
  if (r.toleranceMm && !(*r.toleranceMm > 0.0 && std::isfinite(*r.toleranceMm)))
    return "tolerance_mm is a number of mm more than 0, not " +
           Mm(*r.toleranceMm);
  const Feature* scanF = doc.FindFeatureById(r.scanId);
  if (scanF == nullptr)
    return "scan: no feature with id " + std::to_string(r.scanId) +
           " -- get_state lists every body with the feature that made it "
           "(created_by)";
  const Feature* bodyF = doc.FindFeatureById(r.bodyId);
  if (bodyF == nullptr)
    return "body: no feature with id " + std::to_string(r.bodyId) +
           " -- get_state lists every body with the feature that made it "
           "(created_by)";
  if (r.scanId == r.bodyId)
    return "body " + std::to_string(r.bodyId) + " is the scan itself ('" +
           scanF->GetName() +
           "'): compare the scan with ANOTHER body -- the model built on it";
  out.scanName = scanF->GetName();
  out.bodyName = bodyF->GetName();
  const std::vector<Body>& all = doc.GetBodies();
  std::vector<std::shared_ptr<const mesh::TriMesh>> scans;
  std::vector<const Body*> bodies;
  std::vector<int> bodyIndex;
  bool scanMadeAny = false;
  for (size_t i = 0; i < all.size(); i++) {
    const Body& b = all[i];
    if (b.createdBy == r.scanId) {
      scanMadeAny = true;
      if (b.IsMesh() && b.mesh != nullptr && !b.mesh->triangles.empty()) {
        scans.push_back(b.mesh);
        out.scanBodies.push_back(static_cast<int>(i));
      }
    } else if (b.createdBy == r.bodyId) {
      bodies.push_back(&b);
      bodyIndex.push_back(static_cast<int>(i));
      out.bodyBodies.push_back(static_cast<int>(i));
    }
  }
  if (!scanMadeAny)
    return "scan " + std::to_string(r.scanId) + " ('" + out.scanName + "', a " +
           scanF->TypeName() +
           ") made no body: name the Import of the scan (get_state's "
           "created_by)";
  if (scans.empty())
    return "scan " + std::to_string(r.scanId) + " ('" + out.scanName +
           "') is not a mesh body: a scan is compared as the triangles it "
           "is. Import it with representation mesh";
  if (bodies.empty())
    return "body " + std::to_string(r.bodyId) + " ('" + out.bodyName + "', a " +
           bodyF->TypeName() +
           ") made no body to compare the scan with: name the feature that "
           "made the body (get_state's created_by)";
  out.scan = Joined(scans);

  const double tol =
      r.toleranceMm ? *r.toleranceMm : mesh::ScanTolerance(*out.scan);
  std::vector<std::pair<const Body*, int>> pairs;
  for (size_t k = 0; k < bodies.size(); k++)
    pairs.emplace_back(bodies[k], bodyIndex[k]);
  AddBodies(pairs, tol, out);
  if (out.body == nullptr || out.body->triangles.empty())
    return "body " + std::to_string(r.bodyId) + " ('" + out.bodyName +
           "') has no surface to compare the scan with";
  return "";
}
Outcome Run(const Input& in, const std::atomic<bool>* cancel,
            std::atomic<double>* fraction) {
  mesh::DeviationOptions o;
  o.toleranceMm = in.request.toleranceMm.value_or(0.0);
  o.chordMm = in.chordMm;
  o.cancel = cancel;
  if (fraction != nullptr)
    o.progress = [fraction](double f) { fraction->store(f); };
  Outcome out;
  out.deviation = mesh::CompareToBody(in.scan, in.body, in.bodyFaces, o);
  if (!out.deviation.cancelled) {
    out.reply = ReplyJson(in, out.deviation);
    out.receipt = ReceiptOf(in, out.deviation);
  }
  return out;
}

nlohmann::json ReplyJson(const Input& in, const mesh::Deviation& d) {
  nlohmann::json out;
  out["scan"] = {{"feature_id", in.request.scanId},
                 {"name", in.scanName},
                 {"vertices", in.scan->positions.size()}};
  out["body"] = {{"feature_id", in.request.bodyId}, {"name", in.bodyName}};
  out["tolerance_mm"] = d.toleranceMm;
  out["tolerance_from"] = d.toleranceFrom;
  out["noise_mm"] = d.noiseMm;
  out["floor_mm"] = d.chordMm;
  out["within"] = d.within;
  out["mean_mm"] = d.meanMm;
  out["rms_mm"] = d.rmsMm;
  out["p95_mm"] = d.p95Mm;
  out["min_mm"] = d.minMm;
  out["max_mm"] = d.maxMm;
  out["no_scan"] = d.noScan;
  out["unmatched"] = d.unmatched;
  out["opposed"] = d.opposed;
  out["reach_mm"] = d.reachMm;
  size_t features = 0;
  for (const mesh::DeviationHotspot& h : d.hotspots)
    features += h.feature ? 1 : 0;
  out["hotspot_count"] = d.hotspots.size();
  out["feature_count"] = features;
  nlohmann::json spots = nlohmann::json::array();
  for (size_t i = 0; i < d.hotspots.size() && i < kReplyHotspots; i++) {
    const mesh::DeviationHotspot& h = d.hotspots[i];
    nlohmann::json j{
        {"at_mm", Json(h.atMm)},  {"depth_mm", h.depthMm},
        {"area_mm2", h.areaMm2},  {"kind", h.feature ? "feature" : "texture"},
        {"vertices", h.vertices}, {"box_mm", {Json(h.lo), Json(h.hi)}}};
    if (h.face != mesh::kNone && h.face < in.faces.size()) {
      const FaceRef& f = in.faces[h.face];
      j["body_index"] = f.body;
      if (f.face >= 0) {
        j["face"] = f.face;
        if (!f.element.empty())
          j["element"] = {{"feature_id", in.request.bodyId},
                          {"name", f.element}};
      }
    }
    spots.push_back(std::move(j));
  }
  out["hotspots"] = std::move(spots);
  return out;
}

receipt::Receipt ReceiptOf(const Input& in, const mesh::Deviation& d) {
  receipt::Receipt r;
  const receipt::Oracle oracle{"designer/scan-deviation", "1"};
  size_t features = 0;
  for (const mesh::DeviationHotspot& h : d.hotspots)
    features += h.feature ? 1 : 0;
  receipt::Claim c(
      "no patch of '" + in.scanName + "' stands past " + Mm(d.toleranceMm) +
          " mm of '" + in.bodyName + "' over more than " +
          Mm(mesh::kHotspotFeatureAreaMm2) + " mm2",
      oracle, features == 0 ? receipt::Verdict::Pass : receipt::Verdict::Fail);
  c.value = static_cast<double>(features);
  c.note = std::to_string(features) + " feature hotspot(s), " +
           std::to_string(d.hotspots.size() - features) + " texture";
  r.Add(std::move(c));
  // What the scan never saw was not checked.
  constexpr double kSeen = 0.02;
  if (d.noScan > kSeen) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * d.noScan);
    r.Unverifiable("'" + in.bodyName + "' where no scan lies under it", oracle,
                   std::string(buf) +
                       " of the body's area has no scan facing its way "
                       "under it: nothing there was compared");
  } else {
    receipt::Claim seen("the scan covers '" + in.bodyName + "'", oracle,
                        receipt::Verdict::Pass);
    seen.value = 1.0 - d.noScan;
    r.Add(std::move(seen));
  }
  return r;
}

} // namespace scancompare
