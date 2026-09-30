#ifdef HAVE_CAM

// Engraving (docs/cam/operations.md#engrave): the tool's axis ON sketch
// curves, down from the surface the sketch lies on, and a verifier that knows
// the groove was cut on purpose -- and nothing under it or beside it.

#include "cam_fixtures.h"
#include "core/cam/CamEngrave.h"

namespace {

// A 40 x 30 x 20 block, its top at z = 20, and a sketch ON that top.
struct Engraved {
  std::unique_ptr<Document> doc;
  uint64_t sketch = 0;
  cam::Setup setup;
};

Engraved EngravedBlock(const std::function<void(SketchFeature&)>& draw,
                       const SketchPlane& plane = SketchPlane(
                           PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f))) {
  Engraved e;
  e.doc = BlockDoc();
  auto sk = std::make_unique<SketchFeature>("Label", plane);
  draw(*sk);
  e.doc->AddFeature(std::move(sk));
  e.sketch = e.doc->GetFeature(1)->GetId();
  e.setup = BasicSetup();
  e.setup.tools[0] = ShapedTool(cam::ToolType::VBit, 6.0, 90.0);
  // The stock's top IS the surface engraved: nothing to face first.
  e.setup.stock.margin.z = 0.0;
  return e;
}

cam::Target TargetOf(const Engraved& e, uint32_t entity) {
  return cam::Target{0, ElementRef{e.sketch, "g" + std::to_string(entity)}};
}

std::vector<cam::SketchCurves>
CurvesOf(const Engraved& e, const std::vector<cam::Target>& targets) {
  std::vector<cam::SketchCurves> out;
  for (const cam::Target& t : targets)
    out.push_back(cam::SketchEntityCurves(*e.doc, t.ref));
  return out;
}

// Derive for the first target, then cut every target -- the same op a
// caller of add_cam_op gets.
cam::Operation Derived(const Engraved& e,
                       const std::vector<cam::Target>& targets) {
  const cam::DerivedOp d =
      cam::DeriveOperation(e.setup, e.doc->GetBodies(), cam::OpType::Engrave,
                           targets.front(), 0, {}, CurvesOf(e, targets));
  INFO("derivation: " << d.why);
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 1;
  op.targets = targets;
  return op;
}

cam::Toolpath Cut(const Engraved& e, const cam::Operation& op) {
  cam::GenerateContext ctx;
  ctx.sketchCurves = CurvesOf(e, op.targets);
  return cam::Generate(e.setup, op, e.doc->GetBodies(), &ctx);
}

double DistToSegment(glm::dvec2 p, glm::dvec2 a, glm::dvec2 b) {
  const glm::dvec2 ab = b - a;
  const double t = std::clamp(
      glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-18), 0.0, 1.0);
  return glm::length(p - (a + ab * t));
}

} // namespace

