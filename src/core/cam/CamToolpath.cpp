#include "core/cam/CamToolpath.h"

#include "core/Measure.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

namespace cam {

namespace {

// Clipper2 works in integers. 1e4 puts the quantum at 0.1 µm — four orders
// below anything a mill can hold, and far enough from int64's range that a
// metre-scale part cannot overflow. ONE constant, because two would disagree.
constexpr double kScale = 1.0e4;

Clipper2Lib::Path64 ToClipper(const std::vector<glm::dvec2>& pts) {
  Clipper2Lib::Path64 out;
  out.reserve(pts.size());
  for (const glm::dvec2& p : pts)
    out.emplace_back(static_cast<int64_t>(std::llround(p.x * kScale)),
                     static_cast<int64_t>(std::llround(p.y * kScale)));
  return out;
}

std::vector<glm::dvec2> FromClipper(const Clipper2Lib::Path64& path) {
  std::vector<glm::dvec2> out;
  out.reserve(path.size());
  for (const Clipper2Lib::Point64& p : path)
    out.push_back(
        {static_cast<double>(p.x) / kScale, static_cast<double>(p.y) / kScale});
  return out;
}

Clipper2Lib::Paths64 ToClipper(const std::vector<Loop>& loops) {
  Clipper2Lib::Paths64 out;
  out.reserve(loops.size());
  for (const Loop& l : loops)
    if (l.pts.size() >= 3)
      out.push_back(ToClipper(l.pts));
  return out;
}

// Offsets a set of closed paths. Positive grows, negative shrinks; an empty
// result means the region closed up, which is how concentric clearing knows
// it is done.
Clipper2Lib::Paths64 Offset(const Clipper2Lib::Paths64& paths, double deltaMm) {
  if (paths.empty())
    return {};
  Clipper2Lib::ClipperOffset co;
  // Round joins: a mill cannot cut a sharp inside corner anyway, and square
  // joins on an outside profile put the tool through the part corner.
  co.AddPaths(paths, Clipper2Lib::JoinType::Round,
              Clipper2Lib::EndType::Polygon);
  Clipper2Lib::Paths64 out;
  co.Execute(deltaMm * kScale, out);
  return out;
}

// Traversal direction. The convention, stated once because every machinist
// checks it on the first cut: with a right-hand cutter and the tool OUTSIDE
// the material, climb milling runs clockwise seen from +Z; inside a pocket it
// runs counter-clockwise. Clipper hands back outers CCW and holes CW.
void Orient(Clipper2Lib::Path64& path, bool climb, bool insideMaterial) {
  const bool wantCcw = climb == insideMaterial;
  if (Clipper2Lib::IsPositive(path) != wantCcw)
    std::reverse(path.begin(), path.end());
}

double Perimeter(const std::vector<glm::dvec2>& pts) {
  double total = 0.0;
  for (size_t i = 0; i + 1 < pts.size(); i++)
    total += glm::length(pts[i + 1] - pts[i]);
  if (pts.size() > 2)
    total += glm::length(pts.front() - pts.back());
  return total;
}

// Walks one wire into an ordered polyline, honouring edge orientation so the
// points come out in sequence rather than in map order.
std::vector<glm::dvec2> WireToLoop(const TopoDS_Wire& wire, double deflection) {
  std::vector<glm::dvec2> pts;
  BRepTools_WireExplorer ex(wire);
  for (; ex.More(); ex.Next()) {
    const TopoDS_Edge& edge = ex.Current();
    BRepAdaptor_Curve curve(edge);
    GCPnts_QuasiUniformDeflection sampler(curve, deflection);
    if (!sampler.IsDone())
      continue;
    const int n = sampler.NbPoints();
    const bool reversed = edge.Orientation() == TopAbs_REVERSED;
    for (int k = 0; k < n; k++) {
      const gp_Pnt p = sampler.Value(reversed ? n - k : k + 1);
      const glm::dvec2 xy(p.X(), p.Y());
      // The last point of one edge is the first of the next.
      if (!pts.empty() && glm::length(pts.back() - xy) < 1e-9)
        continue;
      pts.push_back(xy);
    }
  }
  if (pts.size() > 2 && glm::length(pts.front() - pts.back()) < 1e-9)
    pts.pop_back();
  return pts;
}

void Emit(Toolpath& tp, Move::Kind kind, const glm::dvec3& to, double feed) {
  Move m;
  m.kind = kind;
  m.to = to;
  m.feed = feed;
  tp.moves.push_back(m);
}

// Rapid over, plunge, cut the loop, retract. Tabs raise the tool over evenly
// spaced spans so the part stays attached on the final pass.
//
// The subtlety that bit on the first run: a tab span has to SPLIT the
// segments it crosses. A profiled rectangle is four segments of 40 mm, and
// testing "is the end of this segment inside a tab?" steps straight over
// every tab on the part — silently, with the file looking plausible. So the
// loop is walked by arc length and cut at each tab boundary, with an explicit
// vertical move there, which also makes the tab square-walled rather than a
// diagonal ramp.
// Entry ramp angle. Shallow enough for a non-centre-cutting end mill to
// enter metal, steep enough not to spend a lap descending. A knob later if
// anyone needs one; a fixed sane value beats a vertical plunge today.
constexpr double kRampAngleDeg = 3.0;

// Cut the loop at depth `z`, entering from `entryZ` by RAMPING along the path
// rather than plunging straight down — a vertical plunge with a
// non-centre-cutting mill rubs rather than cuts. The ramp runs along the
// toolpath itself, so it needs no room the cut does not already have; the lap
// then starts where the ramp finished, which is why the point list is
// rotated rather than restarted.
void EmitLoop(Toolpath& tp, const Setup& setup, const Tool& tool,
              const std::vector<glm::dvec2>& pts, double z, double safeZ,
              int tabCount, double tabWidth, double tabZ, double entryZ) {
  if (pts.size() < 2)
    return;
  const double perimeter = Perimeter(pts);
  if (perimeter < 1e-9)
    return;

  // Rapid over, then down to the height we enter from: that is either the
  // stock top or the depth the previous pass already cut here, so it is air.
  Emit(tp, Move::Kind::Rapid, {pts[0].x, pts[0].y, safeZ}, 0.0);
  const double from = std::max(entryZ, z);
  Emit(tp, Move::Kind::Rapid, {pts[0].x, pts[0].y, from}, 0.0);

  const double descend = from - z;
  double rampLen =
      descend > 1e-9 ? descend / std::tan(glm::radians(kRampAngleDeg)) : 0.0;
  // A loop shorter than the ramp would spiral forever: descend over one lap
  // instead, steeper than asked but bounded and still not a plunge.
  rampLen = std::min(rampLen, perimeter);

  // Walk the loop, emitting the descent, and note where it ends.
  double travelled = 0.0;
  size_t idx = 0;
  glm::dvec2 cursor = pts[0];
  if (rampLen > 1e-9) {
    while (travelled < rampLen - 1e-9) {
      const glm::dvec2& next = pts[(idx + 1) % pts.size()];
      const double seg = glm::length(next - cursor);
      if (seg < 1e-12) {
        idx = (idx + 1) % pts.size();
        cursor = next;
        continue;
      }
      if (travelled + seg >= rampLen - 1e-9) {
        const double t = (rampLen - travelled) / seg;
        cursor = cursor + (next - cursor) * t;
        travelled = rampLen;
      } else {
        travelled += seg;
        cursor = next;
        idx = (idx + 1) % pts.size();
      }
      const double rz = from - descend * std::min(1.0, travelled / rampLen);
      Emit(tp, Move::Kind::Cut, {cursor.x, cursor.y, rz}, tool.feedPlunge);
    }
  } else {
    Emit(tp, Move::Kind::Plunge, {cursor.x, cursor.y, z}, tool.feedPlunge);
  }

  // The full lap at depth, starting where the ramp finished.
  std::vector<glm::dvec2> lap;
  lap.reserve(pts.size() + 1);
  lap.push_back(cursor);
  for (size_t k = 1; k <= pts.size(); k++)
    lap.push_back(pts[(idx + k) % pts.size()]);
  lap.push_back(cursor);

  const bool wantTabs =
      tabCount > 0 && tabWidth > 0.0 && perimeter > tabWidth * tabCount;
  std::vector<std::pair<double, double>> tabs;
  if (wantTabs)
    for (int i = 0; i < tabCount; i++) {
      const double centre = perimeter * (static_cast<double>(i) + 0.5) /
                            static_cast<double>(tabCount);
      tabs.emplace_back(centre - tabWidth * 0.5, centre + tabWidth * 0.5);
    }
  const auto inTab = [&](double s) {
    for (const auto& [lo, hi] : tabs)
      if (s > lo && s < hi)
        return true;
    return false;
  };
  const auto zAt = [&](double s) { return inTab(s) ? std::max(z, tabZ) : z; };

  double s0 = 0.0;
  double currentZ = zAt(0.0);
  if (std::abs(currentZ - z) > 1e-9)
    Emit(tp, Move::Kind::Cut, {lap[0].x, lap[0].y, currentZ}, tool.feedCut);
  for (size_t i = 1; i < lap.size(); i++) {
    const glm::dvec2& a = lap[i - 1];
    const glm::dvec2& b = lap[i];
    const double len = glm::length(b - a);
    if (len < 1e-12)
      continue;
    // Tab boundaries strictly inside this segment split it, so a tab is never
    // stepped over by a long edge (the bug the first tab test caught).
    std::vector<double> cuts;
    for (const auto& [lo, hi] : tabs)
      for (const double edge : {lo, hi})
        if (edge > s0 + 1e-9 && edge < s0 + len - 1e-9)
          cuts.push_back(edge);
    std::sort(cuts.begin(), cuts.end());
    for (const double sc : cuts) {
      const glm::dvec2 p = a + (b - a) * ((sc - s0) / len);
      Emit(tp, Move::Kind::Cut, {p.x, p.y, currentZ}, tool.feedCut);
      const double next = zAt(sc + 1e-6);
      if (std::abs(next - currentZ) > 1e-9) {
        Emit(tp, Move::Kind::Cut, {p.x, p.y, next}, tool.feedCut);
        currentZ = next;
      }
    }
    s0 += len;
    Emit(tp, Move::Kind::Cut, {b.x, b.y, currentZ}, tool.feedCut);
  }
  Emit(tp, Move::Kind::Rapid, {lap.back().x, lap.back().y, safeZ}, 0.0);
}

} // namespace

double Toolpath::CutLength() const {
  double total = 0.0;
  for (size_t i = 1; i < moves.size(); i++)
    if (moves[i].kind == Move::Kind::Cut)
      total += glm::length(moves[i].to - moves[i - 1].to);
  return total;
}

double Toolpath::MinZ() const {
  double lowest = 0.0;
  bool any = false;
  for (const Move& m : moves) {
    if (!any || m.to.z < lowest) {
      lowest = m.to.z;
      any = true;
    }
  }
  return any ? lowest : 0.0;
}

std::vector<Loop> SectionLoops(const std::vector<Body>& bodies, double z,
                               double deflection) {
  std::vector<Loop> loops;
  // The section machinery the Section TOOL already uses (I3): one plane, the
  // filled cut face per body. Reusing it means a toolpath cannot disagree
  // with what the section view shows.
  const std::vector<CADShape> caps =
      Measure::SectionCaps(bodies, glm::vec3(0.0f, 0.0f, static_cast<float>(z)),
                           glm::vec3(0.0f, 0.0f, 1.0f));
  for (const CADShape& cap : caps) {
    if (!cap.IsValid())
      continue;
    for (TopExp_Explorer fx(cap.GetShape(), TopAbs_FACE); fx.More();
         fx.Next()) {
      const TopoDS_Face face = TopoDS::Face(fx.Current());
      const TopoDS_Wire outer = BRepTools::OuterWire(face);
      for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
        const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
        Loop loop;
        loop.pts = WireToLoop(wire, deflection);
        if (loop.pts.size() < 3)
          continue;
        loop.hole = !wire.IsSame(outer);
        loops.push_back(std::move(loop));
      }
    }
  }
  return loops;
}

