#ifdef HAVE_CAM

// Thread milling (docs/cam/operations.md#thread-mill): FreeCAD's helix, driven
// by the Thread on the face, posted as helical G2/G3, and judged on its own
// terms by a verifier whose height map cannot see a flank that faces sideways
// (docs/cam/simulation.md#threads).

#include "cam_fixtures.h"
#include "core/ThreadStandards.h"
#include "core/cam/CamGcode.h"
#include "core/cam/CamThreadMill.h"
#include "core/features/FilletFeature.h"
#include "core/features/HoleFeature.h"
#include "core/features/ThreadFeature.h"

#include <regex>

namespace {

constexpr double kP16 = 25.4 / 16.0; // 3/8-16's pitch

// A 60 x 60 plate `thick` mm thick, its top at z = thick, with a 3/8-16
// tapped hole at its centre -- through, or `blindDepth` deep -- and, unless
// `bareHole`, a Thread on the bore: `designation` of `hand`.
struct Plate {
  std::unique_ptr<Document> doc;
  ElementRef bore;
};

Plate TappedPlate(double thick = 10.0, double blindDepth = 0.0,
                  const std::string& designation = "3/8-16 UNC-2B",
                  bool rightHand = true, bool bareHole = false,
                  bool tapped = true) {
  Plate out;
  out.doc = std::make_unique<Document>();
  Document& doc = *out.doc;
  auto pad =
      std::make_unique<SketchFeature>("Plate", SketchPlane(PlaneType::XY));
  pad->AddRectangle({-30, -30}, {30, 30});
  doc.AddFeature(std::move(pad));
  doc.AddFeature(std::make_unique<ExtrudeFeature>(
      "Pad", doc.GetFeature(0)->GetId(), thick, ExtrudeMode::NewBody));
  auto centre = std::make_unique<SketchFeature>(
      "Centre", SketchPlane(glm::vec3(0, 0, thick), glm::vec3(0, 0, 1),
                            glm::vec3(1, 0, 0)));
  centre->AddEntity(sketch::Point{{0, 0}, 1});
  doc.AddFeature(std::move(centre));
  const uint64_t at = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto hole =
      std::make_unique<HoleFeature>("Tripod", at, HoleKind::Simple, 7.9375,
                                    blindDepth > 0.0 ? blindDepth : thick);
  hole->through = blindDepth <= 0.0;
  if (tapped) {
    hole->fitThread = "3/8-16 UNC";
    hole->fitKind = "tapped";
  }
  doc.AddFeature(std::move(hole));
  doc.Regenerate();
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names)
    if (name.find("/bore") != std::string::npos)
      out.bore = ElementRef{body.createdBy, name};
  REQUIRE(out.bore.IsSet());
  if (!bareHole) {
    auto thread = std::make_unique<ThreadFeature>("Tripod thread", out.bore,
                                                  designation, kP16);
    thread->rightHanded = rightHand;
    doc.AddFeature(std::move(thread));
    doc.Regenerate();
    INFO(doc.GetFeature(doc.FeatureCount() - 1)->GetError());
    REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  }
  return out;
}

// A 6 mm single-profile thread mill, its head derived from its diameter.
cam::Tool ThreadMill6(int number = 2, double diameter = 6.0) {
  cam::Tool t = ShapedTool(cam::ToolType::ThreadMill, diameter);
  t.number = number;
  t.name = "6 mm thread mill";
  t.feedCut = 300.0;
  t.feedPlunge = 150.0;
  cam::FillThreadMillShape(t, /*angleStated=*/false);
  return t;
}

// The plate's job: a 6 mm flat that bores the tap drill (T1) and the thread
// mill (T2). `drill` false leaves the bore to nobody.
cam::Setup ThreadJob(const Document& doc, const ElementRef& bore,
                     bool drill = true, const cam::Tool& mill = ThreadMill6(),
                     double bottomMargin = 0.0) {
  cam::Setup s = BasicSetup();
  s.tools.push_back(mill);
  // By default the plate IS the stock, sitting on the spoilboard; a margin
  // puts a slab of stock under it, which the tap drill has to go through.
  s.stock.bottomMargin = bottomMargin;
  s.safeZ = cam::DeriveSafeZ(s, doc.GetBodies());
  REQUIRE(cam::RecomputeOrigin(s, doc.GetBodies()));
  uint64_t id = 1;
  if (drill) {
    const cam::DerivedOp d = cam::DeriveOperation(
        s, doc.GetBodies(), cam::OpType::Drill, cam::Target{0, bore}, 1);
    INFO(d.why);
    REQUIRE(d.ok);
    s.operations.push_back(d.op);
    s.operations.back().id = id++;
  }
  const cam::DerivedOp t = cam::DeriveOperation(
      s, doc.GetBodies(), cam::OpType::ThreadMill, cam::Target{0, bore});
  INFO(t.why);
  REQUIRE(t.ok);
  s.operations.push_back(t.op);
  s.operations.back().id = id;
  return s;
}

