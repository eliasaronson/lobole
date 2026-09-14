#include "core/ElementHistory.h"

#include "common/Logger.h"

#include <BRepAlgoAPI_BooleanOperation.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepTools_History.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>

#include <algorithm>
#include <cfloat>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace topo {

namespace {

// One boolean operand: its shape and the names it carries (may be empty).
struct NamedInput {
  const TopoDS_Shape& shape;
  const ElementMap& map;
};

// Every face and edge of `shape`, in the index order ElementMap uses.
void MapSubshapes(const TopoDS_Shape& shape, TopTools_IndexedMapOfShape& faces,
                  TopTools_IndexedMapOfShape& edges) {
  TopExp::MapShapes(shape, TopAbs_FACE, faces);
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
}

// Writes `name` for `sub` (a face or edge of the result) into `out`, keyed by
// the result's index maps. First writer wins: inputs are processed target
// first, tool second, index order within each, so collisions (e.g. both
// operands generating the same section edge) resolve deterministically.
struct Placer {
  const TopTools_IndexedMapOfShape& resFaces;
  const TopTools_IndexedMapOfShape& resEdges;
  ElementMap& out;

  void operator()(const TopoDS_Shape& sub, const std::string& name) const {
    if (sub.ShapeType() == TopAbs_FACE) {
      const int idx = resFaces.FindIndex(sub) - 1;
      if (idx >= 0) {
        out.names.try_emplace(idx, name);
      }
    } else if (sub.ShapeType() == TopAbs_EDGE) {
      const int idx = resEdges.FindIndex(sub) - 1;
      if (idx >= 0) {
        out.edgeNames.try_emplace(idx, name);
      }
    }
  }
};

// Places every shape of `list`, disambiguating a 1->N fan-out with a
// positional ".<i>" suffix ordered by result index (the one place stability
// leaks — accepted for our history depths).
void PlaceAll(const TopTools_ListOfShape& list, const std::string& base,
              const Placer& place, const TopTools_IndexedMapOfShape& resFaces,
              const TopTools_IndexedMapOfShape& resEdges) {
  if (list.Extent() == 1) {
    place(list.First(), base);
    return;
  }
  std::vector<std::pair<int, const TopoDS_Shape*>> ordered;
  for (const TopoDS_Shape& s : list) {
    const auto& m = s.ShapeType() == TopAbs_FACE ? resFaces : resEdges;
    const int idx = m.FindIndex(s) - 1;
    if (idx >= 0) {
      ordered.emplace_back(idx, &s);
    }
  }
  // Faces and edges occupy separate index spaces; keep the suffix sequence
  // deterministic across the mix by sorting on (type, index).
  std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
    if (a.second->ShapeType() != b.second->ShapeType()) {
      return a.second->ShapeType() < b.second->ShapeType();
    }
    return a.first < b.first;
  });
  for (size_t i = 0; i < ordered.size(); i++) {
    place(*ordered[i].second, base + "." + std::to_string(i));
  }
}

