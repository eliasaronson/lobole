#include "core/cam/CamToolpath.h"

#include "core/cam/CamDropCutter.h"
#include "core/cam/CamNames.h"
#include "core/cam/CamRotaryFeed.h"

#include "core/cam/CamToolpathInternal.h"

#include "core/Measure.h"
#include "core/cam/CamAdaptive.h"

#include <clipper2/clipper.h>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepBndLib.hxx>
#include <BRepLProp_SLProps.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <GCPnts_QuasiUniformDeflection.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Cylinder.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <optional>
#include <string>

#include <glm/trigonometric.hpp>

// Deriving an operation from a pick: depths, stepover, tool changes, the
// edge break a tool can make, and reordering.

// One of six files split from CamToolpath.cpp
// (docs/architecture.md#source-layout).

namespace cam {

using namespace detail;

namespace detail {

// The largest break this tool can make within `window` mm of wall below the
// top edge, or the size asked for, checked against the tool.
EdgeBreak EdgeBreakFor(const Tool& tool, double asked, double window) {
  EdgeBreak out;
  const double r = tool.diameter * 0.5;
  if (tool.type == ToolType::Flat || tool.type == ToolType::Drill) {
    out.why = "tool " + std::to_string(tool.number) + " is a " +
              (tool.type == ToolType::Flat ? "flat end mill" : "drill") +
              " — breaking an edge needs a ball nose (a roundover) or a V-bit "
              "(a chamfer). Set the tool's type.";
    return out;
  }
  if (tool.type == ToolType::VBit) {
    const double half = glm::radians(tool.tipAngleDeg * 0.5);
    const double tanHalf = std::tan(half);
    if (tanHalf < 1e-9 || half >= glm::radians(89.9)) {
      out.why = "a V-bit with a " + Round2(tool.tipAngleDeg) +
                " degree included angle has no flank to cut a bevel with";
      return out;
    }
    // cot(half) mm of depth per mm of width; the bevel must fit the wall,
    // and the flank must reach the top surface (so w <= the tool radius).
    const double cot = 1.0 / tanHalf;
    const double fits = std::min(r, window / cot);
    out.size = asked > 0.0 ? asked : fits;
    if (out.size > r + 1e-9) {
      out.why = "tool " + std::to_string(tool.number) + " is " +
                Round2(tool.diameter) + " mm across, so its flank can cut a " +
                Round2(r) + " mm bevel at most — " + Round2(out.size) +
                " mm would bury the shank";
      return out;
    }
    out.depth = out.size * cot;
  } else {
    out.size = asked > 0.0 ? asked : std::min(r, window);
    // The ball hangs its own radius below the deepest contact point.
    out.depth = out.size + r;
  }
  if (out.size <= 1e-9) {
    out.why =
        "there is no room for a break here — the wall below the edge is " +
        Round2(window) + " mm";
    return out;
  }
  out.ok = true;
  return out;
}

struct ModelledBreak {
  bool found = false;
  double topZ = 0.0;   // the real edge: the top of the break
  double width = 0.0;  // how far the break steps back from the wall
  double height = 0.0; // how far it drops to the wall's own top
};

// The break a model ALREADY has on a wall's top edge: a chamfer or a fillet
// between the wall and the face above it. Found by sectioning just above the
// wall's own top: on a sharp edge there is nothing there, and on a broken one
// the material steps back from the wall by the break's width at that height,
// until the top face. Bisected to the highest height with material within
// `reach` of the wall -- the real edge -- and measured there.
ModelledBreak FindModelledBreak(const std::vector<Body>& bodies,
                                const WallContour& wall, double reach,
                                double ceiling) {
  ModelledBreak out;
  const auto within = [&](double z) -> std::optional<double> {
    double best = std::numeric_limits<double>::max();
    for (const Loop& l : SectionLoops(bodies, z))
      best = std::min(best, DistanceToLoop(l.pts, wall.at));
    if (best <= reach)
      return best;
    return std::nullopt;
  };
  constexpr double kAbove = 0.02; // clear of the wall's own top edge
  double lo = wall.topZ + kAbove;
  if (lo >= ceiling || !within(lo))
    return out; // a sharp edge: the face above starts at the wall
  double hi = ceiling;
  if (within(hi))
    return out; // material all the way up: a step, not a break
  while (hi - lo > 1e-3) {
    const double mid = (lo + hi) * 0.5;
    (within(mid) ? lo : hi) = mid;
  }
  const std::optional<double> width = within(lo);
  if (!width)
    return out;
  out.found = true;
  out.topZ = lo;
  out.height = lo - wall.topZ;
  out.width = *width;
  return out;
}

} // namespace detail

DerivedOp DeriveOperation(const Setup& setup,
                          const std::vector<Body>& rawBodies, OpType type,
                          const Target& target, int toolNumber) {
  // Depths are read in the frame the machine cuts in, or an indexed setup
  // derives them from the wrong axis (C9).
  const std::vector<Body> bodies = BodiesInSetupFrame(setup, rawBodies);
  DerivedOp out;
  if (setup.tools.empty()) {
    out.why = "add a tool first — every depth here comes from its diameter";
    return out;
  }
  // Every derived number below is a fraction of the cutter, so deriving for
  // one tool and cutting with another is a stepdown meant for a 6 mm end mill
  // handed to a 3 mm one.
  const Tool* named = toolNumber > 0 ? setup.FindTool(toolNumber) : nullptr;
  if (toolNumber > 0 && named == nullptr) {
    out.why =
        "there is no tool " + std::to_string(toolNumber) + " in this setup";
    return out;
  }
  const Tool& tool = named != nullptr ? *named : setup.tools.front();
  // A V-bit or a drill point is a CONE: run along a floor it leaves a groove
  // as deep as the pass, not a floor. A ball nose is fine — it leaves a cusp,
  // and `DerivedStepover` sizes the step so that cusp is a fifth of a percent
  // of the cutter. A cone has no such number, so this refuses instead of
  // cutting something plausible and wrong.
  if (OpTypeHas(type, OpField::NeedsFlatFloor) &&
      (tool.type == ToolType::VBit || tool.type == ToolType::Drill)) {
    out.why = "tool " + std::to_string(tool.number) + " is a " +
              (tool.type == ToolType::VBit ? "V-bit" : "drill") +
              " — a cone cannot clear a floor flat, it ploughs a groove the "
              "depth of the pass. Use a flat end mill, or a ball nose if a "
              "scalloped floor is acceptable.";
    return out;
  }
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBoundsFramed(setup, bodies, lo, hi)) {
    // Two different reasons land here now — no solid at all, and a fixed
    // workpiece too small for the part — and saying which is the difference
    // between "draw something" and "get a bigger board".
    out.why = StockRefusal(setup, bodies);
    if (out.why.empty())
      out.why = "there is no solid in this part yet, so there is no stock";
    return out;
  }

