#include "cad/CADKernel.h"

#include <cmath>
#include <deque>
#include <filesystem>
#include <functional>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#ifdef HAVE_OPENCASCADE
#pragma message("Compiling with OpenCASCADE support")

#include "cad/XcafDocument.h"
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Shell.hxx>
#include <BRepClass3d_SolidClassifier.hxx>
#include <BRepGProp.hxx>
#include <BRepLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_TangentialDeflection.hxx>
#include <GProp_GProps.hxx>
#include <GeomAbs_CurveType.hxx>
#include <GeomAbs_SurfaceType.hxx>
#include <GeomLProp_SLProps.hxx>
#include <Geom_Surface.hxx>
#include <Interface_Static.hxx>
#include <Message_ProgressIndicator.hxx>
#include <Message_ProgressScope.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <Precision.hxx>
#include <Quantity_Color.hxx>
#include <STEPCAFControl_Reader.hxx>
#include <STEPCAFControl_Writer.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <Standard_Failure.hxx>
#include <Standard_Handle.hxx>
#include <Standard_Version.hxx>
#include <StlAPI_Reader.hxx>
#include <StlAPI_Writer.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TCollection_AsciiString.hxx>
#include <TDF_Label.hxx>
#include <TDF_LabelSequence.hxx>
#include <TDF_Tool.hxx>
#include <TDataStd_Name.hxx>
#include <TDocStd_Document.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListIteratorOfListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_TShape.hxx>
#include <algorithm>

#include <XCAFApp_Application.hxx>
#include <XCAFDoc_ColorTool.hxx>
#include <XCAFDoc_DocumentTool.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Circ.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Mat.hxx>
#include <gp_Pnt2d.hxx>
#include <gp_Quaternion.hxx>
#include <gp_Trsf.hxx>

// CADShape implementation
CADShape::CADShape() {}

CADShape::CADShape(const TopoDS_Shape& shape) : m_shape(shape) {}

CADShape::~CADShape() {}

// Replacing the shape forgets what we remembered about the old one.
// Every in-place transform below goes through here for that reason;
// test_conventions.cpp fails on an assignment that does not.
void CADShape::SetShape(const TopoDS_Shape& shape) {
  m_shape = shape;
  m_boxValid[0] = m_boxValid[1] = false;
}

bool CADShape::IsValid() const { return !m_shape.IsNull(); }

namespace {

// Volume by shape IDENTITY. Every mutating RPC digests every body of the
// part twice (before and after), and the digest is the volume; a part that
// holds a Derived snapshot of a 197-body PCB spent 14 s per sketch add in
// BRepGProp::VolumeProperties on bodies that had not changed (the SO-101
// gripper, docs/modeling/README.md#the-model). A shape moved by a rigid
// placement shares its TShape, so keying on the TShape (plus the location's
// scale, the one part of a location that changes a volume) makes the second
// and every later pass a lookup. The handle in the entry keeps the TShape
// alive, so its address cannot be recycled for a different shape while the
// entry lives; the ring bounds the memory.
struct VolumeCache {
  struct Entry {
    Handle(TopoDS_TShape) keepAlive;
    double scale = 1.0;
    double volume = 0.0;
  };
  static constexpr size_t kCapacity = 4096;
  std::mutex mutex;
  std::unordered_map<const TopoDS_TShape*, Entry> entries;
  std::deque<const TopoDS_TShape*> order; // insertion order, for eviction

  std::optional<double> Find(const TopoDS_Shape& shape) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = entries.find(shape.TShape().get());
    if (it == entries.end() ||
        it->second.scale != shape.Location().Transformation().ScaleFactor())
      return std::nullopt;
    return it->second.volume * Sign(shape);
  }
  // The same TShape the other way out encloses the negative of its volume,
  // so the entry holds the FORWARD figure and a reversed shape reads it
  // negated. Without this, a solid reversed in place — an inside-out result,
  // the thing BooleanRefusal exists to catch — read as the positive volume
  // cached for its forward twin.
  static double Sign(const TopoDS_Shape& shape) {
    return shape.Orientation() == TopAbs_REVERSED ? -1.0 : 1.0;
  }
  void Store(const TopoDS_Shape& shape, double volume) {
    std::lock_guard<std::mutex> lock(mutex);
    const TopoDS_TShape* key = shape.TShape().get();
    if (entries.find(key) == entries.end()) {
      order.push_back(key);
      while (order.size() > kCapacity) {
        entries.erase(order.front());
        order.pop_front();
      }
    }
    entries[key] = {shape.TShape(),
                    shape.Location().Transformation().ScaleFactor(),
                    volume * Sign(shape)};
  }
};

VolumeCache& TheVolumeCache() {
  static VolumeCache cache;
  return cache;
}

} // namespace

const void* CADShape::GeometryId() const {
  return m_shape.IsNull() ? nullptr
                          : static_cast<const void*>(m_shape.TShape().get());
}

bool CADShape::SameGeometryAs(const CADShape& other) const {
  if (m_shape.IsNull() || other.m_shape.IsNull())
    return false;
  return m_shape.IsEqual(other.m_shape);
}

double CADShape::Volume() const {
  if (!IsValid())
    return 0.0;
  if (const std::optional<double> hit = TheVolumeCache().Find(m_shape))
    return *hit;
  GProp_GProps props;
  BRepGProp::VolumeProperties(m_shape, props);
  TheVolumeCache().Store(m_shape, props.Mass());
  return props.Mass();
}

int CADShape::SolidCount() const {
  if (m_shape.IsNull())
    return 0;
  int count = 0;
  for (TopExp_Explorer ex(m_shape, TopAbs_SOLID); ex.More(); ex.Next())
    count++;
  return count;
}

int CADShape::FaceCount() const {
  if (m_shape.IsNull())
    return 0;
  int count = 0;
  for (TopExp_Explorer ex(m_shape, TopAbs_FACE); ex.More(); ex.Next())
    count++;
  return count;
}

bool CADShape::ComputeMassProps(MassProps& out) const {
  if (!IsValid())
    return false;

  GProp_GProps volumeProps;
  BRepGProp::VolumeProperties(m_shape, volumeProps);
  out.volume = volumeProps.Mass();
  const gp_Pnt com = volumeProps.CentreOfMass();
  out.centerOfMass = glm::vec3(com.X(), com.Y(), com.Z());

  // A GProps' inertia matrix is about its system location — anchor a second
  // pass at the center of mass so the matrix is the physical one.
  GProp_GProps aboutCom(com);
  BRepGProp::VolumeProperties(m_shape, aboutCom);
  const gp_Mat inertia = aboutCom.MatrixOfInertia();
  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 3; c++)
      out.inertia[r][c] = inertia.Value(r + 1, c + 1);

  GProp_GProps surfaceProps;
  BRepGProp::SurfaceProperties(m_shape, surfaceProps);
  out.surfaceArea = surfaceProps.Mass();
  return true;
}