std::vector<double> DepthPasses(const Operation& op) {
  std::vector<double> zs;
  const double top = op.topZ;
  const double bottom = op.bottomZ + op.stockToLeave;
  if (bottom >= top) {
    zs.push_back(bottom);
    return zs;
  }
  if (op.stepdown <= 0.0) {
    zs.push_back(bottom);
    return zs;
  }
  double z = top - op.stepdown;
  while (z > bottom + 1e-9) {
    zs.push_back(z);
    z -= op.stepdown;
  }
  // The final pass lands exactly on the requested depth: a pass a hair deeper
  // than asked is a scrapped part, and a hair shallower is a witness ridge.
  zs.push_back(bottom);
  return zs;
}

bool StockBounds(const Setup& setup, const std::vector<Body>& bodies,
                 glm::dvec3& min, glm::dvec3& max) {
  bool any = false;
  glm::vec3 lo(0.0f), hi(0.0f);
  for (const Body& b : bodies) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 bmin(0.0f), bmax(0.0f);
    b.shape.GetBoundingBox(bmin, bmax);
    lo = any ? glm::min(lo, bmin) : bmin;
    hi = any ? glm::max(hi, bmax) : bmax;
    any = true;
  }
  if (!any)
    return false;
  const Stock& s = setup.stock;
  min = glm::dvec3(lo) - glm::dvec3(s.margin.x, s.margin.y, s.bottomMargin);
  max = glm::dvec3(hi) + glm::dvec3(s.margin.x, s.margin.y, s.margin.z);
  return true;
}

