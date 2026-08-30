#pragma once

#ifdef HAVE_OPENCASCADE

#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <glm/glm.hpp>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <memory>
#include <vector>

class CADShape {
public:
  CADShape();
  CADShape(const TopoDS_Shape& shape);
  ~CADShape();

  // Shape operations
  bool IsValid() const;
  double Volume() const;
  void GetBoundingBox(glm::vec3& min, glm::vec3& max) const;
  // Disjoint-lump count (V2 digest): a body that silently split into two
  // solids is invisible in a volume total but not in this.
  int SolidCount() const;

  // Mass properties at unit density: volume, surface area, center of mass,
  // and the inertia matrix about the center of mass (multiply by density
  // for physical values). Inspection I2.
  struct MassProps {
    double volume = 0.0;
    double surfaceArea = 0.0;
    glm::vec3 centerOfMass{0.0f};
    double inertia[3][3] = {}; // about centerOfMass, world-axis aligned
  };
  bool ComputeMassProps(MassProps& out) const;

  // Boolean operations
  CADShape Union(const CADShape& other) const;
  CADShape Intersection(const CADShape& other) const;
  CADShape Difference(const CADShape& other) const;

  // Transformation
  // A copy placed by a rigid transform (rotation + translation; uniform
  // scale tolerated). A sheared matrix returns the shape unmoved — OCC
  // locations are similarity-only.
  CADShape Transformed(const glm::mat4& m) const;
  void Translate(const glm::vec3& offset);
  void Rotate(const glm::vec3& axis, double angle);
  // Rotates `angle` radians about the axis through `point` along `dir`
  // (unlike Rotate, whose axis passes through the origin).
  void RotateAbout(const glm::vec3& point, const glm::vec3& dir, double angle);
  // Reflects across the plane through `point` with the given `normal`.
  void Mirror(const glm::vec3& point, const glm::vec3& normal);
  void Scale(double factor);

  // Gathers disjoint solids into one shape WITHOUT fusing them (a compound).
  //
  // Fusing is what a caller reaches for when it wants "all of these as one
  // tool", and for disjoint solids it is pure waste: BRepAlgoAPI_Fuse
  // re-processes the whole accumulated shape on every call, so building a
  // 480-cylinder cutter one Union at a time cost 245 s where the compound
  // costs milliseconds (the U11 scale round measured it). Booleans accept a
  // compound as an operand, so this is a drop-in replacement wherever the
  // pieces are known not to touch.
  static CADShape CompoundOf(const std::vector<CADShape>& parts);

  // Splits into its connected solids (one entry when the shape is a single
  // solid, several when it is a compound of disjoint solids). Empty if
  // invalid. Used to turn a fused pattern back into per-instance bodies.
  std::vector<CADShape> SplitSolids() const;

  // For the planar face at `faceIndex` (TopExp_Explorer(TopAbs_FACE) order,
  // the same order GenerateMesh / picking use), outputs a work-plane frame:
  // `center` = the face centroid, `normal` = the outward face normal, `uAxis`
  // = a deterministic in-plane axis. Returns false if the index is out of
  // range or the face is not planar (topological naming, sketch-on-face).
  bool FacePlane(int faceIndex, glm::vec3& center, glm::vec3& normal,
                 glm::vec3& uAxis) const;

  // For the cylindrical face at `faceIndex` (same explorer order as
  // FacePlane), outputs an axis frame: cylinder -> origin = face centroid
  // projected onto the axis; cone -> origin = the APEX (where a mating
  // ball's contact converges); sphere -> origin = the CENTER with a
  // deterministic vertical axis (spheres have no natural one — enough for a
  // ball-mate connector, which consumes only the origin). `axis` = the
  // (unit) axis direction, `xAxis` = a deterministic perpendicular. Returns
  // false if the index is out of range or the face is none of those. Used by
  // A3 mate connectors (assemblies A3).
  bool FaceAxis(int faceIndex, glm::vec3& origin, glm::vec3& axis,
                glm::vec3& xAxis) const;

  // Connector inference (A2.5): every snap candidate a face offers, in ONE
  // pass — each non-degenerate bounding edge's index in the deduplicated
  // TopExp::MapShapes(EDGE) order (the ElementMap::edgeNames convention), its
  // characteristic point (a circular edge's CENTRE, else the edge's
  // midpoint), and which of those two it is.
  //
  // One call, because the three it replaced each walked the whole shape —
  // and EdgeSnapPoint did so once PER EDGE, from inside the caller's loop
  // over a face's edges. Resolving the mate-tool hover therefore mapped every
  // edge of the body N+2 times and cost O(face edges x body edges) EVERY
  // FRAME: 10 ms on a 144-hole plate, 88 ms on a 400-hole one. See
  // docs/known-bugs.md. A box hid it completely — the cost is in the product,
  // so it needs a part that is both drilled and hovered.
  struct FaceSnap {
    int edgeIndex = -1;
    glm::vec3 point{0.0f};
    bool circleCenter = false; // false => an edge midpoint
  };
  void FaceSnapPoints(int faceIndex, std::vector<FaceSnap>& out) const;