  Operation op;
  op.type = type;
  op.toolNumber = tool.number;
  // Half the cutter per pass and 45% of it per step-over: the rule of thumb
  // every 2.5D job starts from, expressed in the tool rather than in
  // millimetres so it stays right for a 1 mm cutter and a 12 mm one.
  op.stepdown = DerivedStepdown(tool);
  op.stepover = DerivedStepover(tool);

  // The model's own bottom, asked of the MODEL. "Does this wall go all the
  // way through" is a question about the part, and the old expression --
  // the stock's bottom plus its bottom margin -- only recovers the part's
  // bottom for RELATIVE stock, where the stock IS the model plus that margin.
  // On a fixed sheet the stock's bottom is wherever `place` puts the part in
  // the board, so the same arithmetic landed a millimetre high by default and
  // exactly ON the model's bottom when the margin happened to be zero -- and
  // there an OCC bounding-box epsilon of a few microns decided whether a
  // through hole broke out. It cost a drone's arms their break-through, in
  // silence, on a job that had been correct the run before.
  glm::dvec3 modelLo(0.0), modelHi(0.0);
  const double modelBottom = ModelBoundsFramed(bodies, modelLo, modelHi)
                                 ? modelLo.z
                                 : lo.z + setup.stock.bottomMargin;
  // Two OCC boxes are never equal to the micron, so "reaches the bottom"
  // needs a real tolerance rather than a float epsilon. A thousandth of a
  // millimetre is far below any blind depth anybody models and far above the
  // gap a bounding box introduces (kStockFit is the same number, for the same
  // reason).
  constexpr double kThrough = kStockFit;

