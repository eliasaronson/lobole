// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifdef HAVE_OPENCASCADE

#include <OSD.hxx>
#include <TopoDS_Shape.hxx>
#include <TopoDS_Solid.hxx>
#include <glm/glm.hpp>
#include <gp_Ax2.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <memory>
#include <optional>
#include <vector>

// Turn OCC's SIGSEGV and friends into catchable Standard_Failure exceptions,
// once PER THREAD. Floating-point traps stay unarmed.
//
// Per thread, not once: OSD::SetSignal also sets thread-local state (the FPE
// mask; on Windows the SE translator), and the mesher's pool arms every
// worker (ui/TessellationCache.cpp). A `call_once` would arm whichever thread
// arrived first and leave the guard off everywhere else.
//
// Call it before any OCC algorithm known to fall over rather than fail.
// Exact HLR has needed it for a long time; so does the fillet builder, which
// takes the whole process down on a blend set it cannot build -- nine edges of
// a brace blend and the tenth segfaults inside BRepBlend_SurfRstLineBuilder,
// which for a generator means losing the session rather than one feature.
//
// The only caller of OSD::SetSignal (cad/OccSignals.cpp; test_conventions.cpp
// holds it). A signal's disposition is per PROCESS, and OCC's handler, outside
// an OCC_CATCH_SIGNALS scope, prints "*** Abort *** an exception was raised,
// but no catch was found" and calls exit(1): once anything had armed it, a
// crash anywhere left no crash report and no stack, and Ctrl+C on headless
// was the same exit(1). So the handler that was there before (the crash
// handler, app/CrashHandler.h) is put back, and OCC's is called only for a
// fault inside an OCC catch scope (Standard_ErrorHandler::IsInTryBlock), which
// is the one place it can turn a fault into an exception. Stop requests
// (SIGINT, SIGHUP, SIGQUIT, SIGTERM) are never OCC's.
void ArmOccSignalHandlers();

class CADShape {
public:
  CADShape();
  CADShape(const TopoDS_Shape& shape);
  ~CADShape();