std::vector<cam::Toolpath::Thread>
DeclaredThreads(const std::vector<cam::Toolpath>& paths) {
  std::vector<cam::Toolpath::Thread> out;
  for (const cam::Toolpath& tp : paths)
    out.insert(out.end(), tp.threads.begin(), tp.threads.end());
  return out;
}

std::string Post(const cam::Setup& s, const std::vector<cam::Toolpath>& paths,
                 const std::string& post = "mach3") {
  std::string gcode, why;
  cam::PostOptions opts;
  opts.fitArcs = s.machine.fitArcs;
  REQUIRE(cam::PostProcess(post, s, paths, opts, gcode, &why));
  return gcode;
}

} // namespace

// --- FreeCAD's numbers
// --------------------------------------------------------

TEST_CASE("thread mill radii and passes are FreeCAD's", "[cam][thread]") {
  // FreeCAD's CAM ThreadMilling for 3/8-16 UNC internal, 2B at its 50% fit
  // (major 9.6545, minor 7.9755), a 6 mm cutter with a 0.1 mm crest -- the
  // numbers its own threadRadii / threadPasses print.
  const cam::HelixRadii r = cam::ThreadRadii(true, 9.6545, 7.9755, 6.0, 0.1);
  CHECK_THAT(r.first, WithinAbs(0.98775, 1e-9));
  CHECK_THAT(r.last, WithinAbs(1.908547460, 1e-9));
  const std::vector<double> two = cam::ThreadPassRadii(2, r.first, r.last);
  REQUIRE(two.size() == 2);
  CHECK_THAT(two[0], WithinAbs(1.638852128, 1e-9));
  CHECK_THAT(two[1], WithinAbs(1.908547460, 1e-9));
  const std::vector<double> three = cam::ThreadPassRadii(3, r.first, r.last);
  REQUIRE(three.size() == 3);
  CHECK_THAT(three[0], WithinAbs(1.519372661, 1e-9));
  CHECK_THAT(three[1], WithinAbs(1.739577978, 1e-9));
  CHECK_THAT(three[2], WithinAbs(1.908547460, 1e-9));
  // External (2A's middle): from outside the major, in.
  const cam::HelixRadii e = cam::ThreadRadii(false, 9.3725, 7.4575, 6.0, 0.1);
  CHECK_THAT(e.first, WithinAbs(7.68625, 1e-9));
  CHECK_THAT(e.last, WithinAbs(6.432352540378444, 1e-9));
}

TEST_CASE("a thread mill pass is FreeCAD's generate", "[cam][thread]") {
  // generate(centre 0, "G3", zStart -10, zFinal 0, pitch 25.4/16, the radius
  // above, leadInOut, elevator 0): a lead-in semicircle, twelve half turns
  // each rising P/2, a partial last arc, and the lead-out back to the centre.
  const double R = 1.908547460;
  const std::vector<cam::HelixSegment> up = cam::HelixPass(
      {0, 0}, /*clockwise=*/false, -10.0, 0.0, kP16, R, true, 0.0, true);
  REQUIRE(up.size() == 1 + 12 + 1 + 1);
  CHECK(up[0].arc);
  CHECK_FALSE(up[0].clockwise);
  CHECK_THAT(up[0].to.y, WithinAbs(R, 1e-9));
  CHECK_THAT(up[0].centre.y, WithinAbs(0.954274, 1e-6));
  for (int i = 1; i <= 12; i++) {
    CAPTURE(i);
    CHECK(up[i].arc);
    CHECK_THAT(up[i].to.y, WithinAbs((i % 2) == 1 ? -R : R, 1e-9));
    CHECK_THAT(up[i].to.z, WithinAbs(-10.0 + i * kP16 * 0.5, 1e-9));
  }
  CHECK_THAT(up[13].to.x, WithinAbs(-1.818032, 1e-6));
  CHECK_THAT(up[13].to.y, WithinAbs(-0.580786, 1e-6));
  CHECK_THAT(up[13].to.z, WithinAbs(0.0, 1e-9));
  CHECK(up[14].arc);
  CHECK_THAT(up[14].to.x, WithinAbs(0.0, 1e-9));
  CHECK_THAT(up[14].to.y, WithinAbs(0.0, 1e-9));
  CHECK_THAT(up[14].centre.x, WithinAbs(-0.909016, 1e-6));
  CHECK_THAT(up[14].centre.y, WithinAbs(-0.290393, 1e-6));

  // generate(..., "G2", 0 -> -5, no lead-in, elevator 0.5): straight in,
  // six half turns falling P/2, a partial arc, straight out.
  const std::vector<cam::HelixSegment> down = cam::HelixPass(
      {0, 0}, /*clockwise=*/true, 0.0, -5.0, kP16, R, false, 0.5, true);
  REQUIRE(down.size() == 1 + 6 + 1 + 1);
  CHECK_FALSE(down[0].arc);
  CHECK_THAT(down[6].to.z, WithinAbs(-4.7625, 1e-9));
  CHECK_THAT(down[7].to.x, WithinAbs(1.541268, 1e-6));
  CHECK_THAT(down[7].to.y, WithinAbs(1.125632, 1e-6));
  CHECK_FALSE(down[8].arc);
  CHECK_THAT(down[8].to.x, WithinAbs(0.403780, 1e-6));
  CHECK_THAT(down[8].to.y, WithinAbs(0.294892, 1e-6));
}

