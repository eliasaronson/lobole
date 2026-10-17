// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/FitBands.h"

#include "core/Document.h"
#include "core/Feature.h"
#include "core/FitStandards.h"
#include "core/PrintAllowance.h"
#include "core/features/BearingFeature.h"
#include "core/features/DerivedFeature.h"
#include "core/parts/BearingTable.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Cylinder.hxx>

#include <cctype>
#include <cstdio>

namespace fitband {

std::optional<bool> IsBore(const TopoDS_Face& face) {
  BRepAdaptor_Surface s(face);
  if (s.GetType() != GeomAbs_Cylinder)
    return std::nullopt;
  Standard_Real u0, u1, v0, v1;
  BRepTools::UVBounds(face, u0, u1, v0, v1);
  gp_Pnt p;
  gp_Vec du, dv;
  s.D1(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, du, dv);
  gp_Vec n = du.Crossed(dv);
  if (n.Magnitude() < 1e-12)
    return std::nullopt;
  if (face.Orientation() == TopAbs_REVERSED)
    n.Reverse();
  const gp_Ax1 axis = s.Cylinder().Axis();
  gp_Vec radial(axis.Location(), p);
  const gp_Vec d(axis.Direction());
  radial -= d * radial.Dot(d);
  return n.Dot(radial) < 0.0;
}

namespace {

// The face `name` of `body`, when it is a cylinder: its modelled diameter
// and whether it is a bore.
struct Round {
  double diameter = 0.0;
  bool bore = false;
};
std::optional<Round> RoundFace(const Body& body, const std::string& name) {
  const auto index = body.elementMap.Resolve(name);
  if (!index)
    return std::nullopt;
  int i = 0;
  for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), ++i) {
    if (i != *index)
      continue;
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    const auto bore = IsBore(face);
    if (!bore)
      return std::nullopt;
    return Round{2.0 * BRepAdaptor_Surface(face).Cylinder().Radius(), *bore};
  }
  return std::nullopt;
}

// The body of `doc` the face's feature made.
const Body* BodyOf(const Document& doc, const ElementRef& face) {
  for (const Body& b : doc.GetBodies())
    if (b.createdBy == face.featureId && !b.shape.GetShape().IsNull() &&
        b.elementMap.Resolve(face.name))
      return &b;
  return nullptr;
}

} // namespace

std::optional<PrintedBand> PrintedPressBand(const Document& doc,
                                            const ElementRef& face,
                                            std::string* why) {
  const auto say = [&](std::string w) {
    if (why)
      *why = std::move(w);
    return std::nullopt;
  };
  if (!doc.PrintProcess())
    return say("a printed press reads its band from the part's print "
               "profile, and this part has no print process "
               "(set_print_process)");
  const Body* body = BodyOf(doc, face);
  if (body == nullptr)
    return say("face '" + face.name + "' does not resolve");
  const auto round = RoundFace(*body, face.name);
  if (!round)
    return say("a printed press is between a round bore and a round pin, "
               "and " +
               face.name + " is not a cylinder");
  const auto band = printfit::PrintedPress(*doc.PrintProcess(), round->bore,
                                           round->diameter, why);
  if (!band)
    return std::nullopt;
  return PrintedBand{*band, round->bore, round->diameter};
}

void Stamp(const Document& doc, std::vector<Body>& bodies) {
  const printfit::Process* print =
      doc.PrintProcess() && printfit::Find(*doc.PrintProcess()) != nullptr
          ? &*doc.PrintProcess()
          : nullptr;
  const BearingFeature* bearing = SoleBearingOf(&doc);
  const parts::BearingRow* row =
      bearing ? parts::FindBearing(bearing->designation) : nullptr;
  for (Body& body : bodies) {
    body.fitBands.clear();
    if (body.shape.GetShape().IsNull())
      continue;
    // A Derived copy: the bands its source stamped, carried in the
    // snapshot as the source read them, under the copy's face names.
    if (body.derivedFrom != 0)
      if (const auto* derived = dynamic_cast<const DerivedFeature*>(
              doc.FindFeatureById(body.createdBy)))
        for (const DerivedBody& sb : derived->snapshot) {
          if (sb.sourceCreatedBy != body.derivedFrom)
            continue;
          for (const FitBand& carried : sb.fitBands) {
            FitBand b = carried;
            b.face = ElementRef{
                derived->GetId(),
                derived->DerivedName(sb.sourceCreatedBy, carried.face.name)};
            if (body.elementMap.Resolve(b.face.name))
              body.fitBands.push_back(std::move(b));
          }
        }
    // A generated bearing: its two ring faces carry ISO 492's band, and the
    // ring says which side it is.
    if (row != nullptr && body.createdBy == bearing->GetId())
      for (const bool bore : {true, false}) {
        const auto band = bore ? parts::BoreTolerance(row->d)
                               : parts::OutsideTolerance(row->D);
        const std::string name = bearing->FaceName(
            bore ? BearingFeature::kBore : BearingFeature::kOutside);
        if (!band || !body.elementMap.Resolve(name))
          continue;
        FitBand b;
        b.face = ElementRef{bearing->GetId(), name};
        b.lower = band->lower;
        b.upper = band->upper;
        b.side = bore ? FitBand::Side::Hole : FitBand::Side::Shaft;
        b.label = std::string(row->designation) +
                  (bore ? " bore" : " outside") + " (ISO 492 normal)";
        body.fitBands.push_back(std::move(b));
      }
    for (const Tolerance& t : doc.GetTolerances()) {
      if (t.IsDistance() || t.ref.featureId != body.createdBy)
        continue;
      const auto round = RoundFace(body, t.ref.name);
      if (!round)
        continue; // a size is only a round face's; list_tolerances says so
      FitBand b;
      b.face = t.ref;
      if (t.printedPress) {
        // Modelled at what the press needs: no process allowance on top.
        const auto band = print
                              ? printfit::PrintedPress(*print, round->bore,
                                                       round->diameter, nullptr)
                              : std::nullopt;
        if (!band)
          continue; // list_tolerances says why
        b.lower = band->lowerMm;
        b.upper = band->upperMm;
        b.side = round->bore ? FitBand::Side::Hole : FitBand::Side::Shaft;
        b.printedPress = true;
        b.label = printfit::Label(*print);
        body.fitBands.push_back(std::move(b));
        continue;
      }
      // The size the face is made at: a printed part carries the process's
      // allowance in its model, and prints at the modelled size less it.
      double made = round->diameter;
      std::string printed;
      if (print != nullptr) {
        const auto a = printfit::ForModelled(
            *print,
            round->bore ? printfit::Printed::Hole : printfit::Printed::Shaft,
            round->diameter, nullptr);
        if (!a)
          continue; // check_fits says why
        b.allowance = a->allowanceMm;
        made = a->basicMm;
        printed = ", printed (" + print->process + " " + print->material + ")";
      }
      if (t.isoClass.empty()) {
        b.lower = t.lower;
        b.upper = t.upper;
        char buf[64];
        std::snprintf(buf, sizeof buf, "%+.3f/%+.3f", t.upper, t.lower);
        b.label = buf;
      } else {
        const auto band = fits::Limits(made, t.isoClass, nullptr);
        if (!band)
          continue; // list_tolerances says why
        b.lower = band->lower;
        b.upper = band->upper;
        b.side = std::isupper(static_cast<unsigned char>(t.isoClass[0]))
                     ? FitBand::Side::Hole
                     : FitBand::Side::Shaft;
        b.label = t.isoClass;
      }
      b.label += printed;
      body.fitBands.push_back(std::move(b));
    }
  }
}

} // namespace fitband
