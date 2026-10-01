#ifdef HAVE_CAM

// CAM honest about what the model says and the cut cannot see: a toleranced
// wall cut to the middle of its band and judged against it, an offcut that
// is not leftover, a drill point and a thread depth honoured, and the holes
// and threads a job does not cut listed
// (docs/cam/operations.md#tolerance-bands, docs/cam/simulation.md).

#include "cam_fixtures.h"
#include "core/cam/CamGcode.h"
#include "core/cam/CamThreadMill.h"
#include "core/features/HoleFeature.h"
#include "core/features/PatternFeature.h"

namespace {

constexpr double kP16 = 25.4 / 16.0; // 3/8-16's pitch

// A 60 x 60 plate `thick` mm thick, its top at z = thick, with a hole at its
// centre made by `hole` (a Hole feature, edited by the caller).
struct Plate {
  std::unique_ptr<Document> doc;
  ElementRef bore;
};
template <typename Edit>
Plate HolePlate(double thick, double diameter, double depth, Edit&& edit) {
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
  auto hole = std::make_unique<HoleFeature>("Hole", at, HoleKind::Simple,
                                            diameter, depth);
  edit(*hole);
  doc.AddFeature(std::move(hole));
  doc.Regenerate();
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  const Body& body = doc.GetBodies().at(0);
  for (const auto& [index, name] : body.elementMap.names)
    if (name.find("/bore") != std::string::npos)
      out.bore = ElementRef{body.createdBy, name};
  REQUIRE(out.bore.IsSet());
  return out;
}

// The job's bodies: the part's, with its tolerances stamped as a job does.
std::vector<Body> Stamped(const Document& doc) {
  std::vector<Body> bodies = doc.GetBodies();
  for (Body& b : bodies)
    cam::StampBands(doc, b);
  return bodies;
}

// A setup whose stock top is the part's: a faced plate, so a bore is all
// there is to cut.
cam::Setup Flush() {
  cam::Setup s = BasicSetup();
  s.stock.margin.z = 0.0;
  return s;
}

cam::Setup Ready(cam::Setup s, const std::vector<Body>& bodies) {
  s.safeZ = cam::DeriveSafeZ(s, bodies);
  REQUIRE(cam::RecomputeOrigin(s, bodies));
  return s;
}

std::string Post(const cam::Setup& s, const std::vector<cam::Toolpath>& paths) {
  std::string gcode, why;
  cam::PostOptions opts;
  opts.fitArcs = s.machine.fitArcs;
  REQUIRE(cam::PostProcess("mach3", s, paths, opts, gcode, &why));
  return gcode;
}

template <typename T, typename F>
std::vector<T> Gather(const std::vector<cam::Toolpath>& paths, F field) {
  std::vector<T> out;
  for (const cam::Toolpath& tp : paths)
    out.insert(out.end(), (tp.*field).begin(), (tp.*field).end());
  return out;
}

bool Says(const cam::Verdict& v, const std::string& words) {
  for (const cam::VerdictLine& l : cam::Explain(v, 0.375, 0.0))
    if (l.text.find(words) != std::string::npos)
      return true;
  return false;
}

} // namespace

// --- tolerance bands ---------------------------------------------------------

