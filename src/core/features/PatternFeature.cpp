// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/features/PatternFeature.h"

#include "common/Logger.h"
#include "core/BodyMode.h"
#include "core/ElementOrigin.h"
#include "core/Expression.h"
#include "core/features/DatumPlaneFeature.h" // ResolveParentPlane

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

PatternFeature::PatternFeature(const std::string& name, PatternKind kind,
                               uint64_t sourceId)
    : kind(kind), sourceId(sourceId) {
  m_name = name;
}

namespace {

// One instance's names: the source's, with an instance suffix ("4/g7" ->
// "4/g7#2", a mirror's "#m"), so a face of the third bar is as referenceable
// as one of the first. topo::DecodeElementName reads the suffix as the
// element's pattern instance.
ElementMap Suffixed(const ElementMap& src, const std::string& tag) {
  ElementMap m;
  for (const auto& [i, n] : src.names)
    m.names[i] = n + "#" + tag;
  for (const auto& [i, n] : src.edgeNames)
    m.edgeNames[i] = n + "#" + tag;
  return m;
}

// What the seed's features wrote on its faces -- a Hole's HoleMark, a tapped
// Hole's or a Thread's ThreadMark and ThreadRegion -- for the copies to carry.
// The marks hold no geometry, only the face's name: whoever reads one finds
// the axis and the position from the face itself, so a copy's mark is the
// seed's with the copy's face. Without them CAM drilled a patterned tapped
// hole as a plain bore and listed only the seed as uncut, and a screw in a
// copy clashed with its thread.
struct SeedMarks {
  std::vector<HoleMark> holes;
  std::vector<ThreadMark> threads;
  std::vector<ThreadRegion> regions;
  bool Empty() const {
    return holes.empty() && threads.empty() && regions.empty();
  }
};

// A face's identity across copies: its name with the instance suffixes
// taken out and the modification steps after it dropped ("4/p7/bore#2;M9"
// -> "4/p7/bore"), and the last instance suffix ("2"), empty on the seed.
std::string CopyKey(const std::string& name, std::string* tag) {
  std::string out;
  tag->clear();
  int depth = 0; // inside a piece qualifier, whose names are opaque
  size_t step = std::string::npos;
  for (size_t i = 0; i < name.size();) {
    const char c = name[i];
    if (c == '[' || (c == ']' && depth > 0)) {
      depth += c == '[' ? 1 : -1;
      out += name[i++];
      continue;
    }
    if (depth > 0 || c != '#') {
      if (depth == 0 && step == std::string::npos && c == ';' &&
          i + 1 < name.size() && name[i + 1] == 'M')
        step = out.size();
      out += name[i++];
      continue;
    }
    size_t j = i + 1;
    while (j < name.size() && name[j] != ';' && name[j] != ',' &&
           name[j] != '#' && name[j] != '[')
      j++;
    *tag = name.substr(i + 1, j - i - 1);
    i = j;
  }
  if (step != std::string::npos)
    out.resize(step);
  return out;
}

// The instance suffixes a set of copies' names carry.
void CollectTags(const ElementMap& map, std::set<std::string>& tags) {
  std::string tag;
  for (const auto& [index, name] : map.names) {
    CopyKey(name, &tag);
    if (!tag.empty())
      tags.insert(tag);
  }
}

// Each body's faces that are a copy (by `tags`, and the seed itself when
// `seed`) of a marked seed face get the seed's mark, on that face. A face
// already marked keeps its own.
void CarryMarks(std::vector<Body>& bodies, const SeedMarks& marks,
                const std::set<std::string>& tags, bool seed,
                uint64_t onlyCreatedBy) {
  if (marks.Empty())
    return;
  const auto keyOf = [](const std::string& n) {
    std::string t;
    return CopyKey(n, &t);
  };
  std::string tag;
  for (Body& body : bodies) {
    if (body.reference || !body.shape.IsValid() ||
        (onlyCreatedBy != 0 && body.createdBy != onlyCreatedBy))
      continue;
    for (const auto& [index, name] : body.elementMap.names) {
      const std::string key = CopyKey(name, &tag);
      if (tag.empty() ? !seed : tags.count(tag) == 0)
        continue;
      const ElementRef here{body.createdBy, name};
      const auto same = [&here](const ElementRef& r) {
        return r.featureId == here.featureId && r.name == here.name;
      };
      for (const HoleMark& h : marks.holes) {
        if (keyOf(h.bore.name) != key ||
            std::any_of(body.holes.begin(), body.holes.end(),
                        [&](const HoleMark& o) { return same(o.bore); }))
          continue;
        body.holes.push_back(h);
        body.holes.back().bore = here;
      }
      for (const ThreadMark& t : marks.threads) {
        if (keyOf(t.face.name) != key ||
            std::any_of(body.threads.begin(), body.threads.end(),
                        [&](const ThreadMark& o) { return same(o.face); }))
          continue;
        body.threads.push_back(t);
        body.threads.back().face = here;
      }
      for (const ThreadRegion& r : marks.regions) {
        if (keyOf(r.face.name) != key ||
            std::any_of(body.threadRegions.begin(), body.threadRegions.end(),
                        [&](const ThreadRegion& o) { return same(o.face); }))
          continue;
        body.threadRegions.push_back(r);
        body.threadRegions.back().face = here;
      }
    }
  }
}

} // namespace

