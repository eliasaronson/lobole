#pragma once

#include <algorithm>
#include <initializer_list>
#include <utility>
#include <vector>

namespace ui {

// Where to put the next thing so it can be SEEN (U10).
//
// Every part models around its own origin and every drawing view defaults to
// the sheet centre, so "insert" used to stack things on the same spot: two
// occurrences buried in each other, three views on one square millimetre of
// paper. Each insert site grew its own answer to this; they are one rule.
//
// The rule: keep the new item's span clear of the spans already taken, along
// one axis, with a gap scaled to the item rather than a constant (a 200 mm
// bracket and a 2 mm screw both need to look separated).
struct Span {
  double lo = 0.0;
  double hi = 0.0;
  double size() const { return hi - lo; }
};

// The offset to add to `incoming` so it clears everything in `taken`. Zero
// when nothing is placed yet — the first item keeps its natural coordinates,
// which is what makes a single-part assembly look like the part.
inline double ClearOffset(const std::vector<Span>& taken, Span incoming,
                          double minGap = 1.0) {
  double right = -1e30;
  for (const Span& s : taken)
    right = std::max(right, s.hi);
  if (right <= -1e29)
    return 0.0;
  const double gap = std::max(minGap, incoming.size() * 0.25);
  return right + gap - incoming.lo;
}

// Does `a` overlap `b` at all? Used to decide when a row is full and the next
// item wraps (drawing sheets) rather than running off the edge.
inline bool Overlaps(Span a, Span b) { return a.lo < b.hi && b.lo < a.hi; }

} // namespace ui