// Memoized ON THE SHAPE, one slot per flavour (see the header for why a
// remembered box cannot go stale, and for the warm-single-threaded rule).
//
// An OUTSIDE cache keyed on PlacementId was tried before this and removed,
// and the difference is the whole point: PlacementId is derived from the
// TShape's ADDRESS, which the allocator recycles, so a freed shape's box
// could be handed to whatever landed on that address next — it showed up as
// the determinism test failing under a parallel run and passing alone. A slot
// that lives in the shape has no key to collide: it is reachable only from
// the object whose TShape it describes, and that object is what keeps the
// TShape alive.
void CADShape::GetBoundingBox(glm::vec3& min, glm::vec3& max,
                              bool tight) const {
  if (!IsValid()) {
    min = max = glm::vec3(0.0f);
    return;
  }

  const int slot = tight ? 1 : 0;
  if (m_boxValid[slot]) {
    min = m_boxMin[slot];
    max = m_boxMax[slot];
    return;
  }

  Bnd_Box box;
  // NOT from the triangulation. `Add` uses one when the shape has been meshed
  // and inflates the box by its deflection, so a part with a single curved
  // face grew ~4 um the first time anything drew or sectioned it. Nothing
  // that reads a bounding box wants an answer that depends on whether the
  // shape has been rendered yet: CAM derives the stock, the rapid height and
  // every depth from this box, and a job generated before the first mesh was
  // then verified against a stock 4 um taller than the one it was cut for —
  // reporting a legitimate rapid to the top of the stock as a rapid THROUGH
  // material, twelve times.
  if (tight) {
    // AddOptimal evaluates the surfaces. Plain `Add` bounds a B-spline by its
    // CONTROL POINTS, which for a swept surface lie well outside it: a
    // four-turn helical spring of radius 11 with 3.2 mm wire — true extent
    // +-12.6 — came back as -24.76..15.34, sixty per cent too big, while an
    // analytic cylinder was exact.
    //
    // It costs: uncached it took the air-cooled engine's build from 1.6 s to
    // 4.2 s. The memo above pays the first ask back on every later one, but a
    // build asks about each new shape ONCE, so the flag still earns its place
    // — see the header.
    BRepBndLib::AddOptimal(m_shape, box, /*useTriangulation=*/Standard_False,
                           /*useShapeTolerance=*/Standard_False);
  } else {
    BRepBndLib::Add(m_shape, box, /*useTriangulation=*/Standard_False);
  }

  if (!box.IsVoid()) {
    double xmin, ymin, zmin, xmax, ymax, zmax;
    box.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    min = glm::vec3(xmin, ymin, zmin);
    max = glm::vec3(xmax, ymax, zmax);
  } else {
    min = max = glm::vec3(0.0f);
  }
  // A void box is remembered too: it is an answer, and re-deriving it costs
  // the same full traversal as any other.
  m_boxMin[slot] = min;
  m_boxMax[slot] = max;
  m_boxValid[slot] = true;
}

CADShape CADShape::Union(const CADShape& other) const {
  if (!IsValid() || !other.IsValid())
    return CADShape();

  BRepAlgoAPI_Fuse fuse(m_shape, other.m_shape);
  if (fuse.IsDone()) {
    return CADShape(fuse.Shape());
  }
  return CADShape();
}

CADShape CADShape::Intersection(const CADShape& other) const {
  if (!IsValid() || !other.IsValid())
    return CADShape();

  BRepAlgoAPI_Common common(m_shape, other.m_shape);
  if (common.IsDone()) {
    return CADShape(common.Shape());
  }
  return CADShape();
}

CADShape CADShape::Difference(const CADShape& other) const {
  if (!IsValid() || !other.IsValid())
    return CADShape();

  BRepAlgoAPI_Cut cut(m_shape, other.m_shape);
  if (cut.IsDone()) {
    return CADShape(cut.Shape());
  }
  return CADShape();
}

// The memoized boxes, carried onto a copy moved by quarter turns: a basis
// whose every entry is 0 or +-1 only permutes and flips the axes, so the
// copy's tight box is this one's, permuted -- exactly, nothing evaluated. A
// CAM setup frames every body through such a move (laid on a face, flipped,
// a placement turned 90 degrees) and each framed copy used to recompute its
// tight box from the surfaces: seconds on a turned blank's tori, every time a
// job tab was opened. Any other angle leaves the copy cold, as before.
void CADShape::CarryBoxes(const glm::mat3& basis, const glm::vec3& shift,
                          CADShape& to) const {
  for (int c = 0; c < 3; c++)
    for (int r = 0; r < 3; r++) {
      const float v = std::abs(basis[c][r]);
      if (v > 1e-5f && std::abs(v - 1.0f) > 1e-5f)
        return;
    }
  // The TIGHT box only: it is the surfaces' true extent, which a quarter
  // turn permutes exactly. The conservative one is bounded in the frame it
  // was taken in, and carried it would depend on which way the shape had
  // been turned first.
  for (const int slot : {1}) {
    if (!m_boxValid[slot])
      continue;
    glm::vec3 lo(0.0f), hi(0.0f);
    for (int r = 0; r < 3; r++) {
      // Row r of the rotated box: the one source axis c that lands on r.
      for (int c = 0; c < 3; c++) {
        const float e = std::round(basis[c][r]);
        if (e == 0.0f)
          continue;
        const float a = e * m_boxMin[slot][c], b = e * m_boxMax[slot][c];
        lo[r] = std::min(a, b) + shift[r];
        hi[r] = std::max(a, b) + shift[r];
      }
    }
    to.m_boxValid[slot] = true;
    to.m_boxMin[slot] = lo;
    to.m_boxMax[slot] = hi;
  }
}

CADShape CADShape::Transformed(const glm::mat4& m) const {
  if (m_shape.IsNull())
    return CADShape();
  gp_Trsf t;
  const glm::mat3 basis(m);
  // Is this a plain rotation+translation? Columns unit-length and a positive
  // determinant. Occurrence placements always are; mirrors and anything
  // exotic take the general path below.
  const float det = glm::determinant(basis);
  const bool rigid = det > 0.0f && std::abs(det - 1.0f) < 1e-3f &&
                     std::abs(glm::length(basis[0]) - 1.0f) < 1e-3f &&
                     std::abs(glm::length(basis[1]) - 1.0f) < 1e-3f &&
                     std::abs(glm::length(basis[2]) - 1.0f) < 1e-3f;
  try {
    if (rigid) {
      // Hand OCC an EXACTLY orthonormal rotation. Routed through SetValues,
      // the rounding in a float rotation makes OCC infer a scale factor a
      // hair off 1 — measured: 0.99999998859 for a 45-degree rotation — and
      // TopoDS_Shape::Moved() refuses to move a shape by a scaled location
      // ("Moving with scaling transformation is forbidden").
      //
      // Which version you have decides whether that matters: constructing the
      // TopLoc_Location succeeds on both 7.6.3 and 7.9.3, and 7.9.3 then
      // MOVES by it happily, while 7.6.3 throws on the same input. Both were
      // measured with the same matrix. So this is not the kernel being picky;
      // it is us having always produced a technically-scaled placement that
      // one version forgave.
      const glm::quat q = glm::normalize(glm::quat_cast(basis));
      t.SetRotation(gp_Quaternion(q.x, q.y, q.z, q.w));
      t.SetTranslationPart(gp_Vec(m[3][0], m[3][1], m[3][2]));
    } else {
      // glm is column-major (m[col][row]); SetValues wants row-major 3x4.
      t.SetValues(m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1],
                  m[3][1], m[0][2], m[1][2], m[2][2], m[3][2]);
    }
  } catch (const Standard_Failure&) {
    return CADShape(m_shape); // sheared matrix: OCC can't represent it
  }
  if (rigid) {
    // A pure placement: attach it as a LOCATION, which copies no geometry.
    try {
      CADShape moved(m_shape.Moved(TopLoc_Location(t)));
      CarryBoxes(basis, glm::vec3(m[3]), moved);
      return moved;
    } catch (const Standard_Failure&) {
      // Moved() enforces "no scaling" and throws Standard_Failure, which is
      // NOT a std::exception — uncaught, it took the whole application down
      // rather than one operation.
      return CADShape(m_shape);
    }
  }
  // Mirrors and anything else non-rigid: APPLY the transform to the geometry.
  // A location cannot carry them — Moved() refuses any scale factor that is
  // not 1, and a mirror's is -1, so OCC 7.6 threw and this returned the shape
  // UNMIRRORED. Silently: a mirrored part came back the wrong handedness and
  // nothing noticed until a drawing looked wrong. (7.9.3 accepts -1 there,
  // which is why it only broke on Ubuntu.)
  try {
    BRepBuilderAPI_Transform xform(m_shape, t, /*copy=*/Standard_True);
    if (xform.IsDone())
      return CADShape(xform.Shape());
  } catch (const Standard_Failure&) {
    // fall through
  }
  return CADShape(m_shape);
}

