// What the verdict says about the part beyond the cut surface: the
// toleranced walls judged against their bands, and the holes and threads the
// job does not cut (docs/cam/simulation.md#tolerance-bands,
// docs/cam/simulation.md#what-the-job-does-not-cut).
#include "core/cam/CamOtherSetups.h"
#include "core/cam/CamToolpath.h"
#include "core/cam/CamVerify.h"

#include <BRepAdaptor_Surface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace cam {

namespace {

TopoDS_Face FaceOf(const Body& body, int idx) {
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == idx)
      return TopoDS::Face(ex.Current());
  return {};
}

// Three points down a cylindrical face's axis, a quarter, a half and three
// quarters of the way along it, in the body's own space. False when the face
// is not a cylinder.
bool AxisPoints(const Body& body, const ElementRef& face,
                std::array<glm::dvec3, 3>& out, glm::dvec3& dir) {
  const std::optional<int> idx = body.elementMap.Resolve(face.name);
  if (!idx || !body.shape.IsValid())
    return false;
  const TopoDS_Face f = FaceOf(body, *idx);
  if (f.IsNull())
    return false;
  try {
    const BRepAdaptor_Surface s(f);
    if (s.GetType() != GeomAbs_Cylinder)
      return false;
    const gp_Cylinder c = s.Cylinder();
    double v0 = s.FirstVParameter(), v1 = s.LastVParameter();
    if (v1 < v0)
      std::swap(v0, v1);
    const gp_Pnt o = c.Location();
    const gp_Dir d = c.Axis().Direction();
    dir = {d.X(), d.Y(), d.Z()};
    for (int k = 0; k < 3; k++) {
      const double v = v0 + (v1 - v0) * (k + 1) * 0.25;
      out[static_cast<size_t>(k)] = glm::dvec3(o.X(), o.Y(), o.Z()) + dir * v;
    }
    return true;
  } catch (const Standard_Failure&) {
    return false;
  }
}

std::string Mm(double v, int decimals = 1) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

} // namespace

std::vector<FitVerdict> JudgeFits(const GProgram& program, const Setup& setup,
                                  const std::vector<Toolpath::Fit>& fits) {
  std::vector<FitVerdict> out;
  for (const Toolpath::Fit& f : fits) {
    FitVerdict* same = nullptr;
    for (FitVerdict& v : out)
      if (glm::distance(v.centre, f.centre) < 1e-3 &&
          std::abs(v.nominal - f.nominal) < 1e-4 && v.internal == f.internal)
        same = &v;
    if (same == nullptr) {
      FitVerdict v;
      v.centre = f.centre;
      v.internal = f.internal;
      v.nominal = f.nominal;
      v.min = f.Min();
      v.max = f.Max();
      v.aim = f.aim;
      v.label = f.label;
      out.push_back(v);
      same = &out.back();
    }
    if (std::find(same->ops.begin(), same->ops.end(), f.op) == same->ops.end())
      same->ops.push_back(f.op);
  }
  // The span each wall is judged over, from its declarations.
  for (FitVerdict& v : out) {
    double zLo = std::numeric_limits<double>::max(), zHi = -zLo;
    for (const Toolpath::Fit& f : fits)
      if (std::find(v.ops.begin(), v.ops.end(), f.op) != v.ops.end() &&
          glm::distance(v.centre, f.centre) < 1e-3) {
        zLo = std::min(zLo, f.zLo);
        zHi = std::max(zHi, f.zHi);
      }
    // The rim the file reached: a cut's endpoints lie ON its arc, so the
    // widest (narrowest round a boss) of them is the wall it left.
    double reach = v.internal ? -1.0 : std::numeric_limits<double>::max();
    bool any = false;
    for (const GMove& m : program.moves) {
      if (m.rapid ||
          std::find(v.ops.begin(), v.ops.end(), m.op) == v.ops.end() ||
          m.to.z > zHi + 1e-3 || m.to.z < zLo - 1e-3)
        continue;
      const Tool* tool = setup.FindTool(m.tool);
      if (tool == nullptr)
        continue;
      const double d = glm::distance(glm::dvec2(m.to), v.centre);
      const double r = tool->diameter * 0.5;
      any = true;
      reach = v.internal ? std::max(reach, d + r) : std::min(reach, d - r);
    }
    if (!any) {
      v.why = "no cutting move of its operations reaches the wall";
      continue;
    }
    v.judged = true;
    v.cut = 2.0 * reach;
  }
  return out;
}

