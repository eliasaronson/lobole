// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/DrawingBalloons.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace drawing {

namespace {

double Cross(const glm::dvec2& a, const glm::dvec2& b) {
  return a.x * b.y - a.y * b.x;
}

bool BoxesTouch(const glm::dvec2& amn, const glm::dvec2& amx,
                const glm::dvec2& bmn, const glm::dvec2& bmx, double pad) {
  return amn.x < bmx.x + pad && amx.x > bmn.x - pad && amn.y < bmx.y + pad &&
         amx.y > bmn.y - pad;
}

// How far a point inside the view's box is from its edge: an anchor near
// the edge is reached by a short leader that crosses little of the view.
double DepthIn(const glm::dvec2& p, const glm::dvec2& mn,
               const glm::dvec2& mx) {
  return std::max(0.0,
                  std::min({p.x - mn.x, mx.x - p.x, p.y - mn.y, mx.y - p.y}));
}

double AngleAbout(const glm::dvec2& p, const glm::dvec2& c) {
  return std::atan2(p.y - c.y, p.x - c.x);
}

double PointSegmentDistance(const glm::dvec2& p, const glm::dvec2& a,
                            const glm::dvec2& b) {
  const glm::dvec2 ab = b - a;
  const double L2 = glm::dot(ab, ab);
  const double t =
      L2 < 1e-18 ? 0.0 : std::clamp(glm::dot(p - a, ab) / L2, 0.0, 1.0);
  return glm::length(p - (a + ab * t));
}

struct Slot {
  glm::dvec2 at{0.0};
  int ring = 0;
};

} // namespace

bool SegmentsCross(const glm::dvec2& a, const glm::dvec2& b,
                   const glm::dvec2& c, const glm::dvec2& d) {
  const double d1 = Cross(b - a, c - a), d2 = Cross(b - a, d - a);
  const double d3 = Cross(d - c, a - c), d4 = Cross(d - c, b - c);
  constexpr double eps = 1e-9;
  return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) &&
         ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

std::pair<glm::dvec2, glm::dvec2> BalloonLeader(const glm::dvec2& centre,
                                                double radius,
                                                const glm::dvec2& anchor) {
  const glm::dvec2 d = anchor - centre;
  const double L = glm::length(d);
  if (L <= radius)
    return {anchor, anchor};
  return {centre + d / L * radius, anchor};
}

