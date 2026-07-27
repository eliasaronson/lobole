#include "core/drawing/DrawingDocument.h"

#include <algorithm>

uint64_t DrawingDocument::AddView(DrawingView view) {
  view.id = m_nextId++;
  m_views.push_back(std::move(view));
  BumpVersion();
  return m_views.back().id;
}

bool DrawingDocument::RemoveView(uint64_t id) {
  const auto it =
      std::find_if(m_views.begin(), m_views.end(),
                   [id](const DrawingView& v) { return v.id == id; });
  if (it == m_views.end())
    return false;
  m_views.erase(it);
  // A dimension whose view is gone has nothing to measure — drop it too.
  m_dims.erase(
      std::remove_if(m_dims.begin(), m_dims.end(),
                     [id](const DrawingDim& d) { return d.viewId == id; }),
      m_dims.end());
  BumpVersion();
  return true;
}

DrawingView* DrawingDocument::FindView(uint64_t id) {
  const auto it =
      std::find_if(m_views.begin(), m_views.end(),
                   [id](const DrawingView& v) { return v.id == id; });
  return it == m_views.end() ? nullptr : &*it;
}

const DrawingView* DrawingDocument::FindView(uint64_t id) const {
  const auto it =
      std::find_if(m_views.begin(), m_views.end(),
                   [id](const DrawingView& v) { return v.id == id; });
  return it == m_views.end() ? nullptr : &*it;
}

uint64_t DrawingDocument::AddDim(DrawingDim dim) {
  dim.id = m_nextId++;
  m_dims.push_back(std::move(dim));
  BumpVersion();
  return m_dims.back().id;
}

bool DrawingDocument::RemoveDim(uint64_t id) {
  const auto it =
      std::find_if(m_dims.begin(), m_dims.end(),
                   [id](const DrawingDim& d) { return d.id == id; });
  if (it == m_dims.end())
    return false;
  m_dims.erase(it);
  BumpVersion();
  return true;
}

DrawingDim* DrawingDocument::FindDim(uint64_t id) {
  const auto it =
      std::find_if(m_dims.begin(), m_dims.end(),
                   [id](const DrawingDim& d) { return d.id == id; });
  return it == m_dims.end() ? nullptr : &*it;
}

const DrawingDim* DrawingDocument::FindDim(uint64_t id) const {
  const auto it =
      std::find_if(m_dims.begin(), m_dims.end(),
                   [id](const DrawingDim& d) { return d.id == id; });
  return it == m_dims.end() ? nullptr : &*it;
}

glm::dvec2 SheetExtentMM(SheetSize size) {
  switch (size) {
  case SheetSize::A4:
    return {210.0, 297.0};
  case SheetSize::A3:
    return {297.0, 420.0};
  case SheetSize::A2:
    return {420.0, 594.0};
  case SheetSize::A1:
    return {594.0, 841.0};
  case SheetSize::A0:
    return {841.0, 1189.0};
  case SheetSize::Letter:
    return {215.9, 279.4};
  }
  return {210.0, 297.0};
}

void OrientationAxes(ViewOrientation o, glm::dvec3& viewDir, glm::dvec3& up) {
  switch (o) {
  case ViewOrientation::Front:
    viewDir = {0, 1, 0};
    up = {0, 0, 1};
    break;
  case ViewOrientation::Back:
    viewDir = {0, -1, 0};
    up = {0, 0, 1};
    break;
  case ViewOrientation::Right:
    viewDir = {-1, 0, 0};
    up = {0, 0, 1};
    break;
  case ViewOrientation::Left:
    viewDir = {1, 0, 0};
    up = {0, 0, 1};
    break;
  case ViewOrientation::Top:
    viewDir = {0, 0, -1};
    up = {0, 1, 0};
    break;
  case ViewOrientation::Bottom:
    viewDir = {0, 0, 1};
    up = {0, -1, 0};
    break;
  case ViewOrientation::Iso:
    // Standard Z-up isometric: camera above and to the front-right.
    viewDir = glm::normalize(glm::dvec3(-1, 1, -1));
    up = {0, 0, 1};
    break;
  }
}

const char* SheetSizeToString(SheetSize size) {
  switch (size) {
  case SheetSize::A4:
    return "a4";
  case SheetSize::A3:
    return "a3";
  case SheetSize::A2:
    return "a2";
  case SheetSize::A1:
    return "a1";
  case SheetSize::A0:
    return "a0";
  case SheetSize::Letter:
    return "letter";
  }
  return "a4";
}

bool SheetSizeFromString(const std::string& s, SheetSize& out) {
  if (s == "a4")
    out = SheetSize::A4;
  else if (s == "a3")
    out = SheetSize::A3;
  else if (s == "a2")
    out = SheetSize::A2;
  else if (s == "a1")
    out = SheetSize::A1;
  else if (s == "a0")
    out = SheetSize::A0;
  else if (s == "letter")
    out = SheetSize::Letter;
  else
    return false;
  return true;
}

const char* ViewOrientationToString(ViewOrientation o) {
  switch (o) {
  case ViewOrientation::Front:
    return "front";
  case ViewOrientation::Back:
    return "back";
  case ViewOrientation::Left:
    return "left";
  case ViewOrientation::Right:
    return "right";
  case ViewOrientation::Top:
    return "top";
  case ViewOrientation::Bottom:
    return "bottom";
  case ViewOrientation::Iso:
    return "iso";
  }
  return "front";
}

bool ViewOrientationFromString(const std::string& s, ViewOrientation& out) {
  if (s == "front")
    out = ViewOrientation::Front;
  else if (s == "back")
    out = ViewOrientation::Back;
  else if (s == "left")
    out = ViewOrientation::Left;
  else if (s == "right")
    out = ViewOrientation::Right;
  else if (s == "top")
    out = ViewOrientation::Top;
  else if (s == "bottom")
    out = ViewOrientation::Bottom;
  else if (s == "iso")
    out = ViewOrientation::Iso;
  else
    return false;
  return true;
}

const char* DrawingDimKindToString(DrawingDimKind k) {
  switch (k) {
  case DrawingDimKind::Distance:
    return "distance";
  case DrawingDimKind::DistanceX:
    return "distance_x";
  case DrawingDimKind::DistanceY:
    return "distance_y";
  case DrawingDimKind::Radius:
    return "radius";
  case DrawingDimKind::Diameter:
    return "diameter";
  case DrawingDimKind::Angle:
    return "angle";
  }
  return "distance";
}

bool DrawingDimKindFromString(const std::string& s, DrawingDimKind& out) {
  if (s == "distance")
    out = DrawingDimKind::Distance;
  else if (s == "distance_x")
    out = DrawingDimKind::DistanceX;
  else if (s == "distance_y")
    out = DrawingDimKind::DistanceY;
  else if (s == "radius")
    out = DrawingDimKind::Radius;
  else if (s == "diameter")
    out = DrawingDimKind::Diameter;
  else if (s == "angle")
    out = DrawingDimKind::Angle;
  else
    return false;
  return true;
}