  // Shape operations
  bool IsValid() const;
  double Volume() const;
  // Geometry identity, for caching what is expensive to compute from a
  // shape (tessellation, volume). `GeometryId` is the address of the shared
  // underlying TShape — every copy and every rigid placement of a shape has
  // the same one, so it buckets; `SameGeometryAs` then compares exactly
  // (TShape, placement AND orientation, which flips normals). A cache entry
  // must hold its CADShape so the TShape stays alive and the id cannot be
  // recycled for a different shape.
  const void* GeometryId() const;
  bool SameGeometryAs(const CADShape& other) const;
  // The axis-aligned box, in the shape's own space.
  //
  // TWO ANSWERS, and the difference is not precision but PURPOSE.
  //
  // The default is fast and CONSERVATIVE: never too small, sometimes too big.
  // OCC bounds a B-spline by its control points, which for a swept surface
  // lie outside it — a four-turn helical spring of radius 11 with 3.2 mm wire
  // (true extent +-12.6) comes back as -24.76..15.34. That is exactly what a
  // broad phase, a scene-reach or a "does this pair need checking" wants: a
  // bound that cannot miss, cheaply.
  //
  // `tight` evaluates the surfaces instead and returns the real extent. That
  // is what a PERSON or a script wants, because a part whose reported size is
  // sixty per cent over its real one reads as a modelling error — it cost an
  // afternoon on a spring that turned out to be correct. It is not the
  // default because it took one engine's build from 1.6 s to 4.2 s.
  //
  // BOTH answers are MEMOIZED on the shape, a slot each. Neither reads
  // anything but `m_shape`, and a shape is immutable except through the five
  // in-place transforms below — which drop both slots, because they go
  // through SetShape — so a remembered box cannot go stale. A regeneration
  // builds NEW shapes, which start cold. That is what makes a memo HERE work
  // where keying an outside cache on PlacementId did not: the repeat asks are
  // about the same shape OBJECT, not about a rebuilt one.
  //
  // It earns its place because callers re-ask constantly for a number that
  // only a regen can change. The section tool asked four times a frame
  // through every visible body of an assembly: 54% of the app's entire CPU
  // and 5.6 -> 30 ms of frame time on a 33-occurrence engine, for the same
  // box every time
  // (docs/modeling/README.md#bounding-boxes-are-memoized-on-the-shape).
  //
  // THREADING: filling a slot writes to the shape, so it lives by the rule
  // the parallel clash sweep already set — OpenCASCADE fills its own caches
  // lazily on the shape too, so shapes are warmed SINGLE-THREADED before any
  // worker touches them (`Measure::WorldBounds` over every entity in
  // RemoteInspection.cpp, `MeshFor` over every candidate in MotionStudy.cpp).
  // A worker asking about a shape only it holds — the per-thread copies both
  // sweeps make — is safe either way.
  //
  // Neither depends on whether the shape has been meshed, which is the
  // property the .cpp's older comment protects (CAM derives stock and depths
  // from this box, and a 4 um difference between a meshed and unmeshed part
  // once reported twelve legitimate rapids as cutting through material).
  // The memo makes that invariant load-bearing rather than merely tidy:
  // GenerateMesh writes a triangulation INTO the shape, so a box that read
  // one would be remembered from before the mesh existed and quietly disagree
  // with the same call made after.
  void GetBoundingBox(glm::dvec3& min, glm::dvec3& max,
                      bool tight = false) const;
  // The tight box is ALSO remembered by shape identity and location, for
  // LOCATED shapes: every CADShape holding the same TShape at the same
  // placement is the same point set, so a copy framed again -- a job tab
  // revisited, a stock box derived from a fresh framing -- reads the box
  // another copy paid for (docs/cam/gui.md#job-tab-frame-cost). Bounded, and
  // the entries hold their TShapes, as the volume cache's do.
  //
  // `RememberTightBox` puts a box computed ELSEWHERE -- on a detached copy,
  // by a worker -- on `shape` and under its identity. The caller vouches that
  // it is `shape`'s box.
  static void RememberTightBox(const CADShape& shape, const glm::dvec3& min,
                               const glm::dvec3& max);
  [[nodiscard]] bool HasTightBox() const;
  // The tight box, computed, reading and writing no memo -- for a worker
  // bounding a detached copy whose answer belongs to another shape.
  void ExactBoxUnmemoized(glm::dvec3& min, glm::dvec3& max) const;
  [[nodiscard]] static size_t SharedTightBoxCount();
  static void ForgetSharedTightBoxes();
  // A deep copy (BRepBuilderAPI_Copy) at NO location: it shares no TShape
  // with this one, so a worker may evaluate it while the frame meshes the
  // original. `LocatedLike` puts a shape at another's location without
  // copying geometry -- together they give a worker its own copy of a framed
  // shape (docs/ui/long-operations.md).
  [[nodiscard]] CADShape DetachedUnlocated() const;
  [[nodiscard]] CADShape LocatedLike(const CADShape& like) const;
  // Disjoint-lump count (V2 digest): a body that silently split into two
  // solids is invisible in a volume total but not in this.
  int SolidCount() const;

  // How many faces the B-Rep has — the honest measure of what an exact
  // boolean against this shape will cost. A modelled part is tens; an
  // imported STL is one planar face per TRIANGLE, so tens of thousands, and
  // the contact sweep needs to know the difference before it starts one.
  int FaceCount() const;