TEST_CASE("a toleranced bore is cut to its band's middle, posted and judged",
          "[cam]") {
  // A bearing seat: a 32 mm bore 9 deep, flat-floored, in a 12 mm plate.
  const Plate plate = HolePlate(12.0, 32.0, 9.0, [](HoleFeature&) {});
  Tolerance tol;
  tol.ref = plate.bore;
  tol.lower = 0.1; // a band the grid can see: 32.1 to 32.3
  tol.upper = 0.3;
  plate.doc->SetTolerance(tol);
  const std::vector<Body> bodies = Stamped(*plate.doc);
  REQUIRE(bodies[0].bands.size() == 1);
  CHECK(bodies[0].bands[0].lower == 0.1);

  cam::Setup s = Ready(Flush(), bodies);
  cam::DerivedOp d = cam::DeriveOperation(s, bodies, cam::OpType::Drill,
                                          cam::Target{0, plate.bore}, 1);
  REQUIRE(d.ok);
  d.op.id = 1;
  s.operations.push_back(d.op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  REQUIRE(paths[0].fits.size() == 1);
  const cam::Toolpath::Fit& fit = paths[0].fits[0];
  CHECK(fit.internal);
  CHECK_THAT(fit.nominal, WithinAbs(32.0, 1e-6));
  CHECK_THAT(fit.aim, WithinAbs(32.2, 1e-9));

  // The program says what it aimed at.
  const std::string gcode = Post(s, paths);
  CHECK(gcode.find("(fit op 1: bore D32") != std::string::npos);
  CHECK(gcode.find("aimed at 32.2") != std::string::npos);

  // And the verifier measures it against the band, not nominal.
  const std::vector<cam::Toolpath::Fit> fits =
      Gather<cam::Toolpath::Fit>(paths, &cam::Toolpath::fits);
  const cam::Verdict v = cam::VerifyGcode(gcode, s, bodies, 0.25, {}, {}, {},
                                          nullptr, {}, {}, fits);
  REQUIRE(v.fits.size() == 1);
  INFO(v.fits[0].why);
  REQUIRE(v.fits[0].judged);
  CHECK_THAT(v.fits[0].cut, WithinAbs(32.2, 2e-3));
  CHECK(v.fits[0].InBand());
  INFO("leftover " << v.maxLeftover << " at " << v.leftoverAt.x << ","
                   << v.leftoverAt.y << " gouge " << v.maxGouge << " at "
                   << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.Safe(0.375));
  CHECK(v.Complete(0.375));
  CHECK(Says(v, "in its band"));

  // The same bore cut at nominal -- what every bore was before -- is short
  // of the band: metal left on the wall, not complete.
  const std::vector<Body> bare = plate.doc->GetBodies();
  const std::vector<cam::Toolpath> nominal = cam::GenerateAll(s, bare);
  CHECK(nominal.at(0).fits.empty()); // an untoleranced face is cut as before
  const cam::Verdict atNominal = cam::VerifyGcode(
      Post(s, nominal), s, bodies, 0.25, {}, {}, {}, nullptr, {}, {}, fits);
  REQUIRE(atNominal.fits.size() == 1);
  CHECK_THAT(atNominal.fits[0].cut, WithinAbs(32.0, 2e-3));
  CHECK(atNominal.fits[0].Short());
  CHECK_FALSE(atNominal.Complete(0.375));
  CHECK(Says(atNominal, "short of its band"));

  // Cut past a band on the side nothing puts back: scrap, not safe.
  std::vector<cam::Toolpath::Fit> tight = fits;
  tight[0].lower = -0.3;
  tight[0].upper = -0.25;
  const cam::Verdict over = cam::VerifyGcode(gcode, s, bodies, 0.25, {}, {}, {},
                                             nullptr, {}, {}, tight);
  REQUIRE(over.fits.size() == 1);
  CHECK(over.fits[0].Scrap());
  CHECK_FALSE(over.Safe(0.375));
  CHECK(Says(over, "scrap"));
}

TEST_CASE("an ISO class is read at the face's size, and a profile aims too",
          "[cam]") {
  const Plate plate = HolePlate(12.0, 32.0, 9.0, [](HoleFeature&) {});
  Tolerance tol;
  tol.ref = plate.bore;
  tol.isoClass = "K7"; // +7/-18 um at 32
  plate.doc->SetTolerance(tol);
  const std::vector<Body> bodies = Stamped(*plate.doc);
  REQUIRE(bodies[0].bands.size() == 1);
  CHECK(bodies[0].bands[0].label == "K7");
  CHECK_THAT(bodies[0].bands[0].lower, WithinAbs(-0.018, 1e-6));
  CHECK_THAT(bodies[0].bands[0].upper, WithinAbs(0.007, 1e-6));

  // A spring pass round the wall: an inside profile, one pass.
  cam::Setup s = Ready(Flush(), bodies);
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.targets = {cam::Target{0, plate.bore}};
  op.topZ = 12.0;
  op.bottomZ = 3.0;
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  REQUIRE(paths[0].fits.size() == 1);
  CHECK_THAT(paths[0].fits[0].aim, WithinAbs(31.9945, 1e-9));
  const cam::Verdict v = cam::VerifyGcode(
      Post(s, paths), s, bodies, 0.25, {}, {}, {}, nullptr, {}, {},
      Gather<cam::Toolpath::Fit>(paths, &cam::Toolpath::fits));
  REQUIRE(v.fits.size() == 1);
  REQUIRE(v.fits[0].judged);
  // The file's four decimals put the rim within a micron of the aim.
  // Within the chords a profile follows: a polygon's offset corners stand
  // inside the circle by r (1 / cos - 1), a few microns at 0.02 mm chords.
  CHECK_THAT(v.fits[0].cut, WithinAbs(31.9945, 0.01));
  CHECK(v.fits[0].InBand());
}

// --- tabs and offcuts --------------------------------------------------------

TEST_CASE("a part cut out on tabs verifies complete; one with none is said",
          "[cam]") {
  // On a sheet: a frame of stock round the part, wider than the cutter.
  auto doc = BlockDoc();
  cam::Setup sheet = BasicSetup();
  sheet.stock.margin = {12.0, 12.0, 1.0};
  cam::Setup setup = Ready(sheet, doc->GetBodies());
  cam::DerivedOp derived = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{});
  REQUIRE(derived.ok);
  REQUIRE(derived.op.tabCount > 0);
  derived.op.id = 1;
  setup.operations.push_back(derived.op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  const std::vector<cam::Toolpath::Tab> tabs =
      Gather<cam::Toolpath::Tab>(paths, &cam::Toolpath::tabs);
  REQUIRE(tabs.size() == static_cast<size_t>(derived.op.tabCount));
  const cam::Verdict v = cam::VerifyGcode(Post(setup, paths), setup,
                                          doc->GetBodies(), 0.4, {}, tabs);
  INFO("leftover " << v.maxLeftover << " in " << v.leftoverRegions << " below "
                   << v.belowStock << " free " << v.cutFree.size()
                   << " tabs at " << tabs[0].topZ << " r " << tabs[0].radius);
  CHECK(v.Safe(0.6));
  CHECK(v.Complete(0.6)); // the tabs, and the frame round the part
  CHECK(v.cutFree.empty());
  CHECK(v.offcutArea > 0.0); // the frame is offcut, not leftover

  // With no tabs the part is cut free: complete, and said.
  setup.operations[0].tabCount = 0;
  const std::vector<cam::Toolpath> bare =
      cam::GenerateAll(setup, doc->GetBodies());
  const cam::Verdict loose =
      cam::VerifyGcode(Post(setup, bare), setup, doc->GetBodies(), 0.4);
  CHECK(loose.Complete(0.6));
  REQUIRE(loose.cutFree.size() == 1);
  CHECK(loose.cutFree[0].part);
  CHECK_THAT(loose.cutFree[0].area, WithinRel(40.0 * 30.0, 0.05));
  CHECK(Says(loose, "it would fly"));
}

TEST_CASE("an opening's slug is offcut, and flagged when nothing holds it",
          "[cam]") {
  // A 20 mm opening through a 10 mm plate, profiled with a 6 mm cutter and
  // lead arcs, which swing into the slug.
  const Plate plate =
      HolePlate(10.0, 20.0, 10.0, [](HoleFeature& h) { h.through = true; });
  cam::Setup s = Ready(BasicSetup(), plate.doc->GetBodies());
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Inside;
  op.leadArcs = true;
  op.targets = {cam::Target{0, plate.bore}};
  op.topZ = 10.0;
  op.bottomZ = -1.5; // through the stock's 1 mm under the plate
  s.operations.push_back(op);
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(s, plate.doc->GetBodies());
  REQUIRE(paths.at(0).error.empty());
  const cam::Verdict v =
      cam::VerifyGcode(Post(s, paths), s, plate.doc->GetBodies(), 0.25);
  INFO("leftover " << v.maxLeftover << " in " << v.leftoverRegions);
  CHECK(v.Complete(0.375));
  REQUIRE(v.cutFree.size() == 1);
  CHECK_FALSE(v.cutFree[0].part);
  CHECK_THAT(v.cutFree[0].area, WithinRel(std::numbers::pi * 16.0, 0.15));
  CHECK(Says(v, "An offcut"));
}

// --- a distance tolerance ----------------------------------------------------

namespace {
// The down-facing planar face whose centroid sits at height `z`.
ElementRef DownFaceAt(const Document& doc, double z) {
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::dvec3 c(0.0), n(0.0), u(0.0);
      if (b.shape.FacePlane(index, c, n, u) && n.z < -0.99 &&
          std::abs(c.z - z) < 1e-6)
        return ElementRef{b.createdBy, name};
    }
  return {};
}

// A 40 x 30 x 20 block with a step milled off its -X end: a shoulder at
// z = 12 from x = 0 to 15, its wall at x = 15.
std::unique_ptr<Document> StepDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 40, 30, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Step", glm::vec3(-1, -1, 12), 16, 32, 10));
  const uint64_t step = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Stepped", block, step,
                                                   BooleanOp::Subtract));
  doc->Regenerate();
  return doc;
}
} // namespace

