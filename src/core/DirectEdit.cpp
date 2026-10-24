// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/DirectEdit.h"

#include "cad/OccSignals.h"
#include "core/ElementHistory.h"
#include "core/ElementOrigin.h"
#include "core/Measure.h"

#include <BRepBuilderAPI_Copy.hxx>
#include <BRepTools.hxx>
#include <BRepTools_History.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>

namespace directedit {

namespace {

std::string Num(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.4g", v);
  return buf;
}

std::string Point(const glm::dvec3& p) {
  return "(" + Num(p.x) + ", " + Num(p.y) + ", " + Num(p.z) + ")";
}

std::string Adjective(const std::string& kind) {
  if (kind == "plane")
    return "planar";
  if (kind == "cylinder")
    return "cylindrical";
  if (kind == "cone")
    return "conical";
  if (kind == "sphere")
    return "spherical";
  if (kind == "torus")
    return "toroidal";
  return "freeform";
}

std::string Fingerprint(const FaceFingerprint& f) {
  return "a " + f.surface + " of " + Num(f.areaMm2) + " mm² at " +
         Point(f.centroid);
}

} // namespace

bool IsFilePositional(const RegenContext& ctx, const std::string& name) {
  const topo::ElementOrigin o = topo::DecodeElementName(name);
  if (!o.ok || o.role != topo::ElementRole::Positional || !o.derived.empty())
    return false;
  const Feature* seed = ctx.FindById(o.seedTag);
  return seed != nullptr && seed->TypeName() == "Import";
}

std::optional<FaceFingerprint> FingerprintAt(const Body& body, int faceIndex) {
  const auto info = Measure::DescribeFaceAt(body, faceIndex);
  if (!info)
    return std::nullopt;
  return FaceFingerprint{info->kind, info->area, info->centroid};
}

std::string DescribeFace(const Body& body, int faceIndex,
                         const std::string& name) {
  const auto info = Measure::DescribeFaceAt(body, faceIndex);
  if (!info)
    return "face `" + name + "`";
  return Adjective(info->kind) + " face `" + name + "`, " + Num(info->area) +
         " mm²";
}

std::string NameList(const std::vector<std::string>& names) {
  // Six named, the rest counted: a hundred bores is not a sentence.
  constexpr size_t kListed = 6;
  const size_t shown = std::min(names.size(), kListed);
  std::string out;
  for (size_t i = 0; i < shown; i++) {
    if (i > 0)
      out += i + 1 == shown && shown == names.size() ? " and " : ", ";
    out += "`" + names[i] + "`";
  }
  if (names.size() > shown)
    out += " and " + std::to_string(names.size() - shown) + " more";
  return out;
}

const Body* BodyHolding(const std::vector<Body>& bodies, uint64_t createdBy,
                        const std::string& name) {
  const Body* first = nullptr;
  for (const Body& b : bodies) {
    if (b.createdBy != createdBy)
      continue;
    if (first == nullptr)
      first = &b;
    if (!name.empty() && b.elementMap.Resolve(name))
      return &b;
  }
  return first;
}

std::string ResolvePicked(RegenContext& ctx, uint64_t bodyId,
                          const std::vector<ElementRef>& faces,
                          Fingerprints& prints, const std::string& what,
                          Picked& out, const std::vector<std::string>* keep) {
  out = {};
  const Body* holding =
      BodyHolding(ctx.bodies, bodyId, faces.empty() ? "" : faces.front().name);
  const auto body = holding == nullptr
                        ? ctx.bodies.end()
                        : ctx.bodies.begin() + (holding - ctx.bodies.data());
  if (bodyId == 0 || body == ctx.bodies.end())
    return "the body to edit (created by feature " + std::to_string(bodyId) +
           ") no longer exists";
  if (faces.empty())
    return "pick the faces to " + what;
  for (const ElementRef& ref : faces)
    if (ref.featureId != bodyId)
      return "every face must belong to the edited body (feature " +
             std::to_string(bodyId) + "); `" + ref.name +
             "` is on the body of feature " + std::to_string(ref.featureId);
  // Drop the prints of faces no longer picked.
  for (auto it = prints.begin(); it != prints.end();) {
    const bool picked =
        keep != nullptr
            ? std::find(keep->begin(), keep->end(), it->first) != keep->end()
            : std::any_of(faces.begin(), faces.end(), [&](const ElementRef& r) {
                return r.name == it->first;
              });
    it = picked ? std::next(it) : prints.erase(it);
  }
  glm::dvec3 lo, hi;
  body->shape.GetBoundingBox(lo, hi);
  const double scale = std::max(1.0, glm::length(hi - lo));
  for (const ElementRef& ref : faces) {
    const auto idx = body->elementMap.Resolve(ref.name);
    if (!idx)
      return topo::UnresolvedReason(ctx, ref, topo::ElementKind::Face,
                                    "face to " + what);
    if (std::find(out.indices.begin(), out.indices.end(), *idx) !=
        out.indices.end())
      return "face `" + ref.name + "` is picked twice";
    if (IsFilePositional(ctx, ref.name)) {
      const auto now = FingerprintAt(*body, *idx);
      if (!now)
        return "face `" + ref.name + "` has no area to measure";
      const auto was = prints.find(ref.name);
      if (was == prints.end()) {
        prints[ref.name] = *now;
      } else {
        const FaceFingerprint& w = was->second;
        const bool same =
            w.surface == now->surface &&
            std::abs(w.areaMm2 - now->areaMm2) <=
                1e-6 * std::max(1.0, std::abs(w.areaMm2)) &&
            glm::length(w.centroid - now->centroid) <= 1e-6 * scale;
        if (!same)
          return "face `" + ref.name +
                 "` is no longer the face that was picked (was " +
                 Fingerprint(w) + ", now " + Fingerprint(*now) +
                 "); re-pick it";
      }
    }
    out.indices.push_back(*idx);
    out.names.push_back(ref.name);
  }
  out.body = &*body;
  return {};
}

Copied CopyFor(const Body& body, const std::vector<int>& indices) {
  const TopoDS_Shape& shape = body.shape.GetShape();
  BRepBuilderAPI_Copy copy(shape, /*copyGeom=*/false, /*copyMesh=*/false);
  Copied out;
  out.shape = copy.Shape();
  // In the order given, so a builder's report reads back in that order.
  std::vector<TopoDS_Shape> byIndex;
  for (TopExp_Explorer ex(out.shape, TopAbs_FACE); ex.More(); ex.Next())
    byIndex.push_back(ex.Current());
  for (const int i : indices)
    if (i >= 0 && i < static_cast<int>(byIndex.size()))
      out.faces.Append(byIndex[i]);
  return out;
}

std::string CheckResult(const Body& before, const Picked& picked,
                        const CADShape& result, const ElementMap& map,
                        uint64_t tag, const Expect& expect,
                        const std::string& op) {
  if (!result.IsValid() || result.GetShape().IsNull())
    return op + " of " + NameList(picked.names) + " produced no shape";
  if (const std::string bad = Measure::InvalidReasons(result); !bad.empty())
    return op + " of " + NameList(picked.names) +
           " produced an invalid solid (" + bad + ")";
  const int solidsBefore = before.shape.SolidCount();
  const int solidsAfter = result.SolidCount();
  if (solidsAfter != solidsBefore)
    return op + " of " + NameList(picked.names) + " left " +
           std::to_string(solidsAfter) + " solid" +
           (solidsAfter == 1 ? "" : "s") + " where there " +
           (solidsBefore == 1 ? "was " : "were ") +
           std::to_string(solidsBefore);

  const std::string mTag = ";M" + std::to_string(tag);
  for (size_t i = 0; i < picked.names.size(); i++) {
    const std::string& name = picked.names[i];
    bool kept = false, modified = false;
    for (const auto& [idx, n] : map.names) {
      (void)idx;
      if (n == name)
        kept = true;
      else if (n.compare(0, name.size() + mTag.size(), name + mTag) == 0)
        modified = true;
    }
    const std::string face = DescribeFace(before, picked.indices[i], name);
    if (expect.faces == Expect::Faces::Gone && (kept || modified))
      return op + " left " + face + " in place";
    if (expect.faces == Expect::Faces::Modified && !modified)
      return op + " did not change " + face;
  }

  const double v0 = before.shape.Volume();
  const double v1 = result.Volume();
  const double tol = expect.volumeTolerance * std::max(1.0, std::abs(v0));
  const double dv = v1 - v0;
  const std::string faces = NameList(picked.names);
  switch (expect.volume) {
  case Expect::Volume::Any:
    break;
  case Expect::Volume::Changes:
    if (std::abs(dv) <= tol)
      return op + " had no effect on " + faces + ": the volume is unchanged";
    break;
  case Expect::Volume::Grows:
    if (dv <= tol)
      return op + " of " + faces + " removed " + Num(-dv) +
             " mm³ where it should only add material";
    break;
  case Expect::Volume::Shrinks:
    if (dv >= -tol)
      return op + " of " + faces + " added " + Num(dv) +
             " mm³ where it should only remove material";
    break;
  case Expect::Volume::Same:
    if (std::abs(dv) > tol)
      return op + " of " + faces + " changed the volume by " + Num(dv) +
             " mm³; it should move material, not add or remove it";
    break;
  }

  glm::dvec3 lo0, hi0, lo1, hi1;
  before.shape.GetBoundingBox(lo0, hi0, /*tight=*/true);
  result.GetBoundingBox(lo1, hi1, /*tight=*/true);
  const double slack = 1e-6 * glm::length(hi0 - lo0) + 1e-6;
  for (int a = 0; a < 3; a++) {
    const double past = std::max(lo0[a] - lo1[a], hi1[a] - hi0[a]);
    if (past > expect.bboxGrowth + slack)
      return op + " of " + faces + " grew the body " + Num(past) +
             " mm past its own outside along " + std::string(1, "xyz"[a]) +
             (expect.bboxGrowth > 0.0
                  ? " (it may grow " + Num(expect.bboxGrowth) + " mm)"
                  : "") +
             ": OpenCASCADE built the wrong solid";
  }
  return {};
}

ElementMap NameThrough(const TopoDS_Shape& input, const ElementMap& map,
                       const BRepTools_History& history,
                       const TopoDS_Shape& result, uint64_t tag) {
  return topo::NamesThroughHistory(CADShape(input), map, history,
                                   CADShape(result), tag,
                                   /*keepWholeFaces=*/false);
}

void RestoreUntouched(const Body& before, const CADShape& result,
                      ElementMap& map, uint64_t tag,
                      const std::vector<std::string>& picked) {
  const std::string step = ";M" + std::to_string(tag);
  Body after;
  after.shape = result;
  glm::dvec3 lo, hi;
  before.shape.GetBoundingBox(lo, hi);
  const double scale = std::max(1.0, glm::length(hi - lo));
  std::set<std::string> taken;
  for (const auto& [i, n] : map.names)
    taken.insert(n);
  for (auto& [idx, name] : map.names) {
    if (name.size() <= step.size() ||
        name.compare(name.size() - step.size(), step.size(), step) != 0)
      continue;
    const std::string was = name.substr(0, name.size() - step.size());
    if (taken.count(was) ||
        std::find(picked.begin(), picked.end(), was) != picked.end())
      continue;
    const auto at = before.elementMap.Resolve(was);
    if (!at)
      continue;
    const auto a = Measure::DescribeFaceAt(before, *at);
    const auto b = Measure::DescribeFaceAt(after, idx);
    if (!a || !b || a->kind != b->kind ||
        std::abs(a->area - b->area) > 1e-9 * std::max(1.0, a->area) ||
        glm::length(a->centroid - b->centroid) > 1e-9 * scale)
      continue;
    taken.erase(name);
    name = was;
    taken.insert(name);
  }
}

std::string CollapseSteps(const std::string& name, uint64_t tag) {
  const std::string step = ";M" + std::to_string(tag);
  const std::string twice = step + step;
  std::string out = name;
  for (size_t at = out.find(twice); at != std::string::npos;
       at = out.find(twice, at)) {
    const size_t after = at + twice.size();
    // Only at a step boundary: ";M4;M42" is two different steps.
    const bool boundary = after == out.size() || out[after] == ';' ||
                          out[after] == '[' || out[after] == '.' ||
                          out[after] == ',' || out[after] == '#';
    if (boundary)
      out.erase(at, step.size());
    else
      at += step.size();
  }
  return out;
}

std::string Guarded(const std::string& op, const std::function<void()>& f) {
  try {
    std::function<void()> run = f;
    if (const auto why = occsig::RunCatchingOccSignals(run))
      return op + " failed inside OpenCASCADE (" + *why + ")";
  } catch (const Standard_Failure& e) {
    const char* said = e.GetMessageString();
    return op + " failed: OpenCASCADE threw " + e.DynamicType()->Name() +
           (said != nullptr && *said ? std::string(" (") + said + ")" : "");
  }
  return {};
}

std::optional<std::vector<int>> FeatureFaces(const Body& body,
                                             const std::vector<int>& seeds,
                                             std::string* why) {
  const auto fail = [&](const std::string& w) {
    if (why)
      *why = w;
    return std::nullopt;
  };
  const TopoDS_Shape& shape = body.shape.GetShape();
  if (shape.IsNull() || seeds.empty())
    return fail("pick a face of the feature first");
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, parents);
  // The edges of each face's inner loops.
  const auto onInnerLoop = [&](const TopoDS_Face& f, const TopoDS_Shape& e) {
    const TopoDS_Wire outer = BRepTools::OuterWire(f);
    for (TopExp_Explorer w(f, TopAbs_WIRE); w.More(); w.Next()) {
      if (w.Current().IsSame(outer))
        continue;
      for (TopExp_Explorer x(w.Current(), TopAbs_EDGE); x.More(); x.Next())
        if (x.Current().IsSame(e))
          return true;
    }
    return false;
  };
  std::set<int> in;
  std::vector<int> queue;
  for (const int s : seeds)
    if (s >= 0 && s < faces.Extent() && in.insert(s).second)
      queue.push_back(s);
  bool bounded = false;
  while (!queue.empty()) {
    const int a = queue.back();
    queue.pop_back();
    const TopoDS_Face& fa = TopoDS::Face(faces(a + 1));
    for (TopExp_Explorer ex(fa, TopAbs_EDGE); ex.More(); ex.Next()) {
      const int at = parents.FindIndex(ex.Current());
      if (at == 0)
        continue;
      for (const TopoDS_Shape& other : parents(at)) {
        const int n = faces.FindIndex(other) - 1;
        if (n < 0 || n == a || in.count(n))
          continue;
        if (onInnerLoop(TopoDS::Face(other), ex.Current())) {
          bounded = true; // a host: the feature stops at its loop
          continue;
        }
        in.insert(n);
        queue.push_back(n);
      }
    }
  }
  if (static_cast<int>(in.size()) >= faces.Extent())
    return fail("the faces run on into the rest of the body: no loop of "
                "edges in a host face bounds them");
  if (!bounded)
    return fail("no loop of edges in a host face bounds these faces (a "
                "fillet or a chamfer is its own feature: pick its faces)");
  return std::vector<int>(in.begin(), in.end());
}

