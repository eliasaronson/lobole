#include "core/cam/CamAdaptive.h"

#ifdef HAVE_CAM_ADAPTIVE

// The ONLY place in Designer that includes an LGPL header. See CamAdaptive.h:
// the code behind it lives in its own shared library so the licence boundary
// is a link a user can replace.
#include "Adaptive.hpp"

#include <glm/geometric.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <sstream>

namespace cam {

namespace {

// Millimetres both sides — the reason this is a bridge and not a port.
AdaptivePath::DPaths ToAdaptive(const std::vector<Loop>& loops, bool holes) {
  AdaptivePath::DPaths out;
  for (const Loop& l : loops) {
    if (l.hole != holes || l.pts.size() < 3)
      continue;
    AdaptivePath::DPath path;
    path.reserve(l.pts.size());
    for (const glm::dvec2& p : l.pts)
      path.emplace_back(p.x, p.y);
    out.push_back(std::move(path));
  }
  return out;
}

// Adaptive2d reports its trouble as flags on the output rather than as a
// message. Turned into the sentence the panel shows, most consequential
// first — a job that quietly cleared 80% of a pocket is the failure this
// avoids.
std::string Trouble(const AdaptivePath::AdaptiveOutput& out) {
  if (out.StartPointNotFound)
    return "adaptive clearing could not find anywhere to start — the region "
           "may be narrower than the cutter";
  if (out.LeadPathFailed || out.FinishingLeadInFailed)
    return "adaptive clearing could not lead into the cut; a smaller tool or "
           "a larger pocket would";
  if (out.UnclearedAreaRemains)
    return "adaptive clearing left material behind — reduce the step-over or "
           "use a smaller tool";
  if (out.TooManyFailedEngagements || out.UnexpectedRotateIterations)
    return "adaptive clearing gave up on this shape; the concentric pocket "
           "will cut it";
  if (out.FailedToSetUpFinishingPass)
    return "adaptive clearing could not plan its finishing pass";
  return {};
}

} // namespace

bool AdaptiveAvailable() { return true; }

namespace {
// The pass itself. `safeZ` and `rapidFeed` rather than the Setup: they are
// the fields a pass reads, and taking only them is what lets a request carry
// the whole input.
AdaptiveResult ClearImpl(double safeZ, double rapidFeed, const Operation& op,
                         const Tool& tool, const std::vector<Loop>& region,
                         double z, double fromZ) {
  AdaptiveResult result;
  result.available = true;

  const AdaptivePath::DPaths outers = ToAdaptive(region, /*holes=*/false);
  if (outers.empty()) {
    result.error = "nothing to clear at this depth";
    return result;
  }

  AdaptivePath::Adaptive2d engine;
  engine.toolDiameter = tool.diameter;
  engine.stepOverFactor = std::clamp(op.stepover, 0.05, 0.9);
  engine.stockToLeave = op.stockToLeave;
  // The same chord tolerance the loops were flattened at: asking for more
  // precision than the input has only costs time.
  engine.tolerance = 0.02;
  // The helix Adaptive2d plans for itself, sized from the cutter — which is
  // why an adaptive pocket needs none of C6's helix machinery.
  engine.helixRampTargetDiameter = tool.diameter * 0.9;
  engine.helixRampMinDiameter = tool.diameter * 0.2;
  engine.opType = op.side == Side::Outside
                      ? AdaptivePath::OperationType::otClearingOutside
                      : AdaptivePath::OperationType::otClearingInside;

  // The stock at this level IS the region: the caller already subtracted
  // what the model keeps, and anything cleared by a previous depth pass is
  // above this one.
  std::list<AdaptivePath::AdaptiveOutput> outputs;
  // Adaptive2d narrates itself to std::cout ("Tool Diameter: 6", "**
  // Processing region: 1", "All cleared."). Swallowed here rather than
  // patched out of the vendored file: keeping our copy byte-identical to
  // FreeCAD b2da06bf is what makes "we changed nothing" checkable, and it is
  // also the honest answer to the LGPL's modified-versions clause.
  std::ostringstream swallowed;
  std::streambuf* saved = std::cout.rdbuf(swallowed.rdbuf());
  struct Restore {
    std::streambuf* buf;
    ~Restore() { std::cout.rdbuf(buf); }
  } restore{saved};
  try {
    outputs = engine.Execute(outers, outers, AdaptivePath::DPaths{},
                             [](AdaptivePath::TPaths) { return false; });
  } catch (const std::exception& e) {
    result.error = std::string("adaptive clearing failed: ") + e.what();
    return result;
  } catch (...) {
    result.error = "adaptive clearing failed";
    return result;
  }

  for (const AdaptivePath::AdaptiveOutput& out : outputs) {
    const std::string trouble = Trouble(out);
    if (!trouble.empty() && result.error.empty())
      result.error = trouble;
    // No start is no path: its StartPoint is unset, and emitting the entry
    // anyway put a rapid and a plunge at it -- moves, so the concentric
    // fall-back (which takes only an empty result) never ran, and a 6 mm
    // keyway with a 4 mm cutter failed that the rings cut.
    if (out.StartPointNotFound)
      continue;

    // The helix down. Adaptive2d hands back its centre and the entry point;
    // the descent itself is ours to emit, at the same 3 degrees the rest of
    // the app ramps at.
    const glm::dvec2 helix(out.HelixCenterPoint.first,
                           out.HelixCenterPoint.second);
    const glm::dvec2 start(out.StartPoint.first, out.StartPoint.second);
    const double helixRadius = glm::length(start - helix);
    result.moves.push_back(
        {Move::Kind::Rapid, {start.x, start.y, safeZ}, false, 0.0, 0.0});
    if (helixRadius > 1e-6 && fromZ > z + 1e-9) {
      const double perRev =
          2.0 * std::numbers::pi * helixRadius * std::tan(glm::radians(3.0));
      const double revs = perRev > 1e-9 ? (fromZ - z) / perRev : 1.0;
      const int steps = std::max(16, static_cast<int>(std::ceil(revs * 24.0)));
      const double a0 = std::atan2(start.y - helix.y, start.x - helix.x);
      result.moves.push_back(
          {Move::Kind::Rapid, {start.x, start.y, fromZ}, false, 0.0, 0.0});
      for (int i = 1; i <= steps; i++) {
        const double t = static_cast<double>(i) / steps;
        const double a = a0 + 2.0 * std::numbers::pi * revs * t;
        const glm::dvec3 to(helix.x + helixRadius * std::cos(a),
                            helix.y + helixRadius * std::sin(a),
                            fromZ - (fromZ - z) * t);
        // The plunge feed holds only the helix's vertical part (EntryFeed).
        result.moves.push_back({Move::Kind::Cut, to, false, 0.0,
                                EntryFeed(tool, result.moves.back().to, to)});
      }
    } else {
      result.moves.push_back({Move::Kind::Plunge,
                              {start.x, start.y, z},
                              false,
                              0.0,
                              tool.feedPlunge});
    }

    // The cut itself. Three motion types, and the third one is the whole
    // safety story: mtLinkNotClear crosses material this pass has NOT cut,
    // so it has to happen at safe Z. Emitting it at depth drags the cutter
    // through solid stock at feed — which is exactly what the verifier's
    // rapid-through-material check catches.
    bool down = true;
    for (const AdaptivePath::TPath& tpath : out.AdaptivePaths) {
      const int motion = tpath.first;
      for (const AdaptivePath::DPoint& pt : tpath.second) {
        if (motion == AdaptivePath::MotionType::mtLinkNotClear) {
          if (down) {
            result.moves.push_back(
                {Move::Kind::Rapid,
                 {result.moves.back().to.x, result.moves.back().to.y, safeZ},
                 false,
                 0.0,
                 0.0});
            down = false;
          }
          result.moves.push_back({Move::Kind::Rapid,
                                  {pt.first, pt.second, safeZ},
                                  false,
                                  0.0,
                                  0.0});
          continue;
        }
        if (!down) {
          result.moves.push_back({Move::Kind::Plunge,
                                  {pt.first, pt.second, z},
                                  false,
                                  0.0,
                                  tool.feedPlunge});
          down = true;
          continue;
        }
        // A link through what this pass has already cleared is still a G1
        // -- a straight line, which a dog-legged G0 is not -- but at the
        // machine's traverse rate when it states one: at the cutting feed,
        // links through air were 3.9 of the sampler part's 33 minutes.
        const bool cutting = motion == AdaptivePath::MotionType::mtCutting;
        result.moves.push_back(
            {Move::Kind::Cut,
             {pt.first, pt.second, z},
             false,
             0.0,
             cutting ? tool.feedCut : std::max(tool.feedCut, rapidFeed)});
      }
    }
    if (!result.moves.empty())
      result.moves.push_back(
          {Move::Kind::Rapid,
           {result.moves.back().to.x, result.moves.back().to.y, safeZ},
           false,
           0.0,
           0.0});
  }
  return result;
}
} // namespace

AdaptiveResult AdaptiveClear(const Setup& setup, const Operation& op,
                             const Tool& tool, const std::vector<Loop>& region,
                             double z, double fromZ) {
  return ClearImpl(setup.safeZ, setup.rapidFeed, op, tool, region, z, fromZ);
}

AdaptiveResult AdaptiveClear(const AdaptiveRequest& r) {
  return ClearImpl(r.safeZ, r.rapidFeed, r.op, r.tool, r.region, r.z, r.fromZ);
}

} // namespace cam