TEST_CASE("cam: a V-bit engraving follows sketch lines and arcs at the depth "
          "its width derives",
          "[cam]") {
  uint32_t line = 0, arc = 0;
  Engraved e = EngravedBlock([&](SketchFeature& sk) {
    line = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {35.0f, 5.0f}});
    arc = sk.AddEntity(sketch::Arc{{20.0f, 17.0f}, 6.0, 0.0, 3.14159265});
  });
  const cam::Operation op = Derived(e, {TargetOf(e, line), TargetOf(e, arc)});
  const cam::Tool& tool = e.setup.tools[0];
  // The default groove is a quarter of the cutter wide, never a literal, and
  // its depth is the verifier's own TipHeight of half of it.
  CHECK_THAT(op.engraveWidth, WithinAbs(tool.diameter / 4.0, 1e-12));
  const double depth = cam::TipHeight(tool, op.engraveWidth * 0.5);
  CHECK_THAT(op.topZ, WithinAbs(20.0, 1e-6));
  CHECK_THAT(op.bottomZ, WithinAbs(20.0 - depth, 1e-6));

  const cam::Toolpath tp = Cut(e, op);
  INFO("error: " << tp.error);
  REQUIRE(tp.error.empty());
  REQUIRE(tp.skipped.empty());
  // Every cut is at the groove's depth and ON one of the two curves.
  int cuts = 0;
  for (const cam::Move& m : tp.moves) {
    if (m.kind != cam::Move::Kind::Cut)
      continue;
    cuts++;
    CHECK_THAT(m.to.z, WithinAbs(20.0 - depth, 1e-6));
    const glm::dvec2 p(m.to);
    const double onLine = DistToSegment(p, {5.0, 5.0}, {35.0, 5.0});
    const double onArc =
        std::abs(glm::length(p - glm::dvec2(20.0, 17.0)) - 6.0);
    CHECK(std::min(onLine, onArc) < 1e-4);
  }
  CHECK(cuts > 10); // the arc is many chords, the line one
  // Straight down, at the plunge feed, onto each stroke.
  int plunges = 0;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Plunge) {
      plunges++;
      CHECK_THAT(m.to.z, WithinAbs(20.0 - depth, 1e-6));
      CHECK_THAT(m.feed, WithinAbs(tool.feedPlunge, 1e-9));
    }
  CHECK(plunges == 2);
  CHECK(tp.grooves.size() == 2);

  // ...and it posts: the file carries the groove's depth.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", e.setup, {tp}, {}, gcode, &error));
  char z[32];
  std::snprintf(z, sizeof(z), "%.4f", 20.0 - depth - e.setup.origin.z);
  std::string word = z;
  while (word.back() == '0')
    word.pop_back(); // the post trims trailing zeros
  INFO("looking for Z" << word);
  CHECK(gcode.find("Z" + word) != std::string::npos);
}

TEST_CASE("cam: an engraving's depth from its width is TipHeight and a width "
          "past the cone is refused",
          "[cam]") {
  const cam::Tool vbit = ShapedTool(cam::ToolType::VBit, 6.0, 60.0);
  cam::Operation op;
  op.type = cam::OpType::Engrave;
  for (const double w : {0.5, 1.0, 2.0, 6.0}) {
    op.engraveWidth = w;
    op.engraveDepth = 0.0;
    const cam::EngraveGroove g = cam::EngraveGrooveFor(op, vbit);
    INFO("width " << w << ": " << g.why);
    REQUIRE(g.ok);
    CHECK_THAT(g.depth, WithinAbs(cam::TipHeight(vbit, w * 0.5), 1e-12));
    // ...and the depth stated instead gives the width back.
    op.engraveWidth = 0.0;
    op.engraveDepth = g.depth;
    const cam::EngraveGroove back = cam::EngraveGrooveFor(op, vbit);
    REQUIRE(back.ok);
    CHECK_THAT(back.width, WithinAbs(w, 1e-9));
  }
  // Wider than the cone ends: refused, naming both.
  op.engraveWidth = 7.0;
  op.engraveDepth = 0.0;
  const cam::EngraveGroove wide = cam::EngraveGrooveFor(op, vbit);
  CHECK_FALSE(wide.ok);
  CHECK_THAT(wide.why, Catch::Matchers::ContainsSubstring("wider than"));
  // Deeper than the cone is tall: the same groove said the other way.
  op.engraveWidth = 0.0;
  op.engraveDepth = cam::TipHeight(vbit, 3.0) + 0.5;
  CHECK_FALSE(cam::EngraveGrooveFor(op, vbit).ok);
  // Two statements that disagree are refused, not reconciled.
  op.engraveWidth = 1.0;
  op.engraveDepth = 0.2;
  CHECK_FALSE(cam::EngraveGrooveFor(op, vbit).ok);
  // A flat takes a depth; a width is a V-bit's.
  const cam::Tool flat = ShapedTool(cam::ToolType::Flat, 2.0);
  op.engraveWidth = 1.0;
  op.engraveDepth = 0.0;
  CHECK_FALSE(cam::EngraveGrooveFor(op, flat).ok);
  op.engraveWidth = 0.0;
  op.engraveDepth = 0.4;
  const cam::EngraveGroove slot = cam::EngraveGrooveFor(op, flat);
  REQUIRE(slot.ok);
  CHECK_THAT(slot.width, WithinAbs(2.0, 1e-12));
  // A drill is not an engraving tool.
  CHECK_FALSE(
      cam::EngraveGrooveFor(op, ShapedTool(cam::ToolType::Drill, 3.0, 118.0))
          .ok);

  // The generator says the same as the table, in the op's own error.
  uint32_t line = 0;
  Engraved e = EngravedBlock([&](SketchFeature& sk) {
    line = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {35.0f, 5.0f}});
  });
  cam::Operation cut = Derived(e, {TargetOf(e, line)});
  cut.engraveWidth = 6.5;
  const cam::Toolpath tp = Cut(e, cut);
  CHECK(tp.moves.empty());
  CHECK_THAT(tp.error, Catch::Matchers::ContainsSubstring("wider than"));
}

