#ifdef HAVE_CAM

// The machine profile and the posted file: travel limits, dialects,
// feeds, and the G-code itself.
// One of five files split from test_cam.cpp
// (docs/architecture-review-2026-09-06.md, stage 1).

#include "cam_fixtures.h"

TEST_CASE("cam: a name with a bracket in it cannot nest a comment",
          "[cam][post]") {
  // G-code comments do not nest, and every comment the post writes carries a
  // name somebody typed: the setup's, a tool's, an operation's. One "(" in
  // any of them and Mach3 refuses the whole file with "Nested comment found"
  // -- after the job is on the machine and the stock is clamped.
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  setup.name = "Front (baffle side up)";
  setup.tools[0].name = "6 mm flat 2F (through cuts)";
  cam::Operation op;
  op.id = 1;
  op.name = "Cut the opening (roughing)";
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 8.0;
  op.side = cam::Side::Outside;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  for (const std::string& post : cam::PostIds()) {
    std::string gcode, error;
    INFO("post " << post);
    REQUIRE(cam::PostProcess(post, setup, paths, {}, gcode, &error));
    // Every line: at most one "(" and it closes before the next one opens.
    size_t line = 1;
    int depth = 0;
    for (const char c : gcode) {
      if (c == '\n') {
        INFO("line " << line);
        CHECK(depth == 0); // a comment that never closed
        line++;
      } else if (c == '(') {
        INFO("line " << line);
        CHECK(depth == 0); // <-- the nested comment
        depth++;
      } else if (c == ')') {
        depth--;
      }
    }
    // ...and the names are still legible, just bracketed.
    CHECK(gcode.find("Front [baffle side up]") != std::string::npos);
    CHECK(gcode.find("6 mm flat 2F [through cuts]") != std::string::npos);
  }
}

TEST_CASE("cam: the post emits a program that starts and ends safely",
          "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 18.0;
  setup.operations.push_back(op);

  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));

  // The preamble puts the machine in a known state — the absence of this is
  // how a job runs in inches. G91.1 is there because the post fits arcs, and
  // an arc centre read in the wrong mode is a full-radius swing across the
  // table (C7).
  CHECK(gcode.find("G90 G91.1 G21 G17 G40 G49 G80") != std::string::npos);
  CHECK(gcode.find("G54") != std::string::npos);
  CHECK(gcode.find("T1 M6") != std::string::npos);
  CHECK(gcode.find("S12000 M3") != std::string::npos);
  // Mach3 has no tool table: the post pauses for a manual change.
  CHECK(gcode.find("M0 (change tool") != std::string::npos);
  CHECK(gcode.find("M5") != std::string::npos);
  CHECK(gcode.find("M30") != std::string::npos);
  // Feed appears on cutting moves, and modally — not on every line.
  CHECK(gcode.find("F600") != std::string::npos);
  CHECK(gcode.find("F200") != std::string::npos);

  // No coordinate is ever "nan". The first retract happens before anything
  // has moved, and passing the modal X/Y through emitted "G0 Xnan Ynan Z25"
  // — a line a controller may reject, or worse, interpret. Caught by reading
  // a posted file, so it is a test now.
  CHECK(gcode.find("nan") == std::string::npos);
  CHECK(gcode.find("inf") == std::string::npos);

  // LinuxCNC differs where it should and nowhere else. (`linux` is a
  // predefined GCC macro, hence the name.)
  std::string lcnc;
  REQUIRE(cam::PostProcess("linuxcnc", setup, paths, {}, lcnc, &error));
  CHECK(lcnc.find("M2") != std::string::npos);
  CHECK(lcnc.find("M0 (change tool") == std::string::npos);

  // GRBL (docs/cam/router-round.md, R1): no M6 -- GRBL ignores it unless
  // built for it, and an inline "M6 T2" runs on into the next cut with the
  // wrong tool -- so the change is M5, a message naming the tool, M0 (a feed
  // hold every GRBL honours), then the spindle again. No G43 either. The
  // cut itself is the same file.
  std::string grbl;
  REQUIRE(cam::PostProcess("grbl", setup, paths, {}, grbl, &error));
  CHECK(grbl.find("M6") == std::string::npos);
  CHECK(grbl.find("G43") == std::string::npos);
  const size_t stop = grbl.find("M5");
  const size_t msg = grbl.find("(change to T1");
  const size_t hold = grbl.find("\nM0\n");
  const size_t spindle = grbl.find("S12000 M3");
  REQUIRE(stop != std::string::npos);
  REQUIRE(msg != std::string::npos);
  REQUIRE(hold != std::string::npos);
  REQUIRE(spindle != std::string::npos);
  CHECK(stop < msg);
  CHECK(msg < hold);
  CHECK(hold < spindle);
  CHECK(grbl.find("M2\n") != std::string::npos);
  CHECK(grbl.find("G90 G91.1 G21 G17 G40 G49 G80") != std::string::npos);
  // The same cut: the verifier reads both files to the same result.
  const cam::Verdict a = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.5);
  const cam::Verdict b = cam::VerifyGcode(grbl, setup, doc->GetBodies(), 0.5);
  CHECK_THAT(a.maxGouge, WithinAbs(b.maxGouge, 1e-9));
  CHECK_THAT(a.maxLeftover, WithinAbs(b.maxLeftover, 1e-9));

  // ...and the grbl file SAYS which cutter it is running, T word and all.
  // Without it a reader has to assume tool 1: see the two-tool case below.
  CHECK(grbl.find("\nT1\n") != std::string::npos);

  // An unknown post names the ones that exist rather than failing blankly.
  std::string none;
  CHECK_FALSE(cam::PostProcess("haas", setup, paths, {}, none, &error));
  CHECK(error.find("mach3") != std::string::npos);
  CHECK(error.find("grbl") != std::string::npos);
}

TEST_CASE("cam: a failed operation is skipped LOUDLY in the file", "[cam]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Toolpath bad;
  bad.operationId = 9;
  bad.toolNumber = 1;
  bad.error = "targets vanished";
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, {bad}, {}, gcode, &error));
  // A part that comes off the machine missing a feature nobody noticed is
  // the failure this prevents.
  CHECK(gcode.find("SKIPPED op 9: targets vanished") != std::string::npos);
}