#else // !HAVE_CAM_ADAPTIVE

namespace cam {

bool AdaptiveAvailable() { return false; }

AdaptiveResult AdaptiveClear(const Setup&, const Operation&, const Tool&,
                             const std::vector<Loop>&, double, double) {
  AdaptiveResult out;
  out.error = "this build has no adaptive clearing "
              "(-DENABLE_CAM_ADAPTIVE=OFF)";
  return out;
}

AdaptiveResult AdaptiveClear(const AdaptiveRequest&) {
  AdaptiveResult out;
  out.error = "this build has no adaptive clearing "
              "(-DENABLE_CAM_ADAPTIVE=OFF)";
  return out;
}

} // namespace cam

#endif

// --- The key, the request and the cache: built with or without libarea ------

namespace cam {

namespace {
// FNV-1a over the raw bytes of each input, in a fixed order. Doubles are
// hashed by bit pattern, so -0.0 and 0.0 differ — harmless (one extra miss)
// and simpler than canonicalising.
struct Hasher {
  uint64_t h = 1469598103934665603ULL;
  void Bytes(const void* p, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) {
      h ^= c[i];
      h *= 1099511628211ULL;
    }
  }
  void D(double v) { Bytes(&v, sizeof v); }
  void I(int64_t v) { Bytes(&v, sizeof v); }
};
} // namespace

