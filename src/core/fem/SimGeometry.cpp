// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/fem/SimGeometry.h"
#include "cad/Integrate.h"

#include "core/Document.h"
#include "core/ElementOrigin.h"
#include "core/Feature.h"
#include "core/InputHash.h"
#include "core/Measure.h"
#include "core/features/DatumPlaneFeature.h"
#include "core/fem/FemMesher.h"
#include "core/fem/FemSolver.h"

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace fem {

namespace {

bool Refuse(std::string* error, const std::string& why) {
  if (error)
    *error = why;
  return false;
}

std::string FeatureName(const Document& part, uint64_t id) {
  const Feature* f = part.FindFeatureById(id);
  return f ? "'" + f->GetName() + "'" : "feature " + std::to_string(id);
}

std::string LoadName(const Load& l) {
  return "load " + std::to_string(l.id) + " (" + LoadTypeToString(l.type) + ")";
}

std::vector<TopoDS_Shape> FacesOf(const TopoDS_Shape& s) {
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next())
    out.push_back(ex.Current());
  return out;
}

double AreaOf(const TopoDS_Shape& face) {
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  return props.Mass();
}

double VolumeOf(const TopoDS_Shape& s) { return cad::VolumeOf(s); }

// What a suppressed feature did to the element named `name`.
struct Through {
  uint64_t madeBy = 0;  // a suppressed feature made it
  uint64_t splitBy = 0; // a suppressed feature split it into this piece
  std::string name;     // its name with the suppressed features' steps out
};

// The tag of a step "M7", "S7[...]", "G7.2": its letter's feature id, and
// whether anything follows the digits (a piece qualifier, an ordinal).
bool StepTag(const std::string& step, char& letter, uint64_t& tag,
             bool& qualified) {
  if (step.size() < 2 || !std::isupper(static_cast<unsigned char>(step[0])))
    return false;
  size_t i = 1;
  while (i < step.size() && std::isdigit(static_cast<unsigned char>(step[i])))
    ++i;
  if (i == 1)
    return false;
  letter = step[0];
  tag = std::stoull(step.substr(1, i - 1));
  qualified = i < step.size();
  return true;
}

Through ThroughSuppressed(const std::string& name,
                          const std::set<uint64_t>& suppressed) {
  Through out;
  const topo::ElementOrigin o = topo::DecodeElementName(name);
  if (o.ok && o.derived.empty()) {
    if (suppressed.contains(o.madeBy))
      out.madeBy = o.madeBy;
    else if (suppressed.contains(o.seedTag))
      out.madeBy = o.seedTag;
  }
  const std::vector<std::string> steps = topo::NameSteps(name);
  for (size_t k = 0; k < steps.size(); ++k) {
    char letter = 0;
    uint64_t tag = 0;
    bool qualified = false;
    if (k > 0 && StepTag(steps[k], letter, tag, qualified) &&
        suppressed.contains(tag)) {
      if (letter == 'G' && out.madeBy == 0)
        out.madeBy = tag;
      if ((letter == 'M' || letter == 'S') && qualified && out.splitBy == 0)
        out.splitBy = tag;
      if ((letter == 'M' || letter == 'S') && !qualified)
        continue; // the step the suppressed feature added: taken out
    }
    out.name += (out.name.empty() && k == 0 ? "" : ";") + steps[k];
  }
  return out;
}

} // namespace

// ---- The analysis part -----------------------------------------------------

std::shared_ptr<const Document> AnalysisPart(const Document& part,
                                             std::vector<uint64_t> features,
                                             std::string* error) {
  std::sort(features.begin(), features.end());
  features.erase(std::unique(features.begin(), features.end()), features.end());
  for (const uint64_t id : features)
    if (part.FindFeatureById(id) == nullptr) {
      Refuse(error, "the part has no feature " + std::to_string(id) +
                        " to suppress for the analysis");
      return nullptr;
    }
  std::unique_ptr<Document> copy = Document::CopyWithGeometry(part);
  copy->AdoptEvaluations(part);
  {
    Document::BulkEdit bulk(*copy);
    for (const uint64_t id : features)
      copy->FindFeatureById(id)->SetSuppressed(true);
  }
  std::string named;
  for (const uint64_t id : features)
    named += (named.empty() ? "" : ", ") + FeatureName(part, id);
  // A feature that built on the part and fails without the suppressed
  // ones reads them, and the analysis would silently lose it too.
  for (size_t i = 0; i < copy->GetFeatures().size(); ++i) {
    const Feature& f = *copy->GetFeatures()[i];
    if (copy->EffectiveSuppressed(i) || f.GetError().empty())
      continue;
    const Feature* was = part.FindFeatureById(f.GetId());
    if (was != nullptr && was->GetError().empty()) {
      Refuse(error, "suppressing " + named + " for the analysis leaves '" +
                        f.GetName() + "' failing (" + f.GetError() +
                        "): suppress it too, or unsuppress what it reads");
      return nullptr;
    }
  }
  bool any = false;
  for (const Body& b : copy->GetBodies())
    any = any || !b.reference;
  if (!any) {
    Refuse(error, "suppressing " + named +
                      " for the analysis leaves no body to analyse");
    return nullptr;
  }
  return std::shared_ptr<const Document>(std::move(copy));
}