TEST_CASE("cam: the spindle is restated after a manual tool change", "[cam]") {
  // Two tools at the same rpm: the S/M3 was emitted only when the SPEED
  // changed, so the file paused for a manual tool change — which is done with
  // the spindle stopped — and the next line was a cutting move. Found by
  // reading the coupon's own file (docs/cam/dogfood-round.md).
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Tool second = setup.tools.front();
  second.number = 2;
  second.diameter = 3.0;
  setup.tools.push_back(second); // same spindleRpm as tool 1
  std::vector<cam::Toolpath> paths;
  for (int t : {1, 2}) {
    cam::Toolpath tp;
    tp.operationId = static_cast<uint64_t>(t);
    tp.toolNumber = t;
    tp.moves = {{cam::Move::Kind::Rapid, {0.0, 0.0, 25.0}, false, 0.0, 0.0},
                {cam::Move::Kind::Cut, {10.0, 0.0, 5.0}, false, 0.0, 600.0}};
    paths.push_back(tp);
  }
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  const size_t pause = gcode.find("M0 (change tool", gcode.find("T2 M6"));
  REQUIRE(pause != std::string::npos);
  INFO(gcode.substr(pause, 80));
  CHECK(gcode.find("M3", pause) != std::string::npos);
  // …and before the first move of the new tool, not somewhere after it.
  CHECK(gcode.find("M3", pause) < gcode.find("G1", pause));
}

// --- C4: frozen G-code, per dialect --------------------------------------

namespace {

// The job the goldens freeze: one part, one tool, two operations, chosen to
// exercise a tool change comment, a tabbed profile and a facing spiral.
cam::Setup GoldenJob() {
  cam::Setup setup = BasicSetup();
  setup.name = "Golden";
  setup.safeZ = 20.0;
  cam::Operation face;
  face.id = 1;
  face.name = "Face";
  face.type = cam::OpType::Face;
  face.toolNumber = 1;
  face.topZ = 21.0;
  face.bottomZ = 20.0;
  face.stepover = 0.5;
  setup.operations.push_back(face);
  cam::Operation profile;
  profile.id = 2;
  profile.name = "Profile";
  profile.type = cam::OpType::Profile;
  profile.toolNumber = 1;
  profile.topZ = 20.0;
  profile.bottomZ = 0.0;
  profile.stepdown = 8.0;
  profile.tabCount = 4;
  setup.operations.push_back(profile);
  return setup;
}

} // namespace

