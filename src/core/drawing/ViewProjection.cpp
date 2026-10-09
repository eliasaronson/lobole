// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/ViewProjection.h"

#include "cad/CADKernel.h"
#include "core/InputHash.h"

#include <Standard_Version.hxx>

#include <algorithm>
#include <cmath>

namespace drawing {

bool ProjectionInputs::SameAs(const ProjectionInputs& o) const {
  if (viewDir != o.viewDir || mode != o.mode || hiddenLines != o.hiddenLines ||
      deflection != o.deflection || section != o.section ||
      (section && sectionPoint != o.sectionPoint) ||
      bodies.size() != o.bodies.size())
    return false;
  for (size_t i = 0; i < bodies.size(); ++i) {
    const InputBody& a = bodies[i];
    const InputBody& b = o.bodies[i];
    if (!a.shape.IsEqual(b.shape) || a.transform != b.transform ||
        a.geometryRevision != b.geometryRevision || a.groupId != b.groupId)
      return false;
  }
  return true;
}

std::shared_ptr<const SectionCut> CutCache::Get(const TopoDS_Shape& shape,
                                                const glm::dvec3& point,
                                                const glm::dvec3& normal,
                                                std::string* error) {
  ++m_clock;
  for (Entry& e : m_entries)
    if (e.source.IsEqual(shape) && e.point == point && e.normal == normal) {
      e.used = m_clock;
      return e.cut;
    }
  auto cut = CutBody(shape, point, normal, error);
  if (!cut)
    return nullptr;
  // Every body of every section on the sheets in hand: a head's two
  // assembly sections are 120 cuts, and a cache of 64 re-cut all of them
  // (and re-projected the new shapes) on every update and export.
  constexpr size_t kMaxCuts = 1024;
  if (m_entries.size() >= kMaxCuts)
    m_entries.erase(std::min_element(
        m_entries.begin(), m_entries.end(),
        [](const Entry& a, const Entry& b) { return a.used < b.used; }));
  Entry e;
  e.source = shape;
  e.point = point;
  e.normal = normal;
  e.cut = std::make_shared<const SectionCut>(std::move(*cut));
  e.used = m_clock;
  m_entries.push_back(e);
  return m_entries.back().cut;
}

uint64_t ProjectionInputHash(const ProjectionInputs& in) {
  inputhash::Hasher h;
  h.Str("designer drawing projection");
  h.U64(static_cast<uint64_t>(kProjectionRevision));
#ifdef OCC_VERSION_COMPLETE
  h.Str(OCC_VERSION_COMPLETE);
#endif
  h.U64(in.bodies.size());
  for (const InputBody& b : in.bodies) {
    h.Shape(CADShape(b.shape));
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r)
        h.F64(b.transform[c][r]);
    h.U64(b.groupId);
  }
  for (int i = 0; i < 3; ++i)
    h.F64(in.viewDir[i]);
  h.U64(static_cast<uint64_t>(in.mode));
  h.U64(in.hiddenLines ? 1 : 0);
  h.F64(in.deflection);
  h.U64(in.section ? 1 : 0);
  if (in.section)
    for (int i = 0; i < 3; ++i)
      h.F64(in.sectionPoint[i]);
  return h.Digest();
}