bool PatternFeature::Execute(RegenContext& ctx) {
  // A step that lands every copy on the seed builds nothing while the
  // pattern reads as one that worked (a cut source only warned that its
  // copies removed nothing). `angle_deg` is the STEP between instances, so
  // six round a circle is 60 and 360 stacks all six on the first.
  if (kind != PatternKind::Mirror) {
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr)
      if (const auto v =
              ExpressionEngine::Default().EvaluateText(countExpr, *ctx.symbols))
        n = static_cast<int>(std::lround(*v));
    if (n > 1 && kind == PatternKind::Linear && std::abs(spacing) < 1e-9) {
      SetError("spacing 0 puts every copy on the seed");
      return false;
    }
    if (n > 1 && kind == PatternKind::Circular) {
      const double turn = std::fmod(std::abs(angleDeg), 360.0);
      if (turn < 1e-9 || 360.0 - turn < 1e-9) {
        char msg[200];
        std::snprintf(msg, sizeof msg,
                      "angle_deg %g puts every copy on the seed: it is the "
                      "step between instances (%d round a circle is %g)",
                      angleDeg, n, 360.0 / n);
        SetError(msg);
        return false;
      }
    }
  }
  // Collect the seed body/bodies produced by the referenced feature, each
  // with its names — an instance that arrives unnamed leaves unnamed, and a
  // face with no name cannot be picked, threaded, filleted or fitted.
  std::vector<std::pair<CADShape, ElementMap>> instances;
  for (const Body& b : ctx.bodies) {
    if (b.createdBy == sourceId) {
      // copy: the source stays until we replace it
      instances.emplace_back(b.shape, b.elementMap);
    }
  }
  if (instances.empty()) {
    // A CUT or ADD source leaves no body of its own behind to replicate —
    // replicate its TOOL instead: transformed copies of what the source just
    // swept, each applied in the source's mode (R14 for cuts). Patterned
    // holes used to refuse "source body not found", pushing every repeated
    // hole into one multi-loop sketch; so did a ring of teeth added to a
    // pinion face, which went in as one extrude per tooth.
    const Feature* src = ctx.FindById(sourceId);
    const CADShape* tool = src != nullptr ? src->ToolShape() : nullptr;
    if (tool != nullptr && tool->IsValid()) {
      const ElementMap* toolMap = src->ToolMap();
      return ExecuteToolPattern(ctx, *tool,
                                toolMap != nullptr ? *toolMap : ElementMap{},
                                src->ToolAdds());
    }
    SetError("source body (feature " + std::to_string(sourceId) +
             ") not found — did the source feature fail? (an Extrude that "
             "cuts or adds patterns its tool instead)");
    return false;
  }
  const size_t seedCount = instances.size();

  if (kind == PatternKind::Mirror) {
    const auto plane = ResolveParentPlane(ctx, planeId, basePlane);
    if (!plane) {
      SetError("mirror plane not found");
      return false;
    }
    const glm::dvec3 o = plane->GetOrigin();
    const glm::dvec3 n = plane->GetNormal();
    for (size_t i = 0; i < seedCount; i++) {
      CADShape reflected = instances[i].first;
      reflected.Mirror(o, n);
      instances.emplace_back(std::move(reflected),
                             Suffixed(instances[i].second, "m"));
    }
  } else {
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDir{1.0, 0.0, 0.0};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      SetError("pattern axis not found");
      return false;
    }
    // Resolve the count (expression over the parameter table, else literal).
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr) {
      const auto v =
          ExpressionEngine::Default().EvaluateText(countExpr, *ctx.symbols);
      if (v.has_value()) {
        n = static_cast<int>(std::lround(*v));
      }
    }
    n = std::max(1, n);

    for (size_t i = 0; i < seedCount; i++) {
      for (int k = 1; k < n; k++) {
        CADShape copy = instances[i].first;
        if (kind == PatternKind::Linear) {
          copy.Translate(axisDir * (k * spacing));
        } else { // Circular
          copy.RotateAbout(axisOrigin, axisDir,
                           glm::radians(static_cast<double>(k) * angleDeg));
        }
        instances.emplace_back(
            std::move(copy), Suffixed(instances[i].second, std::to_string(k)));
      }
    }
  }

  // The fuse makes new bodies and drops the seeds' marks: collect them
  // first, and give them back to the seed and every copy after.
  SeedMarks marks;
  for (const Body& b : ctx.bodies) {
    if (b.createdBy != sourceId)
      continue;
    marks.holes.insert(marks.holes.end(), b.holes.begin(), b.holes.end());
    marks.threads.insert(marks.threads.end(), b.threads.begin(),
                         b.threads.end());
    marks.regions.insert(marks.regions.end(), b.threadRegions.begin(),
                         b.threadRegions.end());
  }
  std::set<std::string> tags;
  for (size_t i = seedCount; i < instances.size(); i++)
    CollectTags(instances[i].second, tags);

  // The copies are the seed's matter: its material unless this pattern sets
  // its own (Document's stamp says the same after a cached regeneration).
  std::string material = m_material;
  if (material.empty())
    for (const Body& b : ctx.bodies)
      if (b.createdBy == sourceId) {
        material = b.material;
        break;
      }
  std::string fuseErr;
  const bool ok =
      FuseInstancesReplacingSource(ctx.bodies, sourceId, std::move(instances),
                                   m_name, material, GetId(), &fuseErr);
  if (!ok)
    SetError(fuseErr.empty() ? "pattern failed" : fuseErr);
  else
    CarryMarks(ctx.bodies, marks, tags, true, GetId());
  return ok;
}

