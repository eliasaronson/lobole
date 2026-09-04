#include "core/features/FrameFeature.h"

#include "core/FrameMath.h"
#include "core/Measure.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace {

// Feature ids of the bodies present, for a refusal that names what exists.
std::string BodyProducers(const std::vector<Body>& bodies) {
  std::string s;
  size_t n = 0;
  for (const Body& b : bodies) {
    if (n++ == 8) {
      s += ", ...";
      break;
    }
    if (!s.empty())
      s += ", ";
    s += std::to_string(b.createdBy) + " (" + b.name + ")";
  }
  return s.empty() ? "none" : s;
}

} // namespace

FrameFeature::FrameFeature(const std::string& name, FrameKind kind)
    : kind(kind) {
  m_name = name;
}

bool frame::ElementFrame(const std::vector<Body>& bodies, const ElementRef& ref,
                         glm::vec3& origin, glm::vec3& axis, glm::vec3& xHint,
                         bool& circular, std::string& why) {
  circular = false;
  const Body* owner = nullptr;
  for (const Body& b : bodies) {
    if (b.createdBy != ref.featureId)
      continue;
    owner = &b;
    // A face name is tried before an edge name of the same spelling; the
    // two maps never share names in practice, but the order is explicit.
    if (const auto faceIdx = b.elementMap.Resolve(ref.name)) {
      glm::vec3 o, primary, x;
      if (b.shape.FacePlane(*faceIdx, o, primary, x)) {
        origin = o;
        axis = primary;
        xHint = x;
        return true;
      }
      if (b.shape.FaceAxis(*faceIdx, o, primary, x)) {
        origin = o;
        axis = primary;
        xHint = x;
        circular = true;
        return true;
      }
      why = "face '" + ref.name +
            "' has no frame (planar, cylindrical, conical and spherical "
            "faces do)";
      return false;
    }
    if (const auto edgeIdx = b.elementMap.ResolveEdge(ref.name)) {
      const auto info = Measure::DescribeEdgeAt(b, *edgeIdx);
      glm::vec3 p;
      if (!info || !info->hasAxis || !b.shape.EdgeSnapPoint(*edgeIdx, p)) {
        why = "edge '" + ref.name +
              "' is not circular — a frame needs a circle or arc (centre + "
              "axis); a line has no centre";
        return false;
      }
      origin = p;
      axis = info->axis;
      xHint = glm::vec3(0.0f); // Build picks a deterministic perpendicular
      circular = true;
      return true;
    }
  }
  if (owner == nullptr) {
    why = "no body created by feature " + std::to_string(ref.featureId) +
          "; bodies here are created by: " + BodyProducers(bodies);
    return false;
  }
  // Owner found, name gone: the usual cause is a later feature extending
  // the face's history name — offer the names that keep it as a prefix.
  std::string similar;
  for (const auto& [idx, name] : owner->elementMap.names) {
    (void)idx;
    if (name.size() > ref.name.size() &&
        name.compare(0, ref.name.size(), ref.name) == 0) {
      if (!similar.empty())
        similar += ", ";
      similar += name;
    }
  }
  why = "element '" + ref.name + "' not found on the body of feature " +
        std::to_string(ref.featureId) +
        (similar.empty() ? std::string(" (list_elements names what is there)")
                         : "; similar: " + similar);
  return false;
}