  switch (type) {
  case OpType::Adaptive: // the strategy, derived exactly like the verb it is
  case OpType::Pocket: {
    if (IsWallFace(bodies, target.ref, target.jobPartId)) {
      out.why = "click the pocket's FLOOR — a wall says where to go round, "
                "not how deep to go. Cut it out follows a wall.";
      return out;
    }
    const FaceRegion region =
        PickedFaceRegion(bodies, target.ref, 0.02, target.jobPartId);
    if (!region.ok) {
      out.why = region.why;
      return out;
    }
    op.name = "Clear pocket";
    op.targets = {target};
    op.bottomZ = region.z;  // the floor is the face you clicked
    op.topZ = hi.z;         // …down from the top of the stock
    op.side = Side::Inside; // a pocket is cut from inside its own boundary
    // Deeper than one cutter diameter is the regime where adaptive clearing
    // pays: a constant angle of engagement lets a small spindle take the
    // whole depth, where concentric rings take the cutter's full width on
    // the first lap of every pass. Still one verb — the strategy is a
    // consequence of the geometry, not a question for the user (C8). The
    // Advanced disclosure can force either.
    if (AdaptiveAvailable() && op.topZ - op.bottomZ > tool.diameter)
      op.type = OpType::Adaptive;
    if (op.bottomZ >= op.topZ - 1e-9) {
      out.why = "that face is already the top of the stock — there is "
                "nothing above it to clear";
      return out;
    }
    break;
  }
  case OpType::Profile: {
    op.name = "Cut out";
    op.side = Side::Outside;
    op.topZ = hi.z;
    // Through the part and a whisker into the spoilboard, which is what
    // "cut it out" means — a profile that stops on the last skin leaves the
    // part welded to the stock.
    op.bottomZ = lo.z;
    if (IsWallFace(bodies, target.ref, target.jobPartId)) {
      // A wall names the contour to follow AND how far down to follow it.
      const WallContour wall =
          PickedWallContour(bodies, target.ref, 0.02, target.jobPartId);
      if (!wall.ok) {
        out.why = wall.why;
        return out;
      }
      op.targets = {target};
      op.side = wall.opening ? Side::Inside : Side::Outside;
      op.name = wall.opening ? "Cut the opening" : "Cut out";
      // A wall that stops short of the bottom of the part is a step, not a
      // through cut: follow it exactly that far. One that reaches the bottom
      // is through, so it carries on into the spoilboard like any cut-out.
      if (wall.bottomZ > modelBottom + kThrough)
        op.bottomZ = wall.bottomZ;
    } else if (target.IsSet()) {
      const FaceRegion region =
          PickedFaceRegion(bodies, target.ref, 0.02, target.jobPartId);
      if (!region.ok) {
        out.why = region.why;
        return out;
      }
      // "Cut it out" follows the part's OWN side, and a level face only says
      // where that is when nothing stands around it. Click the floor of a
      // pocket and this used to route a full-depth contour around the pocket
      // — a slot through the middle of the board, 22 mm into a 20 mm plate,
      // reported by the verifier as scrap
      // (docs/cam/operations.md#derivation--what-a-click-means). The
      // mirror of this mistake was already guarded: a pocket asked for on a
      // WALL says "click the pocket's FLOOR".
      //
      // The test is what the words mean: is there material immediately
      // outside this contour, just above its own plane? On the top of a part
      // there is nothing above; on a pocket floor there are its walls.
      Clipper2Lib::Paths64 outer;
      for (const Loop& l : region.loops)
        if (!l.hole)
          outer.push_back(ToClipper(l.pts));
      if (!outer.empty()) {
        const Clipper2Lib::Paths64 ring =
            Clipper2Lib::Difference(Offset(outer, tool.diameter), outer,
                                    Clipper2Lib::FillRule::NonZero);
        Clipper2Lib::Paths64 solid, voids;
        for (const Loop& l : SectionLoops(bodies, region.z + 1e-3))
          (l.hole ? voids : solid).push_back(ToClipper(l.pts));
        const Clipper2Lib::Paths64 above = Clipper2Lib::Difference(
            solid, voids, Clipper2Lib::FillRule::NonZero);
        const double ringArea = std::abs(Clipper2Lib::Area(ring));
        const double buried = std::abs(Clipper2Lib::Area(Clipper2Lib::Intersect(
            ring, above, Clipper2Lib::FillRule::NonZero)));
        if (ringArea > 0.0 && buried > ringArea * 0.25) {
          // Lead with the verb they want. The first version of this said
          // what was wrong and offered two alternatives in one sentence, and
          // was read as "pockets are picked by their walls" — which is the
          // opposite of true. A pocket is picked by its FLOOR; a wall only
          // comes into it for an opening that goes all the way through, and
          // has no floor to pick.
          out.why =
              "that is the floor of a pocket — use \"Clear a pocket\" on it. "
              "(\"Cut it out\" follows the part's own side, or the wall of an "
              "opening that goes all the way through. Around a pocket floor "
              "it would route a slot through the part.)";
          return out;
        }
      }
      op.targets = {target};
    }
    // Tabs, so the part does not break free under the cutter on the last
    // pass. Sized from the tool (a tab narrower than the cutter is cut away
    // as the tool passes) and from the depth, with a floor so a shallow part
    // still gets a tab rather than a scratch. An OPENING needs none: the slug
    // that falls out of it is scrap, and tabbing it only leaves stubs to
    // clean off the part.
    if (op.side != Side::Inside) {
      op.tabCount = 4;
      op.tabWidth = tool.diameter * 2.0;
      op.tabHeight = std::max(0.5, (op.topZ - op.bottomZ) * 0.2);
    }
    break;
  }
  case OpType::Face: {
    op.name = "Face top";
    op.topZ = hi.z;
    op.bottomZ = hi.z - setup.stock.margin.z; // the part's own top
    if (setup.stock.margin.z <= 1e-9) {
      out.why = "there is no stock above the part to face — the top of the "
                "part is already the top of the stock (Stock and safety > "
                "Above the part sets how much sits above it)";
      return out;
    }
    break;
  }
  case OpType::EdgeRound: {
    // Derives from the wall it breaks: the edge is the wall's own top, and
    // the break is the biggest the tool can make without running past the
    // bottom of that wall. Both numbers come from geometry the user clicked.
    if (!target.IsSet()) {
      out.why = "an edge break follows a wall — pick the vertical face whose "
                "top edge to round or chamfer";
      return out;
    }
    const WallContour wall =
        PickedWallContour(bodies, target.ref, 0.02, target.jobPartId);
    if (!wall.ok) {
      out.why = wall.why;
      return out;
    }
    // The edge is where the MODEL has it. A modelled chamfer or fillet
    // lowers the wall's own top by the break's height, and deriving from
    // that put the break below the modelled one, sized to the tool: the
    // sampler part's 1.5 mm chamfer came out 6 mm, 4.5 mm into the part all
    // the way round, and its r3 round-over gouged the top beside it. Found,
    // the break is the model's, from the model's top edge.
    const ModelledBreak modelled =
        FindModelledBreak(bodies, wall, tool.diameter * 0.5 + 1e-6, hi.z);
    double edgeTop = wall.topZ;
    double asked = 0.0;
    if (modelled.found) {
      if (tool.type == ToolType::VBit) {
        const double modelDeg =
            glm::degrees(std::atan2(modelled.width, modelled.height) * 2.0);
        if (std::abs(modelDeg - tool.tipAngleDeg) > 2.0) {
          out.why = "the model's chamfer on that edge is " + Round2(modelDeg) +
                    " degrees included (" + Round2(modelled.width) +
                    " mm wide, " + Round2(modelled.height) +
                    " mm tall) and tool " + std::to_string(tool.number) +
                    " cuts " + Round2(tool.tipAngleDeg) +
                    " -- it would cut a different chamfer. Use a V-bit of the "
                    "model's angle, or change the chamfer";
          return out;
        }
      } else if (tool.type == ToolType::Ball &&
                 std::abs(modelled.width - modelled.height) >
                     0.1 * std::max(modelled.width, modelled.height)) {
        out.why = "the model's break on that edge is " +
                  Round2(modelled.width) + " mm wide and " +
                  Round2(modelled.height) +
                  " mm tall, which is not a round -- a ball rounds an edge. "
                  "Use a V-bit for a chamfer";
        return out;
      }
      edgeTop = modelled.topZ;
      // A chamfer's width is exact at its top; a round meets the top face
      // tangentially, so its width just below the top falls short of the
      // radius (1.96 for an r2), while its HEIGHT is the radius exactly.
      asked = tool.type == ToolType::Ball ? modelled.height : modelled.width;
    }
    const EdgeBreak brk = EdgeBreakFor(tool, asked, edgeTop - wall.bottomZ);
    if (!brk.ok) {
      out.why = brk.why;
      return out;
    }
    op.name = tool.type == ToolType::Ball ? "Round the edge" : "Chamfer edge";
    op.targets = {target};
    op.side = wall.opening ? Side::Inside : Side::Outside;
    op.topZ = edgeTop;
    op.bottomZ = edgeTop - brk.depth;
    op.edgeSize = brk.size;
    break;
  }
  case OpType::Drill: {
    // Derives from the hole it makes: top from the stock (entry rapids
    // belong above it — an explicit part-top here once rapided through the
    // millimetre of stock over the part), bottom from the hole's own
    // extent. The old refusal ("…not a verb") fired even when the caller
    // passed cylindrical targets, leaving explicit top_z+bottom_z as the
    // undocumented way in.
    if (!target.IsSet()) {
      out.why = "drill derives from the holes it makes — pass targets: the "
                "cylindrical faces to drill (list_elements names them)";
      return out;
    }
    op.name = "Drill";
    op.topZ = hi.z;
    bool found = false;
    glm::dvec2 holeCentre(0.0);
    double holeRadius = 0.0;
    for (const Body& body : bodies) {
      if (body.createdBy != target.ref.featureId || !body.shape.IsValid())
        continue;
      const std::optional<int> idx = body.elementMap.Resolve(target.ref.name);
      if (!idx)
        continue;
      glm::vec3 origin(0.0f), axis(0.0f), xAxis(0.0f);
      if (!body.shape.FaceAxis(*idx, origin, axis, xAxis)) {
        out.why = "drilling picks cylindrical faces — that one is not a "
                  "cylinder (list_elements shows each face's kind)";
        return out;
      }
      if (std::abs(axis.z) < 0.99f) {
        out.why = "that hole does not point at the spindle — lay a face up "
                  "to clamp the part so it does, or drill it in another setup";
        return out;
      }
      int i = 0;
      for (TopExp_Explorer ex(body.shape.GetShape(), TopAbs_FACE); ex.More();
           ex.Next(), i++) {
        if (i != *idx)
          continue;
        Bnd_Box box;
        BRepBndLib::Add(ex.Current(), box);
        if (!box.IsVoid()) {
          double x0, y0, z0, x1, y1, z1;
          box.Get(x0, y0, z0, x1, y1, z1);
          // A through hole drills to the part's bottom face, a blind one
          // to its own floor — the face's extent says which.
          op.bottomZ = z0;
          found = true;
          holeCentre = {origin.x, origin.y};
          const BRepAdaptor_Surface surf(TopoDS::Face(ex.Current()));
          if (surf.GetType() == GeomAbs_Cylinder)
            holeRadius = surf.Cylinder().Radius();
        }
        break;
      }
      if (found)
        break;
    }
    if (!found) {
      out.why = "that face is not on a body this job can machine";
      return out;
    }
    // A THROUGH hole has to break out, and a drill is a cone: stopping its
    // lowest point at the far face leaves a ring of material the shape of
    // that cone -- the hole is not a hole. So a bore that reaches the bottom
    // of the part goes on down by the point's own height, which is the
    // clearance a person adds without thinking about it. A BLIND hole keeps
    // its floor: there the cone is the bottom of the hole.
    // (The verifier has always modelled the point -- TipHeight is one
    // function -- so before this the two disagreed, and correct drilling read
    // as 1.2 mm of material left standing in every hole.)
    if (const Tool* drill = setup.FindTool(op.toolNumber);
        drill != nullptr && op.bottomZ <= modelBottom + kThrough)
      op.bottomZ -= TipHeight(*drill, drill->diameter * 0.5);
    else if (drill != nullptr) {
      // A BLIND hole: the point is the floor. Said when the model's floor is
      // FLAT, because then the cone is material the model does not have --
      // correct drilling, and the verifier's leftover, with nothing having
      // said why. A section half way down the point's height finds the hole
      // still its full width when the floor is flat.
      const double cone = TipHeight(*drill, drill->diameter * 0.5);
      if (cone > 1e-3 && holeRadius > 0.0) {
        double nearest = std::numeric_limits<double>::max();
        for (const Loop& l : SectionLoops(bodies, op.bottomZ + cone * 0.5))
          nearest = std::min(nearest, DistanceToLoop(l.pts, holeCentre));
        if (std::abs(nearest - holeRadius) < 0.05)
          out.notes.push_back(
              "the hole is blind and its floor is flat in the model, but "
              "tool " +
              std::to_string(drill->number) + " is pointed (" +
              Round2(drill->tipAngleDeg) + " degrees): it leaves a cone " +
              Round2(cone) +
              " mm deep in the floor. Model the point (a drilled hole), or "
              "finish the floor with a flat mill");
      }
    }
    op.targets = {target};
    // Pecking, derived from the cutter: a hole deeper than three diameters
    // packs its flutes, so retract every diameter and let the chips out.
    if (const Tool* drill = setup.FindTool(op.toolNumber);
        drill != nullptr && op.topZ - op.bottomZ > 3.0 * drill->diameter)
      op.peck = drill->diameter;
    break;
  }
  case OpType::SurfaceParallel:
  case OpType::SurfaceWaterline: {
    // A finishing pass reads the part's SURFACE, so the tool has to have one
    // a surface can be read against. A flat mill leaves a facet the width of
    // its rim on every slope; a V-bit or a drill is a cone and ploughs. The
    // refusal names the cutter rather than quietly producing a plausible
    // path, because the result would look ordinary and be wrong everywhere
    // the part is not level.
    if (tool.type == ToolType::VBit || tool.type == ToolType::Drill) {
      out.why = "tool " + std::to_string(tool.number) + " is a " +
                (tool.type == ToolType::VBit ? "V-bit" : "drill") +
                " — a cone cannot finish a surface, it ploughs a groove. Use "
                "a ball nose, or a flat mill if the surface is level.";
      return out;
    }
    op.name =
        op.type == OpType::SurfaceParallel ? "Finish surface" : "Finish walls";
    // A finish is not a stack of levels. The generic stepdown above is the
    // 2.5D rule; left on a raster, it now reads as LAYERS (v125), which is a
    // rough (DeriveSurfaceRough says so on purpose).
    op.stepdown = 0.0;
    if (target.ref.IsSet())
      op.targets = {target};
    // The band to finish. The top of the stock down to the lowest point the
    // region reaches -- derived from the model, never typed, because a
    // literal here is right for one part and wrong for every other.
    SurfaceRegion region =
        target.ref.IsSet()
            ? PickedSurfaceRegion(bodies, target.ref, 0.02, target.jobPartId)
            : WholePartSilhouette(bodies, 0.02);
    if (!region.ok) {
      out.why = region.why;
      return out;
    }
    op.topZ = hi.z;
    op.bottomZ = region.minZ;
    if (op.bottomZ >= op.topZ - 1e-9) {
      out.why = "that face is level with the top of the stock — there is no "
                "surface under it to finish";
      return out;
    }
    // Across, along and down, each from the cutter. `stepover` is the cusp
    // equation DerivedStepover already solves; `sampleStep` and `zStep` stay
    // zero, which means "derive from the CL grid" at generate time -- the
    // grid is not known until the region is.
    op.stepover = DerivedStepover(tool);
    op.cutAngleDeg = 0.0;
    break;
  }
  case OpType::SurfaceRotary: {
    // The same refusal as the two above, for the same reason: a cone cannot
    // finish a surface whichever axis it is presented on.
    if (tool.type == ToolType::VBit || tool.type == ToolType::Drill) {
      out.why = "tool " + std::to_string(tool.number) + " is a " +
                (tool.type == ToolType::VBit ? "V-bit" : "drill") +
                " — a cone cannot finish a surface, it ploughs a groove. Use "
                "a ball nose.";
      return out;
    }
    if (!setup.machine.hasRotaryAxis) {
      out.why = "a rotary finishing pass needs a machine with a 4th axis — "
                "say so on the machine, or pick a 3-axis finish instead";
      return out;
    }
    op.name = "Finish on the rotary";
    // A rotary pass turns the whole part under the cutter, so a picked face
    // cannot bound it AROUND the axis. It bounds it ALONG the axis: the pass
    // runs the face's extent in X and all the way round
    // (docs/cam/rotary.md#continuous-surface_rotary), so one journal or the
    // handle alone can be finished. No pick: the whole length.
    if (target.ref.IsSet())
      op.targets = {target};
    // Depths are not what a rotary pass is bounded by: it runs the length of
    // the part on the axis and all the way round. They are filled in so the
    // panel and the post have something honest to show, and neither is read
    // by the strategy.
    op.topZ = hi.z;
    op.bottomZ = lo.z;
    op.stepover = DerivedStepover(tool);
    // A finish, unless asked for layers. The 2.5D rule of thumb above -- half
    // the cutter per pass -- would turn every rotary op into a rough from the
    // blank, which is a choice about the stock and not about the tool.
    op.stepdown = 0.0;
    break;
  }
  case OpType::Count:
    // Not a type. This switch used to have no case for an unhandled value at
    // all, so it fell through to `ok = true` with a DEFAULT Operation -- top
    // 0, bottom -1, no target -- which is a job that looks derived and cuts
    // nothing. Refusing here means a real new type breaks the build first.
    out.why = "not an operation type";
    return out;
  }