bool PatternFeature::ExecuteToolPattern(RegenContext& ctx, const CADShape& tool,
                                        const ElementMap& toolMap, bool adds) {
  const auto suffixed = [&toolMap](const std::string& tag) {
    return Suffixed(toolMap, tag);
  };
  std::vector<std::pair<CADShape, ElementMap>> cutters;
  if (kind == PatternKind::Mirror) {
    const auto plane = ResolveParentPlane(ctx, planeId, basePlane);
    if (!plane) {
      SetError("mirror plane not found");
      return false;
    }
    CADShape reflected = tool;
    reflected.Mirror(plane->GetOrigin(), plane->GetNormal());
    cutters.emplace_back(std::move(reflected), suffixed("m"));
  } else {
    glm::dvec3 axisOrigin{0.0};
    glm::dvec3 axisDir{1.0, 0.0, 0.0};
    if (!ResolveAxis(ctx, axis, axisOrigin, axisDir)) {
      SetError("pattern axis not found");
      return false;
    }
    int n = count;
    if (!countExpr.empty() && ctx.symbols != nullptr) {
      const auto v =
          ExpressionEngine::Default().EvaluateText(countExpr, *ctx.symbols);
      if (v.has_value())
        n = static_cast<int>(std::lround(*v));
    }
    n = std::max(1, n);
    for (int k = 1; k < n; k++) { // k = 0 is the source cut itself
      CADShape copy = tool;
      if (kind == PatternKind::Linear)
        copy.Translate(axisDir * (k * spacing));
      else
        copy.RotateAbout(axisOrigin, axisDir,
                         glm::radians(static_cast<double>(k) * angleDeg));
      cutters.emplace_back(std::move(copy), suffixed(std::to_string(k)));
    }
  }
  // The source's marks (a Hole's, a tapped Hole's thread), wherever it
  // drilled them, for the copies' faces.
  SeedMarks marks;
  for (const Body& b : ctx.bodies) {
    for (const HoleMark& h : b.holes)
      if (h.featureId == sourceId)
        marks.holes.push_back(h);
    for (const ThreadMark& t : b.threads)
      if (t.featureId == sourceId)
        marks.threads.push_back(t);
    for (const ThreadRegion& r : b.threadRegions)
      if (topo::DecodeElementName(r.face.name).madeBy == sourceId)
        marks.regions.push_back(r);
  }
  std::set<std::string> tags;
  for (auto& [shape, map] : cutters) {
    CollectTags(map, tags);
    if (!ApplyBodyModeReporting(ctx.bodies, std::move(shape), std::move(map),
                                adds ? ExtrudeMode::Add : ExtrudeMode::Cut,
                                *this))
      return false;
  }
  CarryMarks(ctx.bodies, marks, tags, false, 0);
  return true;
}