FaceRegion PickedFaceRegion(const std::vector<Body>& bodies,
                            const ElementRef& ref, double deflection) {
  FaceRegion out;
  if (!ref.IsSet()) {
    out.why = "nothing picked";
    return out;
  }
  for (const Body& body : bodies) {
    if (body.createdBy != ref.featureId || !body.shape.IsValid())
      continue;
    const std::optional<int> idx = body.elementMap.Resolve(ref.name);
    if (!idx) {
      out.why = "that face is gone from the model";
      return out;
    }
    glm::vec3 centre(0.0f), normal(0.0f), uAxis(0.0f);
    if (!body.shape.FacePlane(*idx, centre, normal, uAxis)) {
      out.why = "pick a flat face — that one is curved";
      return out;
    }
    // A 2.5D job cuts in Z, so a face it can machine to is level. A tilted
    // face has no single depth, and guessing one puts the cut somewhere the
    // user did not point at.
    if (std::abs(normal.z) < 0.99f) {
      out.why = "pick a level face — a tilted one has no single depth";
      return out;
    }
    // Walk to the face in the same explorer order FacePlane uses.
    TopoDS_Face face;
    int i = 0;
    for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
         ex.Next(), i++)
      if (i == *idx) {
        face = TopoDS::Face(ex.Current());
        break;
      }
    if (face.IsNull()) {
      out.why = "that face is gone from the model";
      return out;
    }
    const TopoDS_Wire outer = BRepTools::OuterWire(face);
    for (TopExp_Explorer wx(face, TopAbs_WIRE); wx.More(); wx.Next()) {
      const TopoDS_Wire wire = TopoDS::Wire(wx.Current());
      Loop loop;
      loop.pts = WireToLoop(wire, deflection);
      if (loop.pts.size() < 3)
        continue;
      // Inner wires are islands: material standing in the pocket, which the
      // clearing region has to go around rather than through.
      loop.hole = !wire.IsSame(outer);
      if (loop.hole)
        out.loops.push_back(std::move(loop));
      else
        out.loops.insert(out.loops.begin(), std::move(loop));
    }
    if (out.loops.empty()) {
      out.why = "that face has no usable boundary";
      return out;
    }
    out.z = centre.z;
    out.ok = true;
    return out;
  }
  out.why = "that face is not on a body this job can machine";
  return out;
}