TEST_CASE("cam: the emitted program is frozen, per dialect", "[cam][golden]") {
  // Substring checks prove the pieces are present; only a byte-for-byte
  // golden proves the FILE stopped changing. Regenerate deliberately with
  // DESIGNER_REFRESH_CAM_GOLDEN=1 and read the diff before committing it —
  // every line of this file eventually moves a machine.
  auto doc = BlockDoc(40, 30, 20);
  const cam::Setup setup = GoldenJob();
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());

  for (const std::string& post : cam::PostIds()) {
    std::string gcode, error;
    REQUIRE(cam::PostProcess(post, setup, paths, {}, gcode, &error));
    const std::filesystem::path golden =
        std::filesystem::path(DESIGNER_SOURCE_DIR) / "tests" / "golden" /
        ("cam-block-" + post + ".nc");

    if (std::getenv("DESIGNER_REFRESH_CAM_GOLDEN") != nullptr) {
      std::ofstream(golden) << gcode;
      continue;
    }
    INFO("golden: " << golden.string());
    REQUIRE(std::filesystem::exists(golden));
    std::ifstream in(golden);
    const std::string want((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // Point at the first differing line rather than dumping two programs.
    size_t line = 1, at = 0;
    while (at < want.size() && at < gcode.size() && want[at] == gcode[at]) {
      if (want[at] == '\n')
        line++;
      at++;
    }
    INFO("first difference at line " << line);
    CHECK(gcode == want);
  }
}

// ---------------------------------------------------------------------------
// The machine profile (docs/cam/job-document.md, round two; format v77).

TEST_CASE("cam machine: JSON round-trips, and an unknown post is refused",
          "[cam][machine]") {
  cam::Machine m;
  m.name = "Shop router";
  m.travelMin = {0.0, 0.0, -100.0};
  m.travelMax = {1200.0, 800.0, 0.0};
  m.post = "mach3";
  m.fitArcs = false;
  m.hasRotaryAxis = true;
  m.rapidFeed = 3000.0;
  cam::Tool t;
  t.number = 2;
  t.diameter = 3.0;
  m.library.push_back(t);
  const auto j = DocumentIO::MachineToJson(m);
  const cam::Machine back = DocumentIO::MachineFromJson(j);
  CHECK(back == m);
  // Defaults are omitted, so the job's own default machine is a small block.
  const auto small = DocumentIO::MachineToJson(cam::Machine{});
  CHECK(!small.contains("travel"));
  CHECK(!small.contains("fit_arcs"));
  CHECK(!small.contains("tools"));
  CHECK(DocumentIO::MachineFromJson(small) == cam::Machine{});
  // Refused, naming what is accepted: a silent mach3 posts for the wrong
  // controller with nothing to say so.
  nlohmann::json bad = j;
  bad["post"] = "haas";
  CHECK_THROWS_WITH(DocumentIO::MachineFromJson(bad),
                    Catch::Matchers::ContainsSubstring("haas") &&
                        Catch::Matchers::ContainsSubstring("mach3"));
}

TEST_CASE("cam machine: a setup embeds it, and a v76 file still reads",
          "[cam][machine][job]") {
  cam::Setup setup;
  setup.name = "Sheet";
  setup.machine.name = "Shop router";
  setup.machine.post = "mach3";
  setup.machine.hasRotaryAxis = true;
  setup.machine.travelMax = {600.0, 400.0, 80.0};
  const auto j = DocumentIO::CamSetupToJson(setup);
  REQUIRE(j.contains("machine"));
  CHECK(!j.contains("post"));            // the three old keys are gone
  CHECK(!j.contains("has_rotary_axis")); // from the setup itself
  const cam::Setup back = DocumentIO::CamSetupFromJson(j);
  CHECK(back.machine == setup.machine);

  // The legacy shape: post/has_rotary_axis/fit_arcs as the setup's own keys,
  // read into an UNNAMED machine so the job cuts exactly as it did.
  const nlohmann::json legacy = {{"id", 3},
                                 {"name", "Old"},
                                 {"post", "mach3"},
                                 {"rotation_a", 90.0},
                                 {"fit_arcs", false}};
  const cam::Setup old = DocumentIO::CamSetupFromJson(legacy);
  CHECK(old.machine.name.empty());
  CHECK(old.machine.post == "mach3");
  CHECK(!old.machine.fitArcs);
  CHECK(old.machine.hasRotaryAxis); // implied by a rotary index, as before
  nlohmann::json badLegacy = legacy;
  badLegacy["post"] = "haas";
  CHECK_THROWS_WITH(DocumentIO::CamSetupFromJson(badLegacy),
                    Catch::Matchers::ContainsSubstring("haas"));
}

TEST_CASE("cam machine: the store saves, lists, refuses and deletes",
          "[cam][machine]") {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() /
                       ("designer-machines-" + std::to_string(::getpid()));
  fs::remove_all(dir);
  // No store yet is not an error: nothing has been saved.
  std::string problems = "x";
  CHECK(DocumentIO::LoadMachines(dir.string(), &problems).empty());
  CHECK(problems.empty());

  cam::Machine a;
  a.name = "Shapeoko 4";
  a.travelMax = {800.0, 800.0, 100.0};
  cam::Machine b;
  b.name = "Big router";
  b.post = "linuxcnc";
  std::string error;
  REQUIRE(DocumentIO::SaveMachine(dir.string(), a, &error));
  REQUIRE(DocumentIO::SaveMachine(dir.string(), b, &error));
  CHECK(fs::exists(dir / "shapeoko-4.json"));
  CHECK(fs::exists(dir / "big-router.json"));
  const auto loaded = DocumentIO::LoadMachines(dir.string(), &problems);
  REQUIRE(loaded.size() == 2);
  CHECK(loaded[0].name == "Big router"); // sorted by name
  CHECK(loaded[1] == a);

  // Refusals, each in words.
  cam::Machine unnamed;
  CHECK(!DocumentIO::SaveMachine(dir.string(), unnamed, &error));
  CHECK(error.find("name") != std::string::npos);
  cam::Machine collides;
  collides.name = "shapeoko_4"; // folds to the same file as "Shapeoko 4"
  CHECK(!DocumentIO::SaveMachine(dir.string(), collides, &error));
  CHECK(error.find("Shapeoko 4") != std::string::npos);
  cam::Machine badPost = a;
  badPost.post = "haas";
  CHECK(!DocumentIO::SaveMachine(dir.string(), badPost, &error));
  CHECK(error.find("haas") != std::string::npos);
  CHECK(!DocumentIO::DeleteMachine(dir.string(), "nobody", &error));
  CHECK(error.find("nobody") != std::string::npos);

  // One broken file does not hide the others: it is named, and the rest load.
  {
    std::ofstream out(dir / "broken.json");
    out << "{ \"name\": \"Broken\", \"post\": \"haas\" }";
  }
  const auto withBroken = DocumentIO::LoadMachines(dir.string(), &problems);
  CHECK(withBroken.size() == 2);
  CHECK(problems.find("broken.json") != std::string::npos);
  CHECK(problems.find("haas") != std::string::npos);

  REQUIRE(DocumentIO::DeleteMachine(dir.string(), "Shapeoko 4", &error));
  CHECK(!fs::exists(dir / "shapeoko-4.json"));
  fs::remove_all(dir);
}

TEST_CASE("cam machine: a setup's copy is matched against the store by name",
          "[cam][machine]") {
  cam::Machine global;
  global.name = "Shapeoko 4";
  global.travelMax = {800.0, 800.0, 100.0};
  const std::vector<cam::Machine> store = {global};

  cam::Machine own; // the job's own: nothing to compare against
  CHECK(cam::MatchMachine(store, own) == cam::MachineMatch::Unnamed);
  CHECK(cam::MatchMachine(store, global) == cam::MachineMatch::Global);
  cam::Machine edited = global;
  edited.travelMax.x = 600.0; // the bed got shorter on one side
  CHECK(cam::MatchMachine(store, edited) == cam::MachineMatch::Differs);
  cam::Machine elsewhere = global;
  elsewhere.name = "Someone else's";
  CHECK(cam::MatchMachine(store, elsewhere) == cam::MachineMatch::Missing);
  CHECK(cam::FindMachine(store, "") == nullptr);
  CHECK(cam::FindMachine(store, "Shapeoko 4") == &store[0]);

  // The slug: case and punctuation fold, so two spellings share a file --
  // which is what SaveMachine refuses on.
  CHECK(cam::MachineFileName("Shapeoko 4") == "shapeoko-4.json");
  CHECK(cam::MachineFileName("  Big -- Router! ") == "big-router.json");
  CHECK(cam::MachineFileName("") == "machine.json");
}

TEST_CASE("cam machine: a sheet longer than the travel is refused, in words",
          "[cam][machine][job]") {
  // A 900 x 400 sheet on a machine that travels 600 in X.
  Document doc;
  doc.AddFeature(std::make_unique<BoxFeature>("Panel", glm::vec3(0, 0, 0),
                                              880.0, 380.0, 18.0));
  cam::Setup setup;
  setup.stock.mode = cam::StockMode::Fixed;
  setup.stock.size = {900.0, 400.0, 18.0};
  setup.machine.name = "Small router";
  setup.machine.travelMin = {0.0, 0.0, -90.0};
  setup.machine.travelMax = {600.0, 600.0, 0.0};
  // The PARTS are what the spindle has to reach: 880 on a 600 machine.
  const std::string why = cam::MachineWarning(setup, doc.GetBodies());
  INFO(why);
  CHECK(why.find("X") != std::string::npos);
  CHECK(why.find("880") != std::string::npos);
  CHECK(why.find("600") != std::string::npos);
  CHECK(why.find("280") != std::string::npos); // the shortfall
  CHECK(why.find("Small router") != std::string::npos);
  CHECK(cam::MachineOverhang(setup, doc.GetBodies())
            .empty()); // warned, not noted
  // A WARNING, not a refusal: the job still generates, and the verdict
  // carries the sentence. The machine cuts as far as it reaches; refusing
  // the whole job refused the part the user wanted the first metre of.
  CHECK(cam::PlacementRefusal(setup, doc.GetBodies()).empty());
  const auto lines = cam::MachineLines(setup, doc.GetBodies());
  REQUIRE(lines.size() == 1);
  CHECK(lines[0].level == cam::VerdictLine::Level::Warn);
  CHECK(lines[0].text == why);
  // The machine is NOT grown to fit: what Kiri:Moto does, and the wrong
  // answer -- the picture stays right and the machine is wrong.
  CHECK(setup.machine.travelMax.x == 600.0);

  // A machine that states no work area checks nothing; one big enough fits.
  cam::Setup unstated = setup;
  unstated.machine.travelMax = unstated.machine.travelMin;
  CHECK(cam::MachineWarning(unstated, doc.GetBodies()).empty());
  setup.machine.travelMax.x = 1200.0;
  CHECK(cam::MachineWarning(setup, doc.GetBodies()).empty());

  // A sheet LONGER than the bed with parts that fit is not a refusal: an
  // open-ended router slides it through and cuts in tiles. It is a note.
  Document small;
  small.AddFeature(std::make_unique<BoxFeature>("Bracket", glm::vec3(0, 0, 0),
                                                300.0, 200.0, 18.0));
  cam::Setup tiled;
  tiled.stock.mode = cam::StockMode::Fixed;
  tiled.stock.size = {2440.0, 400.0, 18.0};
  tiled.machine.name = "Small router";
  tiled.machine.travelMin = {0.0, 0.0, -90.0};
  tiled.machine.travelMax = {600.0, 600.0, 0.0};
  CHECK(cam::MachineWarning(tiled, small.GetBodies()).empty());
  CHECK(cam::PlacementRefusal(tiled, small.GetBodies()).empty());
  const std::string note = cam::MachineOverhang(tiled, small.GetBodies());
  INFO(note);
  CHECK(note.find("2440") != std::string::npos);
  CHECK(note.find("hangs") != std::string::npos);
  CHECK(note.find("1840") != std::string::npos); // how far
}

TEST_CASE("cam machine: a setup copies the machine's tool library, unless "
          "something is already cut with the old table",
          "[cam][machine][job]") {
  cam::JobDocument job;
  cam::Machine m;
  m.name = "Shop router";
  cam::Tool small;
  small.number = 1;
  small.diameter = 3.0;
  cam::Tool big;
  big.number = 2;
  big.diameter = 8.0;
  m.library = {small, big};

  cam::Setup& fresh = job.AddSetup("Fresh");
  REQUIRE(fresh.tools.size() == 1); // the default 6 mm
  ui::ApplyMachine(fresh, m);
  REQUIRE(fresh.tools.size() == 2);
  CHECK(fresh.tools[1].diameter == 8.0);
  // COPIED: a library edit afterwards changes nothing the setup holds.
  m.library[1].diameter = 10.0;
  CHECK(fresh.tools[1].diameter == 8.0);

  cam::Setup& busy = job.AddSetup("Busy");
  cam::Operation op;
  op.id = job.AllocateId();
  op.toolNumber = 1;
  busy.operations.push_back(op);
  ui::ApplyMachine(busy, m);
  CHECK(busy.tools.size() == 1); // the table an operation names stays
  CHECK(busy.machine.name == "Shop router"); // the machine still applies
}

TEST_CASE("cam post: one file per tool, each complete, together the job",
          "[cam][post]") {
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Tool small = setup.tools[0];
  small.number = 2;
  small.diameter = 3.0;
  setup.tools.push_back(small);
  // Authored T1, T2, T1: a file per tool needs tool order, and the flag
  // FORCES it rather than refusing the setting it contradicts.
  for (int k = 0; k < 3; k++) {
    cam::Operation op;
    op.id = 10 + k;
    op.type = cam::OpType::Profile;
    op.toolNumber = k == 1 ? 2 : 1;
    op.topZ = 20.0;
    op.bottomZ = 20.0 - 2.0 * (k + 1);
    setup.operations.push_back(op);
  }
  setup.splitOutput = true;
  const std::vector<size_t> order = cam::CuttingOrder(setup);
  REQUIRE(order.size() == 3);
  CHECK(setup.operations[order[2]].toolNumber == 2); // T2 last, T1s together

  std::vector<cam::Toolpath> paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 3);
  std::vector<cam::PostFile> files;
  std::string error;
  REQUIRE(cam::PostProcessSplit("mach3", setup, paths, {}, files, &error));
  REQUIRE(files.size() == 2);
  CHECK(files[0].tool == 1);
  CHECK(files[1].tool == 2);
  // Each is a complete program that loads ITS tool, and only it.
  for (const cam::PostFile& f : files) {
    INFO("file for T" << f.tool);
    CHECK(f.gcode.find("G90 G91.1 G21") != std::string::npos);
    CHECK(f.gcode.find("M30") != std::string::npos);
    const std::string load = "T" + std::to_string(f.tool) + " M6";
    CHECK(f.gcode.find(load) != std::string::npos);
    CHECK(f.gcode.find(load) == f.gcode.rfind(load)); // once
    CHECK(f.gcode.find(f.tool == 1 ? "T2 M6" : "T1 M6") == std::string::npos);
  }
  // Together they hold every operation the one file holds.
  std::string one;
  std::vector<cam::Toolpath> ordered;
  for (const size_t i : order)
    for (const cam::Toolpath& tp : paths)
      if (tp.operationId == setup.operations[i].id)
        ordered.push_back(tp);
  REQUIRE(cam::PostProcess("mach3", setup, ordered, {}, one, &error));
  for (const cam::Operation& op : setup.operations) {
    const std::string tag = "(op " + std::to_string(op.id) + ")";
    REQUIRE(one.find(tag) != std::string::npos);
    bool inSome = false;
    for (const cam::PostFile& f : files)
      inSome = inSome || f.gcode.find(tag) != std::string::npos;
    CHECK(inSome);
  }
  // Paths NOT in tool order are refused, in words. GenerateAll already
  // walks CuttingOrder, so this has to be mis-ordered by hand: T1, T2, T1.
  REQUIRE(ordered.size() == 3);
  const std::vector<cam::Toolpath> shuffled = {ordered[0], ordered[2],
                                               ordered[1]};
  std::vector<cam::PostFile> bad;
  CHECK_FALSE(cam::PostProcessSplit("mach3", setup, shuffled, {}, bad, &error));
  CHECK(error.find("tool order") != std::string::npos);
  // The flag survives a save.
  const auto j = DocumentIO::CamSetupToJson(setup);
  CHECK(j.value("split_output", false));
  CHECK(DocumentIO::CamSetupFromJson(j).splitOutput);
}