TEST_CASE("a floor whose depth from the top is toleranced is cut to the "
          "band's middle, posted and judged",
          "[cam]") {
  // A pocket 8 deep, its floor's distance from the top 8 +0.3/+0.1: the
  // floor is cut 8.2 down, at Z 11.8, and the file says so.
  auto doc = PocketDoc();
  doc->Regenerate();
  const ElementRef floor = PocketFloorRef(*doc);
  const ElementRef top = UpFaceAt(*doc, 20.0);
  REQUIRE(floor.IsSet());
  REQUIRE(top.IsSet());
  Tolerance tol;
  tol.ref = floor;
  tol.ref2 = top;
  tol.lower = 0.1;
  tol.upper = 0.3;
  doc->SetTolerance(tol);
  const std::vector<Body> bodies = Stamped(*doc);
  REQUIRE(bodies[0].bands.size() == 1);
  CHECK(bodies[0].bands[0].reference.IsSet());

  cam::Setup s = Ready(Flush(), bodies);
  cam::DerivedOp d = cam::DeriveOperation(s, bodies, cam::OpType::Pocket,
                                          cam::Target{0, floor}, 1);
  REQUIRE(d.ok);
  CHECK_THAT(d.op.bottomZ, WithinAbs(12.0, 1e-9)); // the model's floor
  d.op.id = 1;
  s.operations.push_back(d.op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  REQUIRE(paths[0].fits.size() == 1);
  const cam::Toolpath::Fit& fit = paths[0].fits[0];
  CHECK(fit.depth);
  CHECK(fit.internal); // a floor below its reference
  CHECK_THAT(fit.nominal, WithinAbs(8.0, 1e-9));
  CHECK_THAT(fit.aim, WithinAbs(8.2, 1e-9));
  double lowest = 1e9;
  for (const cam::Move& m : paths[0].moves)
    lowest = std::min(lowest, m.to.z);
  CHECK_THAT(lowest, WithinAbs(11.8, 1e-6));

  const std::string gcode = Post(s, paths);
  INFO(gcode.substr(0, 600));
  CHECK(gcode.find("(fit op 1: floor 8 from Z20 +0.300/+0.100 8.1-8.3, "
                   "aimed at 8.2 [Z11.8])") != std::string::npos);

  const std::vector<cam::Toolpath::Fit> fits =
      Gather<cam::Toolpath::Fit>(paths, &cam::Toolpath::fits);
  const cam::Verdict v = cam::VerifyGcode(gcode, s, bodies, 0.25, {}, {}, {},
                                          nullptr, {}, {}, fits);
  REQUIRE(v.fits.size() == 1);
  INFO(v.fits[0].why);
  REQUIRE(v.fits[0].judged);
  CHECK(v.fits[0].depth);
  CHECK_THAT(v.fits[0].cut, WithinAbs(8.2, 1e-6));
  CHECK(v.fits[0].InBand());
  INFO("leftover " << v.maxLeftover << " gouge " << v.maxGouge << " at "
                   << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.Safe(0.05)); // the 0.2 below the model is the band, not a gouge
  CHECK(v.Complete(0.375));
  CHECK(Says(v, "Floor 8.000 from Z 20.000"));
  CHECK(Says(v, "in its band"));

  // Cut at the model's floor -- what every floor was before -- it is short
  // of the band; cut past it, scrap.
  const std::vector<Body> bare = doc->GetBodies();
  const std::vector<cam::Toolpath> nominal = cam::GenerateAll(s, bare);
  CHECK(nominal.at(0).fits.empty()); // an untoleranced floor is cut as before
  const cam::Verdict atNominal = cam::VerifyGcode(
      Post(s, nominal), s, bodies, 0.25, {}, {}, {}, nullptr, {}, {}, fits);
  REQUIRE(atNominal.fits.size() == 1);
  CHECK_THAT(atNominal.fits[0].cut, WithinAbs(8.0, 1e-6));
  CHECK(atNominal.fits[0].Short());
  CHECK_FALSE(atNominal.Complete(0.375));
  CHECK(Says(atNominal, "short of its band"));
  std::vector<cam::Toolpath::Fit> shallow = fits;
  shallow[0].lower = -0.2;
  shallow[0].upper = 0.0;
  const cam::Verdict over = cam::VerifyGcode(gcode, s, bodies, 0.25, {}, {}, {},
                                             nullptr, {}, {}, shallow);
  REQUIRE(over.fits.size() == 1);
  CHECK(over.fits[0].Scrap());
  CHECK_FALSE(over.Safe(0.375));
  CHECK(Says(over, "scrap"));
}

TEST_CASE("a shoulder whose height from the base is toleranced is cut to "
          "the band's middle and judged",
          "[cam]") {
  // A shoulder 12 above the part's underside, -0.1/-0.3: removing metal
  // shortens the distance, so it is cut 11.8 up, and a cut lower than 11.7
  // is scrap.
  auto doc = StepDoc();
  const ElementRef shoulder = UpFaceAt(*doc, 12.0);
  const ElementRef base = DownFaceAt(*doc, 0.0);
  REQUIRE(shoulder.IsSet());
  REQUIRE(base.IsSet());
  Tolerance tol;
  tol.ref = base; // either order
  tol.ref2 = shoulder;
  tol.lower = -0.3;
  tol.upper = -0.1;
  doc->SetTolerance(tol);
  const std::vector<Body> bodies = Stamped(*doc);
  cam::Setup s = Ready(Flush(), bodies);
  cam::DerivedOp d =
      cam::DeriveOperation(s, bodies, cam::OpType::Profile,
                           cam::Target{0, WallNear(*doc, 15.0, 15.0)}, 1);
  INFO(d.why);
  REQUIRE(d.ok);
  REQUIRE_THAT(d.op.bottomZ, WithinAbs(12.0, 1e-9)); // the wall stops on it
  d.op.id = 1;
  s.operations.push_back(d.op);
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  REQUIRE(paths[0].fits.size() == 1);
  CHECK(paths[0].fits[0].depth);
  CHECK_FALSE(paths[0].fits[0].internal);
  CHECK_THAT(paths[0].fits[0].ZAt(paths[0].fits[0].aim), WithinAbs(11.8, 1e-9));
  const std::string gcode = Post(s, paths);
  CHECK(gcode.find("(fit op 1: shoulder 12 from Z0 -0.100/-0.300 "
                   "11.7-11.9, aimed at 11.8 [Z11.8])") != std::string::npos);
  const cam::Verdict v =
      cam::VerifyGcode(gcode, s, bodies, 0.25, {}, {}, {}, nullptr, {}, {},
                       Gather<cam::Toolpath::Fit>(paths, &cam::Toolpath::fits));
  REQUIRE(v.fits.size() == 1);
  INFO(v.fits[0].why);
  CHECK_THAT(v.fits[0].cut, WithinAbs(11.8, 1e-6));
  CHECK(v.fits[0].InBand());
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y);
  CHECK(v.Safe(0.05));
}