DerivedOp DeriveOperation(const Setup& setup, const std::vector<Body>& bodies,
                          OpType type, const ElementRef& target) {
  DerivedOp out;
  if (setup.tools.empty()) {
    out.why = "add a tool first — every depth here comes from its diameter";
    return out;
  }
  const Tool& tool = setup.tools.front();
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi)) {
    out.why = "there is no solid in this part yet, so there is no stock";
    return out;
  }

  Operation op;
  op.type = type;
  op.toolNumber = tool.number;
  // Half the cutter per pass and 45% of it per step-over: the rule of thumb
  // every 2.5D job starts from, expressed in the tool rather than in
  // millimetres so it stays right for a 1 mm cutter and a 12 mm one.
  op.stepdown = tool.diameter * 0.5;
  op.stepover = 0.45;

  switch (type) {
  case OpType::Pocket: {
    const FaceRegion region = PickedFaceRegion(bodies, target);
    if (!region.ok) {
      out.why = region.why;
      return out;
    }
    op.name = "Clear pocket";
    op.targets = {target};
    op.bottomZ = region.z;  // the floor is the face you clicked
    op.topZ = hi.z;         // …down from the top of the stock
    op.side = Side::Inside; // a pocket is cut from inside its own boundary
    if (op.bottomZ >= op.topZ - 1e-9) {
      out.why = "that face is already the top of the stock — there is "
                "nothing above it to clear";
      return out;
    }
    break;
  }
  case OpType::Profile: {
    op.name = "Cut out";
    op.side = Side::Outside;
    op.topZ = hi.z;
    if (target.IsSet()) {
      const FaceRegion region = PickedFaceRegion(bodies, target);
      if (!region.ok) {
        out.why = region.why;
        return out;
      }
      op.targets = {target};
    }
    // Through the part and a whisker into the spoilboard, which is what
    // "cut it out" means — a profile that stops on the last skin leaves the
    // part welded to the stock.
    op.bottomZ = lo.z;
    // Tabs, so the part does not break free under the cutter on the last
    // pass. Sized from the tool (a tab narrower than the cutter is cut away
    // as the tool passes) and from the depth, with a floor so a shallow part
    // still gets a tab rather than a scratch.
    op.tabCount = 4;
    op.tabWidth = tool.diameter * 2.0;
    op.tabHeight = std::max(0.5, (op.topZ - op.bottomZ) * 0.2);
    break;
  }
  case OpType::Face: {
    op.name = "Face top";
    op.topZ = hi.z;
    op.bottomZ = hi.z - setup.stock.margin.z; // the part's own top
    if (setup.stock.margin.z <= 1e-9) {
      out.why = "there is no stock above the part to face — the top of the "
                "part is already the top of the stock (Advanced sets how "
                "much material sits above it)";
      return out;
    }
    break;
  }
  case OpType::Drill: {
    out.why = "drilling picks cylindrical faces, not a verb";
    return out;
  }
  }

  out.op = op;
  out.ok = true;
  return out;
}