  out.op = op;
  out.ok = true;
  return out;
}

DerivedOp DeriveSurfaceRough(const Setup& setup,
                             const std::vector<Body>& bodies, int toolNumber) {
  DerivedOp out =
      DeriveOperation(setup, bodies, OpType::SurfaceParallel, {}, toolNumber);
  if (!out.ok)
    return out; // the finish's refusals are the rough's: a cone, no part
  const Tool* tool = setup.FindTool(out.op.toolNumber);
  if (tool == nullptr) {
    out.ok = false;
    out.why = "no tool " + std::to_string(out.op.toolNumber) + " in the setup";
    return out;
  }
  out.op.name = "Rough a surface";
  out.op.stepdown = DerivedStepdown(*tool);
  out.op.stockToLeave = DefaultFinishStock(*tool);
  return out;
}

DerivedOp DeriveRest(const Setup& setup, const std::vector<Body>& bodies,
                     int toolNumber) {
  DerivedOp out;
  const Tool* tool = toolNumber != 0       ? setup.FindTool(toolNumber)
                     : setup.tools.empty() ? nullptr
                                           : &setup.tools.front();
  if (tool == nullptr) {
    out.why =
        "there is no tool " + std::to_string(toolNumber) + " in this setup";
    return out;
  }
  // The finish to clean up after: the LAST surface finish in the setup --
  // not a rough (it leaves a skin everywhere, which is not "what is left")
  // and not another rest pass.
  const Operation* after = nullptr;
  for (const Operation& o : setup.operations)
    if ((o.type == OpType::SurfaceParallel ||
         o.type == OpType::SurfaceWaterline ||
         o.type == OpType::SurfaceRotary) &&
        o.enabled && o.stepdown <= 0.0 && o.restFromTool == 0)
      after = &o;
  if (after == nullptr) {
    out.why = "there is no surface finish to clean up after — finish the "
              "surface with a larger ball first, then this cuts only where "
              "that one could not reach";
    return out;
  }
  const Tool* big = setup.FindTool(after->toolNumber);
  if (big == nullptr) {
    out.why = "the finish it follows names a tool this setup does not have";
    return out;
  }
  if (tool->diameter >= big->diameter - 1e-9) {
    out.why = "tool " + std::to_string(tool->number) + " (" +
              Round2(tool->diameter) + " mm) is not smaller than T" +
              std::to_string(big->number) + " (" + Round2(big->diameter) +
              " mm), which finished it — a clean-up needs a smaller cutter "
              "to reach where that one could not";
    return out;
  }
  // The same kind of pass over the same region; a waterline finish is
  // cleaned up by a raster, because the rest region, not the pattern, is the
  // point.
  const OpType type = after->type == OpType::SurfaceRotary
                          ? OpType::SurfaceRotary
                          : OpType::SurfaceParallel;
  const Target target =
      after->targets.empty() ? Target{} : after->targets.front();
  out = DeriveOperation(setup, bodies, type, target, tool->number);
  if (!out.ok)
    return out;
  out.op.name = "Clean up after T" + std::to_string(big->number);
  out.op.restFromTool = big->number;
  out.op.stepdown = 0.0;
  out.op.stockToLeave = after->stockToLeave;
  return out;
}