// --- the drill point and the thread depth ------------------------------------

namespace {
// The blind tapped-hole coupon: a 20 mm plate, a 3/8-16 tapped hole 15 deep,
// its point and thread depth derived (118 degrees; 15 less 3 pitches).
Plate Coupon() {
  return HolePlate(20.0, 1.0, 15.0, [](HoleFeature& h) {
    h.fitThread = "3/8-16 UNC";
    h.fitKind = "tapped";
  });
}

cam::Tool TapDrill(cam::ToolType type) {
  cam::Tool t = ShapedTool(type, 7.9375, 118.0);
  t.number = 1;
  t.name = "5/16 drill";
  return t;
}

cam::Tool Mill6() {
  cam::Tool t = ShapedTool(cam::ToolType::ThreadMill, 6.0);
  t.number = 2;
  t.name = "6 mm thread mill";
  t.feedCut = 300.0;
  t.feedPlunge = 150.0;
  cam::FillThreadMillShape(t, /*angleStated=*/false);
  return t;
}
} // namespace

TEST_CASE("a blind tapped hole with a drill point verifies clean", "[cam]") {
  const Plate plate = Coupon();
  const std::vector<Body>& bodies = plate.doc->GetBodies();
  REQUIRE(bodies[0].holes.size() == 1);
  CHECK(bodies[0].holes[0].pointAngleDeg == 118.0);
  REQUIRE(bodies[0].threads.size() == 1);
  CHECK_THAT(bodies[0].threads[0].length, WithinAbs(15.0 - 3 * kP16, 1e-9));

  cam::Setup s = BasicSetup();
  s.tools = {TapDrill(cam::ToolType::Drill), Mill6()};
  s.stock.bottomMargin = 0.0;
  s = Ready(s, bodies);
  const cam::DerivedOp drill = cam::DeriveOperation(
      s, bodies, cam::OpType::Drill, cam::Target{0, plate.bore}, 1);
  REQUIRE(drill.ok);
  // Down by the drill's own point, so its full diameter reaches the bore's
  // bottom (z 5) and its point cuts the modelled cone; nothing to say.
  const double point = 7.9375 * 0.5 / std::tan(59.0 * std::numbers::pi / 180);
  CHECK_THAT(drill.op.bottomZ, WithinAbs(5.0 - point, 1e-3));
  CHECK(drill.notes.empty());
  const cam::DerivedOp thread = cam::DeriveOperation(
      s, bodies, cam::OpType::ThreadMill, cam::Target{0, plate.bore}, 2);
  INFO(thread.why);
  REQUIRE(thread.ok);
  s.operations = {drill.op, thread.op};
  s.operations[0].id = 1;
  s.operations[1].id = 2;
  // The thread stops at its own depth, short of the floor.
  const cam::ThreadPlan plan = cam::PlanThread(
      s, s.operations[1], s.tools[1], bodies, s.operations[1].targets[0]);
  REQUIRE(plan.ok);
  CHECK_THAT(plan.thread.zLo, WithinAbs(20.0 - (15.0 - 3 * kP16), 1e-3));
  CHECK(plan.tipLo > 5.0);

  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  REQUIRE(paths.at(0).error.empty());
  REQUIRE(paths.at(1).error.empty());
  const cam::Verdict v = cam::VerifyGcode(
      Post(s, paths), s, bodies, 0.25, {}, {}, {}, nullptr, {},
      Gather<cam::Toolpath::Thread>(paths, &cam::Toolpath::threads));
  INFO("leftover " << v.maxLeftover << " at " << v.leftoverAt.x << ","
                   << v.leftoverAt.y << "; gouge " << v.maxGouge);
  REQUIRE(v.threads.size() == 1);
  CHECK(v.threads[0].judged);
  CHECK(v.threads[0].InBand());
  CHECK(v.Safe(0.375));
  CHECK(v.Complete(0.375)); // no cone left standing under the drill
  CHECK(v.uncut.empty());

  // A flat-bottomed tap drill stops at the bore's bottom and leaves the
  // cone -- said at derivation, and left over in the check.
  cam::Setup flat = s;
  flat.tools[0] = TapDrill(cam::ToolType::Flat);
  const cam::DerivedOp flatDrill = cam::DeriveOperation(
      flat, bodies, cam::OpType::Drill, cam::Target{0, plate.bore}, 1);
  REQUIRE(flatDrill.ok);
  CHECK_THAT(flatDrill.op.bottomZ, WithinAbs(5.0, 1e-3));
  REQUIRE(flatDrill.notes.size() == 1);
  CHECK(flatDrill.notes[0].find("is flat") != std::string::npos);
  flat.operations[0] = flatDrill.op;
  flat.operations[0].id = 1;
  const std::vector<cam::Toolpath> flatPaths = cam::GenerateAll(flat, bodies);
  const cam::Verdict cone = cam::VerifyGcode(
      Post(flat, flatPaths), flat, bodies, 0.25, {}, {}, {}, nullptr, {},
      Gather<cam::Toolpath::Thread>(flatPaths, &cam::Toolpath::threads));
  CHECK_FALSE(cone.Complete(0.375));
}