double DeriveSafeZ(const Setup& setup, const std::vector<Body>& bodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return setup.safeZ;
  // Clear the stock top by a fraction of the job's own height, with a floor
  // so a thin plate still gets real clearance rather than a scrape.
  return hi.z + std::max(2.0, (hi.z - lo.z) * 0.1);
}

double CuttingMinutes(const Setup& setup, const std::vector<Toolpath>& paths) {
  double minutes = 0.0;
  for (const Toolpath& tp : paths) {
    for (size_t i = 1; i < tp.moves.size(); i++) {
      const double len = glm::length(tp.moves[i].to - tp.moves[i - 1].to);
      if (len < 1e-12)
        continue;
      double feed = tp.moves[i].feed;
      if (tp.moves[i].kind == Move::Kind::Rapid)
        feed = setup.rapidFeed; // 0 = not stated, so not counted
      if (feed > 1e-9)
        minutes += len / feed;
    }
  }
  return minutes;
}

namespace {

// The XY contour(s) a profile follows, and the region a pocket clears, when
// the operation was made by clicking a face: the face's own wires, the same
// at every depth. Empty when the operation has no target, which is the
// caller's signal to fall back to sectioning the model.
Clipper2Lib::Paths64 TargetPaths(const Operation& op,
                                 const std::vector<Body>& bodies, bool holesToo,
                                 std::string& error) {
  if (op.targets.empty())
    return {};
  const FaceRegion region = PickedFaceRegion(bodies, op.targets.front());
  if (!region.ok) {
    error = region.why;
    return {};
  }
  Clipper2Lib::Paths64 outer, islands;
  for (const Loop& l : region.loops)
    (l.hole ? islands : outer).push_back(ToClipper(l.pts));
  if (!holesToo || islands.empty())
    return outer;
  return Clipper2Lib::Difference(outer, islands,
                                 Clipper2Lib::FillRule::NonZero);
}

// Profile: offset the section outward (or inward) by the tool radius and
// follow it, once per depth pass.
void GenerateProfile(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Body>& bodies, Toolpath& tp) {
  const double radius = tool.diameter * 0.5;
  const double bottom = op.bottomZ + op.stockToLeave;
  const std::vector<double> passes = DepthPasses(op);
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/false, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  for (size_t pass = 0; pass < passes.size(); pass++) {
    const double z = passes[pass];
    std::vector<Loop> solid;
    if (picked.empty()) {
      // Section slightly ABOVE the pass depth: exactly at the bottom face of
      // a through-cut the section is degenerate or empty.
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      if (loops.empty())
        continue;
      for (const Loop& l : loops)
        if (!l.hole)
          solid.push_back(l);
    }
    const double delta = op.side == Side::Inside ? -(radius + op.stockToLeave)
                         : op.side == Side::On   ? 0.0
                                                 : radius + op.stockToLeave;
    const Clipper2Lib::Paths64 contour =
        picked.empty() ? ToClipper(solid) : picked;
    if (contour.empty())
      continue;
    Clipper2Lib::Paths64 paths =
        delta == 0.0 ? contour : Offset(contour, delta);
    const bool lastPass = pass + 1 == passes.size();
    for (Clipper2Lib::Path64& path : paths) {
      Orient(path, op.climb, op.side == Side::Inside);
      EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ,
               lastPass ? op.tabCount : 0, op.tabWidth, bottom + op.tabHeight,
               pass == 0 ? op.topZ : passes[pass - 1]);
    }
  }
}

