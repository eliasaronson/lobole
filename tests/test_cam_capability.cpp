#ifdef HAVE_CAM

// The capability table, enforced (docs/cam/operation-shape-plan.md).
//
// `CamNames.h`'s OpField column states, per op type, exactly which fields of
// `Operation` that type's generator reads. It was traced by hand through the
// helpers -- CamNames.cpp says so, and says which three traps the tracing had
// to avoid -- and until this file nothing checked it. A claim nobody checks is
// a wish, which is the same sentence test_golden.cpp writes about the version
// history.
//
// The check is behavioural rather than structural, because there is no way to
// ask C++ which fields a function read: set a field the type does NOT claim,
// regenerate, and require the moves to come back identical. A generator
// reading a field it never declared moves the path, and the failure names the
// type and the field.
//
// This is hardening item 9's answer. The item proposed dissolving `Operation`
// into FreeCAD-style dressup wrappers; the plan above records why that shape
// does not fit here (two of the four candidates are inputs to the OFFSET, not
// edits to a finished path) and why the unenforced claim, not the wide struct,
// was the defect worth paying for.

#include "cam_fixtures.h"

namespace {

// A perturbation per field: something visibly different from the default that
// a generator reading it could not ignore.
struct FieldPoke {
  cam::OpField field;
  const char* name;
  void (*poke)(cam::Operation&);
};

constexpr FieldPoke kPokes[] = {
    {cam::OpField::Stepdown, "stepdown",
     [](cam::Operation& o) { o.stepdown = 1.0; }},
    {cam::OpField::StockToLeave, "stock_to_leave",
     [](cam::Operation& o) { o.stockToLeave = 0.7; }},
    {cam::OpField::Stepover, "stepover",
     [](cam::Operation& o) { o.stepover = 0.2; }},
    {cam::OpField::Side, "side",
     [](cam::Operation& o) {
       o.side = o.side == cam::Side::Outside ? cam::Side::Inside
                                             : cam::Side::Outside;
     }},
    {cam::OpField::Climb, "climb",
     [](cam::Operation& o) { o.climb = !o.climb; }},
    {cam::OpField::Tabs, "tabs",
     [](cam::Operation& o) {
       o.tabCount = 4;
       o.tabWidth = 6.0;
       o.tabHeight = 2.0;
     }},
    {cam::OpField::CornerRelief, "corner_relief",
     [](cam::Operation& o) { o.cornerRelief = cam::CornerRelief::Dogbone; }},
    {cam::OpField::LeadArcs, "lead_arcs",
     [](cam::Operation& o) { o.leadArcs = true; }},
    {cam::OpField::DrilledBores, "drilled_bores",
     [](cam::Operation& o) { o.drilledBores = 12.0; }},
    {cam::OpField::Peck, "peck", [](cam::Operation& o) { o.peck = 2.0; }},
    {cam::OpField::EdgeSize, "edge_size",
     [](cam::Operation& o) { o.edgeSize = 1.5; }},
    {cam::OpField::UseOutline, "use_outline",
     [](cam::Operation& o) { o.useOutline = true; }},
    {cam::OpField::Finish, "finish_tool",
     [](cam::Operation& o) {
       o.finishTool = 1;
       o.finishStock = 0.4;
     }},
    {cam::OpField::CutAngle, "cut_angle_deg",
     [](cam::Operation& o) { o.cutAngleDeg = 55.0; }},
    {cam::OpField::SampleStep, "sample_step_mm",
     [](cam::Operation& o) { o.sampleStep = 2.5; }},
    {cam::OpField::ZStep, "z_step_mm",
     [](cam::Operation& o) { o.zStep = 3.5; }},
};
// FullWidth, ClearsInside and NeedsFlatFloor are deliberately absent: they are
// per-type FACTS the table also carries, not fields anything can set.

std::string Fingerprint(const std::vector<cam::Toolpath>& paths) {
  std::string s;
  char buf[160];
  for (const cam::Toolpath& tp : paths) {
    std::snprintf(buf, sizeof(buf), "op%llu t%d e[%s] s[%s]\n",
                  static_cast<unsigned long long>(tp.operationId),
                  tp.toolNumber, tp.error.c_str(), tp.strategy.c_str());
    s += buf;
    for (const cam::Move& m : tp.moves) {
      std::snprintf(buf, sizeof(buf), "%d %.9f %.9f %.9f %d %.9f %.6f\n",
                    static_cast<int>(m.kind), m.to.x, m.to.y, m.to.z,
                    m.hasA ? 1 : 0, m.a, m.feed);
      s += buf;
    }
  }
  return s;
}

// One operation of each type, on a part that has something for all of them:
// a 120 x 90 x 20 block with an 80 x 60 pocket down to z = 12 and a bore
// through its floor. Built through `DeriveOperation` -- the same derivation a
// click uses -- so every depth comes from the model and no number here can
// drift away from the job the app would actually make.
struct TypedJob {
  std::unique_ptr<Document> doc;
  cam::Setup setup;
};

// A 120 x 90 x 20 block with a Phi 50 DOME standing on it. The surfacing types
// need curvature WITH AREA SEEN FROM ABOVE, which is two requirements and not
// one: a waterline over a flat top has no laps and generates nothing, and a
// cylinder's wall is edge-on to the spindle so the derivation refuses it by
// name. Either way the fixture would pass every equality in the loop below
// without testing a thing, which is the failure this whole file is about.
std::unique_ptr<Document> DomedDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 60, 60, 20));
  const uint64_t block = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<SphereFeature>("Dome", glm::vec3(30, 30, 20), 20.0));
  const uint64_t dome = doc->GetFeature(1)->GetId();
  doc->AddFeature(
      std::make_unique<BooleanFeature>("Domed", block, dome, BooleanOp::Union));
  return doc;
}

