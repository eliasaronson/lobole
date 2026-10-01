#include "core/PrintSplit.h"

#include <BRepAlgoAPI_Common.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace printsplit {

namespace {

bool Bounds(const TopoDS_Shape& s, glm::dvec3& lo, glm::dvec3& hi) {
  Bnd_Box box;
  BRepBndLib::AddOptimal(s, box, Standard_False, Standard_False);
  if (box.IsVoid())
    return false;
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  lo = {x0, y0, z0};
  hi = {x1, y1, z1};
  return true;
}

const char* AxisName(int a) { return a == 0 ? "x" : a == 1 ? "y" : "z"; }

std::string Mm(const glm::dvec3& v) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), "%.1f x %.1f x %.1f mm", v.x, v.y, v.z);
  return buf;
}

} // namespace

bool FitsBed(const glm::dvec3& size, const glm::dvec3& bed,
             std::array<int, 3>* orientation) {
  static constexpr std::array<std::array<int, 3>, 6> kPerms = {
      {{0, 1, 2}, {1, 0, 2}, {0, 2, 1}, {2, 0, 1}, {1, 2, 0}, {2, 1, 0}}};
  bool found = false;
  double bestHeight = 0.0;
  for (const auto& p : kPerms) {
    // The piece's axis p[i] lies along the bed's axis i.
    if (size[p[0]] <= bed.x + 1e-9 && size[p[1]] <= bed.y + 1e-9 &&
        size[p[2]] <= bed.z + 1e-9) {
      if (!found || size[p[2]] < bestHeight) {
        bestHeight = size[p[2]];
        if (orientation)
          *orientation = p;
      }
      found = true;
    }
  }
  return found;
}

std::optional<Plan> PlanCuts(const TopoDS_Shape& shapeIn, const glm::dvec3& bed,
                             std::vector<Cut> cuts, std::string* error) {
  auto fail = [&](const std::string& m) -> std::optional<Plan> {
    if (error)
      *error = m;
    return std::nullopt;
  };
  if (!(bed.x > 0 && bed.y > 0 && bed.z > 0))
    return fail("the bed's three sizes must be positive");
  glm::dvec3 lo, hi;
  if (shapeIn.IsNull() || !Bounds(shapeIn, lo, hi))
    return fail("the body has no shape");
  // The planes along each axis, sorted, each inside the body's extent.
  std::array<std::vector<double>, 3> planes;
  for (const Cut& c : cuts) {
    if (c.axis < 0 || c.axis > 2)
      return fail("a cut's axis is x, y or z");
    if (!(c.at > lo[c.axis] && c.at < hi[c.axis])) {
      char buf[200];
      std::snprintf(buf, sizeof(buf),
                    "the cut at %s = %.3f misses the body, which spans %.3f "
                    "to %.3f along %s",
                    AxisName(c.axis), c.at, lo[c.axis], hi[c.axis],
                    AxisName(c.axis));
      return fail(buf);
    }
    planes[c.axis].push_back(c.at);
  }
  for (auto& v : planes) {
    std::sort(v.begin(), v.end());
    v.erase(
        std::unique(v.begin(), v.end(),
                    [](double a, double b) { return std::abs(a - b) < 1e-9; }),
        v.end());
  }

  // A copy: an OpenCASCADE boolean writes to its arguments, and this shape is
  // the document's.
  const TopoDS_Shape shape = BRepBuilderAPI_Copy(shapeIn).Shape();
  const double pad = 1.0 + 0.01 * glm::length(hi - lo);
  std::array<std::vector<double>, 3> edges;
  for (int a = 0; a < 3; ++a) {
    edges[a].push_back(lo[a] - pad);
    for (double v : planes[a])
      edges[a].push_back(v);
    edges[a].push_back(hi[a] + pad);
  }

  Plan plan;
  plan.cuts = cuts;
  for (size_t i = 0; i + 1 < edges[0].size(); ++i)
    for (size_t j = 0; j + 1 < edges[1].size(); ++j)
      for (size_t k = 0; k + 1 < edges[2].size(); ++k) {
        const glm::dvec3 c0(edges[0][i], edges[1][j], edges[2][k]);
        const glm::dvec3 c1(edges[0][i + 1], edges[1][j + 1], edges[2][k + 1]);
        TopoDS_Shape cell = BRepPrimAPI_MakeBox(gp_Pnt(c0.x, c0.y, c0.z),
                                                gp_Pnt(c1.x, c1.y, c1.z))
                                .Shape();
        BRepAlgoAPI_Common common;
        TopTools_ListOfShape args, tools;
        args.Append(shape);
        tools.Append(cell);
        common.SetArguments(args);
        common.SetTools(tools);
        common.SetNonDestructive(Standard_True);
        common.Build();
        if (!common.IsDone())
          return fail("the cut cell " + Mm(c1 - c0) +
                      " did not intersect "
                      "cleanly with the body");
        // A cell can hold several solids (a U cut above its base is two
        // posts). Each is its own piece when a box can take it alone: the
        // solids whose padded boxes overlap go together, and a group whose
        // box would reach into another's goes with it.
        struct Solid {
          glm::dvec3 lo, hi;
          double volume;
        };
        std::vector<Solid> solids;
        for (TopExp_Explorer ex(common.Shape(), TopAbs_SOLID); ex.More();
             ex.Next()) {
          GProp_GProps props;
          BRepGProp::VolumeProperties(ex.Current(), props);
          Solid sd{{}, {}, props.Mass()};
          if (sd.volume < 1e-6 || !Bounds(ex.Current(), sd.lo, sd.hi))
            continue;
          solids.push_back(sd);
        }
        if (solids.empty())
          continue;
        constexpr double kPad = 0.5; // mm round each solid's box
        const auto overlap = [](const glm::dvec3& a0, const glm::dvec3& a1,
                                const glm::dvec3& b0, const glm::dvec3& b1) {
          return a0.x < b1.x && b0.x < a1.x && a0.y < b1.y && b0.y < a1.y &&
                 a0.z < b1.z && b0.z < a1.z;
        };
        std::vector<int> group(solids.size());
        for (size_t s = 0; s < solids.size(); ++s)
          group[s] = static_cast<int>(s);
        const auto boxOf = [&](int g, glm::dvec3& b0, glm::dvec3& b1) {
          bool first = true;
          for (size_t s = 0; s < solids.size(); ++s)
            if (group[s] == g) {
              b0 = first ? solids[s].lo : glm::min(b0, solids[s].lo);
              b1 = first ? solids[s].hi : glm::max(b1, solids[s].hi);
              first = false;
            }
          b0 = glm::max(b0 - kPad, c0);
          b1 = glm::min(b1 + kPad, c1);
        };
        for (bool merged = true; merged;) {
          merged = false;
          for (size_t a = 0; a < solids.size() && !merged; ++a)
            for (size_t b = 0; b < solids.size() && !merged; ++b) {
              if (group[a] == group[b])
                continue;
              glm::dvec3 a0, a1;
              boxOf(group[a], a0, a1);
              if (!overlap(a0, a1, solids[b].lo, solids[b].hi))
                continue;
              const int from = group[b], to = group[a];
              for (int& g : group)
                if (g == from)
                  g = to;
              merged = true;
            }
        }
        std::vector<int> groups(group);
        std::sort(groups.begin(), groups.end());
        groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
        for (int g : groups) {
          Piece pc;
          bool first = true;
          for (size_t s = 0; s < solids.size(); ++s)
            if (group[s] == g) {
              ++pc.solids;
              pc.volume += solids[s].volume;
              pc.min = first ? solids[s].lo : glm::min(pc.min, solids[s].lo);
              pc.max = first ? solids[s].hi : glm::max(pc.max, solids[s].hi);
              first = false;
            }
          if (groups.size() == 1) {
            pc.cellMin = c0;
            pc.cellMax = c1;
          } else {
            boxOf(g, pc.cellMin, pc.cellMax);
          }
          pc.fits = FitsBed(pc.Size(), bed, &pc.orientation);
          plan.pieces.push_back(pc);
        }
      }
  if (plan.pieces.empty())
    return fail("the cuts leave no piece of the body");
  return plan;
}