// --- what the job does not cut -----------------------------------------------

TEST_CASE("a job lists the holes and threads it does not cut", "[cam]") {
  const Plate plate = Coupon();
  const std::vector<Body>& bodies = plate.doc->GetBodies();
  cam::Setup s = BasicSetup();
  s.tools = {TapDrill(cam::ToolType::Drill), Mill6()};
  s = Ready(s, bodies);

  // Drilled, not threaded: the hole is cut, its thread is not.
  cam::DerivedOp drill = cam::DeriveOperation(s, bodies, cam::OpType::Drill,
                                              cam::Target{0, plate.bore}, 1);
  REQUIRE(drill.ok);
  drill.op.id = 1;
  s.operations = {drill.op};
  const std::vector<cam::Toolpath> paths = cam::GenerateAll(s, bodies);
  cam::Verdict v = cam::VerifyGcode(Post(s, paths), s, bodies, 0.25);
  CHECK(v.uncut.empty()); // the hole is drilled
  cam::AddUncutThreads({s}, bodies, {}, v);
  REQUIRE(v.uncut.size() == 1);
  CHECK(v.uncut[0].kind == cam::UncutFeature::Kind::Thread);
  CHECK(v.uncut[0].what == "3/8-16 UNC thread 10.2 long");
  CHECK(Says(v, "Not cut by this job: 1 x 3/8-16 UNC thread"));
  // A finding, not a failure: the tap may do it by hand.
  CHECK(v.Safe(0.375));

  // A thread mill that cut it takes it off the list.
  cam::DerivedOp mill = cam::DeriveOperation(s, bodies, cam::OpType::ThreadMill,
                                             cam::Target{0, plate.bore}, 2);
  REQUIRE(mill.ok);
  mill.op.id = 2;
  s.operations.push_back(mill.op);
  cam::Verdict milled;
  cam::AddUncutThreads({s}, bodies, {2}, milled);
  CHECK(milled.uncut.empty());

  // A job that only faces the plate cuts neither.
  cam::Setup facing = BasicSetup();
  facing.tools = {TapDrill(cam::ToolType::Flat)};
  facing = Ready(facing, bodies);
  cam::DerivedOp face =
      cam::DeriveOperation(facing, bodies, cam::OpType::Face, cam::Target{});
  REQUIRE(face.ok);
  face.op.id = 1;
  facing.operations = {face.op};
  cam::Verdict faced = cam::VerifyGcode(
      Post(facing, cam::GenerateAll(facing, bodies)), facing, bodies, 0.25);
  cam::AddUncutThreads({facing}, bodies, {}, faced);
  REQUIRE(faced.uncut.size() == 2);
  CHECK(faced.uncut[0].kind == cam::UncutFeature::Kind::Hole);
  CHECK(faced.uncut[0].feature == "Hole");
  CHECK(Says(faced, "tap-drill hole"));
}

