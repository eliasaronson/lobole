#include "core/cam/CamEngrave.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/cam/CamToolpath.h" // BodyToSetup
#include "core/features/SketchFeature.h"
#include "core/sketch/SketchProfile.h" // BuildLoops: which curves are open
#include "core/sketch/SketchText.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <expected>
#include <limits>
#include <type_traits>

// The engraving's geometry, built in every configuration: the resolver is how
// a job NAMES a sketch curve (ui::TargetFor asks it), and the rest is
// arithmetic on the job document the panel shows with CAM off too. The
// generator that turns it into moves is CamEngrave.cpp, CAM builds only.
// docs/cam/operations.md#engrave

namespace cam {

namespace {

std::string Mm(double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

// The chord tolerance a curve is flattened to: the one every other CAM
// polyline uses (CamToolpathInternal.h kDeflection), so an engraved circle is
// no coarser than a profiled one.
constexpr double kChord = 0.02;

int SegmentsFor(double radius, double sweep) {
  if (radius <= 1e-9)
    return 1;
  const double c = std::clamp(1.0 - kChord / radius, -1.0, 1.0);
  const double step = std::max(2.0 * std::acos(c), 1e-3);
  return std::clamp(static_cast<int>(std::ceil(std::abs(sweep) / step)), 2,
                    4096);
}

std::vector<glm::dvec2> ArcUV(glm::dvec2 c, double r, double a0, double a1) {
  const int n = SegmentsFor(r, a1 - a0);
  std::vector<glm::dvec2> out;
  out.reserve(static_cast<size_t>(n) + 1);
  for (int i = 0; i <= n; i++) {
    const double a = a0 + (a1 - a0) * i / n;
    out.push_back(c + r * glm::dvec2(std::cos(a), std::sin(a)));
  }
  return out;
}

std::vector<glm::dvec2> ToDouble(const std::vector<glm::vec2>& pts) {
  std::vector<glm::dvec2> out;
  out.reserve(pts.size());
  for (const glm::vec2& p : pts)
    out.emplace_back(p);
  return out;
}

template <class> constexpr bool kNoStrokesYet = false;

// What one entity contributes, in its plane's UV. The ONE place an entity
// type is taught to be engraved: a new alternative in `sketch::Entity`
// fails to compile here until it says what its strokes are, rather than
// engraving nothing and saying so only at the machine. Stroke text arrives
// here as its glyphs' open strokes, with `textHeight` its cap height.
struct EntityStrokes {
  std::vector<std::vector<glm::dvec2>> strokes;
  std::vector<bool> closed;
  double textHeight = 0.0;
  std::string why;
};

EntityStrokes StrokesOf(const sketch::Entity& entity) {
  EntityStrokes out;
  std::visit(
      [&](const auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, sketch::Point>) {
          out.why = "is a point, and a point has no curve to follow";
        } else if constexpr (std::is_same_v<T, sketch::Line>) {
          out.strokes.push_back({glm::dvec2(e.a), glm::dvec2(e.b)});
          out.closed.push_back(false);
        } else if constexpr (std::is_same_v<T, sketch::Circle>) {
          out.strokes.push_back(ArcUV(glm::dvec2(e.center), e.radius, 0.0,
                                      glm::two_pi<double>()));
          out.closed.push_back(true);
        } else if constexpr (std::is_same_v<T, sketch::Arc>) {
          double end = e.endAngle;
          while (end <= e.startAngle)
            end += glm::two_pi<double>(); // CCW-always
          out.strokes.push_back(
              ArcUV(glm::dvec2(e.center), e.radius, e.startAngle, end));
          out.closed.push_back(false);
        } else if constexpr (std::is_same_v<T, sketch::Ellipse>) {
          const int n = SegmentsFor(e.rMajor, glm::two_pi<double>());
          std::vector<glm::dvec2> pts;
          for (int i = 0; i <= n; i++)
            pts.emplace_back(
                sketch::EllipsePointUV(e, glm::two_pi<double>() * i / n));
          out.strokes.push_back(std::move(pts));
          out.closed.push_back(true);
        } else if constexpr (std::is_same_v<T, sketch::EllipseArc>) {
          double end = e.endParam;
          while (end <= e.startParam)
            end += glm::two_pi<double>();
          out.strokes.push_back(ToDouble(sketch::EllipseArcSampleUV(
              e, SegmentsFor(e.rMajor, end - e.startParam))));
          out.closed.push_back(false);
        } else if constexpr (std::is_same_v<T, sketch::BSpline>) {
          std::vector<glm::dvec2> pts =
              ToDouble(sketch::BSplineSampleUV(e, 32));
          const bool closed =
              pts.size() > 2 && glm::length(pts.front() - pts.back()) < 1e-6;
          out.strokes.push_back(std::move(pts));
          out.closed.push_back(closed);
        } else if constexpr (std::is_same_v<T, sketch::Text>) {
          // A label is every contour of every glyph: a stroke font's
          // pen-down strokes (open unless one returns to its start), an
          // outline font's closed outlines. Laid out from its fields the
          // same way the sketch draws it, and a label that cannot be laid
          // out -- an unknown font, a missing glyph -- is refused with the
          // layout's own reason, never engraved as nothing.
          const std::expected<sketch::TextGeometry, std::string> geometry =
              sketch::BuildTextGeometry(e);
          if (!geometry) {
            out.why = "cannot be laid out: " + geometry.error();
            return;
          }
          for (const sketch::TextContour& c : geometry->contours) {
            std::vector<glm::dvec2> pts;
            for (const sketch::Entity& segment : c.segments) {
              const EntityStrokes piece = StrokesOf(segment);
              for (const std::vector<glm::dvec2>& s : piece.strokes)
                for (const glm::dvec2& p : s)
                  if (pts.empty() || glm::length(p - pts.back()) > 1e-9)
                    pts.push_back(p);
            }
            if (pts.size() < 2)
              continue;
            out.strokes.push_back(std::move(pts));
            out.closed.push_back(c.closed);
          }
          // The groove a label is derived with is its cap height / 8.
          out.textHeight = e.height;
        } else {
          static_assert(kNoStrokesYet<T>,
                        "a sketch entity type the engraving resolver does "
                        "not know: say what its strokes are here "
                        "(docs/cam/operations.md#engrave)");
        }
      },
      entity);
  return out;
}

// "g<digits>" -> the id, or 0.
uint32_t EntityIdOf(const std::string& name) {
  if (name.size() < 2 || name[0] != 'g')
    return 0;
  uint64_t v = 0;
  for (size_t i = 1; i < name.size(); i++) {
    if (name[i] < '0' || name[i] > '9')
      return 0;
    v = v * 10 + static_cast<uint64_t>(name[i] - '0');
    if (v > std::numeric_limits<uint32_t>::max())
      return 0;
  }
  return static_cast<uint32_t>(v);
}

} // namespace

SketchCurves SketchEntityCurves(const Document& doc, const ElementRef& ref) {
  SketchCurves out;
  out.ref = ref;
  const Feature* f = doc.FindFeatureById(ref.featureId);
  if (f == nullptr) {
    out.why = "this part has no feature " + std::to_string(ref.featureId);
    return out;
  }
  const auto* sketch = dynamic_cast<const SketchFeature*>(f);
  if (sketch == nullptr) {
    out.why = "'" + f->GetName() + "' is a " + f->TypeName() +
              ", not a sketch: an engraving follows the curves of a sketch "
              "entity, {feature_id: <sketch>, element: \"g<entity id>\"}";
    return out;
  }
  const uint32_t id = EntityIdOf(ref.name);
  if (id == 0) {
    out.why = "'" + ref.name + "' is not a sketch entity of '" +
              sketch->GetName() + "': name one as g<entity id>";
    return out;
  }
  const size_t at = sketch->IndexOfEntity(id);
  if (at == SIZE_MAX) {
    out.why = "sketch '" + sketch->GetName() + "' has no entity " + ref.name;
    return out;
  }
  if (sketch->IsSuppressed()) {
    out.why = "sketch '" + sketch->GetName() + "' is suppressed";
    return out;
  }
  const sketch::Entity& entity = sketch->entities[at];
  if (sketch::IsConstruction(entity)) {
    out.why = ref.name + " of sketch '" + sketch->GetName() +
              "' is construction geometry, which is never cut";
    return out;
  }
  EntityStrokes uv = StrokesOf(entity);
  if (!uv.why.empty()) {
    out.why = ref.name + " of sketch '" + sketch->GetName() + "' " + uv.why;
    return out;
  }
  const SketchPlane& plane = sketch->plane;
  const glm::dvec3 o(plane.GetOrigin()), u(plane.GetUAxis()),
      v(plane.GetVAxis());
  out.planeOrigin = o;
  out.planeNormal = glm::normalize(glm::dvec3(plane.GetNormal()));
  out.textHeight = uv.textHeight;
  for (size_t s = 0; s < uv.strokes.size(); s++) {
    SketchStroke stroke;
    stroke.closed = uv.closed[s];
    for (const glm::dvec2& p : uv.strokes[s]) {
      const glm::dvec3 w = o + u * p.x + v * p.y;
      if (stroke.pts.empty() || glm::length(w - stroke.pts.back()) > 1e-9)
        stroke.pts.push_back(w);
    }
    if (stroke.pts.size() >= 2)
      out.strokes.push_back(std::move(stroke));
  }
  if (out.strokes.empty())
    out.why = ref.name + " of sketch '" + sketch->GetName() +
              "' has no length to follow";
  return out;
}

std::vector<ElementRef> EngravableEntities(const Document& doc) {
  std::vector<ElementRef> out;
  for (const std::unique_ptr<Feature>& f : doc.GetFeatures()) {
    const auto* sketch = dynamic_cast<const SketchFeature*>(f.get());
    if (sketch == nullptr || sketch->IsSuppressed())
      continue;
    // Which entities close a profile: the rest are open curves.
    std::vector<uint32_t> inLoops;
    for (const sketch::Loop& loop : sketch::BuildLoops(sketch->entities))
      for (const uint32_t id : sketch::LoopEntityIds(loop))
        inLoops.push_back(id);
    const auto closesAProfile = [&inLoops](uint32_t id) {
      return std::find(inLoops.begin(), inLoops.end(), id) != inLoops.end();
    };
    bool marking = false;
    std::vector<ElementRef> cuttable;
    for (const sketch::Entity& e : sketch->entities) {
      if (sketch::IsConstruction(e) || std::holds_alternative<sketch::Point>(e))
        continue;
      const uint32_t id = sketch::EntityId(e);
      if (std::holds_alternative<sketch::Text>(e) || !closesAProfile(id))
        marking = true;
      cuttable.push_back(ElementRef{sketch->GetId(), "g" + std::to_string(id)});
    }
    if (marking)
      out.insert(out.end(), cuttable.begin(), cuttable.end());
  }
  return out;
}

const SketchCurves* FindSketchCurves(const std::vector<SketchCurves>& curves,
                                     const Target& t) {
  for (const SketchCurves& c : curves)
    if (c.ref.featureId == t.ref.featureId && c.ref.name == t.ref.name &&
        (t.jobPartId == 0 || c.jobPartId == 0 || c.jobPartId == t.jobPartId))
      return &c;
  return nullptr;
}

EngraveGroove EngraveGrooveFor(const Operation& op, const Tool& tool) {
  EngraveGroove g;
  const std::string who = "tool " + std::to_string(tool.number);
  const double r = tool.diameter * 0.5;
  const double width = op.engraveWidth, depth = op.engraveDepth;
  if (width < 0.0 || depth < 0.0) {
    g.why = "an engraving's width_mm and depth_mm are sizes, and cannot be "
            "negative";
    return g;
  }
  switch (tool.type) {
  case ToolType::Drill:
    g.why = who + " is a drill: its point is not an engraving tool. Use a "
                  "V-bit, or a flat or ball end mill with a depth_mm";
    return g;
  case ToolType::ThreadMill:
    g.why = who + " is a thread mill: it cuts threads and nothing else. Use "
                  "a V-bit, or a flat or ball end mill with a depth_mm";
    return g;
  case ToolType::VBit: {
    const double cone = TipHeight(tool, r);
    if (cone < 1e-6) {
      g.why = who + "'s cone has no height at " + Mm(tool.tipAngleDeg) +
              " degrees included: it cannot cut a groove";
      return g;
    }
    if (width > 0.0) {
      if (width > tool.diameter + 1e-9) {
        g.why = "a " + Mm(width) + " mm groove is wider than " + who +
                "'s cone, which is " + Mm(tool.diameter) +
                " mm across where it ends (" + Mm(cone) +
                " mm up): use a wider V-bit, or a narrower groove";
        return g;
      }
      const double d = TipHeight(tool, width * 0.5);
      if (depth > 0.0 && std::abs(depth - d) > 1e-6) {
        g.why = "width_mm " + Mm(width) + " and depth_mm " + Mm(depth) +
                " are two different grooves for " + who + " (" + Mm(width) +
                " mm wide is " + Mm(d) + " mm deep): state one of them";
        return g;
      }
      g.depth = d;
      g.width = width;
      g.ok = true;
      return g;
    }
    if (depth > 0.0) {
      if (depth > cone + 1e-9) {
        g.why = "a " + Mm(depth) + " mm deep groove is past " + who +
                "'s cone, which is " + Mm(cone) +
                " mm tall: the shank would cut its walls. Use a wider "
                "V-bit, or a shallower groove";
        return g;
      }
      g.depth = depth;
      g.width = 2.0 * depth * std::tan(glm::radians(tool.tipAngleDeg * 0.5));
      g.ok = true;
      return g;
    }
    g.why = "an engraving needs its groove: width_mm (at the surface) or "
            "depth_mm";
    return g;
  }
  case ToolType::Flat:
  case ToolType::Ball:
    break;
  }
  if (width > 0.0) {
    g.why = who + " is a " +
            (tool.type == ToolType::Ball ? "ball" : "flat end mill") +
            ", whose groove is as wide as its depth makes it: state depth_mm, "
            "not width_mm (a width is a V-bit's)";
    return g;
  }
  if (depth <= 0.0) {
    g.why = "an engraving with " + who + " needs a depth_mm";
    return g;
  }
  g.depth = depth;
  if (tool.type == ToolType::Ball && depth < r)
    g.width = 2.0 * std::sqrt(std::max(0.0, r * r - (r - depth) * (r - depth)));
  else
    g.width = tool.diameter;
  g.ok = true;
  return g;
}

double DefaultEngraveWidth(const Tool& tool, double textHeight) {
  const double w = textHeight > 0.0 ? textHeight / 8.0 : tool.diameter / 4.0;
  return std::min(w, tool.diameter);
}

double DefaultEngraveDepth(const Tool& tool) { return tool.diameter / 4.0; }

EngraveFrame EngraveFrameOf(const Setup& setup, const SketchCurves& curves) {
  EngraveFrame out;
  const glm::dmat4 m = BodyToSetup(setup, curves.jobPartId);
  const glm::dvec3 n =
      glm::normalize(glm::dmat3(m) * glm::normalize(curves.planeNormal));
  // A sketch plane is exact, so the tolerance is arithmetic, not a policy: a
  // twentieth of a degree.
  if (n.z < std::cos(glm::radians(0.05))) {
    const double deg = glm::degrees(std::acos(std::clamp(n.z, -1.0, 1.0)));
    out.why = "the plane of sketch entity " + curves.ref.name + " faces " +
              Mm(deg) +
              " degrees away from the spindle in this setup, and an "
              "engraving cuts straight down into it: lay that face up "
              "(lay_face_up), or turn it up with index_a_deg";
    return out;
  }
  out.surfaceZ = (m * glm::dvec4(curves.planeOrigin, 1.0)).z;
  for (const SketchStroke& s : curves.strokes) {
    std::vector<glm::dvec2> pts;
    pts.reserve(s.pts.size());
    for (const glm::dvec3& p : s.pts) {
      const glm::dvec4 q = m * glm::dvec4(p, 1.0);
      pts.emplace_back(q.x, q.y);
    }
    if (s.closed && glm::length(pts.front() - pts.back()) > 1e-9)
      pts.push_back(pts.front());
    out.strokes.push_back(std::move(pts));
    out.closed.push_back(s.closed);
  }
  out.ok = true;
  return out;
}

std::string EngraveSurfaceRefusal(const std::vector<Body>& framed,
                                  const EngraveFrame& frame) {
  // How far either side of the plane to look: finer than any groove worth
  // cutting, coarser than the float in a sketch plane.
  constexpr double kProbe = 0.05;
  // Along the strokes, not only at their vertices: a line crossing a hole
  // has material under both ends. At most this many per target, spread
  // evenly, so a long label costs a bounded number of classifications.
  constexpr size_t kMaxProbes = 240;
  std::vector<glm::dvec2> at;
  double total = 0.0;
  for (const std::vector<glm::dvec2>& s : frame.strokes)
    for (size_t i = 1; i < s.size(); i++)
      total += glm::length(s[i] - s[i - 1]);
  const double step = std::max(total / static_cast<double>(kMaxProbes), 1e-3);
  for (const std::vector<glm::dvec2>& s : frame.strokes) {
    if (s.empty())
      continue;
    at.push_back(s.front());
    double carry = 0.0;
    for (size_t i = 1; i < s.size(); i++) {
      const glm::dvec2 a = s[i - 1], b = s[i];
      const double len = glm::length(b - a);
      double t = step - carry;
      while (t < len) {
        at.push_back(a + (b - a) * (t / len));
        t += step;
      }
      carry = len - (t - step);
    }
    at.push_back(s.back());
  }
  // Every probe in one batch per body: one classifier each, not one per
  // point (CADShape::ContainsPoints).
  std::vector<glm::vec3> probes;
  probes.reserve(at.size() * 2);
  for (const glm::dvec2& p : at)
    for (const double dz : {-kProbe, kProbe})
      probes.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y),
                          static_cast<float>(frame.surfaceZ + dz));
  std::vector<bool> inside(probes.size(), false);
  for (const Body& b : framed) {
    if (!b.shape.IsValid())
      continue;
    const std::vector<bool> in = b.shape.ContainsPoints(probes, 1e-6);
    for (size_t i = 0; i < in.size(); i++)
      inside[i] = inside[i] || in[i];
  }
  for (size_t k = 0; k < at.size(); k++) {
    const glm::dvec2& p = at[k];
    const bool under = inside[2 * k];
    const bool over = inside[2 * k + 1];
    if (under && !over)
      continue;
    return std::string("the curve is not on the part's surface at X") +
           Mm(p.x) + " Y" + Mm(p.y) + ": " +
           (over ? "it lies INSIDE the material there"
                 : "there is nothing under it there") +
           " (the sketch plane is at Z" + Mm(frame.surfaceZ) +
           " in this setup). Sketch it on the face it engraves, and keep "
           "it on that face";
  }
  return {};
}

