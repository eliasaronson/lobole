#include "core/features/WormFeature.h"

#include "common/Logger.h"
#include "core/Angles.h"
#include "core/BodyMode.h"
#include "core/ElementHistory.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp_Face.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Ax3.hxx>
#include <gp_Cylinder.hxx>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>

namespace {

// A cylindrical boss picked by name: its body, the cylinder and its axial
// span. False with `why` when it is gone, not a cylinder, or a hole.
struct Blank {
  Body* body = nullptr;
  gp_Cylinder cyl;
  double v0 = 0.0, v1 = 0.0;
};
bool ResolveBlank(RegenContext& ctx, const ElementRef& face, Blank& out,
                  std::string& why) {
  const auto body =
      std::find_if(ctx.bodies.begin(), ctx.bodies.end(), [&](const Body& b) {
        return b.createdBy == face.featureId;
      });
  if (body == ctx.bodies.end()) {
    why = "the blank's body no longer exists";
    return false;
  }
  const auto idx = body->elementMap.Resolve(face.name);
  if (!idx) {
    why = "the blank's face '" + face.name + "' no longer exists";
    return false;
  }
  TopoDS_Face occFace;
  int i = 0;
  for (TopExp_Explorer ex(body->shape.GetShape(), TopAbs_FACE); ex.More();
       ex.Next(), i++)
    if (i == *idx) {
      occFace = TopoDS::Face(ex.Current());
      break;
    }
  if (occFace.IsNull()) {
    why = "the blank's face could not be resolved";
    return false;
  }
  BRepAdaptor_Surface surf(occFace);
  if (surf.GetType() != GeomAbs_Cylinder) {
    why = "pick the blank's CYLINDRICAL outside face";
    return false;
  }
  out.cyl = surf.Cylinder();
  out.v0 = surf.FirstVParameter();
  out.v1 = surf.LastVParameter();
  if (out.v1 < out.v0)
    std::swap(out.v0, out.v1);
  // A boss, not a bore: the face's normal points away from the axis.
  BRepGProp_Face gface(occFace);
  gp_Pnt p;
  gp_Vec n;
  gface.Normal(0.5 * (surf.FirstUParameter() + surf.LastUParameter()),
               0.5 * (out.v0 + out.v1), p, n);
  const gp_Dir d = out.cyl.Axis().Direction();
  const gp_Vec toP(out.cyl.Location(), p);
  const gp_Vec radial = toP - gp_Vec(d.XYZ() * toP.Dot(gp_Vec(d)));
  if (n.Dot(radial) <= 0.0) {
    why = "the face is a bore: a worm or a wheel is cut from the OUTSIDE of "
          "a cylindrical blank";
    return false;
  }
  out.body = &*body;
  return true;
}

std::string Fmt(const char* fmt, double a, double b = 0.0) {
  char buf[200];
  snprintf(buf, sizeof(buf), fmt, a, b);
  return buf;
}

// Subtracts `tool` from the blank's body, named, checked and retried like
// every body boolean. False with the reason on a failure; the body is left
// as it was.
bool CutBody(Feature& f, Body& body, const CADShape& tool,
             const std::string& tag, std::string& why) {
  const auto t0 = std::chrono::steady_clock::now();
  ElementMap toolMap;
  topo::NamePrimitiveElements(tool, tag, toolMap);
  CheckedBoolean cut = CheckedBooleanNamed(
      f.GetName(), BooleanOp::Subtract, body.name, body.shape, body.elementMap,
      tool, toolMap, f.GetId());
  if (!cut.refusal.empty() || !cut.shape.IsValid()) {
    why = cut.refusal.empty() ? cut.error : cut.refusal;
    return false;
  }
  LOG_INFO("{} '{}': cut {} in {:.2f} s{}", f.TypeName(), f.GetName(), tag,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
               .count(),
           cut.fuzzy > 0.0 ? " (fuzzy: " + cut.warning + ")" : std::string());
  if (cut.fuzzy > 0.0)
    f.SetWarning(cut.warning);
  body.shape = std::move(cut.shape);
  body.elementMap = std::move(cut.map);
  return true;
}

// The wheel's teeth: forty lofted spaces off the throated blank, named, and
// checked here rather than by BooleanRefusal. Its point-in-tool test
// misjudges a compound of lofted B-spline solids: measured, it named a point
// on a tooth's crest as inside a space, on a result whose volume matched a
// plain cut's to the last digit and passed BRepCheck. What this checks
// instead is what every wrong result measured did get wrong: BRepCheck (each
// one failed it), one solid, and a removed volume between none and every
// space whole. False with the reason; the body is left as it was.
bool CutTeeth(Feature& f, Body& body, const CADShape& spaces, int count,
              double spaceVolume, const std::string& tag, std::string& why) {
  const auto t0 = std::chrono::steady_clock::now();
  ElementMap toolMap;
  topo::NamePrimitiveElements(spaces, tag, toolMap);
  ElementMap map;
  std::string err;
  CADShape out =
      topo::BooleanNamed(BooleanOp::Subtract, body.shape, body.elementMap,
                         spaces, toolMap, f.GetId(), map, &err);
  if (!out.IsValid()) {
    why = err.empty() ? "the kernel could not cut the teeth" : err;
    return false;
  }
  const double before = body.shape.Volume(), after = out.Volume();
  if (!BRepCheck_Analyzer(out.GetShape()).IsValid() || out.SolidCount() != 1) {
    why = "the cut left the wheel an invalid solid";
    return false;
  }
  if (!(after < before) || before - after > count * spaceVolume) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "the cut removed %.1f mm3, where %d spaces hold at most %.1f",
             before - after, count, count * spaceVolume);
    why = buf;
    return false;
  }
  LOG_INFO("{} '{}': cut {} in {:.2f} s", f.TypeName(), f.GetName(), tag,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
               .count());
  body.shape = std::move(out);
  body.elementMap = std::move(map);
  return true;
}