  // Mass properties at unit density: volume, surface area, center of mass,
  // and the inertia matrix about the center of mass (multiply by density
  // for physical values). Inspection I2.
  struct MassProps {
    double volume = 0.0;
    double surfaceArea = 0.0;
    glm::dvec3 centerOfMass{0.0};
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
  // locations are similarity-only. A copy moved by quarter turns and a
  // translation carries the tight box, permuted; any other starts cold
  // (docs/modeling/README.md#bounding-boxes-are-memoized-on-the-shape).
  CADShape Transformed(const glm::dmat4& m) const;
  void Translate(const glm::dvec3& offset);
  void Rotate(const glm::dvec3& axis, double angle);
  // Rotates `angle` radians about the axis through `point` along `dir`
  // (unlike Rotate, whose axis passes through the origin).
  void RotateAbout(const glm::dvec3& point, const glm::dvec3& dir,
                   double angle);
  // Reflects across the plane through `point` with the given `normal`.
  void Mirror(const glm::dvec3& point, const glm::dvec3& normal);
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
  bool FacePlane(int faceIndex, glm::dvec3& center, glm::dvec3& normal,
                 glm::dvec3& uAxis) const;

  // The extent of the face at `faceIndex` in a plane frame: every boundary
  // edge sampled and projected onto (u, v) about `origin`, giving the UV box
  // it spans. False when the index is out of range. What a primitive placed
  // on the face sizes its defaults from and snaps its edges to
  // (docs/modeling/README.md#primitives).
  bool FaceExtent(int faceIndex, const glm::dvec3& origin, const glm::dvec3& u,
                  const glm::dvec3& v, glm::dvec2& lo, glm::dvec2& hi) const;

  // For the cylindrical face at `faceIndex` (same explorer order as
  // FacePlane), outputs an axis frame: cylinder -> origin = face centroid
  // projected onto the axis; cone -> origin = the APEX (where a mating
  // ball's contact converges); sphere -> origin = the CENTER with a
  // deterministic vertical axis (spheres have no natural one — enough for a
  // ball-mate connector, which consumes only the origin). `axis` = the
  // (unit) axis direction, `xAxis` = a deterministic perpendicular. Returns
  // false if the index is out of range or the face is none of those. Used by
  // A3 mate connectors (assemblies A3).
  bool FaceAxis(int faceIndex, glm::dvec3& origin, glm::dvec3& axis,
                glm::dvec3& xAxis) const;

  // Is this point inside the solid (on the boundary counts)? A direct
  // classification, not a ray cast: nothing about it depends on where the
  // query starts or which way it points, which is what makes it the right
  // test for "does this hole meet any material" — a ray bundle offset
  // around the bore can begin INSIDE a boss and report the boss absent.
  [[nodiscard]] bool ContainsPoint(const glm::dvec3& p,
                                   double tol = 1.0e-6) const;
  // The same question for many points, with ONE classifier: building it walks
  // every face of the solid, and a caller asking point by point paid that per
  // point -- the chess board's engraving probed 7,700 points through
  // `ContainsPoint` and spent two minutes building classifiers. Points outside
  // the shape's box are answered without asking.
  [[nodiscard]] std::vector<bool>
  ContainsPoints(const std::vector<glm::dvec3>& points,
                 double tol = 1.0e-6) const;

  // Identity INCLUDING placement: the underlying geometry plus where this
  // copy of it sits. `GeometryId` alone is the TShape, which a rigid
  // Transformed() deliberately shares — so two occurrences of one part have
  // the same GeometryId and different placements, and anything caching
  // world-space results (a tessellation, a collision mesh) must key on this
  // instead or hand the second occurrence the first one's geometry.
  [[nodiscard]] uint64_t PlacementId() const;

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
  // docs/assemblies/README.md#snap-points-while-hovering. A box hid it
  // completely — the cost is in the product, so it needs a part that is both
  // drilled and hovered.
  struct FaceSnap {
    int edgeIndex = -1;
    glm::dvec3 point{0.0};
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
    glm::dvec3 point{0.0};
  };
  void FaceCorners(int faceIndex, std::vector<FaceCorner>& out) const;

  // Where two edges meet, for resolving a stored corner anchor. False when
  // they share no vertex or — the ambiguous case above — more than one.
  bool CornerPoint(int edgeA, int edgeB, glm::dvec3& point) const;

