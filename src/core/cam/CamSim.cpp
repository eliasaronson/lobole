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

double SegmentLength(const GProgram& p, size_t seg) {
  return glm::length(p.moves[seg + 1].to - p.moves[seg].to);
}

} // namespace

Timeline BuildTimeline(GProgram program, double rapidFeed) {
  if (RotatesWhileCutting(program)) {
    Timeline out;
    out.unsupported =
        "the A axis moves in this program and the simulation's stock is a "
        "height map, which cannot represent a part that turns — it would "
        "carve the block as though it stood still. `cam_verify` checks a "
        "rotary file properly (docs/cam/rotary-plan.md); there is no picture "
        "for one yet.";
    return out;
  }
  Timeline tl;
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
    // The same accounting `CuttingMinutes` does, move for move: a rapid is
    // counted only when the setup states a traverse rate, because otherwise
    // the number would be invented.
    const double feed = m.rapid ? rapidFeed : m.feed;
    if (feed > 1e-9 && len > 1e-12)
      minutes += len / feed;
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

} // namespace cam