void FindUncutHoles(const Setup& setup, const std::vector<Body>& rawBodies,
                    const HeightMap& machined, const OtherSetups* others,
                    Verdict& v) {
  const double tol = std::max(0.01, machined.res * 0.1);
  for (const Body& body : rawBodies) {
    if (body.holes.empty())
      continue;
    bool disabled = false;
    for (const Placement& p : setup.placements)
      disabled = disabled || (p.jobPartId == body.jobPartId && !p.enabled);
    if (disabled)
      continue;
    const glm::dmat4 toSetup = BodyToSetup(setup, body.jobPartId);
    for (const HoleMark& h : body.holes) {
      std::array<glm::dvec3, 3> pts{};
      glm::dvec3 dir(0.0);
      if (!AxisPoints(body, h.bore, pts, dir))
        continue; // a bore a later feature split or removed: nothing to ask
      // Here: the bore stands up in this setup and the map is air at each
      // point of its axis.
      const glm::dvec3 up(toSetup * glm::dvec4(dir, 0.0));
      bool here = std::abs(up.z) > 0.99;
      for (const glm::dvec3& p : pts) {
        if (!here)
          break;
        const glm::dvec3 q(toSetup * glm::dvec4(p, 1.0));
        const int i =
            static_cast<int>(std::floor((q.x - machined.x0) / machined.res));
        const int j =
            static_cast<int>(std::floor((q.y - machined.y0) / machined.res));
        here = i >= 0 && j >= 0 && i < machined.nx && j < machined.ny &&
               machined.At(i, j) < q.z - tol;
      }
      // Or in another setup of the job.
      bool elsewhere = false;
      if (!here && others != nullptr)
        for (const SetupStock& s : others->stocks) {
          bool all = true;
          for (const glm::dvec3& p : pts)
            all = all && s.Removes(p, tol);
          elsewhere = elsewhere || all;
        }
      if (here || elsewhere)
        continue;
      UncutFeature u;
      u.kind = UncutFeature::Kind::Hole;
      u.jobPartId = body.jobPartId;
      u.featureId = h.featureId;
      u.feature = h.feature;
      u.face = h.bore;
      u.what = (h.thread.empty() ? "hole" : h.thread + " tap-drill hole") +
               std::string(" \xC3\x98") + Mm(h.diameter, 2) +
               (h.through ? " through" : " deep " + Mm(h.depth));
      u.at = pts[1];
      v.uncut.push_back(std::move(u));
    }
  }
}

void AddUncutThreads(const std::vector<Setup>& setups,
                     const std::vector<Body>& rawBodies,
                     const std::vector<uint64_t>& milled, Verdict& v) {
  for (const Body& body : rawBodies) {
    // One thread per face: a Thread on a tapped Hole's bore comes after the
    // Hole's own mark, and is the one a thread mill reads.
    std::vector<const ThreadMark*> marks;
    for (const ThreadMark& m : body.threads) {
      const std::optional<int> at = body.elementMap.Resolve(m.face.name);
      bool replaced = false;
      for (const ThreadMark*& have : marks)
        if (have->face.featureId == m.face.featureId &&
            (have->face.name == m.face.name ||
             (at && body.elementMap.Resolve(have->face.name) == at))) {
          have = &m;
          replaced = true;
        }
      if (!replaced)
        marks.push_back(&m);
    }
    for (const ThreadMark* m : marks) {
      const std::optional<int> face = body.elementMap.Resolve(m->face.name);
      bool cut = false;
      for (const Setup& s : setups) {
        bool disabled = false;
        for (const Placement& p : s.placements)
          disabled = disabled || (p.jobPartId == body.jobPartId && !p.enabled);
        if (disabled)
          continue;
        for (const Operation& op : s.operations) {
          if (!op.enabled || op.type != OpType::ThreadMill ||
              std::find(milled.begin(), milled.end(), op.id) == milled.end())
            continue;
          for (const Target& t : op.targets)
            cut = cut ||
                  ((t.jobPartId == 0 || t.jobPartId == body.jobPartId) &&
                   t.ref.featureId == m->face.featureId &&
                   (t.ref.name == m->face.name ||
                    (face && body.elementMap.Resolve(t.ref.name) == face)));
        }
      }
      if (cut)
        continue;
      UncutFeature u;
      u.kind = UncutFeature::Kind::Thread;
      u.jobPartId = body.jobPartId;
      u.featureId = m->featureId;
      u.feature = m->designation;
      u.face = m->face;
      u.what = m->designation +
               (m->threadClass.empty() ? "" : "-" + m->threadClass) +
               (m->external ? " external" : "") + " thread" +
               (m->length > 1e-9 ? " " + Mm(m->length) + " long" : "");
      std::array<glm::dvec3, 3> pts{};
      glm::dvec3 dir(0.0);
      if (AxisPoints(body, m->face, pts, dir))
        u.at = pts[1];
      v.uncut.push_back(std::move(u));
    }
  }
}

} // namespace cam