TEST_CASE("cam: an engraving reverses a stroke where that travels less than "
          "the drawn order",
          "[cam]") {
  // Two lines drawn the same way, left to right. Drawn order hops back
  // across the whole width; the engraving takes the second one from its
  // right end.
  uint32_t a = 0, b = 0;
  Engraved e = EngravedBlock([&](SketchFeature& sk) {
    a = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {35.0f, 5.0f}});
    b = sk.AddEntity(sketch::Line{{5.0f, 12.0f}, {35.0f, 12.0f}});
  });
  const cam::Operation op = Derived(e, {TargetOf(e, a), TargetOf(e, b)});
  const cam::Toolpath tp = Cut(e, op);
  INFO("error: " << tp.error);
  REQUIRE(tp.error.empty());
  std::vector<glm::dvec2> plunges;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Plunge)
      plunges.emplace_back(m.to);
  REQUIRE(plunges.size() == 2);
  CHECK(glm::length(plunges[0] - glm::dvec2(5.0, 5.0)) < 1e-6);
  CHECK(glm::length(plunges[1] - glm::dvec2(35.0, 12.0)) < 1e-6);

  // The order itself, against the fixed-order control.
  const std::vector<std::vector<glm::dvec2>> strokes = {
      {{5.0, 5.0}, {35.0, 5.0}},
      {{5.0, 12.0}, {35.0, 12.0}},
      {{5.0, 19.0}, {35.0, 19.0}}};
  const std::vector<bool> open(3, false);
  const glm::dvec2 from(5.0, 5.0);
  const double drawn =
      cam::StrokeTravel(cam::OrderStrokes(strokes, open, from, false), from);
  const std::vector<cam::StrokeRun> best =
      cam::OrderStrokes(strokes, open, from, true);
  CHECK(cam::StrokeTravel(best, from) < drawn - 40.0);
  REQUIRE(best.size() == 3);
  CHECK_FALSE(best[0].reversed);
  CHECK(best[1].reversed);
  CHECK_FALSE(best[2].reversed);

  // A stroke that starts where the last one ended keeps the tool down: an L
  // drawn as two lines is one plunge.
  uint32_t l1 = 0;
  Engraved corner = EngravedBlock([&](SketchFeature& sk) {
    l1 = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {20.0f, 5.0f}});
  });
  // Two strokes of ONE label, as a stroke font draws a letter.
  const std::vector<std::vector<glm::dvec2>> ell = {{{20.0, 5.0}, {20.0, 20.0}},
                                                    {{5.0, 5.0}, {20.0, 5.0}}};
  const std::vector<cam::StrokeRun> runs =
      cam::OrderStrokes(ell, {false, false}, {5.0, 5.0}, true);
  REQUIRE(runs.size() == 2);
  CHECK(glm::length(runs[0].pts.back() - runs[1].pts.front()) < 1e-9);
  (void)l1;
}