OpType AddOperationType(const Setup& setup) {
  // What the setup already cuts is the evidence, the same evidence DeriveRest
  // reads to choose its pattern. The machine alone is not: a plate clamped to
  // the bed of a 4th-axis machine is a 3-axis job, and handing it a rotary
  // pass would be this bug turned round. A 3-axis "cut out" on a part held
  // on the trunnion contours the dowel from its top to its bottom -- a
  // plausible operation and a wrong one
  // (docs/cam/operations.md#derivation--what-a-click-means).
  if (setup.machine.hasRotaryAxis)
    for (const Operation& o : setup.operations)
      if (o.type == OpType::SurfaceRotary)
        return OpType::SurfaceRotary;
  return OpType::Profile;
}

DerivedOp DeriveRotaryRough(const Setup& setup, const std::vector<Body>& bodies,
                            int toolNumber) {
  DerivedOp out =
      DeriveOperation(setup, bodies, OpType::SurfaceRotary, {}, toolNumber);
  if (!out.ok)
    return out; // the finish's refusals are the rough's: no axis, a cone
  const Tool* tool = setup.FindTool(out.op.toolNumber);
  if (tool == nullptr) {
    out.ok = false;
    out.why = "no tool " + std::to_string(out.op.toolNumber) + " in the setup";
    return out;
  }
  out.op.name = "Rough on the rotary";
  // Half the cutter per layer: the rule the 2.5D operations derive their
  // stepdown by, and the one DeriveOperation leaves off a rotary finish.
  out.op.stepdown = DerivedStepdown(*tool);
  out.op.stockToLeave = DefaultFinishStock(*tool);
  return out;
}