TEST_CASE("the direction table is FreeCAD's", "[cam][thread]") {
  // Inside, right-hand: climb is G3 bottom to top, conventional G2 down.
  CHECK_FALSE(cam::ThreadDirection(true, true, true).clockwise);
  CHECK(cam::ThreadDirection(true, true, true).upward);
  CHECK(cam::ThreadDirection(true, true, false).clockwise);
  CHECK_FALSE(cam::ThreadDirection(true, true, false).upward);
  // Left-hand swaps which way each rises.
  CHECK_FALSE(cam::ThreadDirection(true, false, true).clockwise);
  CHECK_FALSE(cam::ThreadDirection(true, false, true).upward);
  // Round a boss, right-hand: climb G2 down, conventional G3 up.
  CHECK(cam::ThreadDirection(false, true, true).clockwise);
  CHECK_FALSE(cam::ThreadDirection(false, true, true).upward);
  CHECK_FALSE(cam::ThreadDirection(false, true, false).clockwise);
  CHECK(cam::ThreadDirection(false, true, false).upward);
}

TEST_CASE("a class's pitch-diameter band is the standard's", "[cam][thread]") {
  std::string why;
  const auto tripod = *threads::Lookup("3/8-16 UNC");
  // ASME B1.1 tables: 2B 8.494 / 8.639, 2A 8.349 / 8.461 (mm). The formula's
  // unrounded terms land within a hundredth.
  const auto b2 = threads::PitchDiameterBand(
      threads::Series::UNC, tripod.majorDia, tripod.pitch, "2B", false, &why);
  REQUIRE(b2);
  CHECK_THAT(b2->min, WithinAbs(8.494, 0.002));
  CHECK_THAT(b2->max, WithinAbs(8.639, 0.01));
  const auto a2 = threads::PitchDiameterBand(
      threads::Series::UNC, tripod.majorDia, tripod.pitch, "2A", true, &why);
  REQUIRE(a2);
  CHECK_THAT(a2->min, WithinAbs(8.349, 0.01));
  CHECK_THAT(a2->max, WithinAbs(8.461, 0.005));
  // ISO 965-1: M8 6H 7.188 / 7.348, 6g 7.042 / 7.160.
  const auto h6 = threads::PitchDiameterBand(threads::Series::Metric, 8.0, 1.25,
                                             "6H", false, &why);
  REQUIRE(h6);
  CHECK_THAT(h6->min, WithinAbs(7.188, 0.001));
  CHECK_THAT(h6->max, WithinAbs(7.348, 0.001));
  const auto g6 = threads::PitchDiameterBand(threads::Series::Metric, 8.0, 1.25,
                                             "6g", true, &why);
  REQUIRE(g6);
  CHECK_THAT(g6->min, WithinAbs(7.042, 0.005)); // ISO rounds Td2 to 118
  CHECK_THAT(g6->max, WithinAbs(7.160, 0.001));
  // A class of the other side, or of another series, is refused by name.
  CHECK_FALSE(threads::PitchDiameterBand(threads::Series::UNC, 9.525, kP16,
                                         "2A", false, &why));
  CHECK(why.find("2B") != std::string::npos);
  CHECK_FALSE(threads::PitchDiameterBand(threads::Series::UNC, 9.525, kP16,
                                         "6H", false, &why));
}

// --- a job -------------------------------------------------------------------

TEST_CASE("a tapped hole's thread is on its body, and a Thread says more",
          "[cam][thread]") {
  const Plate bare = TappedPlate(10.0, 0.0, "", true, /*bareHole=*/true);
  const Body& b = bare.doc->GetBodies()[0];
  REQUIRE(b.threads.size() == 1);
  CHECK(b.threads[0].designation == "3/8-16 UNC");
  CHECK(b.threads[0].threadClass.empty());
  CHECK_THAT(b.threads[0].pitch, WithinAbs(kP16, 1e-9));
  CHECK_FALSE(b.threads[0].external);
  const Plate classed = TappedPlate();
  const Body& c = classed.doc->GetBodies()[0];
  REQUIRE(c.threads.size() == 2);
  CHECK(c.threads.back().threadClass == "2B");
}