std::optional<glm::dvec3> HostNormal(const Body& body,
                                     const std::vector<int>& faces) {
  const TopoDS_Shape& shape = body.shape.GetShape();
  if (shape.IsNull())
    return std::nullopt;
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(shape, TopAbs_FACE, all);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, parents);
  std::optional<glm::dvec3> best;
  double bestArea = 0.0;
  for (const int f : faces) {
    if (f < 0 || f >= all.Extent())
      continue;
    for (TopExp_Explorer ex(all(f + 1), TopAbs_EDGE); ex.More(); ex.Next()) {
      const int at = parents.FindIndex(ex.Current());
      if (at == 0)
        continue;
      for (const TopoDS_Shape& other : parents(at)) {
        const int n = all.FindIndex(other) - 1;
        if (n < 0 || std::find(faces.begin(), faces.end(), n) != faces.end())
          continue;
        const auto info = Measure::DescribeFaceAt(body, n);
        if (info && info->kind == "plane" && info->area > bestArea) {
          bestArea = info->area;
          best = glm::normalize(info->normal);
        }
      }
    }
  }
  return best;
}

std::string FreeformNeighbour(const Picked& picked, const std::string& op,
                              const std::string& does) {
  const Body& body = *picked.body;
  TopTools_IndexedMapOfShape all;
  TopExp::MapShapes(body.shape.GetShape(), TopAbs_FACE, all);
  TopTools_IndexedDataMapOfShapeListOfShape parents;
  TopExp::MapShapesAndAncestors(body.shape.GetShape(), TopAbs_EDGE, TopAbs_FACE,
                                parents);
  for (size_t k = 0; k < picked.indices.size(); k++) {
    for (TopExp_Explorer ex(all(picked.indices[k] + 1), TopAbs_EDGE); ex.More();
         ex.Next()) {
      const int at = parents.FindIndex(ex.Current());
      if (at == 0)
        continue;
      for (const TopoDS_Shape& g : parents(at)) {
        const int n = all.FindIndex(g) - 1;
        if (n < 0 || std::find(picked.indices.begin(), picked.indices.end(),
                               n) != picked.indices.end())
          continue;
        const auto info = Measure::DescribeFaceAt(body, n);
        if (info && info->kind == "other") {
          const std::string* name = body.elementMap.NameOf(n);
          return op + " " + does + " the faces around " +
                 NameList({picked.names[k]}) + ", and " +
                 DescribeFace(body, n, name ? *name : "face") +
                 " beside it is freeform (a thread's flank, a spline), "
                 "which it does not extend; pick it with them, or edit the "
                 "feature that made them";
        }
      }
    }
  }
  return {};
}

namespace {

class Deadline : public Message_ProgressIndicator {
public:
  explicit Deadline(double seconds)
      : m_end(std::chrono::steady_clock::now() +
              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                  std::chrono::duration<double>(seconds))) {}
  Standard_Boolean UserBreak() override {
    if (!m_expired && std::chrono::steady_clock::now() > m_end)
      m_expired = true;
    return m_expired;
  }
  void Show(const Message_ProgressScope&, const Standard_Boolean) override {}
  bool expired() const { return m_expired; }

private:
  std::chrono::steady_clock::time_point m_end;
  bool m_expired = false;
};

} // namespace

Handle(Message_ProgressIndicator) MakeDeadline(double seconds) {
  return new Deadline(seconds);
}

bool Expired(const Handle(Message_ProgressIndicator) & deadline) {
  const Handle(Deadline) d = Handle(Deadline)::DownCast(deadline);
  return !d.IsNull() && d->expired();
}

double ThinnestExtent(const Body& body) {
  glm::dvec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi, /*tight=*/true);
  const glm::dvec3 e = hi - lo;
  return std::min({e.x, e.y, e.z});
}

} // namespace directedit