  // The characteristic point of the edge at `edgeIndex` (same order): a
  // circular edge's center, else the edge's midpoint. Returns false if out of
  // range. Used to derive a snapped connector frame origin — ONE edge, once
  // per mate per solve, so its whole-shape map is not in a hot path. Anything
  // resolving a whole face's worth of these wants FaceSnapPoints.
  bool EdgeSnapPoint(int edgeIndex, glm::dvec3& point) const;

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

  // `deflection` is the chord tolerance in mm. ZERO -- the default -- means
  // "you choose", and the choice is 0.2% of the model's bounding diagonal, so
  // a large model does not tessellate into millions of triangles. A POSITIVE
  // value is honoured as asked: a caller that has a reason (a check whose grid
  // is finer than that rule would give) states it and gets it. It used to be
  // the other way round -- the argument was overwritten whenever the shape had
  // a bounding box, so every value ever passed here was decoration, and the
  // CAM verifier compared machined stock against a 0.4 mm-faceted "designed"
  // surface on a 0.25 mm grid (docs/cam/simulation.md#one-engine).
  Mesh GenerateMesh(double deflection = 0.0) const;

  // The same mesh, WITHOUT touching this shape's own triangulation.
  //
  // `GenerateMesh` writes its result into the shape, and the triangulation
  // lives on the shared underlying TShape -- so it is visible through every
  // copy of the shape, and through every other copy the document is holding.
  // Asking one Body for a fine mesh therefore makes the RENDERER's mesh of
  // the same body fine too, whenever it next asks. (A cylinder meshed at the
  // derived tolerance gives 248 triangles; mesh any copy of it at 0.01 mm and
  // every copy answers 888 from then on.) This is the same hazard as the
  // bounding box that disagreed with itself once a shape had been meshed.
  //
  // So: a caller wanting a tolerance of its OWN -- finer than the shape would
  // otherwise carry, for a check rather than for the screen -- asks here, and
  // pays a deep copy to keep the side effect to itself.
  Mesh GenerateMeshDetached(double deflection) const;

  // Tessellates each B-Rep edge into a world-space polyline, indexed by
  // edgeIndex in the deduplicated TopExp::MapShapes(TopAbs_EDGE)
  // first-appearance order — the convention ElementMap::edgeNames and edge
  // picking share. Degenerate edges (a revolve pole) yield an empty
  // polyline so indices stay aligned. Curve-based tessellation, so the
  // result is independent of the face triangulation.
  std::vector<std::vector<glm::vec3>>
  GenerateEdgePolylines(double deflection = 0.1) const;

private:
  // The ONE way the shape is replaced, because it is also what forgets the
  // memoized boxes. A bare `m_shape = ` anywhere in this class would keep a
  // box that describes the shape it USED to be — a silent wrong answer, of
  // the kind that reaches CAM as a stock size. `tests/test_conventions.cpp`
  // fails on one.
  void SetShape(const TopoDS_Shape& shape);
  // Transformed's: the tight box onto a copy moved by quarter turns.
  void CarryBoxes(const glm::dmat3& basis, const glm::dvec3& shift,
                  CADShape& to) const;

  TopoDS_Shape m_shape;

  // GetBoundingBox's memo: slot 0 conservative, slot 1 tight. Mutable
  // because asking a shape how big it is does not change it — see the long
  // note on GetBoundingBox for why it cannot go stale, and for the
  // single-threaded-warm rule it shares with OCC's own lazy caches.
  mutable bool m_boxValid[2] = {false, false};
  mutable glm::dvec3 m_boxMin[2] = {glm::dvec3(0.0), glm::dvec3(0.0)};
  mutable glm::dvec3 m_boxMax[2] = {glm::dvec3(0.0), glm::dvec3(0.0)};
};

class CADKernel {
public:
  static CADKernel& Instance();