TEST_CASE("a 3/8-16 UNC-2B thread mill aims at the band's middle",
          "[cam][thread]") {
  const Plate plate = TappedPlate();
  const cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const cam::Operation& op = s.operations.back();
  CHECK(op.type == cam::OpType::ThreadMill);
  CHECK(op.toolNumber == 2); // the setup's thread mill, not its first tool
  CHECK(op.climb);
  CHECK(op.leadArcs);

  const cam::ThreadPlan plan =
      cam::PlanThread(s, op, s.tools[1], plate.doc->GetBodies(), op.targets[0]);
  REQUIRE(plan.ok);
  const cam::Toolpath::Thread& t = plan.thread;
  CHECK(t.internal);
  CHECK(t.rightHand);
  CHECK(t.through);
  CHECK(t.threadClass == "2B");
  CHECK_THAT(t.pdAim, WithinAbs(0.5 * (t.pdMin + t.pdMax), 1e-12));
  // The last pass puts the rim where the sharp V's point would be for that
  // pitch diameter, less the flat's reach: apex = PD/2 + H/2.
  const double H = kP16 * std::sqrt(3.0) / 2.0;
  const double tip = t.pdAim / 2.0 + H / 2.0 - 0.1 * std::sqrt(3.0) / 2.0;
  CHECK_THAT(plan.finalRadius, WithinAbs(tip - 3.0, 1e-9));
  CHECK_THAT(plan.startRadius, WithinAbs(7.9375 / 2.0 - 3.0, 1e-4));
  // Three passes of equal area (the first takes at most half the 1.2 mm
  // profile), from the tap drill's wall out.
  REQUIRE(plan.passes.size() == 3);
  const std::vector<double> want =
      cam::ThreadPassRadii(3, plan.startRadius, plan.finalRadius);
  for (size_t i = 0; i < 3; i++)
    CHECK_THAT(plan.passes[i], WithinAbs(want[i], 1e-12));
  // Through: the V's middle runs from a pitch below the plate's bottom to
  // half a pitch above its top.
  CHECK_THAT(plan.tipLo + plan.headWidth / 2.0, WithinAbs(-kP16, 1e-6));
  CHECK_THAT(plan.tipHi + plan.headWidth / 2.0,
             WithinAbs(10.0 + kP16 / 2, 1e-6));
  CHECK_THAT(t.fullLo, WithinAbs(0.0, 1e-9));
  CHECK_THAT(t.fullHi, WithinAbs(10.0, 1e-9));
}

TEST_CASE("the thread mill's G-code: helical arcs, half a pitch each",
          "[cam][thread]") {
  const Plate plate = TappedPlate();
  const cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, plate.doc->GetBodies());
  REQUIRE(paths.size() == 2);
  const cam::Toolpath& tp = paths[1];
  INFO(tp.error);
  REQUIRE(tp.error.empty());
  REQUIRE(tp.threads.size() == 1);
  CHECK_FALSE(tp.arcs.empty());
  // The moves are the polyline everything else reads: every helix chord on
  // the tool-centre circle of its pass.
  const cam::ThreadPlan plan =
      cam::PlanThread(s, s.operations[1], s.tools[1],
                      cam::BodiesInSetupFrame(s, plate.doc->GetBodies()),
                      s.operations[1].targets[0]);
  REQUIRE(plan.ok);
  const double zLow = tp.MinZ();
  CHECK_THAT(zLow, WithinAbs(plan.tipLo, 1e-9));

  const std::string gcode = Post(s, paths);
  // The file, in setup space: the helix as the machine will run it.
  const cam::GProgram program = cam::ProgramInSetupSpace(gcode, s);
  REQUIRE(program.errors.empty());
  // Mach3's words: G3 (climb, right-hand, bottom to top) with a Z on every
  // half turn, and flat G3s for the lead-in and lead-out.
  std::istringstream in(gcode);
  std::string line;
  int helical = 0, flat = 0, g2 = 0;
  std::vector<double> zs;
  bool inThread = false;
  const std::regex zWord(" Z(-?[0-9.]+)");
  while (std::getline(in, line)) {
    if (line == "(op 2)")
      inThread = true;
    if (!inThread)
      continue;
    if (line.rfind("G2", 0) == 0)
      g2++;
    if (line.rfind("G3", 0) != 0)
      continue;
    std::smatch z;
    if (std::regex_search(line, z, zWord)) {
      helical++;
      zs.push_back(std::stod(z[1]));
    } else {
      flat++;
    }
  }
  CHECK(g2 == 0);
  // Three passes of 12 half turns over the 13.1 mm the head's V covers, and
  // a partial arc each; a lead-in and a lead-out each.
  CHECK(flat == 6);
  CHECK(helical >= 3 * 13);
  // Z rises by exactly half a pitch between whole half turns.
  int halfPitchSteps = 0;
  for (size_t i = 1; i < zs.size(); i++)
    if (std::abs((zs[i] - zs[i - 1]) - kP16 / 2.0) < 1e-4)
      halfPitchSteps++;
  CHECK(halfPitchSteps >= 3 * 12);

  // The same file in LinuxCNC's and GRBL's dialects: the same arcs.
  for (const std::string& post : cam::PostIds()) {
    CAPTURE(post);
    CHECK(cam::PostWritesHelicalArcs(post));
    const std::string other = Post(s, paths, post);
    CHECK(other.find(" Z") != std::string::npos);
    const cam::GProgram p = cam::ProgramInSetupSpace(other, s);
    CHECK(p.errors.empty());
  }
}