// --- a pattern of holes ------------------------------------------------------

namespace {
// The coupon's bores by instance: the seed's, then each copy's ("#1", "#2").
std::vector<ElementRef> Bores(const Body& body) {
  std::vector<ElementRef> out;
  for (const char* tag : {"", "#1", "#2"})
    for (const auto& [index, name] : body.elementMap.names)
      if (name.find("/bore") != std::string::npos &&
          (*tag == 0 ? name.find('#') == std::string::npos
                     : name.find(tag) != std::string::npos))
        out.push_back(ElementRef{body.createdBy, name});
  return out;
}
} // namespace

TEST_CASE("a patterned tapped hole is drilled, threaded and listed like one",
          "[cam][pattern]") {
  // A Hole writes its HoleMark and ThreadMark on the bores it cut; a pattern
  // of it cut copies whose bores carried neither, so CAM drilled a copy flat
  // as a plain bore, refused to thread it, and listed only the seed as uncut.
  Plate plate = Coupon();
  Document& doc = *plate.doc;
  const uint64_t holeId = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  auto pat =
      std::make_unique<PatternFeature>("Holes x3", PatternKind::Linear, holeId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 3;
  pat->spacing = 12.0;
  doc.AddFeature(std::move(pat));
  doc.Regenerate();
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  const std::vector<Body>& bodies = doc.GetBodies();
  REQUIRE(bodies.size() == 1);
  const std::vector<ElementRef> bores = Bores(bodies[0]);
  REQUIRE(bores.size() == 3);
  REQUIRE(bodies[0].holes.size() == 3);
  REQUIRE(bodies[0].threads.size() == 3);
  CHECK(bodies[0].threadRegions.size() == 3);
  for (const HoleMark& h : bodies[0].holes) {
    CHECK(h.pointAngleDeg == 118.0);
    CHECK(h.thread == "3/8-16 UNC");
    CHECK(h.featureId == holeId);
  }

  cam::Setup s = BasicSetup();
  s.tools = {TapDrill(cam::ToolType::Drill), Mill6()};
  s.stock.bottomMargin = 0.0;
  s = Ready(s, bodies);
  const double point = 7.9375 * 0.5 / std::tan(59.0 * std::numbers::pi / 180);
  for (const ElementRef& bore : bores) {
    INFO(bore.name);
    // Drilled to the bore's bottom by the drill's point, as the seed is.
    const cam::DerivedOp drill = cam::DeriveOperation(
        s, bodies, cam::OpType::Drill, cam::Target{0, bore}, 1);
    REQUIRE(drill.ok);
    CHECK_THAT(drill.op.bottomZ, WithinAbs(5.0 - point, 1e-3));
    // Threaded to the seed's thread depth.
    const cam::DerivedOp thread = cam::DeriveOperation(
        s, bodies, cam::OpType::ThreadMill, cam::Target{0, bore}, 2);
    INFO(thread.why);
    REQUIRE(thread.ok);
    cam::Setup one = s;
    one.operations = {thread.op};
    const cam::ThreadPlan plan =
        cam::PlanThread(one, one.operations[0], s.tools[1], bodies,
                        one.operations[0].targets[0]);
    REQUIRE(plan.ok);
    CHECK_THAT(plan.thread.zLo, WithinAbs(20.0 - (15.0 - 3 * kP16), 1e-3));
  }

  // A job that only faces the plate lists every hole and every thread.
  cam::Setup facing = BasicSetup();
  facing.tools = {TapDrill(cam::ToolType::Flat)};
  facing = Ready(facing, bodies);
  cam::DerivedOp face =
      cam::DeriveOperation(facing, bodies, cam::OpType::Face, cam::Target{});
  REQUIRE(face.ok);
  face.op.id = 1;
  facing.operations = {face.op};
  cam::Verdict faced = cam::VerifyGcode(
      Post(facing, cam::GenerateAll(facing, bodies)), facing, bodies, 0.25);
  cam::AddUncutThreads({facing}, bodies, {}, faced);
  int holes = 0, threads = 0;
  for (const cam::UncutFeature& u : faced.uncut)
    (u.kind == cam::UncutFeature::Kind::Hole ? holes : threads)++;
  CHECK(holes == 3);
  CHECK(threads == 3);
}

TEST_CASE("a pattern of a drilled body carries its holes to every copy",
          "[cam][pattern]") {
  // The other path: a pattern of a BODY (the plate the hole is in) fuses
  // copies into new bodies, which dropped every mark, the seed's too.
  Plate plate = HolePlate(10.0, 1.0, 0.0, [](HoleFeature& h) {
    h.through = true;
    h.fitThread = "M6";
    h.fitKind = "tapped";
  });
  Document& doc = *plate.doc;
  const uint64_t padId = doc.GetFeature(1)->GetId();
  auto pat =
      std::make_unique<PatternFeature>("Plates x2", PatternKind::Linear, padId);
  pat->axis.kind = AxisRef::Kind::X;
  pat->count = 2;
  pat->spacing = 100.0;
  doc.AddFeature(std::move(pat));
  doc.Regenerate();
  REQUIRE(doc.GetFeature(doc.FeatureCount() - 1)->GetError().empty());
  size_t holes = 0, threads = 0;
  for (const Body& b : doc.GetBodies()) {
    for (const HoleMark& h : b.holes) {
      CHECK(h.bore.featureId == b.createdBy);
      CHECK(b.elementMap.Resolve(h.bore.name).has_value());
      CHECK(h.through);
    }
    holes += b.holes.size();
    threads += b.threads.size();
  }
  CHECK(holes == 2);
  CHECK(threads == 2);
}

#endif // HAVE_CAM
