// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/DrawingValidate.h"

#include "core/drawing/DrawingBalloons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace drawing {
namespace {

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.1f", v);
  return buf;
}

bool RectsOverlap(const glm::dvec2& aMin, const glm::dvec2& aMax,
                  const glm::dvec2& bMin, const glm::dvec2& bMax) {
  // Strict: touching edges are not an overlap.
  return aMin.x < bMax.x && bMin.x < aMax.x && aMin.y < bMax.y &&
         bMin.y < aMax.y;
}

// Whether segment ab passes through the rectangle [mn, mx] (Liang-Barsky).
bool SegmentHitsRect(glm::dvec2 a, glm::dvec2 b, const glm::dvec2& mn,
                     const glm::dvec2& mx) {
  double t0 = 0.0, t1 = 1.0;
  const glm::dvec2 d = b - a;
  const double p[4] = {-d.x, d.x, -d.y, d.y};
  const double q[4] = {a.x - mn.x, mx.x - a.x, a.y - mn.y, mx.y - a.y};
  for (int i = 0; i < 4; ++i) {
    if (std::abs(p[i]) < 1e-12) {
      if (q[i] < 0.0)
        return false; // parallel and outside
      continue;
    }
    const double t = q[i] / p[i];
    if (p[i] < 0.0)
      t0 = std::max(t0, t);
    else
      t1 = std::min(t1, t);
    if (t0 > t1)
      return false;
  }
  return true;
}

// Whether the ray from `p` along an axis (`axis` 0 = x, 1 = y; `sign` +-1)
// crosses any of the view's curves.
bool RayHitsCurve(const SheetView& sv, const glm::dvec2& p, int axis,
                  double sign) {
  const int other = 1 - axis;
  for (const SheetCurve& c : sv.curves)
    for (size_t i = 1; i < c.points.size(); ++i) {
      const glm::dvec2 a = c.points[i - 1], b = c.points[i];
      if ((a[other] - p[other]) * (b[other] - p[other]) > 0.0)
        continue; // the segment is wholly to one side of the ray's line
      const double d = b[other] - a[other];
      const double at =
          std::abs(d) < 1e-12
              ? std::min(a[axis], b[axis])
              : a[axis] + (p[other] - a[other]) / d * (b[axis] - a[axis]);
      if (sign * (at - p[axis]) >= 0.0)
        return true;
    }
  return false;
}

// Whether a label box sits ON a view: over a line the view draws, or inside
// its outline (a line found every way from the label's centre) -- not merely
// inside its bounding box. A label in the corner of a disc's box covers
// nothing, and the box test flagged every callout placed beside a round
// part's rim as sitting on it.
bool LabelOnView(const SheetView& sv, const glm::dvec2& mn,
                 const glm::dvec2& mx) {
  if (!sv.hasGeometry || !RectsOverlap(mn, mx, sv.bboxMin, sv.bboxMax))
    return false;
  // The part's lines only: a section's hatching is interrupted by the text
  // knocked out of it, which is how drafting puts a value in a section.
  for (const SheetCurve& c : sv.curves)
    for (size_t i = 1; i < c.points.size(); ++i)
      if (SegmentHitsRect(c.points[i - 1], c.points[i], mn, mx))
        return true;
  if (sv.sectioned)
    return false; // inside a section is on its hatching, knocked out
  const glm::dvec2 centre = 0.5 * (mn + mx);
  return RayHitsCurve(sv, centre, 0, -1.0) &&
         RayHitsCurve(sv, centre, 0, 1.0) &&
         RayHitsCurve(sv, centre, 1, -1.0) && RayHitsCurve(sv, centre, 1, 1.0);
}

// How far `p` pokes outside [min, max]; empty string when inside.
std::string OutsideBy(const glm::dvec2& p, const glm::dvec2& min,
                      const glm::dvec2& max) {
  if (p.x < min.x)
    return Mm(min.x - p.x) + " mm past the left border";
  if (p.x > max.x)
    return Mm(p.x - max.x) + " mm past the right border";
  if (p.y < min.y)
    return Mm(min.y - p.y) + " mm below the border";
  if (p.y > max.y)
    return Mm(p.y - max.y) + " mm above the border";
  return {};
}

} // namespace