// The core rewrite: input MappedNames -> result ElementMap, driven by a
// normalized BRepTools_History plus the boolean's section edges (empty list
// for non-boolean ops), finished by the U/L fallback passes.
ElementMap PropagateNames(const std::vector<NamedInput>& inputs,
                          const BRepTools_History& hist,
                          const TopTools_ListOfShape& seamEdges,
                          const TopoDS_Shape& result, uint64_t featureTag) {
  ElementMap out;
  TopTools_IndexedMapOfShape resFaces, resEdges;
  MapSubshapes(result, resFaces, resEdges);
  const Placer place{resFaces, resEdges, out};

  const std::string tag = std::to_string(featureTag);
  const std::string mTag = ";M" + tag;
  const std::string gTag = ";G" + tag;

  // History pass: survivor keeps its name; Modified appends M; Generated
  // appends G (booleans: a pierced face *generates* its section edges, a
  // fillet spine edge generates its blend face); removed-with-nothing-else
  // names simply die, so references to them fail — the intended signal.
  for (const NamedInput& input : inputs) {
    TopTools_IndexedMapOfShape faces, edges;
    MapSubshapes(input.shape, faces, edges);
    auto propagate = [&](const TopoDS_Shape& sub, const std::string& name) {
      const TopTools_ListOfShape& mods = hist.Modified(sub);
      if (mods.IsEmpty() && !hist.IsRemoved(sub)) {
        place(sub, name); // survivor: identity preserved for free
      } else if (!mods.IsEmpty()) {
        PlaceAll(mods, name + mTag, place, resFaces, resEdges);
      }
      const TopTools_ListOfShape& gens = hist.Generated(sub);
      if (!gens.IsEmpty()) {
        PlaceAll(gens, name + gTag, place, resFaces, resEdges);
      }
    };
    for (const auto& [faceIdx, name] : input.map.names) {
      if (faceIdx >= 0 && faceIdx + 1 <= faces.Extent()) {
        propagate(faces(faceIdx + 1), name);
      }
    }
    for (const auto& [edgeIdx, name] : input.map.edgeNames) {
      if (edgeIdx >= 0 && edgeIdx + 1 <= edges.Extent()) {
        propagate(edges(edgeIdx + 1), name);
      }
    }
  }

  // Boolean section edges history did not reach: "<tag>/seam<i>".
  int seam = 0;
  for (const TopoDS_Shape& e : seamEdges) {
    // A section edge handed back by the maker may itself have been modified.
    const TopTools_ListOfShape& mods = hist.Modified(e);
    if (mods.IsEmpty()) {
      place(e, tag + "/seam" + std::to_string(seam++));
    } else {
      for (const TopoDS_Shape& m : mods) {
        place(m, tag + "/seam" + std::to_string(seam++));
      }
    }
  }

  return out;
}

// U pass then L pass over `shape`, writing into `map` (see header).
void FallbackPasses(const TopoDS_Shape& shape, ElementMap& map) {
  TopTools_IndexedMapOfShape resFaces, resEdges;
  MapSubshapes(shape, resFaces, resEdges);

  // U: a named face names its unnamed boundary edges. localIdx counts every
  // unique boundary edge (named or not, deduped — a closed surface's seam
  // edge appears twice in the wire), so the index is stable.
  for (const auto& [faceIdx, faceName] : map.names) {
    if (faceIdx + 1 > resFaces.Extent()) {
      continue;
    }
    TopTools_IndexedMapOfShape boundary;
    TopExp::MapShapes(resFaces(faceIdx + 1), TopAbs_EDGE, boundary);
    for (int k = 1; k <= boundary.Extent(); k++) {
      const int idx = resEdges.FindIndex(boundary(k)) - 1;
      if (idx >= 0) {
        map.edgeNames.try_emplace(idx, faceName + ";U" + std::to_string(k - 1));
      }
    }
  }

  // L: an unnamed face is named from its sorted boundary-edge names (catches
  // fillet corner patches bounded by the blend faces' U edges, seam faces,
  // revolve caps the history missed).
  for (int f = 1; f <= resFaces.Extent(); f++) {
    if (map.names.count(f - 1)) {
      continue;
    }
    TopTools_IndexedMapOfShape boundary;
    TopExp::MapShapes(resFaces(f), TopAbs_EDGE, boundary);
    std::vector<std::string> edgeNames;
    for (int k = 1; k <= boundary.Extent(); k++) {
      const int idx = resEdges.FindIndex(boundary(k)) - 1;
      if (const std::string* n = idx >= 0 ? map.EdgeNameOf(idx) : nullptr) {
        edgeNames.push_back(*n);
      }
    }
    if (edgeNames.empty()) {
      continue;
    }
    std::sort(edgeNames.begin(), edgeNames.end());
    std::string joined;
    for (const std::string& n : edgeNames) {
      joined += joined.empty() ? n : "," + n;
    }
    map.names[f - 1] = joined + ";L";
  }
}

// All faces and edges of `shape`, as the explicit argument list
// BRepTools_History's template constructor requires (it queries each argument
// directly — handing it the solid yields a silently empty history).
void AppendSubshapeArgs(const TopoDS_Shape& shape, TopTools_ListOfShape& args) {
  TopTools_IndexedMapOfShape faces, edges;
  MapSubshapes(shape, faces, edges);
  for (int i = 1; i <= faces.Extent(); i++) {
    args.Append(faces(i));
  }
  for (int i = 1; i <= edges.Extent(); i++) {
    args.Append(edges(i));
  }
}

} // namespace