TEST_CASE("a left-hand thread turns the other way", "[cam][thread]") {
  const Plate plate = TappedPlate(10.0, 0.0, "3/8-16 UNC-2B", false);
  const cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, plate.doc->GetBodies());
  REQUIRE(paths[1].error.empty());
  const std::string gcode = Post(s, paths);
  // Climb inside a left-hand thread: still G3, now from the top DOWN.
  const std::string thread = gcode.substr(gcode.find("(op 2)"));
  CHECK(thread.find("\nG2") == std::string::npos);
  const cam::Verdict v =
      cam::VerifyGcode(gcode, s, plate.doc->GetBodies(), 0.25, {}, {}, {},
                       nullptr, {}, DeclaredThreads(paths));
  REQUIRE(v.threads.size() == 1);
  REQUIRE(v.threads[0].judged);
  CHECK_FALSE(v.threads[0].rightHand);
  CHECK_FALSE(v.threads[0].handMeasured);
  CHECK(v.threads[0].Good(0.05));
}

TEST_CASE("a blind thread stops the head on the floor", "[cam][thread]") {
  const Plate plate = TappedPlate(20.0, 12.0);
  const cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const cam::Operation& op = s.operations.back();
  const cam::ThreadPlan plan =
      cam::PlanThread(s, op, s.tools[1], plate.doc->GetBodies(), op.targets[0]);
  REQUIRE(plan.ok);
  CHECK_FALSE(plan.thread.through);
  // The bore's full diameter ends 12 mm down (the drill point below it): the
  // head's tip stops there, and the last half pitch and a half V above it
  // are not full thread -- said, not hidden.
  CHECK_THAT(plan.tipLo, WithinAbs(plan.thread.zLo, 1e-6));
  CHECK(plan.thread.fullLo > plan.thread.zLo + 0.5);
  bool said = false;
  for (const std::string& n : plan.notes)
    said = said || n.find("floor stops the head") != std::string::npos;
  CHECK(said);
}

TEST_CASE("a bore tapped from the other side is refused, and milled flipped",
          "[cam][thread]") {
  // The pan/tilt head's base plate: its tripod thread is tapped up from the
  // underside. Its axis is vertical in the top-up setup too, and a vertical
  // axis was all the check asked -- the head went in through the metal over
  // the blind end. Here a blind hole from the top, the setup flipped.
  const Plate plate = TappedPlate(20.0, 12.0);
  cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const cam::Operation op = s.operations.back();
  s.frame = cam::FlipFrame(s.frame);
  const cam::DerivedOp flipped =
      cam::DeriveOperation(s, plate.doc->GetBodies(), cam::OpType::ThreadMill,
                           cam::Target{0, plate.bore});
  REQUIRE_FALSE(flipped.ok);
  CHECK(flipped.why.find("opens downward") != std::string::npos);
  CHECK(flipped.why.find("Flip") != std::string::npos);
  // The drill is refused the same way.
  const cam::DerivedOp drill =
      cam::DeriveOperation(s, plate.doc->GetBodies(), cam::OpType::Drill,
                           cam::Target{0, plate.bore}, 1);
  REQUIRE_FALSE(drill.ok);
  CHECK(drill.why.find("opens downward") != std::string::npos);
  // Flipped back, the side it opens on is up again.
  s.frame = cam::FlipFrame(s.frame);
  const cam::DerivedOp up =
      cam::DeriveOperation(s, plate.doc->GetBodies(), cam::OpType::ThreadMill,
                           cam::Target{0, plate.bore});
  INFO(up.why);
  CHECK(up.ok);
  CHECK(op.type == cam::OpType::ThreadMill);
}