std::vector<StrokeRun>
OrderStrokes(const std::vector<std::vector<glm::dvec2>>& strokes,
             const std::vector<bool>& closed, const glm::dvec2& from,
             bool optimise) {
  std::vector<StrokeRun> runs;
  const auto isClosed = [&](size_t i) {
    return i < closed.size() && closed[i];
  };
  if (!optimise) {
    for (size_t i = 0; i < strokes.size(); i++)
      if (!strokes[i].empty())
        runs.push_back({strokes[i], i, false});
    return runs;
  }
  // Nearest neighbour.
  std::vector<bool> used(strokes.size(), false);
  glm::dvec2 at = from;
  for (size_t n = 0; n < strokes.size(); n++) {
    double best = std::numeric_limits<double>::max();
    size_t which = SIZE_MAX;
    bool rev = false;
    size_t startAt = 0;
    for (size_t i = 0; i < strokes.size(); i++) {
      if (used[i] || strokes[i].empty())
        continue;
      const std::vector<glm::dvec2>& s = strokes[i];
      if (isClosed(i)) {
        for (size_t k = 0; k < s.size(); k++)
          if (const double d = glm::length(s[k] - at); d < best - 1e-12) {
            best = d;
            which = i;
            rev = false;
            startAt = k;
          }
      } else {
        const double d0 = glm::length(s.front() - at);
        const double d1 = glm::length(s.back() - at);
        if (d0 < best - 1e-12) {
          best = d0;
          which = i;
          rev = false;
        }
        if (d1 < best - 1e-12) {
          best = d1;
          which = i;
          rev = true;
        }
      }
    }
    if (which == SIZE_MAX)
      break;
    used[which] = true;
    StrokeRun run{strokes[which], which, rev};
    if (isClosed(which) && startAt != 0) {
      // A closed loop begins where the tool is: rotate it (its last point
      // repeats its first, so drop that and close it again).
      std::vector<glm::dvec2> ring(run.pts.begin(), run.pts.end() - 1);
      std::rotate(ring.begin(),
                  ring.begin() +
                      static_cast<std::ptrdiff_t>(
                          startAt % std::max<size_t>(1, ring.size())),
                  ring.end());
      ring.push_back(ring.front());
      run.pts = std::move(ring);
    }
    if (rev)
      std::reverse(run.pts.begin(), run.pts.end());
    at = run.pts.back();
    runs.push_back(std::move(run));
  }
  // 2-opt over the sequence: reversing a stretch of it turns every stroke in
  // the stretch round, so only its two joins change length.
  const auto startOf = [&](size_t i) { return runs[i].pts.front(); };
  const auto endOf = [&](size_t i) { return runs[i].pts.back(); };
  for (int round = 0; round < 64; round++) {
    bool improved = false;
    for (size_t i = 0; i < runs.size(); i++) {
      const glm::dvec2 before = i == 0 ? from : endOf(i - 1);
      for (size_t j = i + 1; j < runs.size(); j++) {
        const bool last = j + 1 == runs.size();
        const double old =
            glm::length(startOf(i) - before) +
            (last ? 0.0 : glm::length(startOf(j + 1) - endOf(j)));
        const double now =
            glm::length(endOf(j) - before) +
            (last ? 0.0 : glm::length(startOf(j + 1) - startOf(i)));
        if (now < old - 1e-9) {
          std::reverse(runs.begin() + static_cast<std::ptrdiff_t>(i),
                       runs.begin() + static_cast<std::ptrdiff_t>(j) + 1);
          for (size_t k = i; k <= j; k++) {
            std::reverse(runs[k].pts.begin(), runs[k].pts.end());
            runs[k].reversed = !runs[k].reversed;
          }
          improved = true;
        }
      }
    }
    if (!improved)
      break;
  }
  return runs;
}

double StrokeTravel(const std::vector<StrokeRun>& runs,
                    const glm::dvec2& from) {
  double travel = 0.0;
  glm::dvec2 at = from;
  for (const StrokeRun& r : runs) {
    travel += glm::length(r.pts.front() - at);
    at = r.pts.back();
  }
  return travel;
}

} // namespace cam