CADShape BooleanNamed(BooleanOp op, const CADShape& target,
                      const ElementMap& targetMap, const CADShape& tool,
                      const ElementMap& toolMap, uint64_t featureTag,
                      ElementMap& out, std::string* error) {
  out = {};
  if (!target.IsValid() || !tool.IsValid()) {
    if (error)
      *error = "boolean input shape is invalid";
    return CADShape();
  }
  try {
    std::unique_ptr<BRepAlgoAPI_BooleanOperation> mk;
    switch (op) {
    case BooleanOp::Union:
      mk = std::make_unique<BRepAlgoAPI_Fuse>(target.GetShape(),
                                              tool.GetShape());
      break;
    case BooleanOp::Subtract:
      mk =
          std::make_unique<BRepAlgoAPI_Cut>(target.GetShape(), tool.GetShape());
      break;
    case BooleanOp::Intersect:
      mk = std::make_unique<BRepAlgoAPI_Common>(target.GetShape(),
                                                tool.GetShape());
      break;
    }
    if (!mk->IsDone()) {
      if (error)
        *error = "boolean operation did not complete";
      return CADShape();
    }
    const TopoDS_Shape result = mk->Shape();

    // Normalize the maker's reports onto BRepTools_History (T1 prereq 5).
    TopTools_ListOfShape args;
    AppendSubshapeArgs(target.GetShape(), args);
    AppendSubshapeArgs(tool.GetShape(), args);
    const BRepTools_History hist(args, *mk);

    out = PropagateNames(
        {{target.GetShape(), targetMap}, {tool.GetShape(), toolMap}}, hist,
        mk->SectionEdges(), result, featureTag);
    FallbackPasses(result, out);
    return CADShape(result);
  } catch (const Standard_Failure& e) {
    LOG_ERROR("BooleanNamed: OCCT error: {}", e.GetMessageString());
    if (error)
      *error = std::string("OCCT error: ") + e.GetMessageString();
    out = {};
    return CADShape();
  }
}

namespace {

// Shared tail of both blend makers: build, verify, propagate, fall back.
template <typename Maker>
CADShape RunEdgeBlend(Maker& mk, const TopoDS_Shape& shape,
                      const ElementMap& bodyMap, uint64_t featureTag,
                      ElementMap& out, std::string* error) {
  mk.Build();
  if (!mk.IsDone()) {
    if (error) {
      *error = "the blend could not be built (radius too large for the "
               "geometry?)";
    }
    return CADShape();
  }
  const TopoDS_Shape result = mk.Shape();
  if (result.IsNull()) {
    return CADShape();
  }

  TopTools_ListOfShape args;
  AppendSubshapeArgs(shape, args);
  const BRepTools_History hist(args, mk);
  static const TopTools_ListOfShape kNoSeams;
  out = PropagateNames({{shape, bodyMap}}, hist, kNoSeams, result, featureTag);
  FallbackPasses(result, out);
  return CADShape(result);
}

} // namespace