bool FrameFeature::Execute(RegenContext& ctx) {
  switch (kind) {
  case FrameKind::Explicit: {
    if (glm::dot(zAxis, zAxis) < 1e-12f) {
      SetError("z_axis is zero");
      return false;
    }
    m_frame =
        frame::Build(glm::vec3(origin[0], origin[1], origin[2]), xAxis, zAxis);
    return true;
  }
  case FrameKind::FromElement: {
    if (elements.empty()) {
      SetError("no element named — a frame from elements needs one face or "
               "edge, or several circular ones for a bolt circle");
      return false;
    }
    if (elements.size() == 1) {
      glm::vec3 o, z, x;
      bool circular = false;
      std::string why;
      if (!frame::ElementFrame(ctx.bodies, elements[0], o, z, x, circular,
                               why)) {
        SetError(why);
        return false;
      }
      m_frame = frame::Build(o, x, flip ? -z : z);
      return true;
    }
    // A bolt circle: every element circular, axes parallel, origin at the
    // mean of the centres, X toward the first hole so the frame is oriented
    // by the pattern and not by an accident of the kernel.
    glm::vec3 sum(0.0f), z0(0.0f), first(0.0f);
    for (size_t i = 0; i < elements.size(); i++) {
      glm::vec3 o, z, x;
      bool circular = false;
      std::string why;
      if (!frame::ElementFrame(ctx.bodies, elements[i], o, z, x, circular,
                               why)) {
        SetError(why);
        return false;
      }
      if (!circular) {
        SetError("element " + std::to_string(i + 1) + " ('" + elements[i].name +
                 "') is planar — a bolt circle is made of circular edges or "
                 "cylindrical faces");
        return false;
      }
      if (i == 0) {
        z0 = z;
        first = o;
      } else {
        // An axis has no sign; align each with the first before comparing.
        if (glm::dot(z, z0) < 0.0f)
          z = -z;
        const float cosang = glm::dot(glm::normalize(z), glm::normalize(z0));
        if (cosang < 0.9962f) { // > 5 degrees apart
          SetError("element " + std::to_string(i + 1) + " ('" +
                   elements[i].name + "') is not parallel to the first (" +
                   std::to_string(std::acos(std::min(1.0f, cosang)) * 180.0f /
                                  3.14159265f) +
                   " degrees off) — a bolt circle's holes share one axis "
                   "direction");
          return false;
        }
      }
      sum += o;
    }
    const glm::vec3 centre = sum / static_cast<float>(elements.size());
    const glm::vec3 toFirst = first - centre;
    if (glm::dot(toFirst, toFirst) < 1e-12f) {
      SetError("the elements are concentric — a bolt circle's centres must "
               "differ (for one hole, name that hole alone)");
      return false;
    }
    m_frame = frame::Build(centre, toFirst, flip ? -z0 : z0);
    return true;
  }
  case FrameKind::Offset: {
    glm::mat4 parent(1.0f);
    if (parentId != 0) {
      const Feature* pf = ctx.FindById(parentId);
      const auto* parentFrame = dynamic_cast<const FrameFeature*>(pf);
      if (parentFrame == nullptr) {
        std::string frames;
        for (size_t i = 0; i < ctx.currentIndex; i++)
          if (dynamic_cast<const FrameFeature*>(ctx.features[i].get())) {
            if (!frames.empty())
              frames += ", ";
            frames += std::to_string(ctx.features[i]->GetId()) + " (" +
                      ctx.features[i]->GetName() + ")";
          }
        SetError("parent " + std::to_string(parentId) +
                 " is not an earlier Frame; frames before this one: " +
                 (frames.empty() ? std::string("none (0 = the part origin)")
                                 : frames));
        return false;
      }
      parent = parentFrame->Matrix();
    }
    const glm::mat4 t = glm::translate(
        glm::mat4(1.0f), glm::vec3(offset[0], offset[1], offset[2]));
    const glm::mat4 r =
        glm::rotate(glm::mat4(1.0f), glm::radians(float(rotationDeg[0])),
                    glm::vec3(1, 0, 0)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(float(rotationDeg[1])),
                    glm::vec3(0, 1, 0)) *
        glm::rotate(glm::mat4(1.0f), glm::radians(float(rotationDeg[2])),
                    glm::vec3(0, 0, 1));
    const glm::mat4 m = parent * t * r;
    // Re-orthonormalize through Build so float drift never accumulates
    // down a chain of offsets.
    m_frame = frame::Build(glm::vec3(m[3]), glm::vec3(m[0]), glm::vec3(m[2]));
    return true;
  }
  }
  SetError("unknown frame kind");
  return false;
}