// The dome -- a spherical face, which is the one face here that is neither
// planar nor axis-aligned.
ElementRef DomeFace(const Document& doc) {
  for (const Body& b : doc.GetBodies())
    for (const auto& [index, name] : b.elementMap.names) {
      glm::vec3 c(0.0f), n(0.0f), u(0.0f);
      glm::vec3 o(0.0f), axis(0.0f), x(0.0f);
      if (!b.shape.FacePlane(index, c, n, u) &&
          !b.shape.FaceAxis(index, o, axis, x))
        return ElementRef{b.createdBy, name};
    }
  return {};
}

// A 60 x 45 x 20 block with a 36 x 24 pocket down to z = 12 and a Phi 10 bore
// through its floor -- FlangeDoc's shape at half the size. Half, because this
// file regenerates every type's toolpath once per field and the clearing
// strategies cost area: the full-size fixture put the pair of checks at seven
// minutes, which is more than the whole [slow] set costs today.
std::unique_ptr<Document> PartDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Block", glm::vec3(0, 0, 0), 60, 45, 20));
  uint64_t solid = doc->GetFeature(0)->GetId();
  doc->AddFeature(
      std::make_unique<BoxFeature>("Void", glm::vec3(12, 10, 12), 36, 24, 20));
  const uint64_t voidId = doc->GetFeature(1)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Pocket", solid, voidId,
                                                   BooleanOp::Subtract));
  solid = doc->GetFeature(2)->GetId();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Bore", glm::vec3(30, 22, -1), glm::vec3(0, 0, 1), 5.0, 30.0));
  const uint64_t bore = doc->GetFeature(3)->GetId();
  doc->AddFeature(std::make_unique<BooleanFeature>("Holed", solid, bore,
                                                   BooleanOp::Subtract));
  return doc;
}

// A bar ON the rotary axis (setup-space +X) with a turned-down waist: what a
// continuous 4-axis pass is for, and the only fixture here that is not
// machined from above.
std::unique_ptr<Document> BarDoc() {
  auto doc = std::make_unique<Document>();
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Blank", glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), 20.0, 60.0));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Waist", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 20.0, 20.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Necked", doc->GetFeature(0)->GetId(), doc->GetFeature(1)->GetId(),
      BooleanOp::Subtract));
  doc->AddFeature(std::make_unique<CylinderFeature>(
      "Journal", glm::vec3(20, 0, 0), glm::vec3(1, 0, 0), 15.0, 20.0));
  doc->AddFeature(std::make_unique<BooleanFeature>(
      "Shaft", doc->GetFeature(2)->GetId(), doc->GetFeature(3)->GetId(),
      BooleanOp::Union));
  return doc;
}