TEST_CASE("a thread follows its bore through a chamfer on its mouth",
          "[cam][thread]") {
  // A face a later feature modifies is renamed (";M<tag>" appended), and
  // the thread recorded on it before kept the old name: the pan/tilt base
  // plate's tripod hole, chamfered after it was tapped, had "no thread" on
  // its bore for the thread mill.
  Plate plate = TappedPlate(20.0, 12.0);
  Document& doc = *plate.doc;
  const Body& body = doc.GetBodies().at(0);
  std::vector<ElementRef> mouth;
  const auto polylines = body.shape.GenerateEdgePolylines();
  for (size_t e = 0; e < polylines.size(); e++) {
    bool onRim = polylines[e].size() >= 2;
    for (const glm::vec3& p : polylines[e])
      onRim = onRim && std::abs(p.z - 20.0f) < 1e-3f &&
              std::abs(glm::length(glm::vec2(p)) - 7.9375f / 2.0f) < 1e-2f;
    if (!onRim)
      continue;
    const std::string* name = body.elementMap.EdgeNameOf(static_cast<int>(e));
    REQUIRE(name != nullptr);
    mouth.push_back(ElementRef{body.createdBy, *name});
  }
  REQUIRE_FALSE(mouth.empty());
  doc.AddFeature(std::make_unique<FilletFeature>("Break", mouth, 0.5,
                                                 EdgeBlendOp::Chamfer));
  REQUIRE(doc.Regenerate());
  const Body& after = doc.GetBodies().at(0);
  REQUIRE_FALSE(after.threads.empty());
  for (const ThreadMark& m : after.threads) {
    INFO(m.face.name);
    CHECK(after.elementMap.Resolve(m.face.name).has_value());
  }
  ElementRef bore;
  for (const auto& [index, name] : after.elementMap.names)
    if (name.find("/bore") != std::string::npos)
      bore = ElementRef{after.createdBy, name};
  REQUIRE(bore.IsSet());
  CHECK(bore.name != plate.bore.name); // the chamfer renamed it
  const cam::Setup s = ThreadJob(doc, bore);
  CHECK(s.operations.back().type == cam::OpType::ThreadMill);
}

TEST_CASE("an external thread is milled round a boss", "[cam][thread]") {
  // A 3/8-16 UNC-2A boss standing 12 mm, its blank at the major diameter.
  Document doc;
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Boss", glm::vec3(0), glm::vec3(0, 0, 1), 9.525 / 2.0, 12.0));
  const Body& body = doc.GetBodies().at(0);
  ElementRef side;
  for (const auto& [index, name] : body.elementMap.names) {
    glm::vec3 o(0.0f), axis(0.0f), x(0.0f);
    if (body.shape.FaceAxis(index, o, axis, x))
      side = ElementRef{body.createdBy, name};
  }
  REQUIRE(side.IsSet());
  doc.AddFeature(
      std::make_unique<ThreadFeature>("Stud", side, "3/8-16 UNC-2A", kP16));
  REQUIRE(doc.GetFeature(1)->GetError().empty());
  cam::Setup s = BasicSetup();
  s.tools.push_back(ThreadMill6());
  s.stock.bottomMargin = 0.0;
  const cam::DerivedOp d = cam::DeriveOperation(
      s, doc.GetBodies(), cam::OpType::ThreadMill, cam::Target{0, side});
  INFO(d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  const cam::ThreadPlan plan =
      cam::PlanThread(s, op, s.tools[1], doc.GetBodies(), op.targets[0]);
  REQUIRE(plan.ok);
  CHECK_FALSE(plan.thread.internal);
  // Outside, from beyond the blank inward: the last pass nearer the axis.
  CHECK(plan.finalRadius < plan.startRadius);
  // Climb round a boss is G2, and a right-hand thread's G2 falls.
  CHECK(plan.direction.clockwise);
  CHECK_FALSE(plan.direction.upward);
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, doc.GetBodies());
  REQUIRE(paths.size() == 1);
  REQUIRE(paths[0].error.empty());
  const std::string gcode = Post(s, paths);
  CHECK(gcode.find("\nG2") != std::string::npos);
  // The helix, judged: in 2A's band, right-handed, a pitch a turn. (The ring
  // round the boss is stock nobody cleared here, which the check says.)
  const cam::Verdict v =
      cam::VerifyGcode(gcode, s, doc.GetBodies(), 0.25, {}, {}, {}, nullptr, {},
                       DeclaredThreads(paths));
  REQUIRE(v.threads.size() == 1);
  const cam::ThreadVerdict& t = v.threads[0];
  REQUIRE(t.judged);
  CHECK_FALSE(t.internal);
  CHECK(t.handMeasured);
  CHECK(t.InBand());
  CHECK_THAT(t.pitchMeasured, WithinAbs(kP16, 1e-4));
  CHECK(t.boreStanding > 1.0);
}