uint64_t AdaptiveKey(const Setup& setup, const Operation& op, const Tool& tool,
                     const std::vector<Loop>& region, double z, double fromZ) {
  // Every field a pass reads, and the loops. A read added to ClearImpl that
  // is not added here fails `test_conventions`
  // ("the adaptive key covers every field the pass reads").
  Hasher hash;
  hash.D(setup.safeZ);
  hash.D(setup.rapidFeed);
  hash.D(op.stepover);
  hash.D(op.stockToLeave);
  hash.I(static_cast<int64_t>(op.side));
  hash.D(tool.diameter);
  hash.D(tool.feedPlunge);
  hash.D(tool.feedCut);
  hash.D(z);
  hash.D(fromZ);
  hash.I(static_cast<int64_t>(region.size()));
  for (const Loop& l : region) {
    hash.I(l.hole ? 1 : 0);
    hash.I(static_cast<int64_t>(l.pts.size()));
    for (const glm::dvec2& p : l.pts) {
      hash.D(p.x);
      hash.D(p.y);
    }
  }
  return hash.h;
}

AdaptiveRequest MakeAdaptiveRequest(const Setup& setup, const Operation& op,
                                    const Tool& tool,
                                    const std::vector<Loop>& region, double z,
                                    double fromZ) {
  AdaptiveRequest r;
  r.key = AdaptiveKey(setup, op, tool, region, z, fromZ);
  r.safeZ = setup.safeZ;
  r.rapidFeed = setup.rapidFeed;
  r.op = op;
  r.tool = tool;
  r.region = region;
  r.z = z;
  r.fromZ = fromZ;
  return r;
}

const AdaptiveResult* AdaptiveCache::Find(uint64_t key) {
  const auto it = m_entries.find(key);
  if (it == m_entries.end()) {
    misses++;
    return nullptr;
  }
  hits++;
  return &it->second;
}

bool AdaptiveCache::Contains(uint64_t key) const {
  return m_entries.find(key) != m_entries.end();
}

void AdaptiveCache::Put(uint64_t key, AdaptiveResult result) {
  if (const auto it = m_entries.find(key); it != m_entries.end()) {
    m_moves -= it->second.moves.size();
    it->second = std::move(result);
    m_moves += it->second.moves.size();
    return;
  }
  m_moves += result.moves.size();
  m_entries.emplace(key, std::move(result));
  m_order.push_back(key);
  while (m_entries.size() > kMaxEntries ||
         (m_moves > kMaxMoves && m_entries.size() > 1)) {
    const uint64_t oldest = m_order.front();
    m_order.pop_front();
    if (const auto it = m_entries.find(oldest); it != m_entries.end()) {
      m_moves -= it->second.moves.size();
      m_entries.erase(it);
    }
  }
}

void AdaptiveCache::Clear() {
  m_entries.clear();
  m_order.clear();
  m_moves = 0;
}

} // namespace cam