std::shared_ptr<const Document>
AnalysisPartCache::Get(const Document& part, std::vector<uint64_t> features,
                       std::string* error) {
  std::sort(features.begin(), features.end());
  features.erase(std::unique(features.begin(), features.end()), features.end());
  std::lock_guard<std::mutex> lock(m_mutex);
  for (size_t i = 0; i < m_entries.size(); ++i) {
    const Entry& e = m_entries[i];
    if (e.instance == part.InstanceId() && e.version == part.Version() &&
        e.configuration == part.ActiveConfiguration() &&
        e.features == features) {
      Entry hit = e;
      m_entries.erase(m_entries.begin() + static_cast<long>(i));
      m_entries.push_back(hit);
      if (!hit.doc)
        Refuse(error, hit.error);
      return hit.doc;
    }
  }
  Entry e;
  e.instance = part.InstanceId();
  e.version = part.Version();
  e.configuration = part.ActiveConfiguration();
  e.features = features;
  e.doc = AnalysisPart(part, features, &e.error);
  if (!e.doc)
    Refuse(error, e.error);
  m_entries.push_back(e);
  constexpr size_t kKept = 8;
  if (m_entries.size() > kKept)
    m_entries.erase(m_entries.begin());
  return e.doc;
}

bool StudyForAnalysis(const Document& part, Study& study,
                      const std::vector<uint64_t>& suppressedIds,
                      std::string* error, SymmetryPlane* symmetry) {
  const std::set<uint64_t> suppressed(suppressedIds.begin(),
                                      suppressedIds.end());
  if (suppressed.empty())
    return true;
  const auto map = [&](ElementRef& ref, const std::string& what) {
    if (ref.name.empty())
      return true; // a body's heat source, a load with no face
    const Through t = ThroughSuppressed(ref.name, suppressed);
    if (what.empty() && t.madeBy != 0)
      return Refuse(error, "'" + ref.name + "' is a face " +
                               FeatureName(part, t.madeBy) +
                               " made, and it is suppressed for the analysis: "
                               "put " +
                               FeatureName(part, t.madeBy) +
                               " back first, or pick another face");
    if (what.empty() && t.splitBy != 0)
      return Refuse(error, "'" + ref.name + "' is a piece of a face " +
                               FeatureName(part, t.splitBy) +
                               " split, and it is suppressed for the "
                               "analysis: pick the whole face");
    if (t.madeBy != 0)
      return Refuse(error, what + " is on '" + ref.name + "', which " +
                               FeatureName(part, t.madeBy) +
                               " made, and it is suppressed for the "
                               "analysis: put it on another face, or "
                               "unsuppress " +
                               FeatureName(part, t.madeBy));
    if (t.splitBy != 0)
      return Refuse(error, what + " is on '" + ref.name +
                               "', a piece of a face " +
                               FeatureName(part, t.splitBy) +
                               " split, and it is suppressed for the "
                               "analysis, so the face is whole there: put "
                               "it on the whole face, or unsuppress " +
                               FeatureName(part, t.splitBy));
    ref.name = t.name;
    return true;
  };
  // A load with no id is one being placed: named by its face alone.
  for (Load& l : study.loads)
    if (!map(l.face, l.id == 0 ? std::string() : LoadName(l)))
      return false;
  for (Refinement& r : study.refinements)
    if (!map(r.face, "a refinement"))
      return false;
  for (ElementRef& k : study.keepFaces)
    if (!map(k, "a kept face"))
      return false;
  if (symmetry != nullptr && !map(symmetry->face, "the symmetry plane"))
    return false;
  return true;
}