void CADShape::Translate(const glm::vec3& offset) {
  if (!IsValid())
    return;

  gp_Trsf transform;
  transform.SetTranslation(gp_Vec(offset.x, offset.y, offset.z));

  BRepBuilderAPI_Transform transformer(m_shape, transform);
  if (transformer.IsDone()) {
    SetShape(transformer.Shape());
  }
}

void CADShape::Rotate(const glm::vec3& axis, double angle) {
  if (!IsValid())
    return;

  gp_Ax1 rotationAxis(gp_Pnt(0, 0, 0), gp_Dir(axis.x, axis.y, axis.z));
  gp_Trsf transform;
  transform.SetRotation(rotationAxis, angle);

  BRepBuilderAPI_Transform transformer(m_shape, transform);
  if (transformer.IsDone()) {
    SetShape(transformer.Shape());
  }
}

void CADShape::Scale(double factor) {
  if (!IsValid())
    return;

  gp_Trsf transform;
  transform.SetScale(gp_Pnt(0, 0, 0), factor);

  BRepBuilderAPI_Transform transformer(m_shape, transform);
  if (transformer.IsDone()) {
    SetShape(transformer.Shape());
  }
}

void CADShape::RotateAbout(const glm::vec3& point, const glm::vec3& dir,
                           double angle) {
  if (!IsValid())
    return;

  gp_Ax1 axis(gp_Pnt(point.x, point.y, point.z), gp_Dir(dir.x, dir.y, dir.z));
  gp_Trsf transform;
  transform.SetRotation(axis, angle);

  BRepBuilderAPI_Transform transformer(m_shape, transform);
  if (transformer.IsDone()) {
    SetShape(transformer.Shape());
  }
}

void CADShape::Mirror(const glm::vec3& point, const glm::vec3& normal) {
  if (!IsValid())
    return;

  // The gp_Ax2's XY plane is the mirror plane; its main direction = normal.
  gp_Ax2 plane(gp_Pnt(point.x, point.y, point.z),
               gp_Dir(normal.x, normal.y, normal.z));
  gp_Trsf transform;
  transform.SetMirror(plane);

  BRepBuilderAPI_Transform transformer(m_shape, transform);
  if (transformer.IsDone()) {
    SetShape(transformer.Shape());
  }
}

CADShape CADShape::CompoundOf(const std::vector<CADShape>& parts) {
  TopoDS_Compound compound;
  BRep_Builder builder;
  builder.MakeCompound(compound);
  int added = 0;
  for (const CADShape& p : parts) {
    if (!p.IsValid())
      continue;
    builder.Add(compound, p.GetShape());
    added++;
  }
  if (added == 0)
    return CADShape();
  return CADShape(compound);
}

std::vector<CADShape> CADShape::SplitSolids() const {
  std::vector<CADShape> solids;
  if (!IsValid())
    return solids;
  for (TopExp_Explorer exp(m_shape, TopAbs_SOLID); exp.More(); exp.Next()) {
    solids.emplace_back(exp.Current());
  }
  // A lone solid isn't wrapped in a compound, but TopExp still yields it;
  // if somehow nothing was found, fall back to the shape itself.
  if (solids.empty()) {
    solids.push_back(*this);
  }
  return solids;
}

bool CADShape::FacePlane(int faceIndex, glm::vec3& center, glm::vec3& normal,
                         glm::vec3& uAxis) const {
  if (!IsValid() || faceIndex < 0)
    return false;

  // Walk to the face in the same order GenerateMesh / the ElementMap use.
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) {
      face = TopoDS::Face(exp.Current());
      break;
    }
  }
  if (face.IsNull())
    return false;

  // Sketch-on-face requires a planar face.
  BRepAdaptor_Surface surf(face);
  if (surf.GetType() != GeomAbs_Plane)
    return false;

  // Centroid.
  GProp_GProps props;
  BRepGProp::SurfaceProperties(face, props);
  const gp_Pnt c = props.CentreOfMass();
  center = glm::vec3(c.X(), c.Y(), c.Z());

  // The plane's surface normal, then the face orientation for the outward
  // sense. The AXIS is the surface normal only when the plane's frame is
  // right-handed; a revolve's end faces often carry a left-handed one, and
  // taking the axis there pointed the normal into the solid -- a turned
  // piece laid "underside up" was clamped underside down.
  gp_Dir nd = surf.Plane().Axis().Direction();
  if (!surf.Plane().Position().Direct())
    nd.Reverse();
  glm::vec3 n(nd.X(), nd.Y(), nd.Z());
  if (face.Orientation() == TopAbs_REVERSED)
    n = -n;
  normal = glm::normalize(n);

  // Deterministic in-plane u-axis (matches the sketch-on-face baking
  // heuristic), so the sketch frame is stable across upstream edits.
  glm::vec3 u =
      std::abs(normal.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  uAxis = glm::normalize(u - glm::dot(u, normal) * normal);
  return true;
}

bool CADShape::FaceAxis(int faceIndex, glm::vec3& origin, glm::vec3& axis,
                        glm::vec3& xAxis) const {
  if (!IsValid() || faceIndex < 0)
    return false;

  // Walk to the face in the same order FacePlane / GenerateMesh / picking use.
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) {
      face = TopoDS::Face(exp.Current());
      break;
    }
  }
  if (face.IsNull())
    return false;

  BRepAdaptor_Surface surf(face);
  const GeomAbs_SurfaceType type = surf.GetType();
  if (type == GeomAbs_Sphere) {
    // A sphere has no natural axis — anchor at the CENTER with a
    // deterministic vertical axis. Enough for a ball-mate connector (which
    // only consumes the origin) and predictable everywhere else.
    const gp_Pnt c = surf.Sphere().Location();
    origin = glm::vec3(c.X(), c.Y(), c.Z());
    axis = glm::vec3(0.0f, 0.0f, 1.0f);
    xAxis = glm::vec3(1.0f, 0.0f, 0.0f);
    return true;
  }
  if (type != GeomAbs_Cylinder && type != GeomAbs_Cone)
    return false;

  const gp_Ax1 ax =
      type == GeomAbs_Cone ? surf.Cone().Axis() : surf.Cylinder().Axis();
  const gp_Pnt ap = ax.Location();
  const gp_Dir ad = ax.Direction();
  const glm::vec3 axisPoint(ap.X(), ap.Y(), ap.Z());
  axis = glm::normalize(glm::vec3(ad.X(), ad.Y(), ad.Z()));

  if (type == GeomAbs_Cone) {
    // A cone anchors at its APEX — for a seat face that is exactly where a
    // mating ball's contact ray converges, the point a ball mate wants.
    const gp_Pnt apex = surf.Cone().Apex();
    origin = glm::vec3(apex.X(), apex.Y(), apex.Z());
  } else {
    // Origin = the face centroid projected onto the axis, so the frame sits
    // at the middle of the actual (possibly partial) cylindrical patch
    // rather than at OCC's arbitrary axis location.
    GProp_GProps props;
    BRepGProp::SurfaceProperties(face, props);
    const gp_Pnt c = props.CentreOfMass();
    const glm::vec3 centroid(c.X(), c.Y(), c.Z());
    origin = axisPoint + glm::dot(centroid - axisPoint, axis) * axis;
  }

  // Deterministic perpendicular (matches FacePlane's u-axis heuristic).
  glm::vec3 u =
      std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
  xAxis = glm::normalize(u - glm::dot(u, axis) * axis);
  return true;
}