CADShape EdgeBlendNamed(const CADShape& body, const ElementMap& bodyMap,
                        const std::vector<int>& edgeIndices, EdgeBlendOp op,
                        double radiusOrDistance, uint64_t featureTag,
                        ElementMap& out, std::string* error) {
  out = {};
  if (!body.IsValid() || edgeIndices.empty()) {
    return CADShape();
  }
  // The blend builder segfaults on a set it cannot build rather than
  // reporting a faulty contour, so the catch below only sees anything if
  // OCC's signals are exceptions first.
  InstallOccSignalHandlersOnce();
  try {
    const TopoDS_Shape& shape = body.GetShape();
    TopTools_IndexedMapOfShape edges;
    TopExp::MapShapes(shape, TopAbs_EDGE, edges);
    for (int idx : edgeIndices) {
      if (idx < 0 || idx + 1 > edges.Extent()) {
        if (error) {
          *error = "edge index out of range";
        }
        return CADShape();
      }
    }

    if (op == EdgeBlendOp::Chamfer) {
      BRepFilletAPI_MakeChamfer mk(shape);
      for (int idx : edgeIndices) {
        mk.Add(radiusOrDistance, TopoDS::Edge(edges(idx + 1)));
      }
      return RunEdgeBlend(mk, shape, bodyMap, featureTag, out, error);
    }
    BRepFilletAPI_MakeFillet mk(shape);
    for (int idx : edgeIndices) {
      mk.Add(radiusOrDistance, TopoDS::Edge(edges(idx + 1)));
    }
    CADShape result = RunEdgeBlend(mk, shape, bodyMap, featureTag, out, error);
    if (!result.IsValid() && error && mk.NbFaultyContours() > 0) {
      *error = "the fillet failed on " + std::to_string(mk.NbFaultyContours()) +
               " contour(s) (radius too large for the geometry?)";
    }
    return result;
  } catch (const Standard_Failure& e) {
    LOG_ERROR("EdgeBlendNamed: OCCT error: {}", e.GetMessageString());
    if (error) {
      *error = std::string("OCCT error: ") + e.GetMessageString();
    }
    out = {};
    return CADShape();
  }
}

void NamePrimitiveElements(const CADShape& shape, const std::string& prefix,
                           ElementMap& out) {
  out = {};
  if (!shape.IsValid()) {
    return;
  }
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(shape.GetShape(), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); i++) {
    out.names[i - 1] = prefix + "/face" + std::to_string(i - 1);
  }
  FallbackPasses(shape.GetShape(), out);
}

void ApplyFallbackPasses(const CADShape& shape, ElementMap& map) {
  if (!shape.IsValid()) {
    return;
  }
  FallbackPasses(shape.GetShape(), map);
}

int EdgeIndexOf(const TopoDS_Shape& shape, const TopoDS_Shape& edge) {
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(shape, TopAbs_EDGE, edges);
  return edges.FindIndex(edge) - 1;
}

glm::vec3 PolylineArcMidpoint(const std::vector<glm::vec3>& poly) {
  float total = 0.0f;
  for (size_t i = 0; i + 1 < poly.size(); i++)
    total += glm::length(poly[i + 1] - poly[i]);
  float half = total * 0.5f;
  for (size_t i = 0; i + 1 < poly.size(); i++) {
    const float seg = glm::length(poly[i + 1] - poly[i]);
    if (half <= seg)
      return poly[i] +
             (seg > 0.0f ? half / seg : 0.0f) * (poly[i + 1] - poly[i]);
    half -= seg;
  }
  return poly.empty() ? glm::vec3(0.0f) : poly.back();
}

std::optional<ElementRef> ResolveEdgeOnBody(const Body& input,
                                            const std::string& pickedName,
                                            const glm::vec3& probe) {
  if (input.elementMap.ResolveEdge(pickedName).has_value()) {
    return ElementRef{input.createdBy, pickedName};
  }

  // Geometric fallback: an edge untouched by the downstream op is identical
  // on both body states, so `probe` lies exactly on its input polyline.
  const auto distToPoly = [](const glm::vec3& p,
                             const std::vector<glm::vec3>& poly) {
    float d = FLT_MAX;
    for (size_t i = 0; i + 1 < poly.size(); i++) {
      const glm::vec3 a = poly[i];
      const glm::vec3 ab = poly[i + 1] - a;
      const float len2 = glm::dot(ab, ab);
      const float t = len2 > 0.0f
                          ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f)
                          : 0.0f;
      d = std::min(d, glm::length(p - (a + ab * t)));
    }
    return d;
  };
  const auto polys = input.shape.GenerateEdgePolylines();
  int best = -1;
  float bestD = 1e-3f; // mm; identical geometry, so effectively exact
  for (size_t e = 0; e < polys.size(); e++) {
    if (polys[e].size() < 2) {
      continue;
    }
    const float d = distToPoly(probe, polys[e]);
    if (d < bestD) {
      bestD = d;
      best = static_cast<int>(e);
    }
  }
  if (best < 0) {
    return std::nullopt;
  }
  const std::string* name = input.elementMap.EdgeNameOf(best);
  if (name == nullptr) {
    return std::nullopt;
  }
  return ElementRef{input.createdBy, *name};
}

} // namespace topo