// ---------------------------------------------------------------------------
// Cut what fits: a path never leaves the machine's travel.

TEST_CASE("cam machine: a path is clipped to the travel, and says how much",
          "[cam][machine]") {
  auto doc = BlockDoc(); // 40 x 30 x 20; the stock is 2 mm bigger each way
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.side = cam::Side::Outside;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 8.0;
  setup.operations.push_back(op);
  // Untouched without a work area.
  const auto whole = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(whole.size() == 1);
  CHECK(whole[0].clippedMm == 0.0);
  const double wholeCut = whole[0].CutLength();

  // A machine 30 mm wide in X: the bed sits at the stock's corner, so the
  // far 10-odd millimetres of the block are beyond it.
  setup.machine.name = "Narrow";
  setup.machine.travelMin = {0.0, 0.0, -100.0};
  setup.machine.travelMax = {30.0, 600.0, 0.0};
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::MachineBounds(setup, doc->GetBodies(), lo, hi));
  CHECK_THAT(hi.x - lo.x, WithinAbs(30.0, 1e-9));
  const auto clipped = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(clipped.size() == 1);
  REQUIRE(clipped[0].error.empty());
  CHECK(clipped[0].clippedMm > 20.0);
  CHECK(clipped[0].CutLength() < wholeCut - 20.0);
  // EVERY move stays inside the bed in X and Y -- rapids too: a rapid off
  // the bed is the same alarm.
  for (const cam::Move& m : clipped[0].moves) {
    CHECK(m.to.x >= lo.x - 1e-6);
    CHECK(m.to.x <= hi.x + 1e-6);
    CHECK(m.to.y >= lo.y - 1e-6);
    CHECK(m.to.y <= hi.y + 1e-6);
  }
  // Where the cut runs out of the bed it retracts at the boundary.
  int retracts = 0;
  for (size_t i = 1; i < clipped[0].moves.size(); i++) {
    const cam::Move& a = clipped[0].moves[i - 1];
    const cam::Move& b = clipped[0].moves[i];
    if (b.kind == cam::Move::Kind::Rapid && b.to.z > a.to.z + 1.0 &&
        std::abs(b.to.x - hi.x) < 1e-3)
      retracts++;
  }
  CHECK(retracts >= 1);
  // The SINGLE-operation path -- the one the panel regenerates a verb's
  // operation through -- clips the same. It did not, the morning after this
  // shipped: the clip lived in the full pass only, and the verb's path ran
  // off the bed while the RPC's file stopped at it.
  const cam::Toolpath single =
      cam::Generate(setup, setup.operations[0], doc->GetBodies());
  CHECK(single.clippedMm > 20.0);
  for (const cam::Move& m : single.moves) {
    CHECK(m.to.x <= hi.x + 1e-6);
    CHECK(m.to.y <= hi.y + 1e-6);
  }
  // The file it makes is clean, and the verdict says what was left out.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, clipped, {}, gcode, &error));
  const cam::Verdict v = cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
  CHECK(v.maxGouge < 0.4);
  const auto lines = cam::MachineLines(setup, doc->GetBodies(), &clipped);
  REQUIRE(lines.size() == 1);
  INFO(lines[0].text);
  CHECK(lines[0].text.find("left out of the file") != std::string::npos);
}