  // Primitive creation
  CADShape CreateBox(const glm::dvec3& corner1, const glm::dvec3& corner2);
  CADShape CreateBox(double width, double height, double depth);

  CADShape CreateSphere(const glm::dvec3& center, double radius);

  CADShape CreateCylinder(const glm::dvec3& center, const glm::dvec3& direction,
                          double radius, double height);

  CADShape CreateCone(const glm::dvec3& center, const glm::dvec3& direction,
                      double radius1, double radius2, double height);

  // File I/O
  // One shape per placed leaf product, at its placement — the bodies of a
  // structured file as well as the roots of a flat one.
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
    glm::dmat4 transform{1.0}; // composed placement (world of the file)
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
  // STEP export as another CAD system wants it
  // (docs/interchange/README.md#exporting-step): named, coloured products,
  // placed by reference, nested the way the assembly is. A part occurring
  // twelve times is one product and twelve instances, not twelve anonymous
  // copies.
  struct StepBody {
    CADShape shape;
    std::string name;
    std::optional<glm::vec3> color; // linear RGB, what the viewport lights
    // Faces painted apart from the body (a face appearance override, v164):
    // face index in TopExp_Explorer order -> linear RGB. Written as an XCAF
    // colour on the face's sub-shape label, which a reader applies over the
    // body's own colour.
    std::vector<std::pair<int, glm::vec3>> faceColors;
  };
  // A leaf product. One body is written as the product itself; several make
  // the product an assembly of body products, because STEP colours attach to
  // products and a part's bodies may differ.
  struct StepPart {
    std::string name;
    std::vector<StepBody> bodies;
  };
  struct StepComponent {
    bool isAssembly = false; // `index` into assemblies, else into parts
    size_t index = 0;
    glm::dmat4 local{1.0}; // rigid placement in the parent
    std::string name;      // the instance's name ("" = unnamed)
  };
  struct StepAssembly {
    std::string name;
    std::vector<StepComponent> components;
  };
  struct StepExportModel {
    std::vector<StepPart> parts;
    std::vector<StepAssembly> assemblies;
    // What the file IS: one part (a part-tab export) or one assembly.
    bool rootIsAssembly = false;
    size_t root = 0;
  };
  // Millimetres, always written as such. False on a non-rigid transform, an
  // invalid shape or a reference past the end; `error` says which.
  bool SaveSTEPModel(const std::string& filename, const StepExportModel& model,
                     std::string* error = nullptr);
  // The flat form: products + one assembly of placed instances. Kept for the
  // import tests that author their fixtures with it; it writes through
  // SaveSTEPModel.
  bool SaveSTEPStructured(const std::string& filename,
                          const ImportedModel& model,
                          const std::string& assemblyName = "Assembly");
  // Native OCC BREP text — the exact-geometry sidecar format assembly
  // import extracts per-part geometry into.
  bool LoadBREP(const std::string& filename, CADShape& shape);
  bool SaveSTEP(const std::string& filename,
                const std::vector<CADShape>& shapes);

  bool LoadSTL(const std::string& filename, CADShape& shape);
  // Binary STL, re-meshed at `deflection` (smaller = finer, mm) and
  // `angularDeflectionRad`. Export quality is the caller's choice, not
  // whatever the viewport tessellated, and both bounds are passed: OCC's
  // angular default (0.5 rad) also refines a small radius, which is why the
  // linear one is only an upper bound on what a chord takes.
  bool SaveSTL(const std::string& filename, const CADShape& shape,
               double deflection, double angularDeflectionRad);
  // Native OCC BREP text (BRepTools::Write) — exact, no tessellation.
  bool SaveBREP(const std::string& filename, const CADShape& shape);

private:
  CADKernel() = default;