void CADShape::FaceSnapPoints(int faceIndex, std::vector<FaceSnap>& out) const {
  out.clear();
  if (!IsValid() || faceIndex < 0)
    return;
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) {
      face = TopoDS::Face(exp.Current());
      break;
    }
  }
  if (face.IsNull())
    return;

  // ONCE. This map is the whole reason this function exists: it used to be
  // rebuilt for every edge of the face, from the caller's loop.
  // Global edge order (0-based) matches ElementMap::edgeNames / edge picking.
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) {
    const TopoDS_Edge& edge = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(edge))
      continue;
    const int idx = edges.FindIndex(edge) - 1; // -> 0-based edgeNames index
    if (idx < 0)
      continue;
    FaceSnap snap;
    snap.edgeIndex = idx;
    BRepAdaptor_Curve curve(edge);
    if (curve.GetType() == GeomAbs_Circle) {
      const gp_Pnt c = curve.Circle().Location();
      snap.point = glm::vec3(c.X(), c.Y(), c.Z());
      snap.circleCenter = true;
    } else {
      const gp_Pnt m =
          curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
      snap.point = glm::vec3(m.X(), m.Y(), m.Z());
    }
    out.push_back(snap);
  }
}

void CADShape::FaceCorners(int faceIndex, std::vector<FaceCorner>& out) const {
  out.clear();
  if (!IsValid() || faceIndex < 0)
    return;
  int i = 0;
  TopoDS_Face face;
  for (TopExp_Explorer exp(m_shape, TopAbs_FACE); exp.More(); exp.Next(), i++) {
    if (i == faceIndex) {
      face = TopoDS::Face(exp.Current());
      break;
    }
  }
  if (face.IsNull())
    return;

  // One map for the global edge order, one ancestor pass for the face's
  // vertex -> incident-edge relation. O(edges of the face), not O(pairs).
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  TopTools_IndexedDataMapOfShapeListOfShape vertexToEdges;
  TopExp::MapShapesAndAncestors(face, TopAbs_VERTEX, TopAbs_EDGE,
                                vertexToEdges);

  std::vector<FaceCorner> candidates;
  for (int v = 1; v <= vertexToEdges.Extent(); v++) {
    std::vector<int> incident;
    for (TopTools_ListIteratorOfListOfShape it(vertexToEdges(v)); it.More();
         it.Next()) {
      const TopoDS_Edge& edge = TopoDS::Edge(it.Value());
      if (BRep_Tool::Degenerated(edge))
        continue;
      const int idx = edges.FindIndex(edge) - 1;
      if (idx >= 0 &&
          std::find(incident.begin(), incident.end(), idx) == incident.end())
        incident.push_back(idx);
    }
    // Exactly two edges meeting is what a corner IS. One is an endpoint of a
    // seam or a loop closing on itself; three or more is not a face corner.
    if (incident.size() != 2)
      continue;
    FaceCorner c;
    c.edgeA = std::min(incident[0], incident[1]);
    c.edgeB = std::max(incident[0], incident[1]);
    const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vertexToEdges.FindKey(v)));
    c.point = glm::vec3(p.X(), p.Y(), p.Z());
    candidates.push_back(c);
  }

  // Drop any pair that meets at more than one vertex: two named edges then
  // name two points, so they name neither. Refusing beats picking one.
  for (const FaceCorner& c : candidates) {
    int seen = 0;
    for (const FaceCorner& o : candidates)
      if (o.edgeA == c.edgeA && o.edgeB == c.edgeB)
        seen++;
    if (seen == 1)
      out.push_back(c);
  }
}

bool CADShape::CornerPoint(int edgeA, int edgeB, glm::vec3& point) const {
  if (!IsValid() || edgeA < 0 || edgeB < 0 || edgeA == edgeB)
    return false;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  if (edgeA >= edges.Extent() || edgeB >= edges.Extent())
    return false;
  const auto verticesOf = [](const TopoDS_Shape& e) {
    std::vector<TopoDS_Shape> v;
    for (TopExp_Explorer ex(e, TopAbs_VERTEX); ex.More(); ex.Next())
      v.push_back(ex.Current());
    return v;
  };
  const std::vector<TopoDS_Shape> va = verticesOf(edges(edgeA + 1));
  const std::vector<TopoDS_Shape> vb = verticesOf(edges(edgeB + 1));
  const TopoDS_Shape* shared = nullptr;
  int count = 0;
  for (const TopoDS_Shape& a : va)
    for (const TopoDS_Shape& b : vb)
      if (a.IsSame(b)) {
        // Same vertex reached twice (a closed edge lists it twice) is still
        // one vertex.
        if (shared && shared->IsSame(a))
          continue;
        shared = &a;
        count++;
      }
  if (count != 1 || !shared)
    return false; // no corner, or an ambiguous pair — refuse either way
  const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(*shared));
  point = glm::vec3(p.X(), p.Y(), p.Z());
  return true;
}

uint64_t CADShape::PlacementId() const {
  if (m_shape.IsNull())
    return 0;
  uint64_t h = reinterpret_cast<uint64_t>(m_shape.TShape().get());
  const auto mix = [&h](double v) {
    // Quantized before hashing: a placement that differs in the last bits of
    // a float is the same placement, and two of them must not read as
    // different meshes.
    const int64_t q = static_cast<int64_t>(std::llround(v * 1.0e6));
    h ^= static_cast<uint64_t>(q) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  };
  const gp_Trsf& t = m_shape.Location().Transformation();
  for (int row = 1; row <= 3; row++)
    for (int col = 1; col <= 4; col++)
      mix(t.Value(row, col));
  mix(static_cast<double>(m_shape.Orientation()));
  return h;
}

bool CADShape::ContainsPoint(const glm::vec3& p, double tol) const {
  if (m_shape.IsNull())
    return false;
  BRepClass3d_SolidClassifier classifier(m_shape);
  classifier.Perform(gp_Pnt(p.x, p.y, p.z), tol);
  const TopAbs_State state = classifier.State();
  return state == TopAbs_IN || state == TopAbs_ON;
}

bool CADShape::EdgeSnapPoint(int edgeIndex, glm::vec3& point) const {
  if (!IsValid() || edgeIndex < 0)
    return false;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  if (edgeIndex >= edges.Extent())
    return false;
  const TopoDS_Edge& edge = TopoDS::Edge(edges(edgeIndex + 1)); // 1-based map
  if (edge.IsNull())
    return false;
  BRepAdaptor_Curve curve(edge);
  if (curve.GetType() == GeomAbs_Circle) {
    const gp_Pnt c = curve.Circle().Location();
    point = glm::vec3(c.X(), c.Y(), c.Z());
  } else {
    // Midpoint of the parameter range for a non-circular edge.
    const gp_Pnt m =
        curve.Value(0.5 * (curve.FirstParameter() + curve.LastParameter()));
    point = glm::vec3(m.X(), m.Y(), m.Z());
  }
  return true;
}