int RederiveDepths(Setup& setup, const std::vector<Body>& bodies) {
  int moved = 0;
  for (Operation& op : setup.operations) {
    if (op.targets.empty())
      continue;
    const DerivedOp d = DeriveOperation(setup, bodies, op.type,
                                        op.targets.front(), op.toolNumber);
    if (!d.ok)
      continue;
    // Depths only. The tool, the tabs, the stepover and anything else typed
    // are answers to different questions and the frame did not change them.
    if (std::abs(op.topZ - d.op.topZ) > 1e-9 ||
        std::abs(op.bottomZ - d.op.bottomZ) > 1e-9) {
      op.topZ = d.op.topZ;
      op.bottomZ = d.op.bottomZ;
      moved++;
    }
  }
  return moved;
}

double DerivedStepover(const Tool& tool) {
  if (tool.type != ToolType::Ball)
    return 0.45;
  const double r = std::max(1e-6, tool.diameter * 0.5);
  const double cusp = r * 0.02;
  const double step =
      2.0 * std::sqrt(std::max(0.0, 2.0 * r * cusp - cusp * cusp));
  // As a fraction of the DIAMETER, which is what every reader of `stepover`
  // multiplies by. Never wider than the flat-mill rule: a ball that stepped
  // further than a flat mill would be cutting with its shank.
  return std::clamp(step / tool.diameter, 0.02, 0.45);
}