TEST_CASE("cam feeds: a cutter that rubs instead of cutting is named",
          "[cam][feeds]") {
  // The real numbers off the speaker baffle: a 6 mm FOUR-flute at 9000 rpm
  // and 1200 mm/min is 0.033 mm/tooth in MDF. It cut both recesses and a
  // d148 opening CLEANLY at that load, then packed and grabbed in a d100
  // slot of the same depth -- so the chip-load line is a note about heat and
  // tool life, and the chip-ROOM line is the one that saw it coming.
  // Everything needed to say either was already on the setup.
  auto doc = BlockDoc();
  for (Body& b : const_cast<std::vector<Body>&>(doc->GetBodies()))
    b.material = "mdf";
  cam::Setup setup = BasicSetup();
  setup.machine.name = "6090";
  setup.machine.rapidFeed = 1500.0;
  setup.tools[0].diameter = 6.0;
  setup.tools[0].flutes = 4;
  setup.tools[0].spindleRpm = 9000.0;
  setup.tools[0].feedCut = 1200.0;
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = -0.5; // 20.5 mm of full-width slot with a 6 mm cutter
  op.stepdown = 2.5;
  op.side = cam::Side::Outside;
  setup.operations.push_back(op);

  const cam::FeedReport report = cam::FeedsFor(setup, doc->GetBodies());
  std::vector<std::string> why;
  for (const cam::Finding& f : report.findings)
    why.push_back(f.text);
  for (const std::string& w : why)
    INFO(w);
  REQUIRE(why.size() >= 3);
  const auto says = [&](const char* txt) {
    for (const std::string& w : why)
      if (w.find(txt) != std::string::npos)
        return true;
    return false;
  };
  CHECK(says("0.033 mm/tooth"));       // the number it actually runs at
  CHECK(says("heat and tool life"));   // the honest cost -- it DID cut
  CHECK(says("no feed here reaches")); // the machine cannot reach the band
  CHECK(says("1500"));                 // the ceiling, by name
  CHECK(says("the cut that bites"));   // chip ROOM: what actually grabbed

  // The band is a scaling rule: 6 mm in sheet goods is 0.12-0.24 mm/tooth.
  const cam::ChiploadBand band = cam::BandFor(cam::CutClass::Sheet, 6.0);
  REQUIRE(band.known);
  CHECK_THAT(band.min, WithinAbs(0.12, 1e-9));
  CHECK_THAT(band.max, WithinAbs(0.24, 1e-9));

  // Two flutes at the same spindle IS reachable, which is the advice given.
  setup.tools[0].flutes = 2;
  setup.tools[0].spindleRpm = 6000.0;
  setup.tools[0].feedCut = 1440.0; // 6000 * 2 * 0.12
  const cam::FeedReport clean = cam::FeedsFor(setup, doc->GetBodies());
  for (const cam::Finding& f : clean.findings)
    INFO(f.text);
  for (const std::string& u : clean.unchecked)
    INFO("unchecked: " << u);
  CHECK(clean.Empty());

  // A body nobody labelled gets NO band rather than one from nowhere.
  for (Body& b : const_cast<std::vector<Body>&>(doc->GetBodies()))
    b.material.clear();
  setup.tools[0].flutes = 4;
  setup.tools[0].spindleRpm = 9000.0;
  setup.tools[0].feedCut = 1200.0;
  const cam::FeedReport blind = cam::FeedsFor(setup, doc->GetBodies());
  CHECK(blind.findings.empty());
  REQUIRE(blind.unchecked.size() == 1);
  INFO(blind.unchecked[0]);
  CHECK(blind.unchecked[0].find("no material") != std::string::npos);
  CHECK_FALSE(blind.Empty()); // NOT silence
}