CADShape::Mesh CADShape::GenerateMesh(double deflection) const {
  Mesh mesh;
  if (!IsValid())
    return mesh;

  // Generate mesh. A fixed absolute deflection facets curved surfaces badly
  // (a small cylinder becomes a visible ~16-gon whose flat facets flicker as
  // the camera orbits). Scale the linear deflection to the model's size and
  // add an angular deflection so circles stay smooth at any scale.
  //
  // Only when the caller did not say. A stated deflection is a caller that
  // knows what it is meshing FOR, and this rule -- which is about triangle
  // budget, not accuracy -- has no business overruling it.
  double linDeflection = deflection;
  if (linDeflection <= 0.0) {
    Bnd_Box bbox;
    BRepBndLib::Add(m_shape, bbox);
    if (!bbox.IsVoid()) {
      double xmin, ymin, zmin, xmax, ymax, zmax;
      bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
      const double diag = std::sqrt((xmax - xmin) * (xmax - xmin) +
                                    (ymax - ymin) * (ymax - ymin) +
                                    (zmax - zmin) * (zmax - zmin));
      if (diag > Precision::Confusion())
        linDeflection = diag * 0.002; // 0.2% of the bounding diagonal
    }
    // A degenerate shape has no size to derive from. Anything positive will
    // do; OCC refuses a deflection of zero.
    if (linDeflection <= 0.0)
      linDeflection = 0.1;
  }
  const double angDeflection = 0.2; // radians (~11.5°) → ~32 segments / circle
  BRepMesh_IncrementalMesh meshGen(m_shape, linDeflection, Standard_False,
                                   angDeflection, Standard_True);
  if (!meshGen.IsDone())
    return mesh;

  // Extract triangular mesh, one contiguous vertex/triangle block per
  // B-Rep face, with exact surface normals where the surface provides them
  uint32_t faceId = 0;
  for (TopExp_Explorer faceExp(m_shape, TopAbs_FACE); faceExp.More();
       faceExp.Next(), faceId++) {
    const TopoDS_Face& face = TopoDS::Face(faceExp.Current());
    TopLoc_Location loc;
    Handle(Poly_Triangulation) triangulation =
        BRep_Tool::Triangulation(face, loc);
    if (triangulation.IsNull())
      continue;

    const bool reversed = (face.Orientation() == TopAbs_REVERSED);
    int baseIndex = mesh.vertices.size();

    Handle(Geom_Surface) surface = BRep_Tool::Surface(face);
    const bool hasUV = triangulation->HasUVNodes() && !surface.IsNull();

    for (int i = 1; i <= triangulation->NbNodes(); i++) {
      gp_Pnt p = triangulation->Node(i).Transformed(loc);
      Vertex vertex;
      vertex.position = glm::vec3(p.X(), p.Y(), p.Z());
      vertex.normal = glm::vec3(0.0f); // triangle fallback fills these
      if (hasUV) {
        const gp_Pnt2d uv = triangulation->UVNode(i);
        GeomLProp_SLProps props(surface, uv.X(), uv.Y(), 1,
                                Precision::Confusion());
        if (props.IsNormalDefined()) {
          gp_Dir n = props.Normal().Transformed(loc.Transformation());
          vertex.normal = glm::vec3(n.X(), n.Y(), n.Z());
          if (reversed)
            vertex.normal = -vertex.normal;
        }
      }
      mesh.vertices.push_back(vertex);
    }

    for (int i = 1; i <= triangulation->NbTriangles(); i++) {
      const Poly_Triangle& tri = triangulation->Triangle(i);
      int n1, n2, n3;
      tri.Get(n1, n2, n3);

      if (reversed) {
        mesh.indices.push_back(baseIndex + n1 - 1);
        mesh.indices.push_back(baseIndex + n3 - 1);
        mesh.indices.push_back(baseIndex + n2 - 1);
      } else {
        mesh.indices.push_back(baseIndex + n1 - 1);
        mesh.indices.push_back(baseIndex + n2 - 1);
        mesh.indices.push_back(baseIndex + n3 - 1);
      }
      mesh.triangleFaces.push_back(faceId);
    }
  }

  // Fallback for vertices whose surface normal was undefined: average the
  // adjacent triangle normals (winding above makes them outward-facing)
  std::vector<glm::vec3> accumulated(mesh.vertices.size(), glm::vec3(0.0f));
  for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const glm::vec3& v0 = mesh.vertices[mesh.indices[t]].position;
    const glm::vec3& v1 = mesh.vertices[mesh.indices[t + 1]].position;
    const glm::vec3& v2 = mesh.vertices[mesh.indices[t + 2]].position;
    glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
    accumulated[mesh.indices[t]] += n;
    accumulated[mesh.indices[t + 1]] += n;
    accumulated[mesh.indices[t + 2]] += n;
  }
  for (size_t i = 0; i < mesh.vertices.size(); i++) {
    glm::vec3& n = mesh.vertices[i].normal;
    if (glm::dot(n, n) < 0.25f)
      n = accumulated[i];
    float len = glm::length(n);
    n = (len > 1e-8f) ? n / len : glm::vec3(0, 0, 1);
  }

  return mesh;
}

CADShape::Mesh CADShape::GenerateMeshDetached(double deflection) const {
  if (!IsValid())
    return Mesh();
  // A deep copy, so BRepMesh writes the triangulation onto a TShape nobody
  // else holds. Without it a verifier asking for its own tolerance silently
  // re-meshes the document (see the header).
  BRepBuilderAPI_Copy copier(m_shape);
  if (!copier.IsDone())
    return GenerateMesh(deflection);
  return CADShape(copier.Shape()).GenerateMesh(deflection);
}

std::vector<std::vector<glm::vec3>>
CADShape::GenerateEdgePolylines(double deflection) const {
  std::vector<std::vector<glm::vec3>> polylines;
  if (!IsValid())
    return polylines;

  // Same model-relative deflection scaling as GenerateMesh, so edge
  // polylines match the mesh's fidelity.
  Bnd_Box bbox;
  BRepBndLib::Add(m_shape, bbox);
  double linDeflection = deflection;
  if (!bbox.IsVoid()) {
    double xmin, ymin, zmin, xmax, ymax, zmax;
    bbox.Get(xmin, ymin, zmin, xmax, ymax, zmax);
    const double diag = std::sqrt((xmax - xmin) * (xmax - xmin) +
                                  (ymax - ymin) * (ymax - ymin) +
                                  (zmax - zmin) * (zmax - zmin));
    if (diag > Precision::Confusion())
      linDeflection = diag * 0.002;
  }
  const double angDeflection = 0.2; // radians, matches GenerateMesh

  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(m_shape, TopAbs_EDGE, edges);
  polylines.resize(edges.Extent());
  for (int i = 1; i <= edges.Extent(); i++) {
    const TopoDS_Edge& edge = TopoDS::Edge(edges(i));
    if (BRep_Tool::Degenerated(edge))
      continue; // empty keeps indices aligned
    try {
      BRepAdaptor_Curve curve(edge);
      GCPnts_TangentialDeflection discretizer(curve, angDeflection,
                                              linDeflection, /*minPoints=*/2);
      auto& poly = polylines[i - 1];
      poly.reserve(discretizer.NbPoints());
      for (int p = 1; p <= discretizer.NbPoints(); p++) {
        const gp_Pnt pt = discretizer.Value(p);
        poly.emplace_back(pt.X(), pt.Y(), pt.Z());
      }
    } catch (const Standard_Failure&) {
      polylines[i - 1].clear(); // unparameterizable edge: skip, keep index
    }
  }
  return polylines;
}

// CADKernel implementation
CADKernel& CADKernel::Instance() {
  static CADKernel instance;
  static bool initialized = false;

  if (!initialized) {
    // Initialize OpenCASCADE - basic validation
    try {
      gp_Pnt testPoint(0.0, 0.0,
                       0.0); // Basic test to ensure libraries are loaded
    } catch (...) {
      // OpenCASCADE initialization failed
    }
    initialized = true;
  }

  return instance;
}

CADShape CADKernel::CreateBox(const glm::vec3& corner1,
                              const glm::vec3& corner2) {
  gp_Pnt p1 = ToOCCPoint(corner1);
  gp_Pnt p2 = ToOCCPoint(corner2);

  BRepPrimAPI_MakeBox boxMaker(p1, p2);
  if (!boxMaker.IsDone()) {
    boxMaker.Build();
  }

  if (boxMaker.IsDone()) {
    return CADShape(boxMaker.Shape());
  }
  return CADShape();
}