bool Retool(const Setup& setup, Operation& op, int toolNumber,
            std::string& why) {
  const Tool* next = setup.FindTool(toolNumber);
  if (next == nullptr) {
    why = "there is no tool " + std::to_string(toolNumber) + " in this setup";
    return false;
  }
  const Tool* prev = setup.FindTool(op.toolNumber);
  // "Still derived" is the test, not "never edited": a stepdown equal to the
  // old cutter's own derivation is one nobody has stated, so it follows.
  // A rotary op's zero is not "unstated": it is "no layers", which is its
  // derivation (above), and a new cutter must not turn it into a rough.
  // Since v125 a raster's zero means the same (a finish, where a stepdown
  // makes it a rough), and so does a rest pass's.
  const bool rotaryFinish = (op.type == OpType::SurfaceRotary ||
                             op.type == OpType::SurfaceParallel) &&
                            op.stepdown <= 0.0;
  const bool derived =
      !rotaryFinish &&
      (op.stepdown <= 0.0 ||
       (prev != nullptr &&
        std::abs(op.stepdown - DerivedStepdown(*prev)) < 1e-9));
  op.toolNumber = next->number;
  if (derived)
    op.stepdown = DerivedStepdown(*next);
  return true;
}

bool MoveOperation(Setup& setup, uint64_t opId, size_t toIndex) {
  std::vector<Operation>& ops = setup.operations;
  size_t from = ops.size();
  for (size_t i = 0; i < ops.size(); i++)
    if (ops[i].id == opId)
      from = i;
  if (from == ops.size() || toIndex >= ops.size())
    return false;
  if (from == toIndex)
    return true;
  const Operation moved = ops[from];
  ops.erase(ops.begin() + static_cast<ptrdiff_t>(from));
  ops.insert(ops.begin() + static_cast<ptrdiff_t>(toIndex), moved);
  return true;
}

bool SolveIndexA(const glm::dvec3& normal, double& degrees) {
  const double len = glm::length(normal);
  if (len < 1e-9)
    return false;
  const glm::dvec3 n = normal / len;
  // A rotation about X cannot move the X component, so a face with one
  // cannot be brought to face the spindle.
  if (std::abs(n.x) > 1e-3)
    return false;
  // R_x(A) * n = +Z has the solution A = atan2(ny, nz): sin A = ny and
  // cos A = nz, which is consistent because ny^2 + nz^2 = 1 here.
  degrees = glm::degrees(std::atan2(n.y, n.z));
  return true;
}

double DeriveSafeZ(const Setup& setup, const std::vector<Body>& bodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return setup.safeZ;
  // Clear the stock top by a fraction of the job's own height, with a floor
  // so a thin plate still gets real clearance rather than a scrape.
  return hi.z + std::max(2.0, (hi.z - lo.z) * 0.1);
}

double DeriveSafeZ(const Setup& setup, const std::vector<Body>& bodies,
                   const std::vector<Body>& fixtureBodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return setup.safeZ;
  // The same clearance as over the stock, over the taller of the stock and
  // every fixture: a clamp is cleared by as much as the block is.
  double top = hi.z;
  for (const Body& b : fixtureBodies) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 blo, bhi;
    b.shape.GetBoundingBox(blo, bhi, /*tight=*/true);
    top = std::max(top, static_cast<double>(bhi.z));
  }
  return top + std::max(2.0, (hi.z - lo.z) * 0.1);
}

bool ParseFixtureSide(const std::string& text, FixtureSide& out) {
  static const std::pair<const char*, FixtureSide> kSides[] = {
      {"-x", FixtureSide::MinusX},     {"+x", FixtureSide::PlusX},
      {"-y", FixtureSide::MinusY},     {"+y", FixtureSide::PlusY},
      {"centre", FixtureSide::Centre}, {"center", FixtureSide::Centre},
      {"top", FixtureSide::Top}};
  for (const auto& [name, side] : kSides)
    if (text == name) {
      out = side;
      return true;
    }
  return false;
}

std::string FixtureSideName(FixtureSide side) {
  switch (side) {
  case FixtureSide::MinusX:
    return "-x";
  case FixtureSide::PlusX:
    return "+x";
  case FixtureSide::MinusY:
    return "-y";
  case FixtureSide::PlusY:
    return "+y";
  case FixtureSide::Centre:
    return "centre";
  case FixtureSide::Top:
    return "top";
  }
  return "-x";
}