TEST_CASE("a thread mill refuses what it cannot cut", "[cam][thread]") {
  const Plate plate = TappedPlate();
  cam::Setup s = BasicSetup();
  s.tools.push_back(ThreadMill6());
  const auto refuse = [&](const cam::Setup& setup, const ElementRef& face) {
    const cam::DerivedOp d =
        cam::DeriveOperation(setup, plate.doc->GetBodies(),
                             cam::OpType::ThreadMill, cam::Target{0, face});
    CHECK_FALSE(d.ok);
    return d.why;
  };
  // A face with no thread on it: the plate's top.
  ElementRef top;
  for (const auto& [index, name] : plate.doc->GetBodies()[0].elementMap.names) {
    glm::vec3 c(0.0f), n(0.0f), u(0.0f);
    if (plate.doc->GetBodies()[0].shape.FacePlane(index, c, n, u) &&
        n.z > 0.99f && c.z > 9.99f)
      top = ElementRef{plate.doc->GetBodies()[0].createdBy, name};
  }
  REQUIRE(top.IsSet());
  CHECK(refuse(s, top).find("carries no thread") != std::string::npos);
  // A bore with no thread: a plain 7.9375 hole.
  const Plate plain =
      TappedPlate(10.0, 0.0, "", true, /*bareHole=*/true, /*tapped=*/false);
  {
    const cam::DerivedOp d =
        cam::DeriveOperation(s, plain.doc->GetBodies(), cam::OpType::ThreadMill,
                             cam::Target{0, plain.bore});
    CHECK_FALSE(d.ok);
    CHECK(d.why.find("carries no thread") != std::string::npos);
  }
  // A cutter too large for the tap drill: 7 mm plus twice the depth.
  {
    cam::Setup big = BasicSetup();
    big.tools.push_back(ThreadMill6(2, 7.0));
    CHECK(refuse(big, plate.bore).find("too large for this tap-drill bore") !=
          std::string::npos);
  }
  // A thread finer than the cutter: a flat wider than the root's P/8.
  {
    cam::Setup fine = BasicSetup();
    cam::Tool t = ThreadMill6();
    t.tipFlat = 0.3;
    cam::FillThreadMillShape(t, true);
    fine.tools.push_back(t);
    CHECK(refuse(fine, plate.bore).find("too fine") != std::string::npos);
  }
  // A profile too shallow for the thread: the neck would rub.
  {
    cam::Setup shallow = BasicSetup();
    cam::Tool t = ThreadMill6();
    t.neckDiameter = 5.0;
    cam::FillThreadMillShape(t, true);
    shallow.tools.push_back(t);
    CHECK(refuse(shallow, plate.bore).find("neck would rub") !=
          std::string::npos);
  }
  // Not a thread mill.
  {
    const cam::DerivedOp d =
        cam::DeriveOperation(s, plate.doc->GetBodies(), cam::OpType::ThreadMill,
                             cam::Target{0, plate.bore}, /*tool=*/1);
    CHECK_FALSE(d.ok);
    CHECK(d.why.find("not a thread mill") != std::string::npos);
  }
  // A machine that posts no arcs.
  {
    cam::Setup noArcs = s;
    noArcs.machine.fitArcs = false;
    CHECK(refuse(noArcs, plate.bore).find("post no arcs") != std::string::npos);
  }
  // And the thread mill refuses to be anything else.
  {
    const cam::DerivedOp d =
        cam::DeriveOperation(s, plate.doc->GetBodies(), cam::OpType::Drill,
                             cam::Target{0, plate.bore}, /*tool=*/2);
    CHECK_FALSE(d.ok);
    CHECK(d.why.find("thread mill") != std::string::npos);
  }
}