// ---- The symmetry cut
// --------------------------------------------------------

bool ResolveSymmetryPlane(const Document& part, const SymmetryPlane& plane,
                          CutPlane& out, std::string* error) {
  out = CutPlane{};
  if (plane.datum != 0) {
    size_t index = 0;
    const Feature* f = part.FindFeatureById(plane.datum, &index);
    const auto* datum = dynamic_cast<const DatumPlaneFeature*>(f);
    if (datum == nullptr)
      return Refuse(error, "the symmetry plane: feature " +
                               std::to_string(plane.datum) +
                               " is no datum plane of the part");
    if (part.EffectiveSuppressed(index))
      return Refuse(error, "the symmetry plane: datum '" + f->GetName() +
                               "' is suppressed");
    out.origin = datum->OutputPlane().GetOrigin();
    out.normal = glm::normalize(datum->OutputPlane().GetNormal());
    out.name = "datum '" + f->GetName() + "'";
  } else if (plane.face.IsSet()) {
    ResolvedFace rf;
    std::string err;
    if (!ResolveFace(part, plane.face, "symmetry plane", rf, &err))
      return Refuse(error, "the symmetry plane: " + err);
    const auto info =
        Measure::DescribeFaceAt(part.GetBodies()[rf.bodyIndex], rf.faceIndex);
    if (!info || info->kind != "plane")
      return Refuse(error, "the symmetry plane: '" + plane.face.name +
                               "' is not flat; a symmetry plane is a datum "
                               "plane or a flat face");
    out.origin = info->centroid;
    out.normal = glm::normalize(info->normal);
    out.name = "face '" + plane.face.name + "'";
  } else {
    return Refuse(error, "no symmetry plane is set");
  }
  if (plane.flip)
    out.normal = -out.normal;
  return true;
}