std::vector<DrawingIssue>
ValidateDrawing(const DrawingDocument& doc,
                const std::map<uint64_t, SheetView>& renders,
                const std::vector<ResolvedDim>& dims,
                const std::map<uint64_t, bool>& staleViews,
                const std::vector<ResolvedNote>& resolvedNotes) {
  std::vector<DrawingIssue> issues;
  const SheetChrome chrome = ChromeOf(doc.GetSheet());

  const auto viewName = [&](uint64_t id) {
    const DrawingView* v = doc.FindView(id);
    return v ? "view '" + v->name + "'" : "view " + std::to_string(id);
  };

  for (const DrawingView& v : doc.GetViews()) {
    const auto it = renders.find(v.id);
    if (it == renders.end() || it->second.solvedSourceVersion == 0) {
      issues.push_back({"unprojected_view", v.id, 0, 0,
                        viewName(v.id) + " has never been projected - call "
                                         "update_drawing"});
      continue;
    }
    const SheetView& sv = it->second;
    const auto stale = staleViews.find(v.id);
    if (stale != staleViews.end() && stale->second)
      issues.push_back({"stale_view", v.id, 0, 0,
                        viewName(v.id) +
                            " is stale - its source changed since the last "
                            "projection"});
    if (!sv.hasGeometry)
      continue;
    // Inside the border?
    if (sv.bboxMin.x < chrome.borderMin.x ||
        sv.bboxMin.y < chrome.borderMin.y ||
        sv.bboxMax.x > chrome.borderMax.x ||
        sv.bboxMax.y > chrome.borderMax.y) {
      std::string by =
          OutsideBy(sv.bboxMin, chrome.borderMin, chrome.borderMax);
      if (by.empty())
        by = OutsideBy(sv.bboxMax, chrome.borderMin, chrome.borderMax);
      issues.push_back({"view_outside_border", v.id, 0, 0,
                        viewName(v.id) + " extends " + by});
    }
    // Over the title block?
    if (RectsOverlap(sv.bboxMin, sv.bboxMax, chrome.titleBlockMin,
                     chrome.titleBlockMax))
      issues.push_back({"view_overlaps_title_block", v.id, 0, 0,
                        viewName(v.id) + " overlaps the title block"});
  }

  // Pairwise view overlaps, reported once per pair.
  const auto& views = doc.GetViews();
  for (size_t i = 0; i < views.size(); ++i) {
    const auto a = renders.find(views[i].id);
    if (a == renders.end() || !a->second.hasGeometry)
      continue;
    for (size_t j = i + 1; j < views.size(); ++j) {
      const auto b = renders.find(views[j].id);
      if (b == renders.end() || !b->second.hasGeometry)
        continue;
      if (RectsOverlap(a->second.bboxMin, a->second.bboxMax, b->second.bboxMin,
                       b->second.bboxMax))
        issues.push_back({"view_overlap", views[i].id, views[j].id, 0,
                          viewName(views[i].id) + " and " +
                              viewName(views[j].id) + " overlap on the sheet"});
    }
  }

  for (const ResolvedDim& d : dims) {
    if (!d.resolved) {
      issues.push_back({"dangling_dim", 0, 0, d.id,
                        "dim " + std::to_string(d.id) +
                            " no longer resolves - a referenced edge was "
                            "consumed upstream"});
      continue;
    }
    // Which points must stay on the paper depends on the kind's layout.
    std::vector<glm::dvec2> pts{d.labelPos};
    if (d.radial) {
      pts.push_back(d.rim);
    } else if (d.kind == DrawingDimKind::Distance ||
               d.kind == DrawingDimKind::DistanceX ||
               d.kind == DrawingDimKind::DistanceY) {
      pts.push_back(d.dim1);
      pts.push_back(d.dim2);
    }
    for (const glm::dvec2& p : pts) {
      const std::string by = OutsideBy(p, chrome.borderMin, chrome.borderMax);
      if (!by.empty()) {
        issues.push_back({"dim_outside_border", 0, 0, d.id,
                          "dim " + std::to_string(d.id) + " ('" + d.text +
                              "') lands " + by});
        break;
      }
    }
  }

  // Labels must not sit on a view, the title block, or each other (R12).
  // The box comes from the SAME LabelBox auto-placement avoids, so what
  // placement dodges is exactly what this flags — two estimates would
  // drift and one of them would lie.
  struct LabelRect {
    uint64_t id;
    std::string text;
    glm::dvec2 mn, mx;
  };
  std::vector<LabelRect> labels;
  for (const ResolvedDim& d : dims) {
    if (!d.resolved)
      continue;
    glm::dvec2 mn(0.0), mx(0.0);
    DimLabelBox(d, mn, mx);
    if (RectsOverlap(mn, mx, chrome.titleBlockMin, chrome.titleBlockMax))
      issues.push_back({"dim_over_title_block", 0, 0, d.id,
                        "dim " + std::to_string(d.id) + " ('" + d.text +
                            "') sits on the title block"});
    for (const auto& [vid, sv] : renders) {
      if (!LabelOnView(sv, mn, mx))
        continue;
      issues.push_back({"dim_label_over_view", vid, 0, d.id,
                        "dim " + std::to_string(d.id) + " ('" + d.text +
                            "') sits on " + viewName(vid)});
      break;
    }
    labels.push_back({d.id, d.text, mn, mx});
  }
  for (size_t i = 0; i < labels.size(); i++)
    for (size_t j = i + 1; j < labels.size(); j++)
      if (RectsOverlap(labels[i].mn, labels[i].mx, labels[j].mn, labels[j].mx))
        issues.push_back({"dim_labels_overlap", 0, 0, labels[i].id,
                          "dims " + std::to_string(labels[i].id) + " ('" +
                              labels[i].text + "') and " +
                              std::to_string(labels[j].id) + " ('" +
                              labels[j].text + "') overlap"});

  std::vector<ResolvedNote> notes = resolvedNotes;
  if (notes.empty())
    for (const auto& note : doc.GetNotes()) {
      const auto it = renders.find(note.viewId);
      notes.push_back(
          ResolveNote(note, it == renders.end() ? nullptr : &it->second));
    }
  std::vector<LabelRect> noteBoxes;
  for (const auto& note : notes) {
    const auto report = [&](const std::string& kind, const std::string& detail,
                            uint64_t view = 0) {
      issues.push_back({kind, view, 0, 0,
                        "note " + std::to_string(note.id) + ": " + detail,
                        note.id});
    };
    if (!note.resolved)
      report("unresolved_note", note.error);
    glm::dvec2 mn, mx;
    NoteBox(note, mn, mx);
    if (!OutsideBy(mn, chrome.borderMin, chrome.borderMax).empty() ||
        !OutsideBy(mx, chrome.borderMin, chrome.borderMax).empty())
      report("note_outside_border", "text extends outside the sheet border");
    if (RectsOverlap(mn, mx, chrome.titleBlockMin, chrome.titleBlockMax))
      report("note_over_title_block", "text overlaps the title block");
    for (const auto& [id, view] : renders)
      if (LabelOnView(view, mn, mx))
        report("note_over_view", "text overlaps " + viewName(id), id);
    for (const auto& label : labels)
      if (RectsOverlap(mn, mx, label.mn, label.mx))
        report("note_over_dimension",
               "text overlaps dimension " + std::to_string(label.id));
    for (const auto& other : noteBoxes)
      if (RectsOverlap(mn, mx, other.mn, other.mx))
        report("notes_overlap",
               "text overlaps note " + std::to_string(other.id));
    noteBoxes.push_back({note.id, note.text, mn, mx});
  }

  // Balloon leaders that cross each other: two parts' leaders crossing is
  // how a reader follows one to the other's part
  // (docs/drawings/README.md#balloon-layout).
  std::vector<std::pair<const ResolvedNote*, std::pair<glm::dvec2, glm::dvec2>>>
      leaders;
  for (const auto& note : notes) {
    if (note.style != NoteStyle::Balloon || !note.hasLeader)
      continue;
    glm::dvec2 mn, mx;
    NoteBox(note, mn, mx);
    leaders.push_back(
        {&note, BalloonLeader(note.pos, 0.5 * (mx.x - mn.x), note.anchor)});
  }
  for (size_t i = 0; i < leaders.size(); ++i)
    for (size_t j = i + 1; j < leaders.size(); ++j) {
      const auto& [a, b] = leaders[i].second;
      const auto& [c, d] = leaders[j].second;
      if (SegmentsCross(a, b, c, d))
        issues.push_back({"balloon_leaders_cross", 0, 0, 0,
                          "balloons " + leaders[i].first->text + " and " +
                              leaders[j].first->text +
                              ": their leaders cross (layout_balloons lays "
                              "them out so that none do)",
                          leaders[i].first->id});
    }

  return issues;
}

} // namespace drawing