BalloonLayout LayoutBalloons(const BalloonLayoutInput& in) {
  BalloonLayout out;
  const size_t n = in.items.size();
  out.placements.resize(n);
  if (n == 0)
    return out;
  double rMax = 0.0;
  for (const BalloonItem& it : in.items) {
    rMax = std::max(rMax, it.radius);
    if (it.anchors.empty()) {
      out.error = "a part draws nothing visible to point a balloon at";
      return out;
    }
  }
  const glm::dvec2 centre = 0.5 * (in.viewMin + in.viewMax);

  // The slots: rings round the view's box, a balloon's width apart, kept
  // where a balloon there touches nothing else on the sheet.
  std::vector<Slot> slots;
  const double step = 2.0 * rMax + 2.5;
  for (int ring = 0; ring < 3; ++ring) {
    const double d = 7.0 + rMax + ring * (2.0 * rMax + 4.0);
    const glm::dvec2 lo = in.viewMin - glm::dvec2(d),
                     hi = in.viewMax + glm::dvec2(d);
    const glm::dvec2 corners[4] = {lo, {hi.x, lo.y}, hi, {lo.x, hi.y}};
    for (int side = 0; side < 4; ++side) {
      const glm::dvec2 a = corners[side], b = corners[(side + 1) % 4];
      const double L = glm::length(b - a);
      const int count = std::max(1, static_cast<int>(std::floor(L / step)));
      for (int k = 0; k < count; ++k) {
        const glm::dvec2 p = a + (b - a) * ((k + 0.5) / count);
        const glm::dvec2 mn = p - glm::dvec2(rMax), mx = p + glm::dvec2(rMax);
        if (mn.x < in.borderMin.x + 1.0 || mn.y < in.borderMin.y + 1.0 ||
            mx.x > in.borderMax.x - 1.0 || mx.y > in.borderMax.y - 1.0)
          continue;
        bool clear = true;
        for (const auto& [amn, amx] : in.avoid)
          if (BoxesTouch(mn, mx, amn, amx, 1.5)) {
            clear = false;
            break;
          }
        if (clear)
          slots.push_back({p, ring});
      }
    }
  }
  if (slots.size() < n) {
    out.error = "the sheet has room round this view for " +
                std::to_string(slots.size()) + " balloons, not " +
                std::to_string(n);
    return out;
  }

  // First anchors: the one nearest the view's edge (plus its own cost).
  std::vector<size_t> anchorOf(n, 0);
  for (size_t i = 0; i < n; ++i) {
    double best = std::numeric_limits<double>::max();
    for (size_t a = 0; a < in.items[i].anchors.size(); ++a) {
      const auto& an = in.items[i].anchors[a];
      const double c = DepthIn(an.at, in.viewMin, in.viewMax) + an.cost;
      if (c < best) {
        best = c;
        anchorOf[i] = a;
      }
    }
  }
  const auto anchorAt = [&](size_t i) {
    return in.items[i].anchors[anchorOf[i]].at;
  };

  // Matched in the order both go round the view: the cheapest monotone
  // assignment over every rotation of the two circular orders. Leaders
  // that keep the parts' order round the view do not cross, in the common
  // case; the untangling below takes care of the rest.
  std::vector<size_t> itemOrder(n), slotOrder(slots.size());
  std::iota(itemOrder.begin(), itemOrder.end(), 0);
  std::iota(slotOrder.begin(), slotOrder.end(), 0);
  std::sort(itemOrder.begin(), itemOrder.end(), [&](size_t a, size_t b) {
    return AngleAbout(anchorAt(a), centre) < AngleAbout(anchorAt(b), centre);
  });
  std::sort(slotOrder.begin(), slotOrder.end(), [&](size_t a, size_t b) {
    return AngleAbout(slots[a].at, centre) < AngleAbout(slots[b].at, centre);
  });
  const size_t k = slots.size();
  const auto cost = [&](size_t item, size_t slot) {
    return glm::length(anchorAt(item) - slots[slot].at) +
           6.0 * slots[slot].ring;
  };
  std::vector<size_t> slotOf(n, 0);
  double bestTotal = std::numeric_limits<double>::max();
  constexpr double kInf = std::numeric_limits<double>::max() / 4;
  std::vector<double> dp((n + 1) * (k + 1));
  std::vector<unsigned char> took((n + 1) * (k + 1));
  for (size_t ri = 0; ri < n; ++ri)
    for (size_t rs = 0; rs < k; ++rs) {
      const auto I = [&](size_t i) { return itemOrder[(ri + i) % n]; };
      const auto S = [&](size_t j) { return slotOrder[(rs + j) % k]; };
      // dp[i][j]: the first i items on slots among the first j.
      for (size_t i = 0; i <= n; ++i)
        for (size_t j = 0; j <= k; ++j) {
          double& v = dp[i * (k + 1) + j];
          if (i == 0) {
            v = 0.0;
            continue;
          }
          if (j < i) {
            v = kInf;
            continue;
          }
          const double skip = dp[i * (k + 1) + j - 1];
          const double use =
              dp[(i - 1) * (k + 1) + j - 1] + cost(I(i - 1), S(j - 1));
          v = std::min(skip, use);
          took[i * (k + 1) + j] = use <= skip;
        }
      const double total = dp[n * (k + 1) + k];
      if (total < bestTotal) {
        bestTotal = total;
        size_t i = n, j = k;
        while (i > 0) {
          if (took[i * (k + 1) + j]) {
            slotOf[I(i - 1)] = S(j - 1);
            --i;
          }
          --j;
        }
      }
    }

  // Each part's anchor nearest its balloon, then untangle: any two leaders
  // that cross swap balloons. A swap of two crossing segments shortens
  // their total (the triangle inequality), so this ends.
  const auto repick = [&](size_t i) {
    double best = std::numeric_limits<double>::max();
    for (size_t a = 0; a < in.items[i].anchors.size(); ++a) {
      const auto& an = in.items[i].anchors[a];
      const double c = glm::length(an.at - slots[slotOf[i]].at) + an.cost;
      if (c < best) {
        best = c;
        anchorOf[i] = a;
      }
    }
  };
  for (size_t i = 0; i < n; ++i)
    repick(i);
  const auto leader = [&](size_t i) {
    return BalloonLeader(slots[slotOf[i]].at, in.items[i].radius, anchorAt(i));
  };
  const auto conflict = [&](size_t i, size_t j) {
    const auto [a, b] = leader(i);
    const auto [c, d] = leader(j);
    if (SegmentsCross(a, b, c, d))
      return true;
    // A leader through the other balloon is as bad as a crossing.
    return PointSegmentDistance(slots[slotOf[j]].at, a, b) <
               in.items[j].radius + 0.5 ||
           PointSegmentDistance(slots[slotOf[i]].at, c, d) <
               in.items[i].radius + 0.5;
  };
  for (int round = 0; round < 400; ++round) {
    bool swapped = false;
    for (size_t i = 0; i < n && !swapped; ++i)
      for (size_t j = i + 1; j < n && !swapped; ++j) {
        if (!conflict(i, j))
          continue;
        std::swap(slotOf[i], slotOf[j]);
        repick(i);
        repick(j);
        if (conflict(i, j)) { // the swap did not help: put it back
          std::swap(slotOf[i], slotOf[j]);
          repick(i);
          repick(j);
          continue;
        }
        swapped = true;
      }
    if (!swapped)
      break;
  }
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 1; j < n; ++j)
      if (conflict(i, j))
        ++out.crossings;
  for (size_t i = 0; i < n; ++i)
    out.placements[i] = {true, anchorOf[i], slots[slotOf[i]].at};
  return out;
}

} // namespace drawing