std::optional<Plan> PlanAutomatic(const TopoDS_Shape& shape,
                                  const glm::dvec3& bed, std::string* error,
                                  int maxPieces) {
  glm::dvec3 lo, hi;
  if (shape.IsNull() || !Bounds(shape, lo, hi)) {
    if (error)
      *error = "the body has no shape";
    return std::nullopt;
  }
  const glm::dvec3 size = hi - lo;
  if (FitsBed(size, bed)) {
    Plan plan;
    plan.automatic = true;
    plan.fitsWhole = true;
    Piece pc;
    pc.min = lo;
    pc.max = hi;
    pc.cellMin = lo;
    pc.cellMax = hi;
    pc.solids = 1;
    pc.fits = FitsBed(size, bed, &pc.orientation);
    GProp_GProps props;
    BRepGProp::VolumeProperties(shape, props);
    pc.volume = props.Mass();
    plan.pieces.push_back(pc);
    return plan;
  }
  int axis = 0;
  for (int a = 1; a < 3; ++a)
    if (size[a] > size[axis])
      axis = a;
  const double longestBed = std::max({bed.x, bed.y, bed.z});
  const int first =
      std::max(2, static_cast<int>(std::ceil(size[axis] / longestBed)));
  std::string why;
  for (int n = first; n <= maxPieces; ++n) {
    std::vector<Cut> cuts;
    for (int i = 1; i < n; ++i)
      cuts.push_back({axis, lo[axis] + size[axis] * i / n});
    auto plan = PlanCuts(shape, bed, cuts, &why);
    if (!plan) {
      if (error)
        *error = why;
      return std::nullopt;
    }
    plan->automatic = true;
    const auto bad = std::find_if(plan->pieces.begin(), plan->pieces.end(),
                                  [](const Piece& p) { return !p.fits; });
    if (bad == plan->pieces.end())
      return plan;
    why = "a piece " + Mm(bad->Size()) + " still does not fit";
  }
  if (error) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d equal slabs along %s", maxPieces,
                  AxisName(axis));
    *error = "no split into up to " + std::string(buf) + " fits the bed " +
             Mm(bed) + ": " + why +
             " (cuts along the longest axis cannot shrink the others; give "
             "the cuts)";
  }
  return std::nullopt;
}

} // namespace printsplit