  // Helper functions
  gp_Pnt ToOCCPoint(const glm::dvec3& point) const;
  gp_Vec ToOCCVector(const glm::dvec3& vector) const;
  gp_Dir ToOCCDirection(const glm::dvec3& direction) const;
  glm::dvec3 FromOCCPoint(const gp_Pnt& point) const;
  glm::dvec3 FromOCCVector(const gp_Vec& vector) const;
};

#else

// Dummy implementations when OpenCASCADE is not available
// Nothing to arm without OpenCASCADE, and nothing to include either: this
// branch has no <OSD.hxx>. It was a verbatim copy of the real body, which
// compiled only because it is inline and no no-OCC build odr-used it.
inline void ArmOccSignalHandlers() {}

class CADShape {
public:
  CADShape() = default;
  bool IsValid() const { return false; }
  const void* GeometryId() const { return nullptr; }
  bool SameGeometryAs(const CADShape&) const { return false; }
  static void RememberTightBox(const CADShape&, const glm::dvec3&,
                               const glm::dvec3&) {}
  bool HasTightBox() const { return false; }
  void ExactBoxUnmemoized(glm::dvec3& min, glm::dvec3& max) const {
    min = max = glm::dvec3(0);
  }
  static size_t SharedTightBoxCount() { return 0; }
  static void ForgetSharedTightBoxes() {}
  CADShape DetachedUnlocated() const { return CADShape(); }
  CADShape LocatedLike(const CADShape&) const { return CADShape(); }
  double Volume() const { return 0.0; }
  int SolidCount() const { return 0; }
  int FaceCount() const { return 0; }
  void GetBoundingBox(glm::dvec3& min, glm::dvec3& max) const {
    min = max = glm::dvec3(0);
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

  Mesh GenerateMesh(double deflection = 0.0) const { return Mesh(); }
  Mesh GenerateMeshDetached(double) const { return Mesh(); }
  std::vector<std::vector<glm::vec3>>
  GenerateEdgePolylines(double deflection = 0.1) const {
    return {};
  }
  std::vector<CADShape> SplitSolids() const { return {}; }
  bool FacePlane(int, glm::dvec3&, glm::dvec3&, glm::dvec3&) const {
    return false;
  }
  bool FaceExtent(int, const glm::dvec3&, const glm::dvec3&, const glm::dvec3&,
                  glm::dvec2&, glm::dvec2&) const {
    return false;
  }
  bool FaceAxis(int, glm::dvec3&, glm::dvec3&, glm::dvec3&) const {
    return false;
  }
  struct FaceSnap {
    int edgeIndex = -1;
    glm::dvec3 point{0.0};
    bool circleCenter = false;
  };
  void FaceSnapPoints(int, std::vector<FaceSnap>&) const {}
  struct FaceCorner {
    int edgeA = -1, edgeB = -1;
    glm::dvec3 point{0.0};
  };
  void FaceCorners(int, std::vector<FaceCorner>&) const {}
  bool CornerPoint(int, int, glm::dvec3&) const { return false; }
  bool EdgeSnapPoint(int, glm::dvec3&) const { return false; }
  bool ContainsPoint(const glm::dvec3&, double = 1.0e-6) const { return false; }
  std::vector<bool> ContainsPoints(const std::vector<glm::dvec3>& p,
                                   double = 1.0e-6) const {
    return std::vector<bool>(p.size(), false);
  }
  uint64_t PlacementId() const { return 0; }
};

class CADKernel {
public:
  static CADKernel& Instance() {
    static CADKernel instance;
    return instance;
  }

  CADShape CreateBox(const glm::dvec3& corner1, const glm::dvec3& corner2) {
    return CADShape();
  }
  CADShape CreateBox(double width, double height, double depth) {
    return CADShape();
  }
  CADShape CreateSphere(const glm::dvec3& center, double radius) {
    return CADShape();
  }
  CADShape CreateCylinder(const glm::dvec3& center, const glm::dvec3& direction,
                          double radius, double height) {
    return CADShape();
  }
  CADShape CreateCone(const glm::dvec3& center, const glm::dvec3& direction,
                      double radius1, double radius2, double height) {
    return CADShape();
  }
};

#endif