TEST_CASE("cam: verify is clean on an engraving and flags one cut deeper than "
          "its groove",
          "[cam]") {
  uint32_t line = 0, arc = 0;
  Engraved e = EngravedBlock([&](SketchFeature& sk) {
    line = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {35.0f, 5.0f}});
    arc = sk.AddEntity(sketch::Arc{{20.0f, 17.0f}, 6.0, 0.0, 3.14159265});
  });
  const cam::Operation op = Derived(e, {TargetOf(e, line), TargetOf(e, arc)});
  const cam::Toolpath tp = Cut(e, op);
  REQUIRE(tp.error.empty());
  const double res = 0.1, tol = res * 1.5;
  const auto verify = [&](const cam::Toolpath& path,
                          const std::vector<cam::Toolpath::Groove>& grooves) {
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", e.setup, {path}, {}, gcode, &error));
    return cam::VerifyGcode(gcode, e.setup, e.doc->GetBodies(), res, {}, {}, {},
                            nullptr, grooves);
  };

  // As declared: the grooves are engraving, not gouge.
  const cam::Verdict clean = verify(tp, tp.grooves);
  INFO("gouge " << clean.maxGouge << " at " << clean.gougeAt.x << ","
                << clean.gougeAt.y << "; shallow " << clean.grooveShallow);
  CHECK(clean.Safe(tol));
  CHECK(clean.Complete(tol));
  CHECK(clean.grooves == 2);
  CHECK(clean.grooveShallow <= tol);
  // The control: the same program, not told, is scrap by its own depth.
  const cam::Verdict untold = verify(tp, {});
  CHECK(untold.maxGouge > op.topZ - op.bottomZ - tol);

  // Half a millimetre deeper than it declared: that half is gouge.
  cam::Toolpath deep = tp;
  for (cam::Move& m : deep.moves)
    if (m.kind != cam::Move::Kind::Rapid)
      m.to.z -= 0.5;
  const cam::Verdict deeper = verify(deep, tp.grooves);
  CHECK_FALSE(deeper.Safe(tol));
  CHECK_THAT(deeper.maxGouge, WithinAbs(0.5, tol));

  // A millimetre to one side of what it declared: a cut beside the groove.
  cam::Toolpath beside = tp;
  for (cam::Move& m : beside.moves)
    m.to.y += 1.0;
  CHECK_FALSE(verify(beside, tp.grooves).Safe(tol));

  // Half as deep as it declared: safe, and not complete -- the engraving is
  // shallow and the verdict says where.
  cam::Toolpath shallow = tp;
  const double half = (op.topZ - op.bottomZ) * 0.5;
  for (cam::Move& m : shallow.moves)
    if (m.kind != cam::Move::Kind::Rapid)
      m.to.z += half;
  const cam::Verdict light = verify(shallow, tp.grooves);
  CHECK(light.Safe(tol));
  CHECK_THAT(light.grooveShallow, WithinAbs(half, tol));
  CHECK_FALSE(light.Complete(tol));

  // A groove under three cells of the check is said to be one.
  const cam::Verdict coarse = [&] {
    std::string gcode, error;
    REQUIRE(cam::PostProcess("mach3", e.setup, {tp}, {}, gcode, &error));
    return cam::VerifyGcode(gcode, e.setup, e.doc->GetBodies(), 1.0, {}, {}, {},
                            nullptr, tp.grooves);
  }();
  CHECK(coarse.narrowGrooves == 2);
}