// Pocket: clear the voids inside the part's outline. Concentric offsets
// inward until the region closes — islands fall out of the boolean, because
// material inside a pocket is simply not part of the region.
void GeneratePocket(const Setup& setup, const Operation& op, const Tool& tool,
                    const std::vector<Body>& bodies, Toolpath& tp) {
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  // A clicked floor face IS the region to clear, at every depth: its outer
  // wire is the pocket wall and its inner wires are islands (docs/cam/gui.md).
  std::string why;
  const Clipper2Lib::Paths64 picked =
      TargetPaths(op, bodies, /*holesToo=*/true, why);
  if (!why.empty()) {
    tp.error = why;
    return;
  }
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    Clipper2Lib::Paths64 region = picked;
    if (region.empty()) {
      const std::vector<Loop> loops = SectionLoops(bodies, z + 1e-3);
      if (loops.empty())
        continue;
      Clipper2Lib::Paths64 outers, holes;
      for (const Loop& l : loops)
        (l.hole ? holes : outers).push_back(ToClipper(l.pts));
      if (holes.empty())
        continue; // nothing enclosed at this level
      // The void = what the outline encloses minus the material.
      region = Clipper2Lib::Difference(holes, Clipper2Lib::Paths64{},
                                       Clipper2Lib::FillRule::NonZero);
    }
    // First ring sits one radius in from the wall (plus finishing stock).
    double inset = radius + op.stockToLeave;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      for (Clipper2Lib::Path64& path : ring) {
        Orient(path, op.climb, /*insideMaterial=*/true);
        EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, 0, 0.0, z,
                 entryZ);
      }
      inset += step;
      ring = Offset(region, -inset);
    }
    entryZ = z;
  }
}