// The teeth by replication (worm::CutSpacesBySectors), named as one boolean
// of the blank and the spaces would name them, and checked: BRepCheck, one
// solid, and a removed volume that is the sector's loss z2 times over, to a
// part in a million. False with the reason; the body is left as it was.
bool CutTeethBySectors(Feature& f, Body& body, const gp_Ax3& frame, int teeth,
                       const worm::WheelTools& tools, double spaceVolume,
                       const std::string& tag, std::string& why) {
  const auto t0 = std::chrono::steady_clock::now();
  worm::SectorCut cut;
  if (!worm::CutSpacesBySectors(body.shape.GetShape(), frame, teeth, tools,
                                &cut, &why))
    return false;
  const CADShape out(cut.result);
  if (!BRepCheck_Analyzer(cut.result).IsValid() || out.SolidCount() != 1) {
    why = "the replicated teeth left the wheel an invalid solid";
    return false;
  }
  const double before = body.shape.Volume();
  const double removed = before - out.Volume();
  if (!(cut.removed > 0.0) || cut.removed > teeth * spaceVolume ||
      std::abs(removed - cut.removed) > 1e-6 * before) {
    char buf[200];
    snprintf(buf, sizeof(buf),
             "the replicated cut removed %.4f mm3 where its sectors lost "
             "%.4f",
             removed, cut.removed);
    why = buf;
    return false;
  }
  ElementMap toolMap;
  topo::NamePrimitiveElements(tools.spaces, tag, toolMap);
  body.elementMap =
      topo::NamesThroughHistory(body.shape, body.elementMap, tools.spaces,
                                toolMap, *cut.history, out, f.GetId());
  body.shape = out;
  LOG_INFO("{} '{}': cut {} by replication in {:.2f} s", f.TypeName(),
           f.GetName(), tag,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
               .count());
  return true;
}

// The last few wheels built, by their exact inputs: the blank's body (the
// same shape, not an equal one) and names, and every number the wheel
// reads. A wheel's inputs come back unchanged whenever an edit to it is
// undone or redone, or a configuration is switched back — the regeneration
// cache has dropped it by then, since it keeps only the one history it last
// ran. Holding the input keeps its shape alive, so an entry cannot match a
// later shape that reuses its address. Shapes are copied in and out: a
// document's later booleans write onto the shapes it holds, and the open
// path regenerates parts on several threads.
struct WheelMemo {
  TopoDS_Shape input;
  ElementMap inputMap;
  std::string key;
  TopoDS_Shape output;
  ElementMap outputMap;
  WormWheelFeature::Report report;
  std::string warning;
};
std::mutex& WheelMemoMutex() {
  static std::mutex m;
  return m;
}
std::deque<WheelMemo>& WheelMemos() {
  static std::deque<WheelMemo> memos;
  return memos;
}
constexpr size_t kWheelMemos = 4;

TopoDS_Shape Copied(const TopoDS_Shape& s) {
  return BRepBuilderAPI_Copy(s, Standard_True, Standard_False).Shape();
}

} // namespace

