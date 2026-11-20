// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/DatumFrame.h"

#include "core/Document.h"
#include "core/features/DatumAxisFeature.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/features/FrameFeature.h"

#include <cmath>
#include <stdexcept>
#include <string>

namespace datumframe {

bool IsAnchor(const Feature& f) {
  return dynamic_cast<const FrameFeature*>(&f) != nullptr ||
         dynamic_cast<const DatumPlaneFeature*>(&f) != nullptr ||
         dynamic_cast<const DatumAxisFeature*>(&f) != nullptr;
}

namespace {

glm::dmat4 Columns(const glm::dvec3& x, const glm::dvec3& z,
                   const glm::dvec3& origin) {
  const glm::dvec3 zz = glm::normalize(z);
  const glm::dvec3 xx = glm::normalize(x - glm::dot(x, zz) * zz);
  const glm::dvec3 yy = glm::cross(zz, xx);
  glm::dmat4 m(1.0);
  m[0] = glm::dvec4(xx, 0.0);
  m[1] = glm::dvec4(yy, 0.0);
  m[2] = glm::dvec4(zz, 0.0);
  m[3] = glm::dvec4(origin, 1.0);
  return m;
}

} // namespace

glm::dmat4 Matrix(const Feature& f) {
  if (const auto* fr = dynamic_cast<const FrameFeature*>(&f))
    return fr->Matrix();
  if (const auto* dp = dynamic_cast<const DatumPlaneFeature*>(&f)) {
    const SketchPlane& p = dp->OutputPlane();
    return Columns(p.GetUAxis(), p.GetNormal(), p.GetOrigin());
  }
  if (const auto* da = dynamic_cast<const DatumAxisFeature*>(&f)) {
    const glm::dvec3 z = da->Direction();
    const glm::dvec3 x =
        std::abs(z.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return Columns(x, z, da->Origin());
  }
  return glm::dmat4(1.0);
}

const char* KindLabel(const Feature& f) {
  if (dynamic_cast<const DatumPlaneFeature*>(&f) != nullptr)
    return "datum plane";
  if (dynamic_cast<const DatumAxisFeature*>(&f) != nullptr)
    return "datum axis";
  return "frame";
}

const Feature* Find(const Document& part, const nlohmann::json& v,
                    std::string& listing) {
  const Feature* hit = nullptr;
  std::string named; // the ids of every anchor called `v`
  int nNamed = 0;
  listing.clear();
  for (const auto& f : part.GetFeatures()) {
    if (!IsAnchor(*f))
      continue;
    if (v.is_number_integer() && f->GetId() == v.get<uint64_t>())
      hit = f.get();
    if (v.is_string() && f->GetName() == v.get<std::string>()) {
      if (!hit)
        hit = f.get();
      named += (nNamed++ ? ", " : "") + std::to_string(f->GetId());
    }
    if (!listing.empty())
      listing += ", ";
    listing += std::to_string(f->GetId()) + " (" + f->GetName() + ", " +
               KindLabel(*f) + ")";
  }
  // A name names one anchor (docs/scripting/rpc.md#a-name-names-one-thing).
  if (nNamed > 1)
    throw std::runtime_error(std::to_string(nNamed) + " frames are named '" +
                             v.get<std::string>() + "' (ids " + named +
                             "), so the name does not say which: pass the id");
  return hit;
}

} // namespace datumframe