TEST_CASE("cam feeds: a cutter's own figures beat the rule, and say so",
          "[cam][feeds]") {
  // There is no ISO for cutting data the way there is for fastener geometry,
  // so the app ships a scaling rule and takes a vendor's numbers when it has
  // them. Which one was used travels WITH the finding: a limit nobody can
  // trace is a limit nobody can argue with.
  auto doc = BlockDoc();
  for (Body& b : const_cast<std::vector<Body>&>(doc->GetBodies()))
    b.material = "mdf";
  cam::Setup setup = BasicSetup();
  setup.machine.rapidFeed = 6000.0; // roomy: not the limiting factor here
  setup.tools[0].diameter = 6.0;
  setup.tools[0].flutes = 2;
  setup.tools[0].spindleRpm = 12000.0;
  setup.tools[0].feedCut = 2400.0; // 0.100 mm/tooth
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Pocket; // shallow: no chip-room finding to confuse
  op.toolNumber = 1;
  op.topZ = 20.0;
  op.bottomZ = 18.0;
  op.stepdown = 2.0;
  op.side = cam::Side::Inside;
  setup.operations.push_back(op);

  // The rule: sheet goods at 0.020-0.040 x 6 mm = 0.12-0.24. 0.100 is under.
  const cam::FeedReport byRule = cam::FeedsFor(setup, doc->GetBodies());
  REQUIRE(byRule.findings.size() == 1);
  INFO(byRule.findings[0].text);
  CHECK_THAT(byRule.findings[0].measured, WithinAbs(0.100, 1e-9));
  CHECK_THAT(byRule.findings[0].bandMin, WithinAbs(0.12, 1e-9));
  CHECK(byRule.findings[0].source.find("scaling rule") != std::string::npos);
  CHECK(byRule.findings[0].text.find("scaling rule") != std::string::npos);

  // This cutter's maker says 0.05-0.15 for it. Now 0.100 is IN band, and
  // nothing is reported -- the rule was the weaker evidence all along.
  setup.tools[0].chiploadMin = 0.05;
  setup.tools[0].chiploadMax = 0.15;
  const cam::FeedReport byChart = cam::FeedsFor(setup, doc->GetBodies());
  for (const cam::Finding& f : byChart.findings)
    INFO(f.text);
  CHECK(byChart.Empty());

  // ...and when the cutter's own figures DO bite, they are cited as the
  // source rather than the rule.
  setup.tools[0].chiploadMin = 0.20;
  setup.tools[0].chiploadMax = 0.30;
  const cam::FeedReport tight = cam::FeedsFor(setup, doc->GetBodies());
  REQUIRE(tight.findings.size() >= 1);
  INFO(tight.findings[0].text);
  CHECK(tight.findings[0].source.find("own stated") != std::string::npos);
  CHECK_THAT(tight.findings[0].bandMin, WithinAbs(0.20, 1e-9));

  // A tool that states no spindle speed is UNCHECKED, not fine.
  setup.tools[0].chiploadMin = 0.0;
  setup.tools[0].chiploadMax = 0.0;
  setup.tools[0].spindleRpm = 0.0;
  const cam::FeedReport dumb = cam::FeedsFor(setup, doc->GetBodies());
  CHECK(dumb.findings.empty());
  REQUIRE(dumb.unchecked.size() == 1);
  INFO(dumb.unchecked[0]);
  CHECK(dumb.unchecked[0].find("spindle rpm") != std::string::npos);
}

