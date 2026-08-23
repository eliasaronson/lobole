#include "core/drawing/DrawingDocument.h"

#include <algorithm>
#include <cctype>

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

SheetChrome ChromeOf(const Sheet& sheet) {
  SheetChrome c;
  const glm::dvec2 portrait = SheetExtentMM(sheet.size);
  c.extent = sheet.landscape ? glm::dvec2{portrait.y, portrait.x} : portrait;
  c.borderMin = {c.margin, c.margin};
  c.borderMax = c.extent - glm::dvec2{c.margin, c.margin};
  const double tbW = std::min(kTitleBlockWidthMM, c.extent.x - 2.0 * c.margin);
  c.titleBlockMin = {c.extent.x - c.margin - tbW, c.margin};
  c.titleBlockMax = c.titleBlockMin + glm::dvec2{tbW, kTitleBlockHeightMM};
  return c;
}

// This table decides which way each NAMED view looks, so it has to agree
// with the world the model lives in: **up is +Y** here, and the front of a
// part faces +Z (the view cube's Top is +Y — see docs/ui/).
//
// It was written Z-up, which is the usual CAD convention and wrong for this
// app: every "front" view was really the top, every "top" the front. The
// HLR tests missed it because they pass direction/up vectors in by hand and
// never go through a named orientation, so they agreed with the same
// mistake. Found by making a drawing of a plate and noticing the front view
// was 80x60 when the plate is 80 wide and 8 thick.
void OrientationAxes(ViewOrientation o, glm::dvec3& viewDir, glm::dvec3& up) {
  switch (o) {
  case ViewOrientation::Front:
    viewDir = {0, 0, -1};
    up = {0, 1, 0};
    break;
  case ViewOrientation::Back:
    viewDir = {0, 0, 1};
    up = {0, 1, 0};
    break;
  case ViewOrientation::Right:
    viewDir = {-1, 0, 0};
    up = {0, 1, 0};
    break;
  case ViewOrientation::Left:
    viewDir = {1, 0, 0};
    up = {0, 1, 0};
    break;
  case ViewOrientation::Top:
    // Looking down, with the part's front edge at the BOTTOM of the sheet —
    // the fold that puts a top view directly above its front view.
    viewDir = {0, -1, 0};
    up = {0, 0, -1};
    break;
  case ViewOrientation::Bottom:
    viewDir = {0, 1, 0};
    up = {0, 0, 1};
    break;
  case ViewOrientation::Iso:
    // Camera above, to the right, and in front of the part.
    viewDir = glm::normalize(glm::dvec3(-1, -1, -1));
    up = {0, 1, 0};
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

bool SheetSizeFromString(const std::string& sIn, SheetSize& out) {
  std::string s = sIn; // case-insensitive: "A3" and "a3" both mean A3
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
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
  case DrawingDimKind::HoleCallout:
    return "hole_callout";
  case DrawingDimKind::PatternCallout:
    return "pattern_callout";
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
  else if (s == "hole_callout") // v53
    out = DrawingDimKind::HoleCallout;
  else if (s == "pattern_callout") // v53
    out = DrawingDimKind::PatternCallout;
  else
    return false;
  return true;
}

std::vector<const char*> DrawingDimKindNames() {
  return {"distance", "distance_x", "distance_y",   "radius",
          "diameter", "angle",      "hole_callout", "pattern_callout"};
}