// Face: clear the stock top down to topZ over the whole stock rectangle.
void GenerateFace(const Setup& setup, const Operation& op, const Tool& tool,
                  const std::vector<Body>& bodies, Toolpath& tp) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi)) {
    tp.error = "no bodies, so there is no stock to face";
    return;
  }
  const double radius = tool.diameter * 0.5;
  const double step = std::max(0.05, op.stepover) * tool.diameter;
  std::vector<glm::dvec2> rect = {
      {lo.x, lo.y}, {hi.x, lo.y}, {hi.x, hi.y}, {lo.x, hi.y}};
  const Clipper2Lib::Paths64 region{ToClipper(rect)};
  double entryZ = op.topZ;
  for (const double z : DepthPasses(op)) {
    // Start with the cutter centred ON the stock edge so the outside is
    // cleaned up, then spiral inward.
    double inset = 0.0;
    Clipper2Lib::Paths64 ring = Offset(region, -inset);
    while (!ring.empty()) {
      for (Clipper2Lib::Path64& path : ring) {
        Orient(path, op.climb, /*insideMaterial=*/true);
        EmitLoop(tp, setup, tool, FromClipper(path), z, setup.safeZ, 0, 0.0, z,
                 entryZ);
      }
      inset += step;
      if (inset > glm::length(hi - lo))
        break; // guard: a degenerate stock must not spin forever
      ring = Offset(region, -inset);
    }
    entryZ = z;
    (void)radius;
  }
}

// Drill: one plunge per target hole, at the hole's axis.
void GenerateDrill(const Setup& setup, const Operation& op, const Tool& tool,
                   const std::vector<Body>& bodies, Toolpath& tp) {
  if (op.targets.empty()) {
    tp.error = "drill needs target holes; pick the cylindrical faces to drill";
    return;
  }
  int found = 0;
  for (const ElementRef& ref : op.targets) {
    for (const Body& body : bodies) {
      if (body.createdBy != ref.featureId)
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(ref.name);
      if (!idx)
        continue;
      glm::vec3 origin(0.0f), axis(0.0f), xAxis(0.0f);
      if (!body.shape.FaceAxis(*idx, origin, axis, xAxis))
        continue; // not a cylinder: silently skipping would drill nothing
      found++;
      const glm::dvec3 centre(origin.x, origin.y, 0.0);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      Emit(tp, Move::Kind::Plunge, {centre.x, centre.y, op.bottomZ},
           tool.feedPlunge);
      Emit(tp, Move::Kind::Rapid, {centre.x, centre.y, setup.safeZ}, 0.0);
      break;
    }
  }
  if (found == 0)
    tp.error = "none of the drill targets resolved to a cylindrical face";
}

} // namespace

Toolpath Generate(const Setup& setup, const Operation& op,
                  const std::vector<Body>& bodies) {
  Toolpath tp;
  tp.operationId = op.id;
  tp.toolNumber = op.toolNumber;

  const Tool* tool = setup.FindTool(op.toolNumber);
  if (tool == nullptr) {
    tp.error = "no tool " + std::to_string(op.toolNumber) + " in this setup";
    return tp;
  }
  if (tool->diameter <= 0.0) {
    tp.error = "tool diameter must be positive";
    return tp;
  }
  if (op.bottomZ >= op.topZ) {
    tp.error = "the bottom of the cut must be below its top";
    return tp;
  }

  switch (op.type) {
  case OpType::Profile:
    GenerateProfile(setup, op, *tool, bodies, tp);
    break;
  case OpType::Pocket:
    GeneratePocket(setup, op, *tool, bodies, tp);
    break;
  case OpType::Face:
    GenerateFace(setup, op, *tool, bodies, tp);
    break;
  case OpType::Drill:
    GenerateDrill(setup, op, *tool, bodies, tp);
    break;
  }
  if (tp.moves.empty() && tp.error.empty())
    tp.error = "this operation produced no toolpath — check its depths "
               "against the model";
  return tp;
}

std::vector<Toolpath> GenerateAll(const Setup& setup,
                                  const std::vector<Body>& bodies) {
  std::vector<Toolpath> out;
  for (const Operation& op : setup.operations)
    if (op.enabled)
      out.push_back(Generate(setup, op, bodies));
  return out;
}

} // namespace cam