namespace worm {
nlohmann::json LockingJson(double leadAngleRad) {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& [mu, what] :
       {std::pair<double, const char*>{0.2, "dry printed polymer"},
        std::pair<double, const char*>{0.1, "greased"}}) {
    const Locking l = SelfLocking(leadAngleRad, mu);
    out.push_back({{"condition", what},
                   {"mu", mu},
                   {"friction_angle_deg", l.frictionAngleDeg},
                   {"self_locking", l.locks},
                   {"efficiency", l.efficiency}});
  }
  return out;
}
} // namespace worm

WormFeature::WormFeature(const std::string& name, ElementRef face)
    : face(std::move(face)) {
  m_name = name;
}

bool WormFeature::Execute(RegenContext& ctx) {
  report = {};
  if (face.name.empty())
    return true; // previewing: no face picked yet
  Blank blank;
  std::string why;
  if (!ResolveBlank(ctx, face, blank, why)) {
    SetError(why);
    return false;
  }
  try {
    const double r = blank.cyl.Radius();
    worm::PairSpec spec;
    // The blank IS the tip cylinder: d1 = tip - 2m. Its module, when not
    // given, is the one that makes the usual diameter quotient of 10.
    spec.module = module > 0.0 ? module : 2.0 * r / 12.0;
    spec.starts = starts;
    spec.pressureDeg = pressureDeg;
    spec.rightHanded = rightHanded;
    spec.wormPitchDia = 2.0 * r - 2.0 * spec.module;
    if (module < 0.0) {
      SetError("a worm's module is positive (mm), or 0 to derive it from the "
               "blank");
      return false;
    }
    if (const std::string bad = worm::Check(spec, false); !bad.empty()) {
      SetError(bad);
      return false;
    }
    if (backlash < 0.0 || !std::isfinite(backlash)) {
      SetError("backlash is the axial clearance in mm, 0 or more");
      return false;
    }
    double v0 = blank.v0, v1 = blank.v1;
    if (length < 0.0) {
      SetError("length is the worm's threaded length in mm, 0 for the whole "
               "face");
      return false;
    }
    if (length > 1e-9)
      v1 = std::min(v1, v0 + length);

    // An end that opens onto air is run out through it (no cap coincident
    // with the end face); one against material, or a length stopping
    // mid-face, is cut square. Probed in the tooth space's radial band.
    const worm::Sizes sz = worm::Size(spec);
    const gp_Pnt o = blank.cyl.Location();
    const gp_Dir d = blank.cyl.Axis().Direction();
    const gp_Dir x = blank.cyl.Position().XDirection();
    const gp_Dir y = blank.cyl.Position().YDirection();
    const double probe = 0.5 * (sz.wormRootRadius + r);
    const auto opensOnAir = [&](double v, double outward) {
      for (int k = 1; k <= 4; k++)
        for (int a = 0; a < 4; a++) {
          const gp_Vec radial = gp_Vec(x) * std::cos(a * M_PI / 2.0) +
                                gp_Vec(y) * std::sin(a * M_PI / 2.0);
          const gp_Pnt p =
              o.Translated(gp_Vec(d) * (v + outward * 0.5 * k * sz.axialPitch) +
                           radial * probe);
          if (blank.body->shape.ContainsPoint(glm::vec3(p.X(), p.Y(), p.Z())))
            return false;
        }
      return true;
    };
    const bool openStart = opensOnAir(v0, -1.0);
    const bool openEnd = std::abs(v1 - blank.v1) < 1e-9 && opensOnAir(v1, 1.0);

    // A tooth SPACE is centred at the middle of the span on the cylinder's
    // X side, where the wheel's tooth goes (docs/modeling/worm-gears.md
    // #phase).
    const CADShape tool =
        worm::WormSpaceTool(gp_Ax3(o, d, x), v0, v1, !openStart, !openEnd,
                            0.5 * (v0 + v1) + 0.5 * sz.axialPitch, spec,
                            backlash, r + 0.1 * spec.module, &why);
    if (!tool.IsValid()) {
      SetError("the worm could not be built (" + why + ")");
      return false;
    }
    if (!CutBody(*this, *blank.body, tool, std::to_string(GetId()), why)) {
      SetError("cutting the worm failed (" + why + ")");
      return false;
    }
    report.valid = true;
    report.spec = spec;
    report.sizes = sz;
    report.length = v1 - v0;
    return true;
  } catch (const Standard_Failure& e) {
    SetError(std::string("the worm failed: ") + e.GetMessageString());
    return false;
  }
}