  // The corners of a face (A2.5 phase 4c): every vertex where exactly two of
  // its bounding edges meet, named by BOTH of those edges.
  //
  // Why a pair and not the vertex itself: the ElementMap names faces and
  // edges, not vertices, so a raw vertex has no stable name and a
  // measurement or a mate anchored on one would not survive a regen. Two
  // named edges DO, and their shared vertex therefore does too — the same
  // trick the snap origin already plays with one edge.
  //
  // An edge pair that meets at TWO vertices names neither, so both are
  // dropped rather than guessed at. That is not hypothetical: a cylinder's
  // wall has one such pair, so it appears on the first curved face anyone
  // hovers.
  struct FaceCorner {
    int edgeA = -1, edgeB = -1; // ElementMap::edgeNames order, edgeA < edgeB
    glm::vec3 point{0.0f};
  };
  void FaceCorners(int faceIndex, std::vector<FaceCorner>& out) const;

  // Where two edges meet, for resolving a stored corner anchor. False when
  // they share no vertex or — the ambiguous case above — more than one.
  bool CornerPoint(int edgeA, int edgeB, glm::vec3& point) const;

  // The characteristic point of the edge at `edgeIndex` (same order): a
  // circular edge's center, else the edge's midpoint. Returns false if out of
  // range. Used to derive a snapped connector frame origin — ONE edge, once
  // per mate per solve, so its whole-shape map is not in a hot path. Anything
  // resolving a whole face's worth of these wants FaceSnapPoints.
  bool EdgeSnapPoint(int edgeIndex, glm::vec3& point) const;

  // Access to underlying shape
  const TopoDS_Shape& GetShape() const { return m_shape; }

  // Mesh generation for rendering
  struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
  };

  struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    // B-Rep face id per triangle (indices.size()/3 entries). Faces are
    // emitted contiguously, so each face also owns a contiguous vertex
    // range - which picking and highlighting rely on.
    std::vector<uint32_t> triangleFaces;
  };

  Mesh GenerateMesh(double deflection = 0.1) const;

  // Tessellates each B-Rep edge into a world-space polyline, indexed by
  // edgeIndex in the deduplicated TopExp::MapShapes(TopAbs_EDGE)
  // first-appearance order — the convention ElementMap::edgeNames and edge
  // picking share. Degenerate edges (a revolve pole) yield an empty
  // polyline so indices stay aligned. Curve-based tessellation, so the
  // result is independent of the face triangulation.
  std::vector<std::vector<glm::vec3>>
  GenerateEdgePolylines(double deflection = 0.1) const;

private:
  TopoDS_Shape m_shape;
};

class CADKernel {
public:
  static CADKernel& Instance();

  // Primitive creation
  CADShape CreateBox(const glm::vec3& corner1, const glm::vec3& corner2);
  CADShape CreateBox(double width, double height, double depth);

  CADShape CreateSphere(const glm::vec3& center, double radius);

  CADShape CreateCylinder(const glm::vec3& center, const glm::vec3& direction,
                          double radius, double height);

  CADShape CreateCone(const glm::vec3& center, const glm::vec3& direction,
                      double radius1, double radius2, double height);

  // File I/O
  bool LoadSTEP(const std::string& filename, std::vector<CADShape>& shapes);

  // Structured STEP import (XCAF): what the file actually carries beyond
  // bare geometry — product names, colors, and the assembly's instance
  // tree, flattened to leaf products with composed rigid transforms.
  // A flat part file yields one product + one identity instance per free
  // shape, so callers can always iterate `instances`.
  struct ImportedShape {
    CADShape shape;   // the product's geometry, at identity
    std::string name; // XCAF label name ("" when unnamed)
    bool hasColor = false;
    glm::vec3 color{0.0f}; // XCAF surface/generic color when hasColor
  };
  struct ImportedInstance {
    size_t product = 0;        // index into ImportedModel::products
    glm::mat4 transform{1.0f}; // composed placement (world of the file)
    std::string name;          // instance label name ("" when unnamed)
  };
  struct ImportedModel {
    std::vector<ImportedShape> products; // deduped unique geometry
    std::vector<ImportedInstance> instances;
  };
  // How a long STEP read reports itself and how it is stopped. Both hooks are
  // called ON THE READING THREAD, so an implementation that a UI thread also
  // reads must be atomic (see ui::ImportJob).
  //
  // A 48 MB file spends ~2-4 s in ReadFile (no progress available: OCC offers
  // no hook there) and ~8 s in Transfer, which is reported here. `fraction` is
  // 0..1 within the transfer phase.
  struct ImportProgress {
    enum class Phase {
      ReadingFile,  // OCC offers no hook here; fraction is always < 0
      Transferring, // XCAF transfer, with a real fraction
    };
    virtual ~ImportProgress() = default;
    // Which phase, and how far into it (fraction < 0 = indeterminate).
    virtual void OnPhase(Phase phase, double fraction) = 0;
    // True aborts the read. Polled often — keep it trivial.
    virtual bool Cancelled() = 0;
  };

