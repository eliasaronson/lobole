#include "core/cam/CamSim.h"

#include <algorithm>
#include <cmath>

namespace cam {

namespace {

// The point a fraction of the way along segment `seg`.
glm::dvec3 Lerp(const GProgram& p, size_t seg, double t) {
  const glm::dvec3& a = p.moves[seg].to;
  const glm::dvec3& b = p.moves[seg + 1].to;
  return a + (b - a) * std::clamp(t, 0.0, 1.0);
}

// Two consecutive moves of a posted program, as the feed rules see them.
RotarySegment SegmentAt(const GProgram& p, size_t seg) {
  const GMove& a = p.moves[seg];
  const GMove& b = p.moves[seg + 1];
  RotarySegment s;
  s.from = a.to;
  s.aFrom = a.a;
  s.fromHasA = a.hasA;
  s.to = b.to;
  s.aTo = b.a;
  s.toHasA = b.hasA;
  s.rapid = b.rapid;
  s.feed = b.feed;
  return s;
}

// A rotation has length too: the arc the tool tip travels as the part turns
// under it -- the definition `CuttingMinutes` uses, so the clock and the
// slider agree. Without it a pure-A move is zero millimetres and the slider
// jumps straight across every rotation.
double SegmentLength(const GProgram& p, size_t seg) {
  return cam::SegmentLength(SegmentAt(p, seg));
}

double AngleOf(const GMove& m) { return m.hasA ? m.a : 0.0; }

} // namespace

Timeline BuildTimeline(GProgram program, double rapidFeed,
                       RotaryFeedModel model) {
  // A program whose part turns is simulated now -- on a radius field, by
  // `RotaryStockSim`, with the part turning on screen. It used to be REFUSED
  // here, which was the honest answer while the only stock was a height map:
  // the first rotary job, simulated anyway, reported 5,004 mm3 removed
  // against about 77,000 actually cut.
  Timeline tl;
  tl.rotary = RotatesWhileCutting(program);
  tl.program = std::move(program);
  const size_t segs = tl.Segments();
  tl.distance.reserve(segs);
  tl.minutes.reserve(segs);

  double dist = 0.0, minutes = 0.0;
  uint64_t op = 0;
  bool first = true;
  for (size_t k = 0; k < segs; k++) {
    const GMove& m = tl.program.moves[k + 1];
    const double len = SegmentLength(tl.program, k);
    dist += len;
    // The same accounting `CuttingMinutes` does, move for move, except that
    // a file's F on a turning cut was already SCALED for the controller --
    // so it is PostedMinutes, not PlanFeed, or the scaling counts twice.
    minutes += PostedMinutes(SegmentAt(tl.program, k), rapidFeed, model);
    tl.distance.push_back(dist);
    tl.minutes.push_back(minutes);

    if (first || m.op != op) {
      // The preamble before the first `(op N)` is not an operation, and a
      // return to 0 (the postamble) is not one either — a boundary is the
      // start of something to skip TO.
      if (m.op != 0)
        tl.opStarts.push_back(k);
      op = m.op;
      first = false;
    }
    if (m.toolChange)
      tl.toolChanges.push_back(k);
  }
  tl.totalDistance = dist;
  tl.cuttingMinutes = minutes;
  return tl;
}

glm::dvec3 Timeline::PositionAt(SimCursor at) const {
  if (Empty())
    return program.moves.empty() ? glm::dvec3(0.0) : program.moves[0].to;
  at.seg = std::min(at.seg, Segments() - 1);
  return Lerp(program, at.seg, at.t);
}

double Timeline::AngleAt(SimCursor at) const {
  if (Empty())
    return program.moves.empty() ? 0.0 : AngleOf(program.moves[0]);
  at.seg = std::min(at.seg, Segments() - 1);
  const double a0 = AngleOf(program.moves[at.seg]);
  const double a1 = AngleOf(program.moves[at.seg + 1]);
  return a0 + (a1 - a0) * std::clamp(at.t, 0.0, 1.0);
}

const GMove* Timeline::MoveAt(SimCursor at) const {
  if (Empty())
    return nullptr;
  return &program.moves[std::min(at.seg, Segments() - 1) + 1];
}

double Timeline::DistanceAt(SimCursor at) const {
  if (Empty())
    return 0.0;
  at.seg = std::min(at.seg, Segments() - 1);
  const double before = at.seg == 0 ? 0.0 : distance[at.seg - 1];
  return before + (distance[at.seg] - before) * std::clamp(at.t, 0.0, 1.0);
}

double Timeline::MinutesAt(SimCursor at) const {
  if (Empty())
    return 0.0;
  at.seg = std::min(at.seg, Segments() - 1);
  const double before = at.seg == 0 ? 0.0 : minutes[at.seg - 1];
  return before + (minutes[at.seg] - before) * std::clamp(at.t, 0.0, 1.0);
}

SimCursor Timeline::CursorAtDistance(double mm) const {
  if (Empty())
    return {};
  if (mm <= 0.0)
    return {0, 0.0};
  if (mm >= totalDistance)
    return End();
  // The first segment that ENDS at or past `mm`.
  const auto it = std::lower_bound(distance.begin(), distance.end(), mm);
  const size_t seg = static_cast<size_t>(std::distance(distance.begin(), it));
  if (seg >= Segments())
    return End();
  const double before = seg == 0 ? 0.0 : distance[seg - 1];
  const double len = distance[seg] - before;
  return {seg, len > 1e-12 ? std::clamp((mm - before) / len, 0.0, 1.0) : 1.0};
}

SimCursor Timeline::NextOp(SimCursor at) const {
  for (const size_t seg : opStarts)
    if (seg > at.seg)
      return {seg, 0.0};
  return End();
}

SimCursor Timeline::NextToolChange(SimCursor at) const {
  for (const size_t seg : toolChanges)
    if (seg > at.seg)
      return {seg, 0.0};
  return End();
}

// --- StockSim -------------------------------------------------------------

void StockSim::Begin(const Timeline& timeline, const Setup& setup,
                     HeightMap pristine) {
  m_timeline = &timeline;
  m_setup = setup;
  m_pristine = std::move(pristine);
  m_stock = m_pristine;
  m_at = {0, 0.0};
  m_revision++;
}

void StockSim::Clear() {
  m_timeline = nullptr;
  m_pristine = HeightMap{};
  m_stock = HeightMap{};
  m_at = {0, 0.0};
  m_revision++;
}

double StockSim::RemovedVolume() const {
  if (m_pristine.z.size() != m_stock.z.size())
    return 0.0;
  double columns = 0.0;
  for (size_t i = 0; i < m_stock.z.size(); i++)
    columns += static_cast<double>(m_pristine.z[i]) -
               static_cast<double>(m_stock.z[i]);
  return columns * m_stock.res * m_stock.res;
}

void StockSim::Carve(SimCursor from, SimCursor to) {
  const GProgram& p = m_timeline->program;
  // Every segment is carved from its OWN START — whole when the cursor is
  // past it, up to `to.t` for the one the cursor stands in. Re-carving a
  // prefix is free (the map only ever takes the lower value) and it buys the
  // property the whole design rests on: at the end of the program every
  // segment has been carved exactly as `Inspect` carves it, so the last
  // frame is bit-identical to the map the verdict was read from. Carving
  // only the slice between two cursors would instead clamp the tool's Z at
  // the split, which is right for the frame and can differ by a hair at the
  // end.
  for (size_t seg = from.seg; seg <= to.seg && seg < m_timeline->Segments();
       seg++) {
    const double t1 = seg == to.seg ? to.t : 1.0;
    if (t1 <= 0.0)
      continue;
    // Rapids are CHECKED, not applied — the same convention `Inspect` uses.
    // A rapid that ploughs through material is reported as the defect it is
    // rather than quietly machined.
    if (p.moves[seg + 1].rapid)
      continue;
    CarveSweep(p.moves[seg].to, Lerp(p, seg, t1),
               m_setup.FindTool(p.moves[seg + 1].tool), m_stock);
  }
}

void StockSim::SeekTo(SimCursor to) {
  if (m_timeline == nullptr || m_timeline->Empty() || !Ready())
    return;
  to.seg = std::min(to.seg, m_timeline->Segments() - 1);
  to.t = std::clamp(to.t, 0.0, 1.0);
  if (to == m_at)
    return;

  if (to < m_at) {
    // Backwards: the map cannot be un-cut, so start over. See the header.
    m_stock = m_pristine;
    m_at = {0, 0.0};
  }
  Carve(m_at, to);
  m_at = to;
  m_revision++;
}

// --- RotaryStockSim -------------------------------------------------------

namespace {
// What the snapshots may cost, in total. A budget, not a count: the pin's
// 0.5 mm grid is ~800 KB a snapshot, so this is about eighty of them, and a
// part a tenth the size gets ten times as many for the same memory.
constexpr size_t kKeyframeBudgetBytes = 64u << 20;
} // namespace

void RotaryStockSim::Begin(const Timeline& timeline, const Setup& setup,
                           RadiusMap pristine) {
  m_timeline = &timeline;
  m_setup = setup;
  m_pristine = std::move(pristine);
  m_stock = m_pristine;
  m_at = {0, 0.0};
  m_keys.clear();
  const size_t bytes = std::max<size_t>(1, m_pristine.r.size() * sizeof(float));
  const size_t budgetKeys = std::max<size_t>(1, kKeyframeBudgetBytes / bytes);
  // At least a few hundred segments between snapshots: below that the replay
  // a snapshot saves is cheaper than the copy it costs.
  m_every = std::max<size_t>(256, (timeline.Segments() + budgetKeys - 1) /
                                      budgetKeys);
  m_revision++;
}

void RotaryStockSim::Clear() {
  m_timeline = nullptr;
  m_pristine = RadiusMap{};
  m_stock = RadiusMap{};
  m_at = {0, 0.0};
  m_keys.clear();
  m_revision++;
}

double RotaryStockSim::RemovedVolume() const {
  if (m_pristine.r.size() != m_stock.r.size() || m_stock.nTheta <= 0)
    return 0.0;
  // Each cell is a wedge of an annulus: (r0^2 - r1^2)/2 per radian, times the
  // cell's angle and its axial length.
  const double dTheta = 2.0 * 3.14159265358979323846 / m_stock.nTheta;
  double sum = 0.0;
  for (size_t i = 0; i < m_stock.r.size(); i++) {
    const double r0 = m_pristine.r[i];
    const double r1 = m_stock.r[i];
    sum += 0.5 * (r0 * r0 - r1 * r1);
  }
  return sum * dTheta * m_stock.res;
}

void RotaryStockSim::Carve(SimCursor from, SimCursor to) {
  const GProgram& p = m_timeline->program;
  for (size_t seg = from.seg; seg <= to.seg && seg < m_timeline->Segments();
       seg++) {
    // A snapshot at every m_every-th segment boundary on the way past it.
    // Before this segment is carved, every segment before it already is --
    // this call carves in order, and a segment the previous call stopped
    // part way through is carved again from its own start below -- so the
    // snapshot is exactly "all of [0, seg) carved".
    if (m_every > 0 && seg % m_every == 0) {
      const size_t k = seg / m_every;
      if (k == m_keys.size())
        m_keys.push_back(m_stock.r);
    }
    const double t1 = seg == to.seg ? to.t : 1.0;
    if (t1 <= 0.0)
      continue;
    // Rapids are CHECKED, not applied -- `InspectRotary`'s convention, and
    // the planar simulator's.
    if (p.moves[seg + 1].rapid)
      continue;
    const double a0 = AngleOf(p.moves[seg]);
    const double a1 = AngleOf(p.moves[seg + 1]);
    CarveSweepRotary(p.moves[seg].to, Lerp(p, seg, t1), a0,
                     a0 + (a1 - a0) * std::clamp(t1, 0.0, 1.0),
                     m_setup.FindTool(p.moves[seg + 1].tool), m_stock);
  }
}

void RotaryStockSim::SeekTo(SimCursor to) {
  if (m_timeline == nullptr || m_timeline->Empty() || !Ready())
    return;
  to.seg = std::min(to.seg, m_timeline->Segments() - 1);
  to.t = std::clamp(to.t, 0.0, 1.0);
  if (to == m_at)
    return;
  if (to < m_at) {
    // Backwards: restart from the nearest snapshot at or behind `to`, not
    // from the blank. That snapshot is "every segment before k * m_every
    // carved", so carving on from the start of that segment is the same
    // stock a replay from the beginning would reach.
    size_t k = m_every > 0 ? to.seg / m_every : 0;
    k = std::min(k, m_keys.empty() ? size_t{0} : m_keys.size() - 1);
    if (!m_keys.empty()) {
      m_stock.r = m_keys[k];
      m_at = {k * m_every, 0.0};
    } else {
      m_stock = m_pristine;
      m_at = {0, 0.0};
    }
  }
  Carve(m_at, to);
  m_at = to;
  m_revision++;
}

} // namespace cam