TEST_CASE("cam: a single-stroke text label engraves as one operation and "
          "verifies clean",
          "[cam]") {
  // The board's files, in the stroke font: every pen-down stroke of every
  // glyph is one groove, and the label is one target.
  uint32_t label = 0;
  Engraved e = EngravedBlock([&](SketchFeature& sk) {
    sketch::Text t;
    t.anchor = {4.0f, 10.0f};
    t.text = "abcd";
    t.font = "hershey";
    t.height = 8.0;
    label = sk.AddEntity(t);
  });
  const cam::SketchCurves curves =
      cam::SketchEntityCurves(*e.doc, TargetOf(e, label).ref);
  INFO("resolver: " << curves.why);
  REQUIRE(curves.why.empty());
  CHECK(curves.strokes.size() >= 4); // at least a stroke a letter
  CHECK_THAT(curves.textHeight, WithinAbs(8.0, 1e-12));

  const cam::Operation op = Derived(e, {TargetOf(e, label)});
  // The groove is the label's cap height / 8, derived rather than typed.
  CHECK_THAT(op.engraveWidth, WithinAbs(1.0, 1e-12));
  const cam::Toolpath tp = Cut(e, op);
  INFO("error: " << tp.error);
  REQUIRE(tp.error.empty());
  CHECK(tp.grooves.size() == curves.strokes.size());
  // Between strokes of one label the tool hops just over the surface, never
  // to safe Z: the only rapids at safe Z are the way in and the way out.
  int atSafe = 0;
  for (const cam::Move& m : tp.moves)
    if (m.kind == cam::Move::Kind::Rapid &&
        std::abs(m.to.z - e.setup.safeZ) < 1e-9)
      atSafe++;
  CHECK(atSafe == 2);

  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", e.setup, {tp}, {}, gcode, &error));
  const double res = 0.1, tol = res * 1.5;
  const cam::Verdict v = cam::VerifyGcode(gcode, e.setup, e.doc->GetBodies(),
                                          res, {}, {}, {}, nullptr, tp.grooves);
  INFO("gouge " << v.maxGouge << " at " << v.gougeAt.x << "," << v.gougeAt.y
                << "; shallow " << v.grooveShallow << "; rapids "
                << v.rapidsThroughStock);
  CHECK(v.Safe(tol));
  CHECK(v.Complete(tol));
  CHECK(v.grooves == static_cast<int>(tp.grooves.size()));
  CHECK(v.narrowGrooves == 0);

  // A label that cannot be laid out is refused with the layout's reason.
  uint32_t bad = 0;
  Engraved odd = EngravedBlock([&](SketchFeature& sk) {
    sketch::Text t;
    t.anchor = {4.0f, 10.0f};
    t.text = "ab";
    t.font = "no-such-font";
    t.height = 8.0;
    bad = sk.AddEntity(t);
  });
  CHECK_THAT(cam::SketchEntityCurves(*odd.doc, TargetOf(odd, bad).ref).why,
             Catch::Matchers::ContainsSubstring("laid out"));
}

TEST_CASE("cam: an engraving refuses a sketch whose plane does not face the "
          "spindle or is off the surface",
          "[cam]") {
  // On the block's back face (XZ at y = 30, its normal +Y): sideways.
  uint32_t side = 0;
  Engraved e = EngravedBlock(
      [&](SketchFeature& sk) {
        side = sk.AddEntity(sketch::Line{{5.0f, -5.0f}, {35.0f, -5.0f}});
      },
      SketchPlane(PlaneType::XZ, glm::vec3(0.0f, 30.0f, 0.0f)));
  const cam::DerivedOp d = cam::DeriveOperation(
      e.setup, e.doc->GetBodies(), cam::OpType::Engrave, TargetOf(e, side), 0,
      {}, CurvesOf(e, {TargetOf(e, side)}));
  CHECK_FALSE(d.ok);
  CHECK_THAT(d.why, Catch::Matchers::ContainsSubstring("90.00 degrees"));

  // ...and cut at the index that turns it up, it is cut there and only
  // there: of four, the one that takes +Y to +Z (model to setup is R_x(-A)).
  RotaryMachine(e.setup);
  const double a = 270.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Engrave;
  op.toolNumber = 1;
  op.targets = {TargetOf(e, side)};
  op.engraveWidth = 1.0;
  op.topZ = 1.0;
  op.bottomZ = 0.0;
  op.indexA = {0.0, 90.0, 180.0, 270.0};
  const cam::Toolpath turned = Cut(e, op);
  INFO("error: " << turned.error);
  REQUIRE(turned.error.empty());
  CHECK(turned.skipped.empty());
  for (const cam::Move& m : turned.moves) {
    CHECK(m.hasA);
    CHECK_THAT(m.a, WithinAbs(a, 1e-9));
  }

  // Sketched in the air above the part: refused, not air-cut.
  uint32_t high = 0;
  Engraved air = EngravedBlock(
      [&](SketchFeature& sk) {
        high = sk.AddEntity(sketch::Line{{5.0f, 5.0f}, {35.0f, 5.0f}});
      },
      SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 25.0f)));
  const cam::DerivedOp off = cam::DeriveOperation(
      air.setup, air.doc->GetBodies(), cam::OpType::Engrave,
      TargetOf(air, high), 0, {}, CurvesOf(air, {TargetOf(air, high)}));
  CHECK_FALSE(off.ok);
  CHECK_THAT(off.why, Catch::Matchers::ContainsSubstring("nothing under it"));

  // A construction line and a point are not curves to cut; a face is not a
  // sketch entity.
  uint32_t guide = 0, dot = 0;
  Engraved odd = EngravedBlock([&](SketchFeature& sk) {
    sketch::Line l{{5.0f, 5.0f}, {35.0f, 5.0f}};
    l.construction = true;
    guide = sk.AddEntity(l);
    dot = sk.AddEntity(sketch::Point{{10.0f, 10.0f}});
  });
  CHECK_THAT(cam::SketchEntityCurves(*odd.doc, TargetOf(odd, guide).ref).why,
             Catch::Matchers::ContainsSubstring("construction"));
  CHECK_THAT(cam::SketchEntityCurves(*odd.doc, TargetOf(odd, dot).ref).why,
             Catch::Matchers::ContainsSubstring("point"));
  CHECK_THAT(
      cam::SketchEntityCurves(*odd.doc, ElementRef{odd.sketch, "Face3"}).why,
      Catch::Matchers::ContainsSubstring("g<entity id>"));
  // A generator handed no curves refuses rather than cutting nothing quietly.
  cam::Operation lost = op;
  lost.indexA.clear();
  const cam::Toolpath unresolved =
      cam::Generate(odd.setup, lost, odd.doc->GetBodies());
  CHECK_THAT(unresolved.error, Catch::Matchers::ContainsSubstring("resolved"));
}

