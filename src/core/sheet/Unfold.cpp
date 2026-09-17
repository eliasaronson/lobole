#include "core/sheet/Unfold.h"

#include "core/ElementHistory.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <algorithm>
#include <cmath>
#include <deque>

namespace sheet {
namespace {

gp_Pnt P(const glm::dvec3& v) { return gp_Pnt(v.x, v.y, v.z); }
gp_Dir D(const glm::dvec3& v) { return gp_Dir(v.x, v.y, v.z); }
glm::dvec3 G(const gp_Pnt& p) { return {p.X(), p.Y(), p.Z()}; }

// A block covering everything on the +`n` side of the plane through `at`,
// out to `size`. Built as a prism of a square rather than a gp_Ax2 box: the
// frame here is not guaranteed right-handed, and an Ax2's implied third axis
// would then point the wrong way on half the bends.
CADShape HalfSpace(const gp_Pnt& at, const gp_Dir& n, double size) {
  gp_Dir a = std::abs(n.Z()) < 0.9 ? gp_Dir(gp_Vec(n).Crossed(gp_Vec(0, 0, 1)))
                                   : gp_Dir(gp_Vec(n).Crossed(gp_Vec(1, 0, 0)));
  const gp_Dir b(gp_Vec(n).Crossed(gp_Vec(a)));
  const gp_Vec va = gp_Vec(a) * size, vb = gp_Vec(b) * size;
  const gp_Pnt base = at.Translated(-va - vb);
  const gp_Pnt p10 = base.Translated(va * 2.0);
  const gp_Pnt p11 = p10.Translated(vb * 2.0);
  const gp_Pnt p01 = base.Translated(vb * 2.0);
  BRepBuilderAPI_MakeWire wire(BRepBuilderAPI_MakeEdge(base, p10).Edge(),
                               BRepBuilderAPI_MakeEdge(p10, p11).Edge(),
                               BRepBuilderAPI_MakeEdge(p11, p01).Edge(),
                               BRepBuilderAPI_MakeEdge(p01, base).Edge());
  BRepBuilderAPI_MakeFace face(wire.Wire(), Standard_True);
  return CADShape(BRepPrimAPI_MakePrism(face.Face(), gp_Vec(n) * size).Shape());
}

struct Piece {
  CADShape shape;
  ElementMap map;
  gp_Trsf place; // where it ends up in the flat
  bool placed = false;
};

// Is `p` inside `shape`? Used to work out which piece a bend belongs to, and
// which side of it is parent and which child.
bool Contains(const CADShape& shape, const gp_Pnt& p) {
  if (!shape.IsValid())
    return false;
  BRepClass3d_SolidClassifier classify(shape.GetShape(), p, 1e-6);
  return classify.State() == TopAbs_IN || classify.State() == TopAbs_ON;
}

// The connected solid of `shape` that holds `probe`, with the names its faces
// carried on the whole.
//
// A tangent plane is INFINITE, so cutting at one catches every scrap of the
// body on that side of it — two flanges bent up from the same panel both
// stand above the first one's plane, and without this the second wall is
// counted in the first wall's piece AND in its own. The blank then comes back
// with an extra wall in it and a volume that is merely plausible.
bool KeepComponentAt(const CADShape& shape, const ElementMap& map,
                     const gp_Pnt& probe, CADShape& outShape,
                     ElementMap& outMap) {
  if (!shape.IsValid())
    return false;
  std::vector<TopoDS_Shape> solids;
  for (TopExp_Explorer ex(shape.GetShape(), TopAbs_SOLID); ex.More(); ex.Next())
    solids.push_back(ex.Current());
  if (solids.size() <= 1) {
    outShape = shape;
    outMap = map;
    return shape.IsValid();
  }
  TopTools_IndexedMapOfShape allFaces;
  TopExp::MapShapes(shape.GetShape(), TopAbs_FACE, allFaces);
  for (const TopoDS_Shape& solid : solids) {
    BRepClass3d_SolidClassifier classify(solid, probe, 1e-6);
    if (classify.State() != TopAbs_IN && classify.State() != TopAbs_ON)
      continue;
    outShape = CADShape(solid);
    outMap = ElementMap{};
    // The component's faces are the SAME subshapes, so their names come
    // across by looking each one up in the whole's face order.
    int i = 0;
    for (TopExp_Explorer ex(solid, TopAbs_FACE); ex.More(); ex.Next(), ++i) {
      const int whole = allFaces.FindIndex(ex.Current());
      if (whole == 0)
        continue;
      if (const std::string* n = map.NameOf(whole - 1))
        outMap.names[i] = *n;
    }
    return true;
  }
  return false;
}

// The piece of `pieces` holding `p`, or -1.
int PieceAt(const std::vector<Piece>& pieces, const gp_Pnt& p) {
  for (size_t i = 0; i < pieces.size(); ++i)
    if (Contains(pieces[i].shape, p))
      return static_cast<int>(i);
  return -1;
}

// The planar face of `piece` lying in the plane through `at` with normal `n`
// — the cut face the bend's slab is rebuilt from. Nearest-to-the-axis wins
// when a panel happens to have two faces in one plane.
bool CutFace(const CADShape& piece, const gp_Pnt& at, const gp_Dir& n,
             TopoDS_Face& out) {
  double best = 1e30;
  bool found = false;
  for (TopExp_Explorer ex(piece.GetShape(), TopAbs_FACE); ex.More();
       ex.Next()) {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    BRepAdaptor_Surface surf(face);
    if (surf.GetType() != GeomAbs_Plane)
      continue;
    const gp_Pln pln = surf.Plane();
    if (!pln.Axis().Direction().IsParallel(n, 1e-6))
      continue;
    if (std::abs(pln.Distance(at)) > 1e-6)
      continue;
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const double d = props.CentreOfMass().Distance(at);
    if (d < best) {
      best = d;
      out = face;
      found = true;
    }
  }
  return found;
}

CADShape Transformed(const CADShape& s, const gp_Trsf& t) {
  if (!s.IsValid())
    return {};
  BRepBuilderAPI_Transform tr(s.GetShape(), t, Standard_True);
  return tr.IsDone() ? CADShape(tr.Shape()) : CADShape{};
}

} // namespace

std::optional<FlatResult> Unfold(const Body& body, const SheetMetalRules& rules,
                                 const ElementRef& fixedFace, uint64_t tag,
                                 std::string* error) {
  const auto fail = [&](const std::string& why) -> std::optional<FlatResult> {
    if (error)
      *error = why;
    return std::nullopt;
  };
  if (!body.shape.IsValid())
    return fail("the body has no geometry to flatten");

  // Already flattened: the creases keep their angle and radius so a bend note
  // can be written, which is exactly why unfolding again has to be refused
  // rather than quietly producing a longer blank.
  for (const BendRegion& b : body.bends) {
    if (b.flat)
      return fail("this body is already a flat pattern - there is one flat "
                  "pattern per sheet body, and it goes last");
  }

  // A blank is already flat, and saying so beats refusing.
  if (body.bends.empty()) {
    FlatResult flat;
    flat.shape = body.shape;
    flat.map = body.elementMap;
    return flat;
  }

  ArmOccSignalHandlers();
  const double t = rules.thickness;
  glm::vec3 lo, hi;
  body.shape.GetBoundingBox(lo, hi);
  const double size = glm::length(hi - lo) * 4.0 + 100.0;

  try {
    // ---- 1. Partition, entirely in FOLDED coordinates -------------------
    // Every recorded plane is valid here and nowhere else, so all the cutting
    // happens before anything moves.
    std::vector<Piece> pieces;
    pieces.push_back({body.shape, body.elementMap, gp_Trsf(), false});

    struct Link {
      size_t bend = 0;
      int parent = -1, child = -1, slab = -1;
    };
    std::vector<Link> links;

    for (size_t bi = 0; bi < body.bends.size(); ++bi) {
      const BendRegion& b = body.bends[bi];
      const gp_Pnt axis = P(b.axisOrigin);
      const gp_Dir parentDir = D(b.parentDir);
      const gp_Dir up = D(b.upDir);
      // The child's direction, and so the far tangent plane's normal.
      const gp_Vec childVec = gp_Vec(parentDir) * std::cos(b.angle) +
                              gp_Vec(up) * std::sin(b.angle);
      const gp_Dir childDir(childVec);
      // A point in the middle of the bend's own material.
      const gp_Vec mid = gp_Vec(parentDir) * std::sin(b.angle / 2.0) -
                         gp_Vec(up) * std::cos(b.angle / 2.0);
      const gp_Pnt probe = axis.Translated(mid * (b.radius + t / 2.0));
      const int pi = PieceAt(pieces, probe);
      if (pi < 0)
        return fail("a recorded bend is not on this body any more - the "
                    "flat pattern must sit after every feature that "
                    "reshapes the sheet");

      // Where the material definitely is, on each side of the bend: one
      // thickness back into the parent panel, one thickness out along the
      // child wall, and the middle of the bend's own arc.
      const gp_Pnt parentProbe = axis.Translated(
          -gp_Vec(up) * (b.radius + t / 2.0) - gp_Vec(parentDir) * t);
      const gp_Vec childOut = gp_Vec(parentDir) * std::sin(b.angle) -
                              gp_Vec(up) * std::cos(b.angle);
      const gp_Pnt childProbe = axis.Translated(
          childOut * (b.radius + t / 2.0) + gp_Vec(childDir) * t);

      const auto cut = [&](const CADShape& src, const ElementMap& srcMap,
                           const gp_Dir& n, const gp_Pnt& keep,
                           CADShape& outShape, ElementMap& outMap) {
        ElementMap blockMap, wholeMap;
        std::string why;
        const CADShape whole = topo::BooleanNamed(
            BooleanOp::Intersect, src, srcMap, HalfSpace(axis, n, size),
            blockMap, tag, wholeMap, &why);
        if (!KeepComponentAt(whole, wholeMap, keep, outShape, outMap))
          outShape = {};
      };

      Piece parent, child, slab;
      cut(pieces[pi].shape, pieces[pi].map, parentDir.Reversed(), parentProbe,
          parent.shape, parent.map);
      cut(pieces[pi].shape, pieces[pi].map, childDir, childProbe, child.shape,
          child.map);
      CADShape front;
      ElementMap frontMap;
      cut(pieces[pi].shape, pieces[pi].map, parentDir, probe, front, frontMap);
      if (front.IsValid()) {
        ElementMap blockMap, wholeMap;
        std::string why;
        const CADShape whole =
            topo::BooleanNamed(BooleanOp::Intersect, front, frontMap,
                               HalfSpace(axis, childDir.Reversed(), size),
                               blockMap, tag, wholeMap, &why);
        if (!KeepComponentAt(whole, wholeMap, probe, slab.shape, slab.map))
          slab.shape = {};
      }
      if (!parent.shape.IsValid() || !child.shape.IsValid())
        return fail("a bend could not be separated from the panels it joins");

      Link link;
      link.bend = bi;
      pieces[pi] = parent;
      link.parent = pi;
      pieces.push_back(child);
      link.child = static_cast<int>(pieces.size() - 1);
      if (slab.shape.IsValid() && slab.shape.Volume() > 1e-12) {
        pieces.push_back(slab);
        link.slab = static_cast<int>(pieces.size() - 1);
      }
      links.push_back(link);
    }

    // ---- 2. Walk the tree, placing each piece ---------------------------
    int root = 0;
    if (fixedFace.IsSet()) {
      // The named face stays put; whichever piece still carries it is root.
      for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].map.Resolve(fixedFace.name)) {
          root = static_cast<int>(i);
          break;
        }
      }
    }
    pieces[root].placed = true;
    pieces[root].place = gp_Trsf();

    std::vector<BendRegion> creases;
    std::deque<int> queue{root};
    std::vector<bool> done(links.size(), false);
    while (!queue.empty()) {
      const int at = queue.front();
      queue.pop_front();
      for (size_t li = 0; li < links.size(); ++li) {
        if (done[li] || links[li].parent != at)
          continue;
        const BendRegion& b = body.bends[links[li].bend];
        const double k =
            b.kFactor > 0.0 ? b.kFactor : rules.EffectiveKFactor(b.radius);
        const double ba = BendAllowance(b.angle, b.radius, k, t);

        // Unfold: rotate the child back into the parent's plane about the
        // bend axis, then slide it out by the allowance. Those two together
        // ARE the flat pattern; everything else is bookkeeping.
        // A right-handed turn of `angle` about parentDir x upDir is what
        // FOLDED the wall up (it carries parentDir onto the child's
        // direction), so unfolding is the same axis and the NEGATIVE angle.
        // Get the sign wrong and the wall still lands flat and still has the
        // right volume — it just lands on the wrong side of the bend line,
        // which is why the tests check the blank's outline and not only its
        // thickness.
        const gp_Dir unfoldAxis(
            gp_Vec(D(b.parentDir)).Crossed(gp_Vec(D(b.upDir))));
        gp_Trsf rot;
        rot.SetRotation(gp_Ax1(P(b.axisOrigin), unfoldAxis), -b.angle);
        gp_Trsf slide;
        slide.SetTranslation(gp_Vec(D(b.parentDir)) * ba);
        const gp_Trsf local = slide * rot;

        const gp_Trsf full = pieces[at].place * local;
        pieces[links[li].child].place = full;
        pieces[links[li].child].placed = true;
        done[li] = true;
        queue.push_back(links[li].child);

        // The crease, in flat coordinates: the centre of the band the bend
        // becomes, which is what a bend line is drawn along.
        BendRegion crease = b;
        crease.allowance = ba;
        crease.flat = true;
        const gp_Vec half = gp_Vec(D(b.parentDir)) * (ba / 2.0);
        const gp_Pnt tangent =
            P(b.axisOrigin).Translated(gp_Vec(D(b.upDir)) * -b.radius);
        const gp_Pnt a = tangent.Translated(half).Transformed(pieces[at].place);
        // The crease runs the bend's own width along its axis — a unit step
        // would draw every bend line the same length whatever the flange.
        const gp_Vec along =
            gp_Vec(D(b.axisDir)).Transformed(pieces[at].place) * b.width;
        crease.flatA = G(a);
        crease.flatB = G(a.Translated(along));
        creases.push_back(std::move(crease));
      }
    }

    // ---- 3. Rebuild each bend as a flat strip, and fuse ------------------
    CADShape flat;
    ElementMap flatMap;
    const auto join = [&](const CADShape& piece, const ElementMap& map) {
      if (!piece.IsValid() || piece.Volume() <= 1e-12)
        return true;
      if (!flat.IsValid()) {
        flat = piece;
        flatMap = map;
        return true;
      }
      ElementMap out;
      std::string why;
      CADShape fused = topo::BooleanNamed(BooleanOp::Union, flat, flatMap,
                                          piece, map, tag, out, &why);
      if (!fused.IsValid())
        return false;
      flat = std::move(fused);
      flatMap = std::move(out);
      return true;
    };

    std::vector<bool> isSlab(pieces.size(), false);
    for (const Link& l : links)
      if (l.slab >= 0)
        isSlab[l.slab] = true;
    for (size_t i = 0; i < pieces.size(); ++i) {
      const Piece& p = pieces[i];
      if (isSlab[i])
        continue; // replaced by its flat strip below
      if (!p.placed) {
        // A panel the walk never reached is a panel that would vanish from
        // the blank — silently, and the blank would still look like a part.
        return fail("part of this body is not connected to the fixed panel "
                    "by any recorded bend, so it has no place in the blank");
      }
      if (!join(Transformed(p.shape, p.place), p.map))
        return fail("the flat pattern's panels would not join up");
    }
    for (size_t li = 0; li < links.size(); ++li) {
      if (!done[li])
        continue;
      const BendRegion& b = body.bends[links[li].bend];
      const double k =
          b.kFactor > 0.0 ? b.kFactor : rules.EffectiveKFactor(b.radius);
      const double ba = BendAllowance(b.angle, b.radius, k, t);
      // The strip is the parent's own cut face swept by the allowance. Using
      // the FACE rather than a rectangle is what makes a notched or profiled
      // seam come out right.
      TopoDS_Face seam;
      const Piece& parent = pieces[links[li].parent];
      if (!CutFace(parent.shape, P(b.axisOrigin), D(b.parentDir), seam))
        return fail("a bend's seam face could not be found on its panel");
      CADShape strip(
          BRepPrimAPI_MakePrism(seam, gp_Vec(D(b.parentDir)) * ba).Shape());
      ElementMap stripMap;
      topo::NamePrimitiveElements(
          strip, std::to_string(tag) + "/bend" + std::to_string(li), stripMap);
      if (!join(Transformed(strip, parent.place), stripMap))
        return fail("a bend's flat strip would not join its panels");
    }

    if (!flat.IsValid())
      return fail("the flat pattern came out empty");

    FlatResult result;
    result.shape = std::move(flat);
    result.map = std::move(flatMap);
    result.creases = std::move(creases);
    return result;
  } catch (const Standard_Failure& e) {
    return fail(std::string("flattening failed: ") + e.GetMessageString());
  }
}

} // namespace sheet