nlohmann::json WormFeature::ReportJson() const {
  if (!report.valid)
    return nullptr;
  const worm::Sizes& s = report.sizes;
  return {{"module", report.spec.module},
          {"pitch_diameter", report.spec.wormPitchDia},
          {"diameter_quotient", report.spec.wormPitchDia / report.spec.module},
          {"tip_diameter", 2.0 * s.wormTipRadius},
          {"root_diameter", 2.0 * s.wormRootRadius},
          {"axial_pitch", s.axialPitch},
          {"lead", s.lead},
          {"lead_angle_deg", angles::ToDegrees(s.leadAngle)},
          {"length", report.length},
          {"locking", worm::LockingJson(s.leadAngle)}};
}

WormWheelFeature::WormWheelFeature(const std::string& name, ElementRef face)
    : face(std::move(face)) {
  m_name = name;
}

bool WormWheelFeature::Execute(RegenContext& ctx) {
  report = {};
  if (face.name.empty())
    return true; // previewing: no face picked yet
  Blank blank;
  std::string why;
  if (!ResolveBlank(ctx, face, blank, why)) {
    SetError(why);
    return false;
  }
  // Builds the wheel into the blank's body.
  const auto build = [&]() -> bool {
    try {
      const double r = blank.cyl.Radius();
      if (module < 0.0 || wormDiameter < 0.0) {
        SetError("module and worm_diameter are positive (mm), or 0 to derive "
                 "them");
        return false;
      }
      worm::PairSpec spec;
      spec.teeth = teeth;
      // The rim, when the module is not given, is the usual outside diameter
      // (teeth + 3) modules.
      spec.module = module > 0.0 ? module : 2.0 * r / (teeth + 3);
      spec.starts = starts;
      spec.pressureDeg = pressureDeg;
      spec.rightHanded = rightHanded;
      spec.wormPitchDia =
          wormDiameter > 0.0 ? wormDiameter : 10.0 * spec.module;
      if (const std::string bad = worm::Check(spec, true); !bad.empty()) {
        SetError(bad);
        return false;
      }
      const double b = blank.v1 - blank.v0;
      const gp_Pnt o = blank.cyl.Location().Translated(
          gp_Vec(blank.cyl.Axis().Direction()) * (0.5 * (blank.v0 + blank.v1)));
      // A TOOTH is centred on the cylinder's X side — its seam — and space 0
      // half a pitch round from it: a space centred on the seam leaves a
      // sliver of the seam's face inside the cut, which the exact boolean
      // got wrong (measured; docs/modeling/worm-gears.md#phase).
      gp_Ax3 frame(o, blank.cyl.Axis().Direction(),
                   blank.cyl.Position().XDirection());
      frame.Rotate(gp_Ax1(o, blank.cyl.Axis().Direction()), M_PI / spec.teeth);
      // The hob is the worm with each flank 0.05 of a module fuller, so the
      // wheel clears the worm by that much per flank before any backlash.
      const double crowning = 0.05 * spec.module;
      const std::string tag = std::to_string(GetId());
      worm::HobStats stats;
      bool replicated = false;
      {
        worm::WheelTools tools;
        if (!worm::WheelCutTools(frame, 0.5 * b, r, spec, crowning, {}, &tools,
                                 &stats, &why)) {
          SetError(why);
          return false;
        }
        if (!CutBody(*this, *blank.body, tools.rim, tag + "/rim", why)) {
          SetError("cutting the wheel's throat failed (" + why + ")");
          return false;
        }
        // The teeth: one sector's cut, turned round the wheel. Forty lofted
        // spaces cut at once cost forty times one
        // (docs/modeling/worm-gears.md#how-the-teeth-are-cut).
        std::string first;
        const Body before = *blank.body;
        replicated =
            CutTeethBySectors(*this, *blank.body, frame, spec.teeth, tools,
                              stats.spaceVolume, tag + "/teeth", why);
        if (!replicated)
          LOG_INFO("WormWheel '{}': the teeth are cut whole ({})", m_name, why);
        // That fallback, a boolean of forty lofted spaces against the throated
        // blank, is the one step here the kernel can get wrong, and whether it
        // does depends on where the sections happen to fall. So a refused cut
        // is tried again with the sections moved (two more), then with more
        // points on each flank, before the wheel is reported broken.
        const int baseSlices = stats.slices;
        for (int attempt = 0; attempt < 3 && !replicated; attempt++) {
          if (attempt > 0) {
            *blank.body = before;
            worm::HobOptions o;
            o.slices = baseSlices + 2 * attempt;
            o.flankPoints = attempt == 2 ? 16 : 0;
            if (!worm::WheelCutTools(frame, 0.5 * b, r, spec, crowning, o,
                                     &tools, &stats, &why))
              break;
          }
          if (CutTeeth(*this, *blank.body, tools.spaces, spec.teeth,
                       stats.spaceVolume, tag + "/teeth", why)) {
            first.clear();
            break;
          }
          if (first.empty())
            first = why;
          LOG_WARN("WormWheel '{}': the teeth's cut was refused with {} "
                   "sections ({}); trying again",
                   m_name, stats.slices, why);
        }
        if (!first.empty()) {
          *blank.body = before;
          SetError("cutting the wheel's teeth failed (" + first + ")");
          return false;
        }
      }
      report.valid = true;
      report.spec = spec;
      report.sizes = worm::Size(spec);
      report.hob = stats;
      report.faceWidth = b;
      report.replicated = replicated;
      LOG_INFO("WormWheel '{}': {} teeth at module {:.3f} in {:.2f} s", m_name,
               teeth, spec.module, stats.seconds);
      return true;
    } catch (const Standard_Failure& e) {
      SetError(std::string("the worm wheel failed: ") + e.GetMessageString());
      return false;
    }
  };
  char keyBuf[320];
  snprintf(keyBuf, sizeof(keyBuf), "%llu|%llu|%s|%d|%.17g|%d|%.17g|%.17g|%d",
           static_cast<unsigned long long>(GetId()),
           static_cast<unsigned long long>(face.featureId), face.name.c_str(),
           teeth, module, starts, wormDiameter, pressureDeg,
           rightHanded ? 1 : 0);
  const std::string key = keyBuf;
  const TopoDS_Shape input = blank.body->shape.GetShape();
  const ElementMap inputMap = blank.body->elementMap;
  {
    std::lock_guard<std::mutex> lock(WheelMemoMutex());
    for (const WheelMemo& m : WheelMemos())
      if (m.key == key && m.input.IsEqual(input) &&
          m.inputMap.names == inputMap.names &&
          m.inputMap.edgeNames == inputMap.edgeNames) {
        blank.body->shape = CADShape(Copied(m.output));
        blank.body->elementMap = m.outputMap;
        report = m.report;
        report.reused = true;
        if (!m.warning.empty())
          SetWarning(m.warning);
        LOG_INFO("WormWheel '{}': as built before, from its inputs", m_name);
        return true;
      }
  }
  if (!build())
    return false;
  std::lock_guard<std::mutex> lock(WheelMemoMutex());
  std::deque<WheelMemo>& memos = WheelMemos();
  memos.push_front({input, inputMap, key, Copied(blank.body->shape.GetShape()),
                    blank.body->elementMap, report, GetWarning()});
  if (memos.size() > kWheelMemos)
    memos.pop_back();
  return true;
}