TEST_CASE("cam: the job tab offers the sketches that mark, not the ones that "
          "outline",
          "[cam]") {
  // docs/cam/gui.md#engrave-from-the-viewport: a sketch holding a label or
  // an open curve is a marking, and every cuttable entity in it is offered;
  // a sketch of closed profiles only is some feature's outline.
  auto doc = BlockDoc();
  auto outline = std::make_unique<SketchFeature>(
      "Outline", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f)));
  const glm::vec2 c[4] = {{2, 2}, {12, 2}, {12, 8}, {2, 8}};
  for (int i = 0; i < 4; i++)
    (void)outline->AddEntity(sketch::Line{c[i], c[(i + 1) % 4]});
  doc->AddFeature(std::move(outline));

  auto marks = std::make_unique<SketchFeature>(
      "Marks", SketchPlane(PlaneType::XY, glm::vec3(0.0f, 0.0f, 20.0f)));
  sketch::Text label;
  label.anchor = {4.0f, 10.0f};
  label.text = "a1";
  label.font = "hershey";
  label.height = 6.0;
  const uint32_t text = marks->AddEntity(label);
  const uint32_t ring = marks->AddEntity(sketch::Circle{{30, 20}, 3.0});
  const uint32_t tick = marks->AddEntity(sketch::Line{{30, 5}, {36, 5}});
  sketch::Line guide{{0, 0}, {40, 0}};
  guide.construction = true;
  (void)marks->AddEntity(guide);
  (void)marks->AddEntity(sketch::Point{{20, 20}});
  doc->AddFeature(std::move(marks));
  const uint64_t marksId = doc->GetFeature(2)->GetId();

  const std::vector<ElementRef> offered = cam::EngravableEntities(*doc);
  std::vector<std::string> names;
  for (const ElementRef& r : offered) {
    CHECK(r.featureId == marksId); // the outline sketch is not offered
    names.push_back(r.name);
  }
  // The label, the closed ring beside it and the open tick; never the
  // construction line or the point.
  CHECK(names == std::vector<std::string>{"g" + std::to_string(text),
                                          "g" + std::to_string(ring),
                                          "g" + std::to_string(tick)});
  // Every one resolves: what is offered can be cut.
  for (const ElementRef& r : offered)
    CHECK(cam::SketchEntityCurves(*doc, r).why.empty());
}

#endif // HAVE_CAM