namespace {
// The fixture's box, spun as it will be placed, at the bed origin.
bool SpunFixtureBox(const std::vector<Body>& fixtureModel, double rotZdeg,
                    glm::dvec3& flo, glm::dvec3& fhi) {
  Fixture spun;
  spun.rotZdeg = rotZdeg;
  const glm::mat4 turn(FixtureTransform(spun));
  flo = glm::dvec3(1e30);
  fhi = glm::dvec3(-1e30);
  for (const Body& b : fixtureModel) {
    if (!b.shape.IsValid())
      continue;
    glm::vec3 blo, bhi;
    b.shape.Transformed(turn).GetBoundingBox(blo, bhi, /*tight=*/true);
    flo = glm::min(flo, glm::dvec3(blo));
    fhi = glm::max(fhi, glm::dvec3(bhi));
  }
  return flo.x <= fhi.x;
}
} // namespace

FixtureSide DefaultFixtureSide(const Setup& setup,
                               const std::vector<Body>& bodies,
                               const std::vector<Body>& fixtureModel,
                               double rotZdeg) {
  glm::dvec3 lo(0.0), hi(0.0), flo, fhi;
  if (StockBounds(setup, bodies, lo, hi) &&
      SpunFixtureBox(fixtureModel, rotZdeg, flo, fhi) &&
      (fhi.x - flo.x > hi.x - lo.x || fhi.y - flo.y > hi.y - lo.y))
    return FixtureSide::Centre; // it goes round the stock: a vise
  static const FixtureSide kOrder[] = {FixtureSide::MinusX, FixtureSide::PlusX,
                                       FixtureSide::MinusY, FixtureSide::PlusY};
  return kOrder[setup.fixtures.size() % std::size(kOrder)];
}

glm::dvec3 DefaultKeepOutBox(const Setup& setup,
                             const std::vector<Body>& bodies) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return glm::dvec3(0.0);
  const glm::dvec3 size = hi - lo;
  return {size.x * 0.25, size.y * 0.25, size.z * 1.25};
}

std::string FixturePlaceName(const Setup& setup,
                             const std::vector<Body>& bodies,
                             const std::vector<Body>& fixtureModel,
                             const Fixture& f) {
  static const FixtureSide kAll[] = {FixtureSide::MinusX, FixtureSide::PlusX,
                                     FixtureSide::MinusY, FixtureSide::PlusY,
                                     FixtureSide::Centre, FixtureSide::Top};
  static const char* const kSay[] = {"-X side", "+X side", "-Y side",
                                     "+Y side", "centred", "on top"};
  for (size_t k = 0; k < std::size(kAll); k++) {
    glm::dvec3 at;
    if (PlaceFixture(setup, bodies, fixtureModel, f.rotZdeg, kAll[k], at) &&
        glm::length(at - f.pos) < 1e-6)
      return kSay[k];
  }
  return {};
}

bool PlaceFixture(const Setup& setup, const std::vector<Body>& bodies,
                  const std::vector<Body>& fixtureModel, double rotZdeg,
                  FixtureSide side, glm::dvec3& pos) {
  glm::dvec3 lo(0.0), hi(0.0);
  if (!StockBounds(setup, bodies, lo, hi))
    return false;
  glm::dvec3 flo, fhi;
  if (!SpunFixtureBox(fixtureModel, rotZdeg, flo, fhi))
    return false;
  const glm::dvec3 stockMid = (lo + hi) * 0.5, fixMid = (flo + fhi) * 0.5;
  // Centred along the side it is put against; standing on the bed, which is
  // the stock's underside.
  pos = stockMid - fixMid;
  pos.z = lo.z - flo.z;
  switch (side) {
  case FixtureSide::MinusX:
    pos.x = lo.x - fhi.x;
    break;
  case FixtureSide::PlusX:
    pos.x = hi.x - flo.x;
    break;
  case FixtureSide::MinusY:
    pos.y = lo.y - fhi.y;
    break;
  case FixtureSide::PlusY:
    pos.y = hi.y - flo.y;
    break;
  case FixtureSide::Centre:
    break; // centred both ways, standing on the bed: already so
  case FixtureSide::Top:
    // On the top face, straddling the -X edge: a toe clamp's arm.
    pos.x = lo.x - fixMid.x;
    pos.z = hi.z - flo.z;
    break;
  }
  return true;
}

double CuttingMinutes(const Setup& setup, const std::vector<Toolpath>& paths) {
  // A ROTARY move has length too, and it used to have none: a pure A move is
  // zero millimetres in XYZ, so it fell out of this loop and a rotary
  // program's cycle time came back understated -- silently, which is the part
  // that matters. A cut that turns is posted at a SCALED F and timed the way
  // the controller runs it; PlanFeed is the one place that rule lives.
  const RotaryFeedModel model = FeedModelOf(setup.machine.post);
  const AxisLimits limits = AxisLimitsOf(setup);
  double minutes = 0.0;
  for (const Toolpath& tp : paths)
    for (size_t i = 1; i < tp.moves.size(); i++)
      minutes += PlanFeed(SegmentOf(tp.moves[i - 1], tp.moves[i]),
                          setup.rapidFeed, model, limits)
                     .minutes;
  return minutes;
}

} // namespace cam