TEST_CASE("cam verify: the POSTED FILE is checked against the travel",
          "[cam][machine]") {
  // On the artifact, not on the generator that made it. The generator's own
  // idea of reach was wrong once already -- it nailed the travel box to the
  // stock's corner, so turning a part round moved the machine -- and a check
  // that reads the file cannot inherit that mistake.
  auto doc = BlockDoc(); // 40 x 30 x 20
  cam::Setup setup = BasicSetup();
  cam::Operation op;
  op.id = 1;
  op.type = cam::OpType::Profile;
  op.toolNumber = 1;
  op.topZ = 21.0;
  op.bottomZ = -1.0;
  op.stepdown = 8.0;
  op.side = cam::Side::Outside;
  setup.operations.push_back(op);
  setup.safeZ = cam::DeriveSafeZ(setup, doc->GetBodies());

  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));

  // No work area stated: not checked, and every existing job stays as it was.
  {
    const cam::Verdict v =
        cam::VerifyGcode(gcode, setup, doc->GetBodies(), 0.25);
    CHECK(v.outsideTravel == 0);
  }
  // A machine that cannot reach the far side of its own stock. The file is
  // unchanged -- only the claim about the machine is -- so this is the check
  // catching what the generator's clip would have hidden.
  cam::Setup small = setup;
  small.machine.name = "Stubby";
  small.machine.travelMin = {0.0, 0.0, -100.0};
  small.machine.travelMax = {20.0, 600.0, 0.0};
  const cam::Verdict v = cam::VerifyGcode(gcode, small, doc->GetBodies(), 0.25);
  INFO("outside " << v.outsideTravel << " worst " << v.worstOvershoot);
  CHECK(v.outsideTravel > 0);
  CHECK(v.outsideAxis == 'X');
  CHECK(v.worstOvershoot > 10.0); // the block alone is 44 mm of stock
  CHECK(v.firstOutsideLine > 0);
  CHECK_FALSE(v.Safe(0.4)); // a file the controller alarms on is not safe
  const auto lines = cam::Explain(v, 0.4, 1.0);
  bool said = false;
  for (const cam::VerdictLine& l : lines)
    if (l.text.find("cannot go") != std::string::npos)
      said = true;
  CHECK(said);

  // ...and on a part that is CLAMPED, not modelled flat. `Inspect` frames the
  // bodies before this check, so framing them again inside it applied the
  // setup rotation twice -- which an identity frame cannot show. A baffle
  // laid face-up read every one of its 6809 moves as 391 mm outside a machine
  // that reaches all of them.
  cam::Setup turned = setup;
  turned.frame = glm::dquat(glm::dvec3(-glm::half_pi<double>(), 0.0, 0.0));
  turned.machine = cam::Machine{};
  turned.machine.name = "Roomy";
  turned.machine.travelMin = {0.0, 0.0, -100.0};
  turned.machine.travelMax = {600.0, 900.0, 0.0}; // reaches the lot
  REQUIRE(cam::RecomputeOrigin(turned, doc->GetBodies()));
  const auto turnedPaths = cam::GenerateAll(turned, doc->GetBodies());
  std::string turnedGcode;
  REQUIRE(
      cam::PostProcess("mach3", turned, turnedPaths, {}, turnedGcode, &error));
  const cam::Verdict tv =
      cam::VerifyGcode(turnedGcode, turned, doc->GetBodies(), 0.25);
  INFO("outside " << tv.outsideTravel << " worst " << tv.worstOvershoot);
  CHECK(tv.outsideTravel == 0);
}

TEST_CASE("cam verify: a nested comment is a file no controller will load",
          "[cam][post]") {
  // Our parser counts depth and carries on; Mach3 stops on the second "(",
  // refuses the whole file, and the verifier used to call it ok.
  const cam::GProgram good = cam::ParseGcode("(tool 1: 6 mm flat)\nG0 X1\n");
  CHECK(good.warnings.empty());
  const cam::GProgram bad =
      cam::ParseGcode("(tool 1: 6 mm flat 2F (through cuts) D6)\nG0 X1\n");
  REQUIRE(bad.warnings.size() == 1);
  INFO(bad.warnings[0]);
  CHECK(bad.warnings[0].find("nested comment") != std::string::npos);
  CHECK(bad.warnings[0].find("line 1") != std::string::npos);
  // The moves still parse: it is a warning about the FILE, not a parse error.
  CHECK(bad.errors.empty());
  CHECK(bad.moves.size() == 1);
}

TEST_CASE("cam machine: the reach window can be placed ON the sheet",
          "[cam][machine]") {
  // The bug this closes: the reach box was travel-sized with its corner
  // nailed to the STOCK's, so it moved when the part did. Turning a sheet
  // longer than the bed end-for-end pushed work off a machine that could
  // reach it perfectly well -- the sheet is slid through and clamped where
  // the work is, and nothing could say where that was.
  auto doc = HoleDoc(8.0, 12.0); // one bore at (20, 15) in a 40 x 30 block
  cam::Setup setup = BasicSetup();
  const ElementRef wall = HoleWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, wall});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 12;
  setup.operations.push_back(op);

  // A machine that reaches 10 mm in X: anchored at the stock corner, the bore
  // at X20 is outside it and the operation fails (the previous test).
  setup.machine.name = "Stubby";
  setup.machine.travelMin = {0.0, 0.0, -100.0};
  setup.machine.travelMax = {10.0, 600.0, 0.0};
  REQUIRE_FALSE(cam::GenerateAll(setup, doc->GetBodies())[0].error.empty());

  // Slide the bed along the sheet and the SAME machine cuts it.
  const cam::DerivedReach reach =
      cam::DeriveReachOffset(setup, doc->GetBodies());
  INFO(reach.why);
  REQUIRE(reach.ok);
  setup.reachOffset = reach.offset;
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  INFO(paths[0].error);
  CHECK(paths[0].error.empty());
  CHECK(paths[0].clippedMm == 0.0);
  CHECK(paths[0].CutLength() > 10.0);
  // The window really did move: it now starts past the stock's corner.
  glm::dvec3 lo(0.0), hi(0.0);
  REQUIRE(cam::MachineBounds(setup, doc->GetBodies(), lo, hi));
  CHECK(lo.x > -2.0 + 1e-6);                      // stock min is -2
  CHECK_THAT(hi.x - lo.x, WithinAbs(10.0, 1e-9)); // still only 10 mm of it

  // A cut WIDER than the travel is not a placement problem, and says so.
  // The d8 bore with a d6 cutter sweeps its centre round a 1 mm radius, so
  // the cutting spans 2 mm: the travel has to be under that to be too small,
  // which is what this asks (4 mm was wrong -- it FITS, and the derivation
  // was right to say so).
  cam::Setup narrow = setup;
  narrow.reachOffset = {0.0, 0.0};
  narrow.machine.travelMax = {1.0, 600.0, 0.0};
  const cam::DerivedReach no = cam::DeriveReachOffset(narrow, doc->GetBodies());
  CHECK_FALSE(no.ok);
  INFO(no.why);
  CHECK(no.why.find("two setups") != std::string::npos);
}