nlohmann::json WormWheelFeature::ReportJson() const {
  if (!report.valid)
    return nullptr;
  const worm::Sizes& s = report.sizes;
  const double faceMax = 0.67 * report.spec.wormPitchDia;
  nlohmann::json out = {
      {"module", report.spec.module},
      {"teeth", report.spec.teeth},
      {"pitch_diameter", 2.0 * s.wheelPitchRadius},
      {"centre_distance", s.centreDistance},
      {"throat_diameter", 2.0 * s.wheelThroatRadius},
      {"tip_diameter", 2.0 * report.hob.tipRadius},
      {"tip_crest", report.hob.tipCrest},
      // Where the crest is thinnest: off the mid-plane along the axis, and
      // the diameter it was measured at (the tip's, or the throat's there).
      {"tip_crest_z", report.hob.tipCrestZ},
      {"tip_crest_diameter", 2.0 * report.hob.tipCrestRadius},
      {"root_diameter", 2.0 * s.wheelRootRadius},
      {"throat_arc_deg",
       worm::ThroatArcDeg(s, report.faceWidth, report.hob.tipRadius)},
      {"face_width", report.faceWidth},
      {"worm_pitch_diameter", report.spec.wormPitchDia},
      {"lead_angle_deg", angles::ToDegrees(s.leadAngle)},
      {"ratio", static_cast<double>(report.spec.teeth) / report.spec.starts},
      {"locking", worm::LockingJson(s.leadAngle)},
      {"sections", report.hob.slices},
      {"hob_seconds", report.hob.seconds},
      {"teeth_cut", report.replicated ? "replicated" : "whole"},
      {"reused", report.reused}};
  if (report.faceWidth > faceMax)
    out["note"] = Fmt("the face (%.2f mm) is past the effective limit of "
                      "0.67 d1 (%.2f mm): the ends carry no load",
                      report.faceWidth, faceMax);
  return out;
}