  // `progress` may be null (the synchronous callers pass nothing). A cancelled
  // read returns false with `out` empty, exactly like a failed one.
  bool LoadSTEPStructured(const std::string& filename, ImportedModel& out,
                          ImportProgress* progress = nullptr);
  // The write half: products (named, colored) + one flat assembly of
  // placed instances → a STEP with real product structure. Used by the
  // assembly-import round-trip tests; the seed of assembly STEP export.
  bool SaveSTEPStructured(const std::string& filename,
                          const ImportedModel& model,
                          const std::string& assemblyName = "Assembly");
  // Native OCC BREP text — the exact-geometry sidecar format assembly
  // import extracts per-part geometry into.
  bool LoadBREP(const std::string& filename, CADShape& shape);
  bool SaveSTEP(const std::string& filename,
                const std::vector<CADShape>& shapes);

  bool LoadSTL(const std::string& filename, CADShape& shape);
  // Binary STL, re-meshed at `deflection` (smaller = finer). Export quality
  // is the caller's choice, not whatever the viewport tessellated.
  bool SaveSTL(const std::string& filename, const CADShape& shape,
               double deflection = 0.1);
  // Native OCC BREP text (BRepTools::Write) — exact, no tessellation.
  bool SaveBREP(const std::string& filename, const CADShape& shape);

private:
  CADKernel() = default;

  // Helper functions
  gp_Pnt ToOCCPoint(const glm::vec3& point) const;
  gp_Vec ToOCCVector(const glm::vec3& vector) const;
  gp_Dir ToOCCDirection(const glm::vec3& direction) const;
  glm::vec3 FromOCCPoint(const gp_Pnt& point) const;
  glm::vec3 FromOCCVector(const gp_Vec& vector) const;
};

#else

// Dummy implementations when OpenCASCADE is not available
class CADShape {
public:
  CADShape() = default;
  bool IsValid() const { return false; }
  double Volume() const { return 0.0; }
  int SolidCount() const { return 0; }
  void GetBoundingBox(glm::vec3& min, glm::vec3& max) const {
    min = max = glm::vec3(0);
  }

  struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
  };

  struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> triangleFaces;
  };

  Mesh GenerateMesh(double deflection = 0.1) const { return Mesh(); }
  std::vector<std::vector<glm::vec3>>
  GenerateEdgePolylines(double deflection = 0.1) const {
    return {};
  }
  std::vector<CADShape> SplitSolids() const { return {}; }
  bool FacePlane(int, glm::vec3&, glm::vec3&, glm::vec3&) const {
    return false;
  }
  bool FaceAxis(int, glm::vec3&, glm::vec3&, glm::vec3&) const { return false; }
  struct FaceSnap {
    int edgeIndex = -1;
    glm::vec3 point{0.0f};
    bool circleCenter = false;
  };
  void FaceSnapPoints(int, std::vector<FaceSnap>&) const {}
  struct FaceCorner {
    int edgeA = -1, edgeB = -1;
    glm::vec3 point{0.0f};
  };
  void FaceCorners(int, std::vector<FaceCorner>&) const {}
  bool CornerPoint(int, int, glm::vec3&) const { return false; }
  bool EdgeSnapPoint(int, glm::vec3&) const { return false; }
};

class CADKernel {
public:
  static CADKernel& Instance() {
    static CADKernel instance;
    return instance;
  }

  CADShape CreateBox(const glm::vec3& corner1, const glm::vec3& corner2) {
    return CADShape();
  }
  CADShape CreateBox(double width, double height, double depth) {
    return CADShape();
  }
  CADShape CreateSphere(const glm::vec3& center, double radius) {
    return CADShape();
  }
  CADShape CreateCylinder(const glm::vec3& center, const glm::vec3& direction,
                          double radius, double height) {
    return CADShape();
  }
  CADShape CreateCone(const glm::vec3& center, const glm::vec3& direction,
                      double radius1, double radius2, double height) {
    return CADShape();
  }
};

#endif