std::optional<ViewProjection> ProjectInputs(const ProjectionInputs& in,
                                            const ProjectRun& run,
                                            std::string* error) {
  ViewProjection out;
  out.viewDir = in.viewDir;
  out.up = in.up;
  out.sectioned = in.section;
  std::vector<InputBody> bodies;
  if (!in.section) {
    bodies = in.bodies;
  } else {
    // Each body replaced by what the plane leaves of it, in its own frame; a
    // body wholly in front of the plane is dropped, and the curves then
    // count the bodies that are left (Cut::body says which each one was).
    for (size_t i = 0; i < in.bodies.size(); ++i) {
      if (run.cancelled && run.cancelled()) {
        if (error)
          *error = "cancelled";
        return std::nullopt;
      }
      const InputBody& b = in.bodies[i];
      const glm::dmat4 inv = glm::inverse(b.transform);
      const glm::dvec3 p(inv * glm::dvec4(in.sectionPoint, 1.0));
      const glm::dvec3 n =
          glm::normalize(glm::dvec3(inv * glm::dvec4(in.viewDir, 0.0)));
      std::shared_ptr<const SectionCut> cut;
      if (run.cuts) {
        cut = run.cuts->Get(b.shape, p, n, error);
      } else if (auto made = CutBody(b.shape, p, n, error)) {
        cut = std::make_shared<const SectionCut>(std::move(*made));
      }
      if (!cut)
        return std::nullopt;
      if (cut->shape.IsNull() || cut->edgeOrigin.empty())
        continue; // all of it was in front of the plane
      InputBody kept = b;
      kept.shape = cut->shape;
      kept.sourceId = bodies.size();
      bodies.push_back(std::move(kept));
      ViewProjection::Cut c;
      c.body = static_cast<int>(i);
      c.edgeOrigin = cut->edgeOrigin;
      c.edgeFromFace = cut->edgeFromFace;
      c.faceOrigin = cut->faceOrigin;
      c.cutLoops = cut->cutLoops;
      out.cuts.push_back(std::move(c));
    }
    if (bodies.empty()) {
      if (error)
        *error = "the section plane leaves nothing of the source behind "
                 "it -- move its point, or look the other way";
      return std::nullopt;
    }
  }
  ProjectionOptions opts;
  opts.cache = run.cache;
  opts.mode = in.mode;
  opts.hiddenLines = in.hiddenLines;
  opts.deflection = in.deflection;
  opts.cancelled = run.cancelled;
  opts.progress = run.progress;
  auto projected = ProjectView(bodies, in.viewDir, in.up, opts, error);
  if (!projected)
    return std::nullopt;
  out.result = std::move(*projected);
  return out;
}

ProjectionResult TurnedTo(const ViewProjection& vp, const glm::dvec3& up) {
  ProjectionResult r = vp.result;
  // ProjectView's frame: X = normalize(dir x up), Y = (-dir) x X.
  const glm::dvec3 d = glm::normalize(vp.viewDir);
  const glm::dvec3 x0 = glm::normalize(glm::cross(d, vp.up));
  const glm::dvec3 y0 = glm::cross(-d, x0);
  const glm::dvec3 x1 = glm::normalize(glm::cross(d, up));
  const glm::dvec3 y1 = glm::cross(-d, x1);
  if (glm::length(x0 - x1) < 1e-15 && glm::length(y0 - y1) < 1e-15)
    return r;
  // A point (a, b) in the old frame is a*x0 + b*y0 in the plane.
  const double xx = glm::dot(x0, x1), yx = glm::dot(y0, x1);
  const double xy = glm::dot(x0, y1), yy = glm::dot(y0, y1);
  const auto turn = [&](const glm::dvec2& p) {
    return glm::dvec2(p.x * xx + p.y * yx, p.x * xy + p.y * yy);
  };
  for (ProjectedCurve& c : r.curves) {
    for (glm::dvec2& p : c.points)
      p = turn(p);
    if (c.conicRadius > 0.0)
      c.conicCenter = turn(c.conicCenter);
  }
  return r;
}

uint64_t ProjectionDigest(const ProjectionResult& r) {
  // Each curve on its own, then the set of them sorted: the guard is on what
  // ProjectView returns, not on the order OCC's data structure lists edges
  // in. Coordinates rounded to a micrometre, so a different libm's last bit
  // is not a different projection.
  std::vector<uint64_t> curves;
  curves.reserve(r.curves.size());
  for (const ProjectedCurve& c : r.curves) {
    inputhash::Hasher h;
    const auto q = [&](double v) {
      h.U64(static_cast<uint64_t>(std::llround(v * 1000.0)));
    };
    h.U64(static_cast<uint64_t>(c.kind));
    h.U64(c.visible ? 1 : 0);
    h.U64(static_cast<uint64_t>(c.curveType + 1));
    h.U64(c.closed ? 1 : 0);
    h.U64(c.source.sourceId);
    h.U64(static_cast<uint64_t>(c.source.edgeIndex + 1));
    h.U64(static_cast<uint64_t>(c.source.faceIndex + 1));
    h.U64(c.points.size());
    for (const glm::dvec2& p : c.points) {
      q(p.x);
      q(p.y);
    }
    if (c.conicRadius > 0.0) {
      q(c.conicCenter.x);
      q(c.conicCenter.y);
      q(c.conicRadius);
    }
    curves.push_back(h.Digest());
  }
  std::sort(curves.begin(), curves.end());
  inputhash::Hasher all;
  all.U64(curves.size());
  for (uint64_t c : curves)
    all.U64(c);
  return all.Digest();
}

} // namespace drawing