CADShape CADKernel::CreateBox(double width, double height, double depth) {
  // Validate input parameters against Precision::Confusion
  if (width <= Precision::Confusion() || height <= Precision::Confusion() ||
      depth <= Precision::Confusion()) {
    return CADShape();
  }

  try {
    BRepPrimAPI_MakeBox boxMaker(width, height, depth);

    // Call Build() to ensure construction is complete
    if (!boxMaker.IsDone()) {
      boxMaker.Build();
    }

    if (boxMaker.IsDone()) {
      return CADShape(boxMaker.Shape());
    }
  } catch (const Standard_Failure& e) {
    // Log error if needed, but return empty shape
  }

  return CADShape();
}

CADShape CADKernel::CreateSphere(const glm::vec3& center, double radius) {
  if (radius <= Precision::Confusion()) {
    return CADShape();
  }

  try {
    gp_Pnt centerPnt = ToOCCPoint(center);
    BRepPrimAPI_MakeSphere sphereMaker(centerPnt, radius);

    // Call Build() to ensure construction is complete
    if (!sphereMaker.IsDone()) {
      sphereMaker.Build();
    }

    if (sphereMaker.IsDone()) {
      return CADShape(sphereMaker.Shape());
    }
  } catch (const Standard_Failure& e) {
    // Log error if needed, but return empty shape
  }

  return CADShape();
}

CADShape CADKernel::CreateCylinder(const glm::vec3& center,
                                   const glm::vec3& direction, double radius,
                                   double height) {
  if (radius <= Precision::Confusion() || height <= Precision::Confusion()) {
    return CADShape();
  }

  try {
    gp_Pnt centerPnt = ToOCCPoint(center);
    gp_Dir dir = ToOCCDirection(direction);
    gp_Ax2 axis(centerPnt, dir);

    BRepPrimAPI_MakeCylinder cylinderMaker(axis, radius, height);

    // Call Build() to ensure construction is complete
    if (!cylinderMaker.IsDone()) {
      cylinderMaker.Build();
    }

    if (cylinderMaker.IsDone()) {
      return CADShape(cylinderMaker.Shape());
    }
  } catch (const Standard_Failure& e) {
    // Log error if needed, but return empty shape
  }

  return CADShape();
}

CADShape CADKernel::CreateCone(const glm::vec3& center,
                               const glm::vec3& direction, double radius1,
                               double radius2, double height) {
  if (radius1 <= Precision::Confusion() || radius2 <= Precision::Confusion() ||
      height <= Precision::Confusion()) {
    return CADShape();
  }

  try {
    gp_Pnt centerPnt = ToOCCPoint(center);
    gp_Dir dir = ToOCCDirection(direction);
    gp_Ax2 axis(centerPnt, dir);

    BRepPrimAPI_MakeCone coneMaker(axis, radius1, radius2, height);

    // Call Build() to ensure construction is complete
    if (!coneMaker.IsDone()) {
      coneMaker.Build();
    }

    if (coneMaker.IsDone()) {
      return CADShape(coneMaker.Shape());
    }
  } catch (const Standard_Failure& e) {
    // Log error if needed, but return empty shape
  }

  return CADShape();
}

bool CADKernel::LoadSTEP(const std::string& filename,
                         std::vector<CADShape>& shapes) {
  shapes.clear();
  if (!std::filesystem::exists(filename))
    return false;

  // Through XCAF, not STEPControl_Reader: a structured file (ours, since X4)
  // has ONE root — the top assembly — and a flat reader hands that back as a
  // single compound, so a three-body part would read as one body. Walking the
  // product tree gives one shape per placed leaf, placed exactly (gp_Trsf,
  // not a float matrix), and a flat file's free shapes are its leaves.
  std::scoped_lock exchange(DataExchangeMutex()); // XcafDocument.h
  XcafDocument doc;
  STEPCAFControl_Reader reader;
  if (reader.ReadFile(filename.c_str()) != IFSelect_RetDone)
    return false;
  try {
    if (!reader.Transfer(doc))
      return false;
  } catch (const Standard_Failure&) {
    return false;
  }
  Handle(XCAFDoc_ShapeTool) tool = XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  std::function<void(const TDF_Label&, const TopLoc_Location&)> visit =
      [&](const TDF_Label& label, const TopLoc_Location& parent) {
        TDF_Label def = label;
        XCAFDoc_ShapeTool::GetReferredShape(label, def); // no-op if plain
        const TopLoc_Location here =
            parent * XCAFDoc_ShapeTool::GetLocation(label);
        if (XCAFDoc_ShapeTool::IsAssembly(def)) {
          TDF_LabelSequence comps;
          XCAFDoc_ShapeTool::GetComponents(def, comps);
          for (Standard_Integer i = 1; i <= comps.Length(); i++)
            visit(comps.Value(i), here);
          return;
        }
        const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(def);
        if (!shape.IsNull())
          shapes.push_back(CADShape(shape.Moved(here)));
      };
  TDF_LabelSequence roots;
  tool->GetFreeShapes(roots);
  for (Standard_Integer i = 1; i <= roots.Length(); i++)
    visit(roots.Value(i), TopLoc_Location());
  return !shapes.empty();
}

namespace {

std::string XcafLabelName(const TDF_Label& label) {
  Handle(TDataStd_Name) name;
  if (!label.IsNull() && label.FindAttribute(TDataStd_Name::GetID(), name)) {
    const TCollection_ExtendedString& ext = name->Get();
    TCollection_AsciiString ascii(ext);
    std::string out(ascii.ToCString());
    // XCAF writes "=>[0:1:1:5]" on an assembly component whose instance the
    // file did not name: it is a POINTER to the definition label, not a
    // name, and importing the SO-101 arm put a dozen of them in the
    // occurrence list. Empty means "ask the definition", which is what the
    // callers already do.
    if (out.rfind("=>[", 0) == 0)
      return {};
    return out;
  }
  return {};
}

// A rigid gp_Trsf as a glm placement. glm is column-major: m[col][row];
// gp_Trsf::Value is 1-based (row, col) with col 4 = translation.
glm::mat4 TrsfToMat4(const gp_Trsf& t) {
  glm::mat4 m(1.0f);
  for (int row = 0; row < 3; row++)
    for (int col = 0; col < 4; col++)
      m[col][row] = static_cast<float>(t.Value(row + 1, col + 1));
  return m;
}

// Bridges OCC's progress protocol to CADKernel::ImportProgress.
//
// Show() fires ~200k times on a 48 MB file, so what it does per call matters
// more than it looks: calling GetPosition() every time (it walks the scope
// tree) took the transfer of that file from 8.3 s to 20.1 s — a 2.4x tax to
// draw a progress bar. Sampling it every kSampleEvery-th tick makes the cost
// vanish while still giving ~100 updates.
class ProgressBridge : public Message_ProgressIndicator {
public:
  explicit ProgressBridge(CADKernel::ImportProgress* sink) : m_sink(sink) {}

  void Show(const Message_ProgressScope&, const Standard_Boolean) override {
    if (m_sink == nullptr)
      return;
    if (++m_ticks % kSampleEvery != 0)
      return;
    m_sink->OnPhase(CADKernel::ImportProgress::Phase::Transferring,
                    GetPosition());
  }

  Standard_Boolean UserBreak() override {
    // Also polled per tick; the atomic read behind this is cheap, but keep it
    // on the same sampling budget as the position.
    return m_sink != nullptr && m_sink->Cancelled() ? Standard_True
                                                    : Standard_False;
  }

private:
  static constexpr uint64_t kSampleEvery = 2000;
  CADKernel::ImportProgress* m_sink = nullptr;
  uint64_t m_ticks = 0;
};

} // namespace

