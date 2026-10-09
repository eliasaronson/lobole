// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/drawing/DrawingDocument.h"

#include "core/Angles.h"
#include "core/Conventions.h"

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
  m_projections.erase(id);
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

uint64_t DrawingDocument::AddNote(DrawingNote note) {
  note.id = m_nextId++;
  m_notes.push_back(std::move(note));
  BumpVersion();
  return m_notes.back().id;
}

bool DrawingDocument::RemoveNote(uint64_t id) {
  const auto it =
      std::find_if(m_notes.begin(), m_notes.end(),
                   [id](const DrawingNote& n) { return n.id == id; });
  if (it == m_notes.end())
    return false;
  m_notes.erase(it);
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

// Which way each NAMED view looks. Routed through conv::kNamedViews — the
// same table as the view.* commands, the `view` RPC and the view cube — so
// a drawing's "front" IS the front the rest of the app shows.
//
// History: this function used to be its own hand-written Z-up table, which
// was the usual CAD convention and wrong for this app: every "front" view
// was really the top. The HLR tests missed it because they pass direction/up
// vectors in by hand and never go through a named orientation. Found by
// making a drawing of a plate and noticing the front view was 80x60 when
// the plate is 80 wide and 8 thick. Deriving from the one table is the fix
// for the CLASS of bug, not just the instance.
void OrientationAxes(ViewOrientation o, glm::dvec3& viewDir, glm::dvec3& up) {
  const conv::NamedView* v = conv::FindView(ViewOrientationToString(o));
  // Every ViewOrientation spelling is a named view; the gate in
  // tests/test_world_up.cpp checks the enum round-trips through the table.
  viewDir = -v->toCamera;
  up = v->up;
}

bool IsProjRefExtreme(const std::string& s) {
  return s.empty() || s == "left" || s == "right" || s == "top" ||
         s == "bottom";
}

void ViewAxes(const DrawingView& view, glm::dvec3& viewDir, glm::dvec3& up) {
  OrientationAxes(view.orientation, viewDir, up);
  if (view.rotationDeg == 0.0)
    return;
  // The picture turns counter-clockwise by t, so the world vector drawn
  // upward turns clockwise: up' = up cos t + right sin t (sheet x is right).
  const double t = angles::ToRadians(view.rotationDeg);
  const glm::dvec3 right = glm::normalize(glm::cross(viewDir, up));
  up = glm::normalize(up * std::cos(t) + right * std::sin(t));
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

const char* NoteTableToString(NoteTable t) {
  switch (t) {
  case NoteTable::None:
    return "none";
  case NoteTable::Bends:
    return "bends";
  case NoteTable::Bom:
    return "bom";
  }
  return "none";
}

bool NoteTableFromString(const std::string& s, NoteTable& out) {
  if (s == "none")
    out = NoteTable::None;
  else if (s == "bends")
    out = NoteTable::Bends;
  else if (s == "bom")
    out = NoteTable::Bom;
  else
    return false;
  return true;
}

std::string UnknownNoteTable(const std::string& given) {
  return "unknown note table '" + given +
         "' (expected one of: none, bends, bom)";
}

const char* NoteStyleToString(NoteStyle s) {
  switch (s) {
  case NoteStyle::Text:
    return "text";
  case NoteStyle::Balloon:
    return "balloon";
  case NoteStyle::Datum:
    return "datum";
  case NoteStyle::Fcf:
    return "fcf";
  }
  return "text";
}

bool NoteStyleFromString(const std::string& s, NoteStyle& out) {
  if (s == "text")
    out = NoteStyle::Text;
  else if (s == "balloon")
    out = NoteStyle::Balloon;
  else if (s == "datum")
    out = NoteStyle::Datum;
  else if (s == "fcf")
    out = NoteStyle::Fcf;
  else
    return false;
  return true;
}

std::string UnknownNoteStyle(const std::string& given) {
  return "unknown note style '" + given +
         "' (expected one of: text, balloon, datum, fcf)";
}

namespace {

// One table: the name on the wire, the symbol, and what it may carry
// (ISO 1101 Table 2 and its clauses on datums and zones). Form tolerances
// take no datum; orientation, coaxiality, symmetry and runout need one;
// position and profile may stand alone. A Ø zone is meaningful where the
// toleranced feature can be an axis; a material modifier where it can be a
// feature of size.
struct SymbolRow {
  const char* name;
  GeoSymbol symbol;
  GeoSymbolRule rule;
};
using D = GeoSymbolRule::Datums;
constexpr SymbolRow kSymbols[] = {
    {"straightness", GeoSymbol::Straightness, {D::None, true, true}},
    {"flatness", GeoSymbol::Flatness, {D::None, false, false}},
    {"circularity", GeoSymbol::Circularity, {D::None, false, false}},
    {"cylindricity", GeoSymbol::Cylindricity, {D::None, false, false}},
    {"profile_line", GeoSymbol::ProfileLine, {D::Optional, false, false}},
    {"profile_surface", GeoSymbol::ProfileSurface, {D::Optional, false, false}},
    {"parallelism", GeoSymbol::Parallelism, {D::Required, true, true}},
    {"perpendicularity",
     GeoSymbol::Perpendicularity,
     {D::Required, true, true}},
    {"angularity", GeoSymbol::Angularity, {D::Required, true, true}},
    {"position", GeoSymbol::Position, {D::Optional, true, true}},
    {"coaxiality", GeoSymbol::Coaxiality, {D::Required, true, true}},
    {"symmetry", GeoSymbol::Symmetry, {D::Required, false, true}},
    {"circular_runout", GeoSymbol::CircularRunout, {D::Required, false, false}},
    {"total_runout", GeoSymbol::TotalRunout, {D::Required, false, false}},
};

std::string SymbolNames() {
  std::string out;
  for (const SymbolRow& r : kSymbols)
    out += (out.empty() ? "" : ", ") + std::string(r.name);
  return out;
}

} // namespace

const char* GeoSymbolToString(GeoSymbol s) {
  for (const SymbolRow& r : kSymbols)
    if (r.symbol == s)
      return r.name;
  return "position";
}

bool GeoSymbolFromString(const std::string& s, GeoSymbol& out) {
  for (const SymbolRow& r : kSymbols)
    if (s == r.name) {
      out = r.symbol;
      return true;
    }
  return false;
}

std::string UnknownGeoSymbol(const std::string& given) {
  return "unknown tolerance symbol '" + given +
         "' (expected one of: " + SymbolNames() + ")";
}

GeoSymbolRule GeoSymbolRules(GeoSymbol s) {
  for (const SymbolRow& r : kSymbols)
    if (r.symbol == s)
      return r.rule;
  return {};
}

const char* MaterialModifierToString(MaterialModifier m) {
  switch (m) {
  case MaterialModifier::None:
    return "none";
  case MaterialModifier::Mmc:
    return "mmc";
  case MaterialModifier::Lmc:
    return "lmc";
  }
  return "none";
}

bool MaterialModifierFromString(const std::string& s, MaterialModifier& out) {
  if (s == "none" || s.empty())
    out = MaterialModifier::None;
  else if (s == "mmc" || s == "M")
    out = MaterialModifier::Mmc;
  else if (s == "lmc" || s == "L")
    out = MaterialModifier::Lmc;
  else
    return false;
  return true;
}

std::string UnknownMaterialModifier(const std::string& given) {
  return "unknown material modifier '" + given +
         "' (expected one of: none, mmc (M), lmc (L))";
}

std::string ValidateDatumLetter(const std::string& letter) {
  if (letter.size() != 1 || letter[0] < 'A' || letter[0] > 'Z')
    return "a datum is named by one capital letter, A to Z; got '" + letter +
           "'";
  if (letter == "I" || letter == "O" || letter == "Q")
    return "datum letters leave out I, O and Q (ISO 5459: they read as 1, 0 "
           "and 0); got '" +
           letter + "'";
  return {};
}

std::string ValidateFcf(const FeatureControlFrame& f) {
  const GeoSymbolRule rule = GeoSymbolRules(f.symbol);
  const std::string name = GeoSymbolToString(f.symbol);
  if (!(f.tolerance > 0.0))
    return "a " + name + " tolerance is a zone wider than zero";
  if (f.datums.size() > 3)
    return "a frame names at most three datums (primary, secondary, "
           "tertiary); got " +
           std::to_string(f.datums.size());
  if (rule.datums == GeoSymbolRule::Datums::None && !f.datums.empty())
    return name +
           " is a form tolerance: it is measured against its own feature "
           "and takes no datum";
  if (rule.datums == GeoSymbolRule::Datums::Required && f.datums.empty())
    return name + " is measured from a datum: name one in `datums`";
  for (size_t i = 0; i < f.datums.size(); ++i) {
    if (const std::string why = ValidateDatumLetter(f.datums[i]); !why.empty())
      return why;
    for (size_t j = 0; j < i; ++j)
      if (f.datums[j] == f.datums[i])
        return "datum " + f.datums[i] + " is named twice in one frame";
  }
  if (f.diameterZone && !rule.diameterZone)
    return name + " has no cylindrical zone, so no Ø";
  if (f.modifier != MaterialModifier::None && !rule.modifier)
    return name + " takes no material modifier (M or L): its feature is not "
                  "a size";
  return {};
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
  case DrawingDimKind::FastenerCallout:
    return "fastener_callout";
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
  else if (s == "fastener_callout") // v70
    out = DrawingDimKind::FastenerCallout;
  else
    return false;
  return true;
}

std::vector<const char*> DrawingDimKindNames() {
  return {"distance",     "distance_x",      "distance_y",
          "radius",       "diameter",        "angle",
          "hole_callout", "pattern_callout", "fastener_callout"};
}