TEST_CASE("cam origin: stock_max is the FAR corner, for a sheet slid through",
          "[cam][machine]") {
  // The near corner of an overhanging sheet is the end hanging in the air:
  // a datum nobody can put a cutter on.
  auto doc = BlockDoc(); // 40 x 30 x 20, stock -2..42 x -2..32
  cam::Setup setup = BasicSetup();
  setup.originAt = cam::OriginAt::StockMin;
  REQUIRE(cam::RecomputeOrigin(setup, doc->GetBodies()));
  CHECK_THAT(setup.origin.x, WithinAbs(-2.0, 1e-6));
  CHECK_THAT(setup.origin.y, WithinAbs(-2.0, 1e-6));
  const double zWas = setup.origin.z;

  setup.originAt = cam::OriginAt::StockMax;
  REQUIRE(cam::RecomputeOrigin(setup, doc->GetBodies()));
  CHECK_THAT(setup.origin.x, WithinAbs(42.0, 1e-6));
  CHECK_THAT(setup.origin.y, WithinAbs(32.0, 1e-6));
  // Z is untouched: it is the material top either way.
  CHECK_THAT(setup.origin.z, WithinAbs(zWas, 1e-9));
}

TEST_CASE("cam machine: an operation wholly beyond the travel FAILS, it does "
          "not vanish",
          "[cam][machine]") {
  // The gap this closes: the "produced no toolpath" check ran BEFORE the
  // clip, so a station past the travel came back with no moves, no error and
  // failed: 0, and the file posted at full length without it. Found on a
  // 1122 mm speaker baffle whose second driver sat at Y 1046 of 900.
  auto doc = HoleDoc(8.0, 12.0); // one bore at (20, 15)
  cam::Setup setup = BasicSetup();
  const ElementRef wall = HoleWall(*doc);
  REQUIRE(wall.IsSet());
  const cam::DerivedOp d = cam::DeriveOperation(
      setup, doc->GetBodies(), cam::OpType::Profile, cam::Target{0, wall});
  REQUIRE(d.ok);
  cam::Operation op = d.op;
  op.id = 11;
  setup.operations.push_back(op);
  REQUIRE(cam::GenerateAll(setup, doc->GetBodies())[0].error.empty());

  // A machine that reaches 10 mm in X: the bed corner is at the stock's, so
  // the bore at X 20 is entirely outside it.
  setup.machine.name = "Stubby";
  setup.machine.travelMin = {0.0, 0.0, -100.0};
  setup.machine.travelMax = {10.0, 600.0, 0.0};
  const auto paths = cam::GenerateAll(setup, doc->GetBodies());
  REQUIRE(paths.size() == 1);
  CHECK(paths[0].CutLength() == 0.0);
  CHECK(paths[0].clippedMm > 0.0);
  INFO(paths[0].error);
  CHECK_FALSE(paths[0].error.empty());
  CHECK(paths[0].error.find("Stubby") != std::string::npos);
  // ...and the post says so where the operator will read it, rather than
  // writing a file that is simply short of a hole.
  std::string gcode, error;
  REQUIRE(cam::PostProcess("mach3", setup, paths, {}, gcode, &error));
  CHECK(gcode.find("SKIPPED op 11") != std::string::npos);
}

TEST_CASE("cam: a grbl file states its own tool numbers", "[cam][post]") {
  // GRBL takes no M6, so the change is a pause with a message -- and for a
  // year that message was the ONLY thing naming the tool. Nothing that reads
  // the file back could tell which cutter a move belonged to, so everything
  // after the first change was read as tool 1: this project's own verifier
  // simulated a 2 mm cutter's slots with a 3.175 mm one and called a correct
  // job scrap. (It fails the other way just as easily -- a fat second tool
  // gouging, simulated thin, verifies clean.)
  auto doc = BlockDoc();
  cam::Setup setup = BasicSetup();
  cam::Tool small;
  small.number = 4;
  small.name = "2 mm flat";
  small.diameter = 2.0;
  small.feedCut = 500.0;
  small.feedPlunge = 150.0;
  small.spindleRpm = 20000.0;
  setup.tools.push_back(small);
  for (int k = 0; k < 2; k++) {
    cam::Operation op;
    op.id = static_cast<uint64_t>(k + 1);
    op.type = cam::OpType::Profile;
    op.toolNumber = k == 0 ? 1 : 4;
    op.topZ = 20.0;
    op.bottomZ = 18.0;
    op.side = cam::Side::Outside;
    setup.operations.push_back(op);
  }
  const std::vector<cam::Toolpath> paths =
      cam::GenerateAll(setup, doc->GetBodies());
  std::string grbl, error;
  REQUIRE(cam::PostProcess("grbl", setup, paths, {}, grbl, &error));
  CHECK(grbl.find("M6") == std::string::npos); // still no M6, still a pause
  CHECK(grbl.find("\nT4\n") != std::string::npos);

  // What the file says is what a reader gets: the second operation's moves
  // carry tool 4, not tool 1.
  const cam::GProgram program = cam::ParseGcode(grbl);
  bool cutWithFour = false;
  for (const cam::GMove& m : program.moves)
    cutWithFour |= m.tool == 4;
  CHECK(cutWithFour);
}

#endif // HAVE_CAM