bool CADKernel::LoadSTEPStructured(const std::string& filename,
                                   ImportedModel& out,
                                   ImportProgress* progress) {
  out = {};
  if (!std::filesystem::exists(filename))
    return false;

  std::scoped_lock exchange(DataExchangeMutex()); // XcafDocument.h
  XcafDocument doc;
  STEPCAFControl_Reader reader;
  reader.SetNameMode(Standard_True);
  reader.SetColorMode(Standard_True);
  // No progress hook exists for the parse itself — report it as a phase with
  // an indeterminate fraction rather than pretending to a percentage.
  if (progress != nullptr)
    progress->OnPhase(ImportProgress::Phase::ReadingFile, -1.0);
  if (reader.ReadFile(filename.c_str()) != IFSelect_RetDone)
    return false;
  if (progress != nullptr && progress->Cancelled())
    return false;
  try {
    Handle(ProgressBridge) bridge = new ProgressBridge(progress);
    Message_ProgressScope scope(bridge->Start(), nullptr, 1);
    if (!reader.Transfer(doc, scope.Next()))
      return false;
    // A cancelled transfer returns "not done" the same way a failed one does;
    // both leave `out` empty, which is what the callers check.
    if (progress != nullptr && progress->Cancelled())
      return false;
  } catch (const Standard_Failure&) {
    return false;
  }

  Handle(XCAFDoc_ShapeTool) shapes =
      XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) colors =
      XCAFDoc_DocumentTool::ColorTool(doc->Main());

  // One product entry per unique definition label, keyed by its entry
  // string — 100 identical screws stay 1 product + 100 instances.
  std::map<std::string, size_t> productIndex;
  auto productOf = [&](const TDF_Label& def) -> size_t {
    TCollection_AsciiString entry;
    TDF_Tool::Entry(def, entry);
    const std::string key(entry.ToCString());
    auto it = productIndex.find(key);
    if (it != productIndex.end())
      return it->second;

    ImportedShape p;
    p.shape = CADShape(shapes->GetShape(def));
    p.name = XcafLabelName(def);
    Quantity_Color qc;
    if (colors->GetColor(def, XCAFDoc_ColorSurf, qc) ||
        colors->GetColor(def, XCAFDoc_ColorGen, qc) ||
        colors->GetColor(shapes->GetShape(def), XCAFDoc_ColorSurf, qc) ||
        colors->GetColor(shapes->GetShape(def), XCAFDoc_ColorGen, qc)) {
      p.hasColor = true;
      p.color = glm::vec3(static_cast<float>(qc.Red()),
                          static_cast<float>(qc.Green()),
                          static_cast<float>(qc.Blue()));
    }
    productIndex.emplace(key, out.products.size());
    out.products.push_back(std::move(p));
    return out.products.size() - 1;
  };

  // Walk the instance tree, composing locations down to leaf products.
  std::function<void(const TDF_Label&, const gp_Trsf&, const std::string&)>
      visit = [&](const TDF_Label& label, const gp_Trsf& parent,
                  const std::string& instName) {
        TDF_Label def = label;
        XCAFDoc_ShapeTool::GetReferredShape(label, def); // no-op if plain
        const gp_Trsf here =
            parent * XCAFDoc_ShapeTool::GetLocation(label).Transformation();
        if (XCAFDoc_ShapeTool::IsAssembly(def)) {
          TDF_LabelSequence comps;
          XCAFDoc_ShapeTool::GetComponents(def, comps);
          for (Standard_Integer i = 1; i <= comps.Length(); i++) {
            std::string childName = XcafLabelName(comps.Value(i));
            if (childName.empty()) {
              TDF_Label childDef = comps.Value(i);
              XCAFDoc_ShapeTool::GetReferredShape(comps.Value(i), childDef);
              childName = XcafLabelName(childDef);
            }
            visit(comps.Value(i), here, childName);
          }
        } else {
          ImportedInstance inst;
          inst.product = productOf(def);
          inst.transform = TrsfToMat4(here);
          inst.name =
              !instName.empty() ? instName : out.products[inst.product].name;
          out.instances.push_back(std::move(inst));
        }
      };

  TDF_LabelSequence roots;
  shapes->GetFreeShapes(roots);
  for (Standard_Integer i = 1; i <= roots.Length(); i++)
    visit(roots.Value(i), gp_Trsf(), XcafLabelName(roots.Value(i)));

  return !out.instances.empty();
}

bool CADKernel::SaveSTEP(const std::string& filename,
                         const std::vector<CADShape>& shapes) {
  if (shapes.empty())
    return false;

  std::scoped_lock exchange(DataExchangeMutex()); // XcafDocument.h
  STEPControl_Writer writer;

  for (const auto& shape : shapes) {
    if (shape.IsValid()) {
      writer.Transfer(shape.GetShape(), STEPControl_AsIs);
    }
  }

  return writer.Write(filename.c_str()) == IFSelect_RetDone;
}

bool CADKernel::LoadSTL(const std::string& filename, CADShape& shape) {
  StlAPI_Reader reader;
  TopoDS_Shape occShape;

  if (!reader.Read(occShape, filename.c_str()) || occShape.IsNull())
    return false;
  // The reader sews the triangles into a shell of planar faces. Promote a
  // closed (watertight) shell to a solid so volume/mass and booleans work;
  // an open mesh stays a surface shell.
  if (occShape.ShapeType() == TopAbs_SHELL) {
    const TopoDS_Shell shell = TopoDS::Shell(occShape);
    BRepCheck_Shell check(shell);
    if (check.Closed() == BRepCheck_NoError) {
      BRepBuilderAPI_MakeSolid make(shell);
      if (make.IsDone()) {
        TopoDS_Solid solid = make.Solid();
        if (BRepLib::OrientClosedSolid(solid)) // material on the inside
          occShape = solid;
      }
    }
  }
  shape = CADShape(occShape);
  return true;
}

bool CADKernel::SaveSTL(const std::string& filename, const CADShape& shape,
                        double deflection) {
  if (!shape.IsValid())
    return false;

  // Re-mesh explicitly at the requested deflection instead of reusing
  // whatever triangulation the viewport happened to leave on the shape —
  // export quality is then a user choice, not an accident.
  BRepMesh_IncrementalMesh mesh(shape.GetShape(), deflection);
  mesh.Perform();

  StlAPI_Writer writer;
  writer.ASCIIMode() = false; // binary: smaller, faster, slicer-friendly
  return writer.Write(shape.GetShape(), filename.c_str());
}

bool CADKernel::SaveBREP(const std::string& filename, const CADShape& shape) {
  if (!shape.IsValid())
    return false;
  return BRepTools::Write(shape.GetShape(), filename.c_str());
}

bool CADKernel::LoadBREP(const std::string& filename, CADShape& shape) {
  if (!std::filesystem::exists(filename))
    return false;
  TopoDS_Shape s;
  BRep_Builder builder;
  if (!BRepTools::Read(s, filename.c_str(), builder) || s.IsNull())
    return false;
  shape = CADShape(s);
  return true;
}

bool CADKernel::SaveSTEPStructured(const std::string& filename,
                                   const ImportedModel& model,
                                   const std::string& assemblyName) {
  if (model.products.empty() || model.instances.empty())
    return false;
  StepExportModel m;
  for (const auto& p : model.products) {
    StepBody body{p.shape, "", std::nullopt};
    if (p.hasColor)
      body.color = p.color;
    m.parts.push_back({p.name, {std::move(body)}});
  }
  StepAssembly top{assemblyName, {}};
  for (const auto& inst : model.instances)
    top.components.push_back({false, inst.product, inst.transform, inst.name});
  m.assemblies.push_back(std::move(top));
  m.rootIsAssembly = true;
  m.root = 0;
  return SaveSTEPModel(filename, m);
}