TypedJob JobFor(cam::OpType type) {
  TypedJob j;
  const bool rotary = type == cam::OpType::SurfaceRotary;
  const bool surfacing = type == cam::OpType::SurfaceParallel ||
                         type == cam::OpType::SurfaceWaterline;
  j.doc = rotary ? BarDoc() : surfacing ? DomedDoc() : PartDoc();
  j.setup = BasicSetup();
  // A 10 mm cutter, not the fixture's 6. Nothing here is about the tool, and
  // the clearing strategies cost passes: at 6 mm the adaptive cases alone put
  // this case past seven minutes.
  j.setup.tools[0].diameter = 10.0;
  if (surfacing || rotary)
    j.setup.tools[0] = ShapedTool(cam::ToolType::Ball, 10.0);
  if (rotary) {
    j.setup.machine.hasRotaryAxis = true;
    j.setup.safeZ = 40.0;
  }
  if (type == cam::OpType::EdgeRound)
    j.setup.tools[0] = ShapedTool(cam::ToolType::Ball, 6.0);
  ElementRef target;
  switch (type) {
  case cam::OpType::Profile:
  case cam::OpType::EdgeRound:
    target = WallNear(*j.doc, 0.0, 22.0); // the block's own outside wall
    break;
  case cam::OpType::Drill:
    target = BoreWall(*j.doc);
    break;
  case cam::OpType::Face:
    target = UpFaceAt(*j.doc, 20.0); // the top
    break;
  case cam::OpType::SurfaceParallel:
  case cam::OpType::SurfaceWaterline:
    target = DomeFace(*j.doc);
    break;
  case cam::OpType::Pocket:
  case cam::OpType::Adaptive:
    target = PocketFloorRef(*j.doc);
    break;
  case cam::OpType::SurfaceRotary:
    break; // it takes NO pick: the whole part turns under the cutter
  case cam::OpType::Count:
    break;
  }
  const cam::DerivedOp d = cam::DeriveOperation(j.setup, j.doc->GetBodies(),
                                                type, cam::Target{0, target});
  INFO("deriving a " << cam::OpTypeNameOrThrow(type) << ": " << d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  j.setup.operations.push_back(op);
  return j;
}

} // namespace

// NOT tagged [slow], at 11.5 s of CPU -- which is the heaviest thing in the
// fast loop and is deliberate. This is a gate on a table maintained BY HAND,
// and the moment it needs to fire is while somebody is editing that table,
// which is exactly when they are running the fast loop. The bulk poke below
// is what bought the right to make that choice: per field it was 422 s.
TEST_CASE("cam: a generator reads only the fields its type declares", "[cam]") {
  // Both directions in ONE pass over the fixtures, and both of them poked in
  // BULK -- which is what makes this affordable.
  //
  // Forward: set every field the type does NOT declare, all at once. If the
  // path comes back identical, none of them was read; if it moves, the loop
  // below bisects by re-poking one field at a time and names the culprit. One
  // generate in the passing case instead of ten, which matters because
  // adaptive clearing costs about five seconds a call and took the per-field
  // version of this case to ninety.
  //
  // Backward, the non-vacuity control: set every field the type DOES declare
  // and require the path to move. Otherwise the forward half would pass just
  // as well against an inert fixture and would be measuring nothing at all.
  //
  // The control is per TYPE and not per claim, deliberately. Requiring EVERY
  // claim to bite would be a stronger statement and a false one, because
  // whether a claim bites depends on the geometry in front of it:
  // `drilled_bores` on a profile of an outside wall has no bores to leave
  // alone, and `peck` on a hole wide enough to be milled instead of drilled
  // has nothing to peck. Both are the right answer rather than a table that
  // over-claims. What this has to establish is that each type's path is LIVE
  // and sensitive to its own inputs, and the bulk poke says so.
  for (const cam::OpTypeRow& row : cam::OpTypes()) {
    TypedJob j = JobFor(row.type);
    const std::string base =
        Fingerprint(cam::GenerateAll(j.setup, j.doc->GetBodies()));
    INFO("type " << row.name);
    // A fixture that generates nothing passes every equality below without
    // testing one thing, which is the failure this whole file is about.
    REQUIRE(base.size() > 200);

    const auto pokeAll = [&](bool claimed) {
      cam::Setup poked = j.setup;
      for (const FieldPoke& p : kPokes)
        if (cam::OpTypeHas(row.type, p.field) == claimed)
          p.poke(poked.operations[0]);
      return Fingerprint(cam::GenerateAll(poked, j.doc->GetBodies()));
    };

    if (pokeAll(/*claimed=*/false) != base) {
      // Something undeclared is being read. Find out which, one at a time --
      // this path is slow and only runs when the test is already failing.
      for (const FieldPoke& p : kPokes) {
        if (cam::OpTypeHas(row.type, p.field))
          continue;
        cam::Setup poked = j.setup;
        p.poke(poked.operations[0]);
        INFO("a " << row.name << " does not declare `" << p.name
                  << "`, so setting it must change nothing");
        CHECK(Fingerprint(cam::GenerateAll(poked, j.doc->GetBodies())) == base);
      }
      // ...and fail even if the bisection cannot reproduce it singly, which
      // would mean two undeclared fields that only matter together.
      INFO("setting every field a " << row.name
                                    << " does not declare moved its path");
      CHECK(false);
    }

    INFO("setting every field a " << row.name
                                  << " DOES declare must move its path, or "
                                     "this fixture is not testing anything");
    CHECK(pokeAll(/*claimed=*/true) != base);
  }
}

#endif // HAVE_CAM