TEST_CASE("the verifier judges a thread on its own terms", "[cam][thread]") {
  const Plate plate = TappedPlate();
  const cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, plate.doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[1].error.empty());
  const std::string gcode = Post(s, paths);
  const cam::Verdict v =
      cam::VerifyGcode(gcode, s, plate.doc->GetBodies(), 0.25, {}, {}, {},
                       nullptr, {}, DeclaredThreads(paths));
  REQUIRE(v.threads.size() == 1);
  const cam::ThreadVerdict& t = v.threads[0];
  INFO(t.why);
  REQUIRE(t.judged);
  CHECK(t.designation == "3/8-16 UNC");
  CHECK(t.threadClass == "2B");
  CHECK_THAT(t.pitchMeasured, WithinAbs(kP16, 1e-4));
  CHECK(t.handMeasured);
  CHECK(t.InBand());
  CHECK_THAT(t.pdReached, WithinAbs(0.5 * (t.pdMin + t.pdMax), 1e-3));
  CHECK(t.passes == 3);
  CHECK(t.shortMm < 1e-3);      // the file's four decimals
  CHECK(t.boreStanding < 0.05); // the drill bored it first
  CHECK(t.neckClearance > 0.1);
  // The carve inside the thread is the thread, not gouge; nothing else is.
  CHECK(v.maxGouge < 0.05);
  CHECK(v.Safe(0.05));
  bool said = false;
  for (const cam::VerdictLine& l : cam::Explain(v, 0.05, 0.0))
    said = said || (l.level == cam::VerdictLine::Level::Good &&
                    l.text.find("Thread 3/8-16 UNC-2B") != std::string::npos);
  CHECK(said);

  // Without the drill the head goes down into metal: not safe, and said.
  const cam::Setup undrilled = ThreadJob(*plate.doc, plate.bore, false);
  const std::vector<cam::Toolpath> only =
      cam::GenerateAll(undrilled, plate.doc->GetBodies());
  const cam::Verdict bad =
      cam::VerifyGcode(Post(undrilled, only), undrilled, plate.doc->GetBodies(),
                       0.25, {}, {}, {}, nullptr, {}, DeclaredThreads(only));
  REQUIRE(bad.threads.size() == 1);
  CHECK(bad.threads[0].boreStanding > 5.0);
  CHECK_FALSE(bad.Safe(0.05));

  // A thread the file does not cut is not judged, and not complete.
  const cam::Verdict none =
      cam::VerifyGcode(Post(s, {paths[0]}), s, plate.doc->GetBodies(), 0.25, {},
                       {}, {}, nullptr, {}, DeclaredThreads(paths));
  REQUIRE(none.threads.size() == 1);
  CHECK_FALSE(none.threads[0].judged);
  CHECK_FALSE(none.Complete(0.05));
}

// A through hole is drilled through the stock's bottom margin as well as the
// plate. Stopped at the plate, it left a slab under the hole, and the thread
// mill's overtravel -- a pitch past the plate -- ran its head into it.
TEST_CASE("a through thread's tap drill goes through the stock below the part",
          "[cam][thread]") {
  const Plate plate = TappedPlate();
  const cam::Setup s =
      ThreadJob(*plate.doc, plate.bore, true, ThreadMill6(), 2.0);
  // The drill reaches the stock's bottom, not just the part's: two
  // millimetres deeper than on the job with no margin.
  const cam::Setup flush = ThreadJob(*plate.doc, plate.bore);
  CHECK_THAT(s.operations[0].bottomZ,
             WithinAbs(flush.operations[0].bottomZ - 2.0, 1e-3));
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, plate.doc->GetBodies());
  REQUIRE(paths[0].error.empty());
  REQUIRE(paths[1].error.empty());
  const cam::Verdict v =
      cam::VerifyGcode(Post(s, paths), s, plate.doc->GetBodies(), 0.25, {}, {},
                       {}, nullptr, {}, DeclaredThreads(paths));
  REQUIRE(v.threads.size() == 1);
  INFO(v.threads[0].why);
  CHECK(v.threads[0].judged);
  CHECK(v.threads[0].InBand());
  CHECK(v.maxGouge < 0.05);
  CHECK(v.Safe(0.05));
  // The tap drill goes below the stock, into the spoilboard -- reported as
  // that, not as gouge.
  CHECK(v.belowStock > 0.0);
}

TEST_CASE("a thread mill job saves and loads", "[cam][thread]") {
  const Plate plate = TappedPlate();
  cam::Setup s = ThreadJob(*plate.doc, plate.bore);
  s.operations.back().threadPasses = 2;
  s.operations.back().climb = false;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("designer_cam_thread_" + std::to_string(getpid()) + ".json");
  {
    cam::JobDocument job;
    cam::Setup& added = job.AddSetup();
    const uint64_t id = added.id;
    added = s;
    added.id = id;
    REQUIRE(DocumentIO::SaveJob(job, path.string()).has_value());
  }
  auto loaded = DocumentIO::LoadJob(path.string());
  std::filesystem::remove(path);
  REQUIRE(loaded.has_value());
  REQUIRE((*loaded)->GetSetups().size() == 1);
  const cam::Setup& b = (*loaded)->GetSetups()[0];
  CHECK(b.tools[1] == s.tools[1]);
  CHECK(b.operations.back() == s.operations.back());
  CHECK(b.operations.back().type == cam::OpType::ThreadMill);
}

#endif // HAVE_CAM