namespace {
// A rigid glm placement as OCC's, or nullopt for one that is not rigid
// (sheared, scaled, degenerate or mirrored).
std::optional<gp_Trsf> RigidTrsf(const glm::mat4& m) {
  // A placement is a float matrix, so its rotation is orthonormal only to
  // float precision. Handing that to gp_Trsf::SetValues reads the determinant's
  // rounding as a SCALE of 1 ± 1e-7, and OpenCASCADE 7.6 refuses to move a
  // shape by a scaling location ("Moving with scaling transformation is
  // forbidden") where 7.9 lets it pass. So check the axes are unit and
  // orthogonal to float precision, refuse a mirror, and rebuild the rotation
  // from a normalized quaternion: its scale is then exactly 1.
  const gp_XYZ x(m[0][0], m[0][1], m[0][2]);
  const gp_XYZ y(m[1][0], m[1][1], m[1][2]);
  const gp_XYZ z(m[2][0], m[2][1], m[2][2]);
  constexpr double kTol = 1e-4;
  for (const gp_XYZ* a : {&x, &y, &z})
    if (std::abs(a->Modulus() - 1.0) > kTol)
      return std::nullopt;
  if (std::abs(x.Dot(y)) > kTol || std::abs(y.Dot(z)) > kTol ||
      std::abs(z.Dot(x)) > kTol || x.Crossed(y).Dot(z) < 0.0)
    return std::nullopt;
  gp_Quaternion q;
  q.SetMatrix(gp_Mat(x, y, z)); // columns
  q.Normalize();
  gp_Trsf t;
  t.SetTransformation(q, gp_Vec(m[3][0], m[3][1], m[3][2]));
  return t;
}
} // namespace

bool CADKernel::SaveSTEPModel(const std::string& filename,
                              const StepExportModel& model,
                              std::string* error) {
  const auto fail = [&](std::string why) {
    if (error != nullptr)
      *error = std::move(why);
    return false;
  };
  const size_t rootCount =
      model.rootIsAssembly ? model.assemblies.size() : model.parts.size();
  if (model.root >= rootCount)
    return fail("nothing to write: the export has no root");

  std::scoped_lock exchange(DataExchangeMutex()); // XcafDocument.h
  XcafDocument doc;
  Handle(XCAFDoc_ShapeTool) shapes =
      XCAFDoc_DocumentTool::ShapeTool(doc->Main());
  Handle(XCAFDoc_ColorTool) colors =
      XCAFDoc_DocumentTool::ColorTool(doc->Main());
  // Our geometry is in millimetres. Say so on the document and to the
  // writer, rather than relying on both defaulting to it.
  XCAFDoc_DocumentTool::SetLengthUnit(doc, 0.001);
  Interface_Static::SetCVal("write.step.unit", "MM");

  const auto name = [](const TDF_Label& label, const std::string& n) {
    if (!n.empty())
      TDataStd_Name::Set(label, n.c_str());
  };
  // Colours are linear, as the shaders light them; OCC's RGB is linear too
  // and the writer converts to the file's sRGB itself.
  const auto paint = [&](const TDF_Label& label,
                         const std::optional<glm::vec3>& c) {
    if (c)
      colors->SetColor(label,
                       Quantity_Color(c->r, c->g, c->b, Quantity_TOC_RGB),
                       XCAFDoc_ColorSurf);
  };
  const auto bodyLabel = [&](const StepBody& b) {
    TopoDS_Shape shape = b.shape.GetShape();
    // XCAF makes a LOCATED shape an assembly of one placed component, again
    // anonymous ("SOLID"). A body's location is where it sits in its part,
    // which is exactly what the product must hold: bake it into the geometry.
    if (!shape.Location().IsIdentity()) {
      BRepBuilderAPI_Transform bake(shape.Located(TopLoc_Location()),
                                    shape.Location().Transformation(),
                                    /*copy=*/Standard_True);
      if (bake.IsDone())
        shape = bake.Shape();
    }
    TDF_Label label = shapes->AddShape(shape, /*makeAssembly=*/Standard_False);
    name(label, b.name);
    paint(label, b.color);
    return label;
  };

  // Parts first: every placement below refers to one of these labels.
  std::vector<TDF_Label> partLabels;
  for (const StepPart& part : model.parts) {
    if (part.bodies.empty())
      return fail("part \"" + part.name + "\" has no bodies to write");
    for (const StepBody& b : part.bodies)
      if (!b.shape.IsValid())
        return fail("part \"" + part.name + "\" has an invalid body");
    TDF_Label label;
    if (part.bodies.size() == 1) {
      label = bodyLabel(part.bodies.front());
      name(label, part.name);
    } else {
      label = shapes->NewShape();
      name(label, part.name);
      for (const StepBody& b : part.bodies) {
        const TDF_Label comp =
            shapes->AddComponent(label, bodyLabel(b), TopLoc_Location());
        name(comp, b.name);
      }
    }
    partLabels.push_back(label);
  }

  // Assemblies on demand, children before parents; a model built from a tree
  // has no cycle, and `building` refuses one that does rather than recursing
  // forever.
  std::vector<TDF_Label> asmLabels(model.assemblies.size());
  std::vector<char> built(model.assemblies.size(), 0);
  std::string why;
  std::function<bool(size_t)> build = [&](size_t ai) -> bool {
    if (built[ai] == 2)
      return true;
    if (built[ai] == 1) {
      why = "assembly \"" + model.assemblies[ai].name + "\" contains itself";
      return false;
    }
    built[ai] = 1;
    const StepAssembly& a = model.assemblies[ai];
    for (const StepComponent& c : a.components) {
      if (c.isAssembly ? c.index >= model.assemblies.size()
                       : c.index >= model.parts.size()) {
        why = "assembly \"" + a.name + "\" refers past the end of the model";
        return false;
      }
      if (c.isAssembly && !build(c.index))
        return false;
    }
    TDF_Label label = shapes->NewShape();
    name(label, a.name);
    for (const StepComponent& c : a.components) {
      const std::optional<gp_Trsf> t = RigidTrsf(c.local);
      if (!t) {
        why =
            "\"" + c.name + "\" in \"" + a.name + "\" is not a rigid placement";
        return false;
      }
      const TDF_Label comp = shapes->AddComponent(
          label, c.isAssembly ? asmLabels[c.index] : partLabels[c.index],
          TopLoc_Location(*t));
      name(comp, c.name);
    }
    asmLabels[ai] = label;
    built[ai] = 2;
    return true;
  };
  if (model.rootIsAssembly && !build(model.root))
    return fail(why);
  shapes->UpdateAssemblies();

  STEPCAFControl_Writer writer;
  try {
    if (!writer.Transfer(doc, STEPControl_AsIs))
      return fail("the STEP translator refused the model");
    if (writer.Write(filename.c_str()) != IFSelect_RetDone)
      return fail("could not write " + filename);
  } catch (const Standard_Failure& e) {
    return fail(std::string("the STEP translator failed: ") +
                e.GetMessageString());
  }
  return true;
}

// Helper functions
gp_Pnt CADKernel::ToOCCPoint(const glm::vec3& point) const {
  return gp_Pnt(point.x, point.y, point.z);
}

gp_Vec CADKernel::ToOCCVector(const glm::vec3& vector) const {
  return gp_Vec(vector.x, vector.y, vector.z);
}

gp_Dir CADKernel::ToOCCDirection(const glm::vec3& direction) const {
  return gp_Dir(direction.x, direction.y, direction.z);
}

glm::vec3 CADKernel::FromOCCPoint(const gp_Pnt& point) const {
  return glm::vec3(point.X(), point.Y(), point.Z());
}

glm::vec3 CADKernel::FromOCCVector(const gp_Vec& vector) const {
  return glm::vec3(vector.X(), vector.Y(), vector.Z());
}

#endif