bool CutPlanOnSymmetry(StudyPlan& plan, AnalysisType type,
                       const CutPlane& plane, std::string* error) {
  const std::string by = "the symmetry cut on " + plane.name;
  if (type != AnalysisType::Static && type != AnalysisType::Thermal)
    return Refuse(error, by + " is a static or a thermal study's: a half "
                              "model finds only the modes and the buckling "
                              "symmetric about the plane; solve the whole "
                              "part in a simulation with no symmetry plane");
  for (StudyBodyPlan& bp : plan.bodies) {
    const std::string body = "'" + bp.bodyName + "'";
    if (bp.premeshed || bp.shape.IsNull())
      return Refuse(error, by + ": a study of a topology shape is meshed from "
                                "its result, which is not cut");
    if (!bp.members.empty())
      return Refuse(error,
                    by + ": " + body + " is a bonded group, which is not cut");
    if (!bp.partners.empty() || !bp.contacts.empty())
      return Refuse(error, by + ": a contact is not mirror-symmetric once "
                                "cut; solve the whole part");
    if (!bp.heatLoads.empty())
      return Refuse(error, by + ": a thermal load from a thermal study is "
                                "not cut; solve the whole part");
    for (const auto& [face, l] : bp.loads) {
      if (l.type == LoadType::Torque || l.type == LoadType::Remote ||
          l.type == LoadType::Pin || l.type == LoadType::Contact)
        return Refuse(error, by + ": " + LoadName(l) +
                                 " is not mirror-symmetric once cut (a "
                                 "moment, a pin's or a contact's hold "
                                 "turns about the plane); solve the whole "
                                 "part");
      // A bearing load's pressure is spread over the loaded half of its
      // bore to add up to its force; on a bore the plane cuts, the half
      // left is not that half, and the force it should carry is not stated.
      if (l.type == LoadType::Bearing)
        return Refuse(error, by + ": " + LoadName(l) +
                                 " is spread over the loaded half of its "
                                 "whole bore, which a cut does not leave; "
                                 "solve the whole part");
      if (!l.region.IsWhole())
        return Refuse(error, by + ": " + LoadName(l) +
                                 " is on part of a face, which is not cut; "
                                 "load the whole face");
    }

    // The far side, as a box well past the body on that side.
    Bnd_Box bb;
    BRepBndLib::Add(bp.shape, bb);
    if (bb.IsVoid())
      return Refuse(error, by + ": " + body + " has no extent");
    double x0, y0, z0, x1, y1, z1;
    bb.Get(x0, y0, z0, x1, y1, z1);
    const glm::dvec3 lo(x0, y0, z0), hi(x1, y1, z1);
    const glm::dvec3 centre = 0.5 * (lo + hi);
    const glm::dvec3 n = plane.normal;
    const double toPlane = glm::dot(centre - plane.origin, n);
    const double big = 2.0 * (glm::length(hi - lo) + std::abs(toPlane)) + 10.0;
    const glm::dvec3 onPlane = centre - toPlane * n;
    glm::dvec3 u =
        std::abs(n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    u = glm::normalize(u - glm::dot(u, n) * n);
    const glm::dvec3 v = glm::cross(-n, u);
    const glm::dvec3 corner = onPlane - big * u - big * v;
    const gp_Ax2 axes(gp_Pnt(corner.x, corner.y, corner.z),
                      gp_Dir(-n.x, -n.y, -n.z), gp_Dir(u.x, u.y, u.z));
    const TopoDS_Shape farSide =
        BRepPrimAPI_MakeBox(axes, 2.0 * big, 2.0 * big, big).Shape();

    BRepAlgoAPI_Cut cut;
    {
      TopTools_ListOfShape args, tools;
      args.Append(bp.shape);
      tools.Append(farSide);
      cut.SetArguments(args);
      cut.SetTools(tools);
      cut.SetNonDestructive(Standard_True);
      cut.Build();
    }
    if (!cut.IsDone() || cut.Shape().IsNull())
      return Refuse(error, by + ": the cut of " + body + " failed");
    TopoDS_Shape kept;
    int solids = 0;
    for (TopExp_Explorer ex(cut.Shape(), TopAbs_SOLID); ex.More(); ex.Next()) {
      kept = ex.Current();
      ++solids;
    }
    if (solids == 0)
      return Refuse(error, by + " leaves nothing of " + body +
                               ": the plane's normal points away from it "
                               "(keep the other side)");
    if (solids > 1)
      kept = cut.Shape();
    const double whole = VolumeOf(bp.shape);
    const double share = whole > 0.0 ? VolumeOf(kept) / whole : 0.0;
    if (std::abs(share - 0.5) > 0.005) {
      char pct[32];
      std::snprintf(pct, sizeof(pct), "%.1f", share * 100.0);
      return Refuse(error, by + " does not halve " + body + ": it keeps " +
                               pct +
                               " % of its volume, and a half model needs "
                               "a part symmetric about the plane");
    }

    // Each original face onto what is left of it.
    const std::vector<TopoDS_Shape> before = FacesOf(bp.shape);
    const std::vector<TopoDS_Shape> after = FacesOf(kept);
    const auto indexOf = [&](const TopoDS_Shape& f) {
      for (size_t i = 0; i < after.size(); ++i)
        if (after[i].IsSame(f))
          return static_cast<int>(i);
      return -1;
    };
    std::vector<std::vector<int>> pieces(before.size());
    std::vector<char> image(after.size(), 0);
    for (size_t i = 0; i < before.size(); ++i) {
      if (cut.IsDeleted(before[i]))
        continue;
      const TopTools_ListOfShape& mod = cut.Modified(before[i]);
      if (mod.IsEmpty()) {
        if (const int k = indexOf(before[i]); k >= 0)
          pieces[i].push_back(k);
      } else {
        for (TopTools_ListOfShape::Iterator it(mod); it.More(); it.Next())
          if (const int k = indexOf(it.Value()); k >= 0)
            pieces[i].push_back(k);
      }
      for (int k : pieces[i])
        image[k] = 1;
    }
    std::vector<std::pair<int, Load>> loads;
    for (const auto& [face, l] : bp.loads) {
      if (face < 0) {
        // Through the body (a heat source): half the body, half its heat.
        Load half = l;
        if (std::isfinite(half.powerW))
          half.powerW *= share;
        loads.push_back({face, half});
        continue;
      }
      if (face >= static_cast<int>(before.size()))
        continue;
      const std::vector<int>& to = pieces[face];
      // A total (a force, a heat flux's watts) by the area it keeps.
      const bool total =
          l.type == LoadType::Force ||
          (l.type == LoadType::HeatFlux && std::isfinite(l.powerW));
      const double area = total ? AreaOf(before[face]) : 0.0;
      for (int k : to) {
        Load piece = l;
        if (total) {
          const double f = area > 0.0 ? AreaOf(after[k]) / area : 0.0;
          if (l.type == LoadType::Force) {
            piece.magnitude = l.magnitude.value * f;
            piece.magnitude.expr.clear(); // evaluated: this is its share
          }
          if (l.type != LoadType::Force)
            piece.powerW = l.powerW * f;
        }
        loads.push_back({k, piece});
      }
    }
    std::vector<std::pair<int, double>> sizes;
    for (const auto& [face, size] : bp.settings.faceSizes)
      if (face >= 0 && face < static_cast<int>(before.size()))
        for (int k : pieces[face])
          sizes.push_back({k, size});
    // The cut faces: a roller on each on a static study (no motion across
    // the plane); on a thermal one nothing, which is no heat across it.
    int cutFaces = 0;
    for (size_t k = 0; k < after.size(); ++k) {
      if (image[k])
        continue;
      ++cutFaces;
      if (type != AnalysisType::Static)
        continue;
      Load roller;
      roller.id = 0;
      roller.type = LoadType::Roller;
      roller.symmetryCut = true;
      roller.planeNormal = n;
      loads.push_back({static_cast<int>(k), roller});
    }
    if (cutFaces == 0)
      return Refuse(error, by + " does not cut " + body);
    bp.loads = std::move(loads);
    bp.settings.faceSizes = std::move(sizes);
    bp.shape = kept;
    bp.meshKey = 0;
    bp.stored.reset();
  }
  return true;
}

// ---- The geometry check
// ------------------------------------------------------

const char* GeometryFindingKindName(GeometryFinding::Kind kind) {
  switch (kind) {
  case GeometryFinding::Kind::Edge:
    return "edge";
  case GeometryFinding::Kind::Face:
    return "face";
  case GeometryFinding::Kind::Gap:
    return "gap";
  }
  return "edge";
}

namespace {

bool OnFarSide(const TopoDS_Shape& s, const CutPlane* keep) {
  if (keep == nullptr)
    return false;
  Bnd_Box bb;
  BRepBndLib::Add(s, bb);
  if (bb.IsVoid())
    return false;
  double x0, y0, z0, x1, y1, z1;
  bb.Get(x0, y0, z0, x1, y1, z1);
  const double tol =
      1e-6 * (1.0 + std::abs(x1 - x0) + std::abs(y1 - y0) + std::abs(z1 - z0));
  for (int c = 0; c < 8; ++c) {
    const glm::dvec3 p(c & 1 ? x1 : x0, c & 2 ? y1 : y0, c & 4 ? z1 : z0);
    if (glm::dot(p - keep->origin, keep->normal) > tol)
      return false;
  }
  return true;
}

glm::dvec3 Centre(const TopoDS_Shape& s) {
  Bnd_Box bb;
  BRepBndLib::Add(s, bb);
  if (bb.IsVoid())
    return glm::dvec3(0.0);
  double x0, y0, z0, x1, y1, z1;
  bb.Get(x0, y0, z0, x1, y1, z1);
  return 0.5 * glm::dvec3(x0 + x1, y0 + y1, z0 + z1);
}

void Attribute(const Document& part, const ::Body& body,
               const std::string* name, GeometryFinding& f) {
  if (name == nullptr)
    return;
  f.element = *name;
  const topo::ElementOrigin o = topo::DecodeElementName(*name);
  if (!o.ok || !o.derived.empty())
    return;
  f.feature = o.madeBy != 0 ? o.madeBy : o.seedTag;
  if (const Feature* feat = part.FindFeatureById(f.feature)) {
    f.featureName = feat->GetName();
    f.suppressible = f.feature != body.createdBy;
  }
}

} // namespace

void CheckBodyGeometry(const Document& part, const ::Body& body,
                       const TopoDS_Shape& shape, const MeshSettings& settings,
                       const CutPlane* keep, GeometryCheck& out) {
  const double h =
      settings.maxSize > 0.0 ? settings.maxSize : AutoMeshSize(shape);
  const std::vector<TopoDS_Shape> faces = FacesOf(shape);
  std::vector<double> faceH(faces.size(), h);
  for (const auto& [face, size] : settings.faceSizes)
    if (face >= 0 && face < static_cast<int>(faces.size()) && size > 0.0)
      faceH[face] = std::min(faceH[face], size);
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape edgeFaces;
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, edgeFaces);
  const auto faceIndexOf = [&](const TopoDS_Shape& f) {
    for (size_t i = 0; i < faces.size(); ++i)
      if (faces[i].IsSame(f))
        return static_cast<int>(i);
    return -1;
  };
  GeometryCheck::Body summary;
  summary.name = body.name;
  summary.elementMm = h;
  summary.faces = static_cast<int>(faces.size());
  summary.edges = edges.Extent();
  out.bodies.push_back(summary);

  // Edges shorter than an element at them.
  std::vector<double> edgeLength(static_cast<size_t>(edges.Extent()), 0.0);
  for (int i = 1; i <= edges.Extent(); ++i) {
    const TopoDS_Edge& e = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(e))
      continue;
    BRepAdaptor_Curve curve(e);
    const double len = GCPnts_AbscissaPoint::Length(curve);
    edgeLength[i - 1] = len;
    double local = h;
    bool seam = false;
    if (const int at = edgeFaces.FindIndex(e); at > 0)
      for (TopTools_ListOfShape::Iterator it(edgeFaces(at)); it.More();
           it.Next()) {
        // A cylinder's seam is where its surface closes, no edge of the
        // part: the mesher does not stop at it.
        seam = seam || BRep_Tool::IsClosed(e, TopoDS::Face(it.Value()));
        if (const int fi = faceIndexOf(it.Value()); fi >= 0)
          local = std::min(local, faceH[fi]);
      }
    if (seam || len >= local || OnFarSide(e, keep))
      continue;
    GeometryFinding f;
    f.kind = GeometryFinding::Kind::Edge;
    f.body = body.name;
    f.index = i - 1;
    f.sizeMm = len;
    f.elementMm = local;
    f.at = Centre(e);
    Attribute(part, body, body.elementMap.EdgeNameOf(i - 1), f);
    out.findings.push_back(std::move(f));
  }

  // Faces narrower than an element: four times the area over the
  // perimeter (a strip's twice its width, a disc's its diameter).
  for (size_t i = 0; i < faces.size(); ++i) {
    const TopoDS_Face& face = TopoDS::Face(faces[i]);
    double perimeter = 0.0;
    for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
      const TopoDS_Edge& e = TopoDS::Edge(ex.Current());
      if (BRep_Tool::IsClosed(e, face)) // a seam is no boundary
        continue;
      const int at = edges.FindIndex(e);
      perimeter += at > 0 ? edgeLength[at - 1] : 0.0;
    }
    if (perimeter <= 0.0)
      continue;
    // Its width, and on a round face its diameter: a bore or a fillet
    // whose radius is under an element is drawn by too few of them to be
    // round, however long it is.
    double width = 4.0 * AreaOf(face) / perimeter;
    const BRepAdaptor_Surface surface(face);
    switch (surface.GetType()) {
    case GeomAbs_Cylinder:
      width = std::min(width, 2.0 * surface.Cylinder().Radius());
      break;
    case GeomAbs_Sphere:
      width = std::min(width, 2.0 * surface.Sphere().Radius());
      break;
    case GeomAbs_Torus:
      width = std::min(width, 2.0 * surface.Torus().MinorRadius());
      break;
    default:
      break;
    }
    if (width >= faceH[i] || OnFarSide(face, keep))
      continue;
    GeometryFinding f;
    f.kind = GeometryFinding::Kind::Face;
    f.body = body.name;
    f.index = static_cast<int>(i);
    f.sizeMm = width;
    f.elementMm = faceH[i];
    f.at = Centre(face);
    Attribute(part, body, body.elementMap.NameOf(static_cast<int>(i)), f);
    out.findings.push_back(std::move(f));
  }

  // Two faces that share nothing, closer than an element: a wall the mesh
  // has one element through, or a slot it bridges.
  constexpr size_t kGapFaces = 600;
  if (faces.size() > kGapFaces) {
    out.gapsSkipped = true;
    out.note = "'" + body.name + "' has " + std::to_string(faces.size()) +
               " faces; gaps are compared on " + std::to_string(kGapFaces) +
               " at most";
    return;
  }
  std::vector<Bnd_Box> boxes(faces.size());
  std::vector<TopTools_MapOfShape> verts(faces.size());
  for (size_t i = 0; i < faces.size(); ++i) {
    BRepBndLib::Add(faces[i], boxes[i]);
    for (TopExp_Explorer ex(faces[i], TopAbs_VERTEX); ex.More(); ex.Next())
      verts[i].Add(ex.Current());
  }
  for (size_t i = 0; i < faces.size(); ++i)
    for (size_t j = i + 1; j < faces.size(); ++j) {
      const double local = std::min(faceH[i], faceH[j]);
      Bnd_Box grown = boxes[i];
      grown.Enlarge(local);
      if (grown.IsOut(boxes[j]))
        continue;
      bool touch = false;
      for (TopTools_MapOfShape::Iterator it(verts[i]); it.More() && !touch;
           it.Next())
        touch = verts[j].Contains(it.Key());
      if (touch)
        continue;
      BRepExtrema_DistShapeShape dist(faces[i], faces[j]);
      if (!dist.IsDone() || dist.NbSolution() == 0)
        continue;
      const double d = dist.Value();
      if (d <= 1e-6 || d >= local)
        continue;
      const gp_Pnt a = dist.PointOnShape1(1), b = dist.PointOnShape2(1);
      const glm::dvec3 mid(0.5 * (a.X() + b.X()), 0.5 * (a.Y() + b.Y()),
                           0.5 * (a.Z() + b.Z()));
      if (keep != nullptr && glm::dot(mid - keep->origin, keep->normal) < -1e-6)
        continue;
      GeometryFinding f;
      f.kind = GeometryFinding::Kind::Gap;
      f.body = body.name;
      f.index = static_cast<int>(i);
      f.otherIndex = static_cast<int>(j);
      f.sizeMm = d;
      f.elementMm = local;
      f.at = mid;
      // Across material when the middle between them is inside. The
      // nearest pair of two parallel faces lies on their rims, which is on
      // the part's skin, so each face's centroid and its nearest point on
      // the other are asked too.
      std::vector<std::pair<gp_Pnt, gp_Pnt>> pairs;
      for (int k = 1; k <= dist.NbSolution(); ++k)
        pairs.push_back({dist.PointOnShape1(k), dist.PointOnShape2(k)});
      for (const auto& [from, to] :
           {std::pair{faces[i], faces[j]}, std::pair{faces[j], faces[i]}}) {
        GProp_GProps props;
        BRepGProp::SurfaceProperties(from, props);
        BRepExtrema_DistShapeShape onto(
            BRepBuilderAPI_MakeVertex(props.CentreOfMass()).Vertex(), to);
        if (onto.IsDone() && onto.NbSolution() > 0 && onto.Value() < local)
          pairs.push_back({props.CentreOfMass(), onto.PointOnShape2(1)});
      }
      for (const auto& [pa, pb] : pairs) {
        const gp_Pnt m(0.5 * (pa.X() + pb.X()), 0.5 * (pa.Y() + pb.Y()),
                       0.5 * (pa.Z() + pb.Z()));
        BRepClass3d_SolidClassifier inside(shape, m, 1e-7);
        if (inside.State() == TopAbs_IN) {
          f.throughMaterial = true;
          break;
        }
      }
      Attribute(part, body, body.elementMap.NameOf(static_cast<int>(i)), f);
      // The face a feature other than the body's own made names it best:
      // a slot's wall, not the plate it was cut in.
      GeometryFinding other;
      Attribute(part, body, body.elementMap.NameOf(static_cast<int>(j)), other);
      f.otherElement = other.element;
      if (!f.suppressible && other.suppressible) {
        f.feature = other.feature;
        f.featureName = other.featureName;
        f.suppressible = true;
      }
      out.findings.push_back(std::move(f));
    }
}

uint64_t GeometryHash(const SimGeometry& geometry) {
  if (geometry.Empty())
    return 0;
  std::vector<SuppressedFeature> s = geometry.suppressed;
  std::sort(s.begin(), s.end(), [](const auto& a, const auto& b) {
    return a.document != b.document ? a.document < b.document
                                    : a.feature < b.feature;
  });
  inputhash::Hasher h;
  h.Str("sim-geometry/1");
  h.U64(s.size());
  for (const SuppressedFeature& f : s) {
    h.U64(f.document);
    h.U64(f.feature);
  }
  h.U64(geometry.symmetry.datum);
  h.U64(geometry.symmetry.face.featureId);
  h.Str(geometry.symmetry.face.name);
  h.U64(geometry.symmetry.flip ? 1 : 0);
  const uint64_t d = h.Digest();
  return d == 0 ? 1 : d;
}

} // namespace fem
