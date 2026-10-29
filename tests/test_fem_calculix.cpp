// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// CalculiX as a study solver (docs/simulation/README.md#calculix). Same
// element, same Gauss rule, same mesh: a static study solved on ccx and
// in-house must agree to the digits ccx prints, so these measure the deck --
// every load kind, the pins' frames and ties, a bonded pair's two materials
// -- not the physics. And the refusals, each with what it promises: nothing
// started, ccx's own words, the run directory kept or gone.
//
// The cases that run ccx are tagged [calculix] and SKIP, naming why, where
// no ccx is found. The refusals run a fake ccx (a shell script) and need
// none.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "cad/CADMaterials.h"
#include "common/ChildProcess.h"
#include "core/Document.h"
#include "core/Feature.h"
#include "core/Measure.h"
#include "core/Occurrence.h"
#include "core/features/BooleanFeature.h"
#include "core/fem/Calculix.h"
#include "core/fem/FemSolver.h"
#include "core/fem/FemStudy.h"
#include "core/io/Serialization.h"

#include <glm/gtc/matrix_transform.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#endif

using Catch::Matchers::ContainsSubstring;

namespace fs = std::filesystem;

namespace {

// A scratch directory of the test's own, removed with it.
struct Scratch {
  fs::path dir;
  Scratch() {
    static int n = 0;
    dir = fs::temp_directory_path() /
          ("lobole-ccx-test-" + std::to_string(::getpid()) + "-" +
           std::to_string(n++));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// Sets an environment variable for a scope, and puts it back.
struct EnvVar {
  std::string key;
  bool had = false;
  std::string old;
  EnvVar(const std::string& k, const std::string& v) : key(k) {
    if (const char* o = std::getenv(k.c_str())) {
      had = true;
      old = o;
    }
    ::setenv(k.c_str(), v.c_str(), 1);
  }
  ~EnvVar() {
    if (had)
      ::setenv(key.c_str(), old.c_str(), 1);
    else
      ::unsetenv(key.c_str());
  }
};

// The ccx this machine has; the test SKIPs, saying why, without one.
std::optional<fem::CalculixInstall> RealCcx() {
  std::string why;
  return fem::FindCalculix("", &why);
}
#define REQUIRE_CCX(var)                                                       \
  const auto var = RealCcx();                                                  \
  if (!var) {                                                                  \
    std::string why_;                                                          \
    fem::FindCalculix("", &why_);                                              \
    SKIP("no CalculiX here: " << why_);                                        \
  }

// A stand-in ccx: answers -v as 2.23 does, and runs `body` for a deck.
std::string FakeCcx(const fs::path& dir, const std::string& body) {
  const fs::path p = dir / "ccx";
  std::ofstream f(p);
  f << "#!/bin/sh\n"
       "if [ \"$1\" = \"-v\" ]; then echo; echo 'This is Version 2.23'; "
       "exit 201; fi\n"
    << body << "\n";
  f.close();
  fs::permissions(p, fs::perms::owner_all);
  return p.string();
}

// A box meshed by hand, no Netgen: (nx, ny, nz) cells of size h, each cut
// into six tets along its diagonal, with their midside nodes. The nodes are
// the ones the tets use, nothing else.
fem::FemMesh BoxMesh(int nx, int ny, int nz, double h) {
  fem::FemMesh m;
  std::map<std::array<int, 3>, int> id;
  const auto node = [&](std::array<int, 3> g) {
    auto [it, fresh] = id.emplace(g, static_cast<int>(m.nodes.size()));
    if (fresh)
      m.nodes.push_back(glm::dvec3(g[0], g[1], g[2]) * (h / 2.0));
    return it->second;
  };
  const int perms[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                           {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
  for (int i = 0; i < nx; ++i)
    for (int j = 0; j < ny; ++j)
      for (int k = 0; k < nz; ++k)
        for (const auto& p : perms) {
          std::array<std::array<int, 3>, 4> c;
          c[0] = {2 * i, 2 * j, 2 * k};
          c[1] = c[0];
          c[1][p[0]] += 2;
          c[2] = c[1];
          c[2][p[1]] += 2;
          c[3] = c[2];
          c[3][p[2]] += 2;
          const auto mid = [&](int a, int b) {
            return std::array<int, 3>{(c[a][0] + c[b][0]) / 2,
                                      (c[a][1] + c[b][1]) / 2,
                                      (c[a][2] + c[b][2]) / 2};
          };
          std::array<int, 10> t{node(c[0]),      node(c[1]),
                                node(c[2]),      node(c[3]),
                                node(mid(0, 1)), node(mid(1, 2)),
                                node(mid(0, 2)), node(mid(0, 3)),
                                node(mid(1, 3)), node(mid(2, 3))};
          m.tets.push_back(t);
        }
  m.stats.nodeCount = m.nodes.size();
  m.stats.tetCount = m.tets.size();
  return m;
}

// A steel cantilever of the hand-made mesh under its own weight times a
// thousand, clamped at x = 0: the deck's nodes, elements, materials,
// boundaries and forces, with nothing a mesher made.
fem::StudyJobBody HandCantilever() {
  fem::StudyJobBody jb;
  jb.bodyName = "Beam";
  jb.mesh = BoxMesh(10, 2, 2, 5.0);
  jb.mat.youngMPa = 200000.0;
  jb.mat.poisson = 0.3;
  jb.mat.densityTPerMM3 = 7.85e-9;
  for (size_t n = 0; n < jb.mesh.nodes.size(); ++n)
    if (jb.mesh.nodes[n].x < 1e-9)
      jb.fixedNodes.push_back(static_cast<int>(n));
  return jb;
}
const glm::dvec3 kHeavy(0.0, 0.0, -9810.0 * 1000.0); // mm/s^2

std::shared_ptr<fem::CalculixRun> RunWith(const fem::CalculixInstall& ccx,
                                          const fs::path& root) {
  auto run = std::make_shared<fem::CalculixRun>();
  run->ccx = ccx;
  run->runRoot = root.string();
  run->threads = 1;
  return run;
}

bool ProcessGone(long pid) {
  return pid > 0 && ::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH;
}

size_t EntriesIn(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return 0;
  size_t n = 0;
  for (auto it = fs::directory_iterator(dir, ec);
       it != fs::directory_iterator(); ++it)
    ++n;
  return n;
}

double Rel(double a, double b) {
  const double m = std::max(std::abs(a), std::abs(b));
  return m > 0 ? std::abs(a - b) / m : 0.0;
}

} // namespace

TEST_CASE("CalculiX solves a hand-made cantilever as the in-house solver does",
          "[fem][calculix]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  const fem::StudyJobBody jb = HandCantilever();
  const std::vector<fem::Material> mats{jb.mat};
  const std::vector<std::vector<fem::FaceLoad>> rhs{{}};
  std::string err;
  fem::Supports sup;
  sup.fixedNodes = jb.fixedNodes;
  sup.reactions = true;
  const auto own = fem::SolveCases(jb.mesh, mats, sup, rhs, &err, kHeavy);
  INFO(err);
  REQUIRE(own);

  fem::SolveOptions options;
  options.calculix = RunWith(*ccx, scratch.dir / "runs");
  std::vector<fem::FemResults> theirs;
  fem::SolveReport report;
  REQUIRE(fem::SolveBodyOnCalculix(jb, mats, rhs, kHeavy, options, theirs,
                                   report, &err));
  INFO(err);
  REQUIRE(theirs.size() == 1);
  CHECK(report.solver == ccx->Name());
  const double dRel =
      Rel(theirs[0].maxDisplacement, own->front().maxDisplacement);
  const double vRel = Rel(theirs[0].maxVonMises, own->front().maxVonMises);
  std::printf("calculix hand-made cantilever (%zu nodes): max displacement "
              "%.9g vs %.9g (rel %.2e), max von Mises %.9g vs %.9g (rel "
              "%.2e), residual %.2e, floor %.2e, energy %.2e\n",
              jb.mesh.nodes.size(), theirs[0].maxDisplacement,
              own->front().maxDisplacement, dRel, theirs[0].maxVonMises,
              own->front().maxVonMises, vRel, report.worstResidual,
              report.worstResidualFloor, report.worstEnergyResidual);
  CHECK(report.worstEnergyResidual < 1e-5);
  CHECK(dRel < 1e-5);
  CHECK(vRel < 1e-5);
  // Solved, the run directory is gone.
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

// ---- The refusals: a stand-in ccx, no real one needed ----------------------

TEST_CASE("no CalculiX: every way to install it is named, and nothing starts",
          "[fem][calculix]") {
  Scratch scratch;
  EnvVar path("PATH", "");
  {
    EnvVar ccx("LOBOLE_CCX", (scratch.dir / "nowhere" / "ccx").string());
    std::string why;
    CHECK_FALSE(fem::FindCalculix("", &why));
    CHECK_THAT(why, ContainsSubstring("LOBOLE_CCX"));
    CHECK_THAT(why, ContainsSubstring("sudo apt install calculix-ccx"));
    CHECK_THAT(why, ContainsSubstring("`calculix-ccx` package"));
    CHECK_THAT(why, ContainsSubstring("dhondt.de"));
    CHECK_THAT(why, ContainsSubstring("calculix_path"));
  }
  {
    EnvVar ccx("LOBOLE_CCX", "");
    std::string why;
    CHECK_FALSE(fem::FindCalculix("", &why));
    CHECK(why == fem::CalculixInstallSentence());
    // A setting that names a path is that path, never the next one found.
    CHECK_FALSE(fem::FindCalculix((scratch.dir / "no-ccx").string(), &why));
    CHECK_THAT(why, ContainsSubstring("calculix_path setting"));
  }
  // A study set to CalculiX and given no ccx is refused, not solved
  // in-house, and no run directory appears.
  fem::StudyJobInput in;
  in.solver = fem::Solver::Calculix;
  in.bodies.push_back(HandCantilever());
  in.gravity = kHeavy / 1000.0;
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err));
  CHECK_THAT(err, ContainsSubstring("CalculiX"));
  CHECK(rt.bodies.empty());
}

TEST_CASE("CalculiX solves static studies only", "[fem][calculix]") {
  CHECK(fem::SolverRefusal(fem::Solver::Calculix, fem::AnalysisType::Static)
            .empty());
  CHECK_THAT(
      fem::SolverRefusal(fem::Solver::Calculix, fem::AnalysisType::Modal),
      ContainsSubstring("modal study is solved in-house"));
  CHECK_THAT(
      fem::SolverRefusal(fem::Solver::Calculix, fem::AnalysisType::Topology),
      ContainsSubstring("topology study is solved in-house"));
  CHECK(fem::SolverRefusal(fem::Solver::InHouse, fem::AnalysisType::Modal)
            .empty());
  fem::Solver s;
  CHECK_FALSE(fem::SolverFromString("ccx", s));
  CHECK_FALSE(fem::SolverFromString("CalculiX", s));

  // The run refuses one, whatever it was handed.
  fem::StudyJobInput in;
  in.type = fem::AnalysisType::Modal;
  in.solver = fem::Solver::Calculix;
  in.bodies.push_back(HandCantilever());
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err));
  CHECK_THAT(err, ContainsSubstring("solved in-house"));

  // So does a file: the study loads as nothing else.
  fem::Study study;
  study.id = 7;
  study.type = fem::AnalysisType::Static;
  study.solver = fem::Solver::Calculix;
  nlohmann::json j = DocumentIO::StudyToJson(study);
  CHECK(j.at("solver") == "calculix");
  CHECK(DocumentIO::StudyFromJson(j).solver == fem::Solver::Calculix);
  j["analysis"] = "modal";
  CHECK_THROWS_WITH(DocumentIO::StudyFromJson(j),
                    ContainsSubstring("solved in-house"));
  j["analysis"] = "static";
  j["solver"] = "ccx";
  CHECK_THROWS_WITH(DocumentIO::StudyFromJson(j),
                    ContainsSubstring("auto, in_house, calculix"));
  // Auto is absent from the file, so a study saved before hashes as it did.
  study.solver = fem::Solver::Auto;
  CHECK_FALSE(DocumentIO::StudyToJson(study).contains("solver"));
}

TEST_CASE("a CalculiX failure is refused with its own *ERROR line, and its "
          "run is kept",
          "[fem][calculix]") {
  Scratch scratch;
  std::string why;
  const auto fake = fem::ProbeCalculix(
      FakeCcx(scratch.dir,
              "echo ' *ERROR in calinput: the deck is not what I wanted'\n"
              "echo ' *ERROR in calinput: a second complaint'\nexit 1"),
      &why);
  INFO(why);
  REQUIRE(fake);
  CHECK(fake->Version() == "2.23");
  fem::SolveOptions options;
  options.calculix = RunWith(*fake, scratch.dir / "runs");
  const fem::StudyJobBody jb = HandCantilever();
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  CHECK_THAT(err, ContainsSubstring(
                      "*ERROR in calinput: the deck is not what I wanted"));
  CHECK_THAT(err, !ContainsSubstring("second complaint"));
  CHECK_THAT(err, ContainsSubstring("the run is kept in"));
  // The directory named holds the deck that failed.
  const size_t at = err.find("kept in ") + 8;
  const fs::path kept = err.substr(at, err.size() - at - 1);
  CHECK(fs::exists(kept / "job.inp"));
  CHECK(out.empty());
}

TEST_CASE("a CalculiX run past timeout_s is killed and refused",
          "[fem][calculix]") {
  Scratch scratch;
  const fs::path pidFile = scratch.dir / "pid";
  const auto fake =
      fem::ProbeCalculix(FakeCcx(scratch.dir, "echo $$ > '" + pidFile.string() +
                                                  "'\nexec sleep 30"),
                         nullptr);
  REQUIRE(fake);
  auto run = RunWith(*fake, scratch.dir / "runs");
  run->timeoutSeconds = 0.5;
  fem::SolveOptions options;
  options.calculix = run;
  const fem::StudyJobBody jb = HandCantilever();
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  const double took =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  CHECK_THAT(err, ContainsSubstring("timeout_s 0.5 s"));
  CHECK(took < 10.0); // a hang guard: the fake sleeps 30 s
  long pid = 0;
  std::ifstream(pidFile) >> pid;
  CHECK(ProcessGone(pid));
}

TEST_CASE("a cancelled CalculiX run leaves no ccx and no run directory",
          "[fem][calculix]") {
  Scratch scratch;
  const fs::path pidFile = scratch.dir / "pid";
  const auto fake =
      fem::ProbeCalculix(FakeCcx(scratch.dir, "echo $$ > '" + pidFile.string() +
                                                  "'\nexec sleep 30"),
                         nullptr);
  REQUIRE(fake);
  fem::SolveOptions options;
  options.calculix = RunWith(*fake, scratch.dir / "runs");
  // Cancelled once the stand-in has said who it is.
  options.cancelled = [&] {
    long pid = 0;
    std::ifstream(pidFile) >> pid;
    return pid > 0;
  };
  const fem::StudyJobBody jb = HandCantilever();
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  CHECK(err == "cancelled");
  long pid = 0;
  std::ifstream(pidFile) >> pid;
  CHECK(ProcessGone(pid));
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

TEST_CASE("a CalculiX run larger than the memory free is refused before it "
          "starts",
          "[fem][calculix]") {
  Scratch scratch;
  const fs::path ran = scratch.dir / "ran";
  const auto fake = fem::ProbeCalculix(
      FakeCcx(scratch.dir, "touch '" + ran.string() + "'\nexit 1"), nullptr);
  REQUIRE(fake);
  auto run = RunWith(*fake, scratch.dir / "runs");
  run->availableBytes = 1000000; // 1 MB free
  uint64_t told = 0;
  run->estimated = [&](uint64_t b) { told = b; };
  fem::SolveOptions options;
  options.calculix = run;
  const fem::StudyJobBody jb = HandCantilever();
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  CHECK(told > run->availableBytes);
  CHECK_THAT(err, ContainsSubstring("0.00 GB is free"));
  CHECK_THAT(err, ContainsSubstring("degrees of freedom"));
  CHECK_FALSE(fs::exists(ran));
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

TEST_CASE("an answer that does not solve our own stiffness is refused",
          "[fem][calculix]") {
  // A stand-in that prints a displacement that is no answer at all: one
  // millimetre at every node along x. Held to our K, it is refused.
  Scratch scratch;
  const fem::StudyJobBody jb = HandCantilever();
  std::string dat = "\n displacements (vx,vy,vz) for set NALL and time  "
                    "0.1000000E+01\n\n";
  for (size_t n = 0; n < jb.mesh.nodes.size(); ++n) {
    char line[96];
    const bool held = jb.mesh.nodes[n].x < 1e-9;
    std::snprintf(line, sizeof(line), "%10zu %13.6E %13.6E %13.6E\n", n + 1,
                  held ? 0.0 : 1.0, 0.0, 0.0);
    dat += line;
  }
  const fs::path datFile = scratch.dir / "answer.dat";
  std::ofstream(datFile) << dat;
  const auto fake = fem::ProbeCalculix(
      FakeCcx(scratch.dir, "cp '" + datFile.string() + "' job.dat"), nullptr);
  REQUIRE(fake);
  fem::SolveOptions options;
  options.calculix = RunWith(*fake, scratch.dir / "runs");
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  CHECK_THAT(err, ContainsSubstring("does not solve this study's own "
                                    "stiffness"));
  CHECK_THAT(err, ContainsSubstring("the run is kept in"));
}

TEST_CASE("a CalculiX result is saved naming its solver, an in-house one "
          "naming none",
          "[fem][calculix][serialize]") {
  fem::StudyJobInput in;
  in.bodies.push_back(HandCantilever());
  in.gravity = kHeavy / 1000.0;
  fem::StudyRuntime rt;
  std::string err;
  REQUIRE(fem::SolvePreparedStudy(in, rt, &err));
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  CHECK_FALSE(j.contains("solver"));
  CHECK(DocumentIO::StudyResultFromJson(j).solver.empty());
  rt.solver = "calculix 2.23";
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  CHECK(j.at("solver") == "calculix 2.23");
  CHECK(DocumentIO::StudyResultFromJson(j).solver == "calculix 2.23");
  j["solver"] = "cpu (Eigen SimplicialLDLT)";
  CHECK_THROWS_WITH(DocumentIO::StudyResultFromJson(j),
                    ContainsSubstring("names only a CalculiX solve"));
}

TEST_CASE("a ccx .dat is read to every node, whatever its exponent",
          "[fem][calculix]") {
  Scratch scratch;
  const fs::path p = scratch.dir / "job.dat";
  std::ofstream(p) << "\n displacements (vx,vy,vz) for set NALL and time  "
                      "0.1000000E+01\n\n"
                      "         1  1.000000E+00 -2.500000E-01  0.000000E+00\n"
                      "         2 -1.234567-100  3.000000E+02  1.000000E-03\n"
                      "\n";
  std::vector<glm::dvec3> u;
  std::string err;
  REQUIRE(fem::ReadDatDisplacements(p.string(), 2, u, &err));
  CHECK(u[0] == glm::dvec3(1.0, -0.25, 0.0));
  CHECK(u[1].x == -1.234567e-100);
  CHECK(u[1].y == 300.0);
  CHECK_FALSE(fem::ReadDatDisplacements(p.string(), 3, u, &err));
  CHECK_THAT(err, ContainsSubstring("2 of 3 nodes"));
}

// ---- Same mesh, two solvers
// --------------------------------------------------
//
// Each study is planned and meshed once; the one mesh is solved in-house and
// on CalculiX, and the two answers compared.

#ifdef HAVE_NETGEN
namespace {

struct Agreement {
  double dispRel = 0.0, vmRel = 0.0, residual = 0.0, floor = 0.0, energy = 0.0;
  size_t nodes = 0;
};

Agreement SolveBoth(fem::StudyJobInput input, const fem::CalculixInstall& ccx,
                    const char* what) {
  Scratch scratch;
  fem::StudyJobInput onCcx = input;
  input.solver = fem::Solver::Auto;
  onCcx.solver = fem::Solver::Calculix;
  fem::StudyRuntime own, theirs;
  std::string err;
  const bool ownOk = fem::SolvePreparedStudy(input, own, &err);
  INFO(err);
  REQUIRE(ownOk);
  fem::SolveOptions options;
  options.calculix = RunWith(ccx, scratch.dir / "runs");
  const bool theirOk = fem::SolvePreparedStudy(onCcx, theirs, &err, options);
  INFO(err);
  REQUIRE(theirOk);
  CHECK(theirs.solver == ccx.Name());
  Agreement a;
  a.dispRel = Rel(theirs.maxDisplacement, own.maxDisplacement);
  a.vmRel = Rel(theirs.maxVonMises, own.maxVonMises);
  a.residual = theirs.residual;
  a.floor = theirs.residualFloor;
  a.energy = theirs.energyResidual;
  for (const auto& b : own.bodies)
    a.nodes += b.mesh.nodes.size();
  // The reactions, which the in-house code computes from either answer.
  for (size_t b = 0; b < own.bodies.size(); ++b)
    for (size_t r = 0; r < own.bodies[b].results.reactions.size(); ++r) {
      const auto& x = own.bodies[b].results.reactions[r];
      const auto& y = theirs.bodies[b].results.reactions[r];
      const double scale = std::max(glm::length(x.forceN), 1e-9);
      CHECK(glm::length(x.forceN - y.forceN) < 1e-4 * scale);
      CHECK(x.contactAreaMm2 == y.contactAreaMm2);
    }
  std::printf("calculix same-mesh %s (%zu nodes): max displacement %.9g vs "
              "%.9g (rel %.2e), max von Mises %.9g vs %.9g (rel %.2e), "
              "residual %.2e, floor %.2e, energy %.2e\n",
              what, a.nodes, theirs.maxDisplacement, own.maxDisplacement,
              a.dispRel, theirs.maxVonMises, own.maxVonMises, a.vmRel,
              a.residual, a.floor, a.energy);
  CHECK(a.energy < 1e-5);
  CHECK(a.dispRel < 1e-5);
  CHECK(a.vmRel < 1e-5);
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
  return a;
}

fem::StudyJobInput Meshed(Document& doc, fem::Study& s) {
  fem::StudyJobInput input;
  std::string err;
  const bool ok = fem::PrepareStudy(doc, s, input, &err);
  INFO(err);
  REQUIRE(ok);
  return input;
}

ElementRef FaceWhere(const Body& body, const std::string& kind,
                     const std::function<bool(const glm::dvec3&)>& at) {
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && info->kind == kind && at(info->centroid))
      return {body.createdBy, name};
  }
  return {};
}

fem::Load MakeLoad(Document& doc, fem::LoadType type, const ElementRef& face,
                   double magnitude = 0.0,
                   glm::dvec3 direction = glm::dvec3(0, 0, -1)) {
  fem::Load l;
  l.id = doc.AllocateId();
  l.type = type;
  l.face = face;
  l.magnitude = magnitude;
  l.direction = direction;
  return l;
}

} // namespace

TEST_CASE("CalculiX and the in-house solver agree on a column clamped at its "
          "base, pushed sideways and weighed",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Column", glm::dvec3(0), 10.0, 10.0, 200.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  fem::Study& s = doc.AddStudy("Column");
  s.meshSize = 2.5;
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(200.0)), 100.0,
                             glm::dvec3(1, 0, 0)));
  s.gravity = glm::dvec3(0, 0, -9.81);
  SolveBoth(Meshed(doc, s), *ccx, "column");
}

TEST_CASE("CalculiX and the in-house solver agree on a lug on a pin held from "
          "turning, a lever on two free pins",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto plate =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0), 80.0, 20.0, 5.0);
  plate->SetMaterial("aluminum");
  doc.AddFeature(std::move(plate));
  uint64_t solid = doc.GetFeature(0)->GetId();
  for (double x : {12.0, 40.0}) {
    doc.AddFeature(std::make_unique<CylinderFeature>(
        "Hole", glm::dvec3(x, 10.0, -1.0), glm::dvec3(0, 0, 1), 4.0, 7.0));
    const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    doc.AddFeature(std::make_unique<BooleanFeature>("Bore", solid, tool,
                                                    BooleanOp::Subtract));
    solid = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  }
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  const auto boreAt = [&](double x) {
    return FaceWhere(body, "cylinder", [x](const glm::dvec3& c) {
      return std::abs(c.x - x) < 1e-3;
    });
  };
  const ElementRef end = FaceWhere(body, "plane", [](const glm::dvec3& c) {
    return std::abs(c.x - 80.0) < 1e-6;
  });
  {
    fem::Study& s = doc.AddStudy("Lug");
    s.meshSize = 2.0;
    fem::Load pin = MakeLoad(doc, fem::LoadType::Pin, boreAt(12.0));
    pin.fixRotation = true;
    s.loads.push_back(pin);
    s.loads.push_back(
        MakeLoad(doc, fem::LoadType::Force, end, 400.0, glm::dvec3(1, 0.2, 0)));
    SolveBoth(Meshed(doc, s), *ccx, "lug on a pin held from turning");
  }
  {
    fem::Study& s = doc.AddStudy("Lever");
    s.meshSize = 2.0;
    s.loads.push_back(MakeLoad(doc, fem::LoadType::Pin, boreAt(12.0)));
    s.loads.push_back(MakeLoad(doc, fem::LoadType::Pin, boreAt(40.0)));
    s.loads.push_back(
        MakeLoad(doc, fem::LoadType::Force, end, 50.0, glm::dvec3(0, -1, 0)));
    SolveBoth(Meshed(doc, s), *ccx, "lever on two pins");
  }
}

TEST_CASE("CalculiX and the in-house solver agree on a shaft under a torque "
          "and a remote load",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto cyl = std::make_unique<CylinderFeature>("Shaft", glm::dvec3(0, 0, 0),
                                               glm::dvec3(0, 0, 1), 6.0, 60.0);
  cyl->SetMaterial("steel");
  doc.AddFeature(std::move(cyl));
  const Body& body = doc.GetBodies()[0];
  const ElementRef side =
      FaceWhere(body, "cylinder", [](const glm::dvec3&) { return true; });
  const ElementRef bottom =
      FaceWhere(body, "plane", [](const glm::dvec3& c) { return c.z < 30.0; });
  const ElementRef top =
      FaceWhere(body, "plane", [](const glm::dvec3& c) { return c.z > 30.0; });
  fem::Study& s = doc.AddStudy("Twist");
  s.meshSize = 2.0;
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed, bottom));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Torque, side, 2000.0));
  fem::Load remote =
      MakeLoad(doc, fem::LoadType::Remote, top, 150.0, glm::dvec3(0, 1, 0));
  remote.point = glm::dvec3(30.0, 0.0, 80.0);
  remote.hasPoint = true;
  s.loads.push_back(remote);
  SolveBoth(Meshed(doc, s), *ccx, "torque and remote load");
}

TEST_CASE("CalculiX and the in-house solver agree on a pull on part of a face",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Beam", glm::dvec3(0), 100.0, 10.0, 10.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Pull");
  s.meshSize = 2.5;
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed,
                             FaceWhere(body, "plane", [](const glm::dvec3& c) {
                               return std::abs(c.x) < 1e-6;
                             })));
  fem::Load pull = MakeLoad(doc, fem::LoadType::Force,
                            FaceWhere(body, "plane",
                                      [](const glm::dvec3& c) {
                                        return std::abs(c.x - 100.0) < 1e-6;
                                      }),
                            2000.0, glm::dvec3(1, 0, 0));
  pull.region.kind = fem::LoadRegion::Kind::Band;
  pull.region.normal = glm::dvec3(0, 0, 1);
  pull.region.fromMm = 5.0;
  pull.region.toMm = 11.0;
  s.loads.push_back(pull);
  SolveBoth(Meshed(doc, s), *ccx, "region load");
}

TEST_CASE("CalculiX and the in-house solver agree on a steel block bonded to "
          "an aluminium one",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  auto lower = std::make_unique<Document>();
  auto upper = std::make_unique<Document>();
  for (auto [d, mat] :
       {std::pair{lower.get(), "aluminum"}, std::pair{upper.get(), "steel"}}) {
    auto box = std::make_unique<BoxFeature>("Block", glm::vec3(0.0f), 60.0,
                                            60.0, 16.0);
    box->SetMaterial(mat);
    d->AddFeature(std::move(box));
  }
  AssemblyDocument asmDoc;
  Occurrence lo;
  lo.name = "Lower";
  lo.partId = 1;
  lo.fixed = true;
  const uint64_t lowerOcc = asmDoc.AddOccurrence(lo);
  Occurrence up;
  up.name = "Upper";
  up.partId = 2;
  up.position = glm::vec3(0.0f, 0.0f, 16.0f);
  const uint64_t upperOcc = asmDoc.AddOccurrence(up);
  fem::AssemblyStudyContext ctx;
  ctx.placedBodies = [&](uint64_t occId) {
    std::vector<Body> out;
    const Document* part = occId == lowerOcc ? lower.get() : upper.get();
    const glm::mat4 world = glm::translate(
        glm::mat4(1.0f),
        occId == lowerOcc ? glm::vec3(0.0f) : glm::vec3(0.0f, 0.0f, 16.0f));
    for (const Body& b : part->GetBodies()) {
      Body nb = b;
      nb.shape = b.shape.Transformed(world);
      out.push_back(std::move(nb));
    }
    return out;
  };
  const auto faceAtZ = [](const Document& d, double z) {
    return FaceWhere(d.GetBodies()[0], "plane", [z](const glm::dvec3& c) {
      return std::abs(c.z - z) < 1e-6;
    });
  };
  fem::Study s;
  s.id = 2;
  s.name = "bonded";
  s.meshSize = 8.0;
  s.participants.push_back({upperOcc, fem::ParticipantRole::Meshed, 0.0, ""});
  s.participants.push_back({lowerOcc, fem::ParticipantRole::Bonded, 0.0, ""});
  fem::Load fix;
  fix.id = 20;
  fix.type = fem::LoadType::Fixed;
  fix.occurrenceId = lowerOcc;
  fix.face = ElementRef{0, faceAtZ(*lower, 0.0).name};
  s.loads.push_back(fix);
  fem::Load press;
  press.id = 21;
  press.type = fem::LoadType::Force;
  press.occurrenceId = upperOcc;
  press.face = ElementRef{0, faceAtZ(*upper, 16.0).name};
  press.magnitude = 5000.0;
  press.direction = glm::dvec3(1, 0, -1);
  s.loads.push_back(press);
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanAssemblyStudy(asmDoc, s, ctx, plan, &err));
  fem::StudyJobInput input;
  REQUIRE(fem::MeshPlannedStudy(plan, input, &err));
  REQUIRE(input.bodies.size() == 1);
  REQUIRE(input.bodies[0].materials.size() == 2);
  SolveBoth(input, *ccx, "bonded steel on aluminium");
}

TEST_CASE("the deck export_study_inp writes is one ccx runs",
          "[fem][calculix][run]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Beam", glm::dvec3(0), 60.0, 10.0, 10.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Beam");
  s.meshSize = 4.0;
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed,
                             FaceWhere(body, "plane", [](const glm::dvec3& c) {
                               return std::abs(c.x) < 1e-6;
                             })));
  s.loads.push_back(MakeLoad(
      doc, fem::LoadType::Pressure,
      FaceWhere(body, "plane",
                [](const glm::dvec3& c) { return std::abs(c.z - 10) < 1e-6; }),
      0.5));
  const fem::StudyJobInput input = Meshed(doc, s);
  std::string err;
  {
    std::ofstream deck(scratch.dir / "job.inp");
    REQUIRE(fem::WriteStudyDeck(input, 0, 0, deck, "export test", &err));
  }
  // ccx itself, in the directory, as a person would run it.
  const std::string cmd = "cd '" + scratch.dir.string() + "' && '" + ccx->path +
                          "' -i job > ccx.out 2>&1";
  CHECK(std::system(cmd.c_str()) == 0);
  std::vector<glm::dvec3> u;
  REQUIRE(fem::ReadDatDisplacements((scratch.dir / "job.dat").string(),
                                    input.bodies[0].mesh.nodes.size(), u,
                                    &err));
  double peak = 0.0;
  for (const glm::dvec3& d : u)
    peak = std::max(peak, glm::length(d));
  CHECK(peak > 0.0);
}
#endif // HAVE_NETGEN

// The check is worth what it refuses: a deck whose material is a tenth of a
// percent stiffer than the study's -- a real ccx run on it, the deck edited
// on the way in -- does not balance our K.
TEST_CASE("a deck a tenth of a percent away from its study is refused",
          "[fem][calculix]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  const auto stiffer = fem::ProbeCalculix(
      FakeCcx(scratch.dir, "sed -i 's/^200000, /200200, /' job.inp\nexec '" +
                               ccx->path + "' \"$@\""),
      nullptr);
  REQUIRE(stiffer);
  fem::SolveOptions options;
  options.calculix = RunWith(*stiffer, scratch.dir / "runs");
  const fem::StudyJobBody jb = HandCantilever();
  std::vector<fem::FemResults> out;
  fem::SolveReport report;
  std::string err;
  CHECK_FALSE(fem::SolveBodyOnCalculix(jb, {jb.mat}, {{}}, kHeavy, options, out,
                                       report, &err));
  CHECK_THAT(err, ContainsSubstring("does not balance this study's own "
                                    "stiffness"));
}

#ifdef HAVE_NETGEN
// How the memory estimate's coefficients were measured (hidden: run it by
// name). A steel column meshed by our pipeline at falling sizes, each deck
// run by ccx, its peak resident memory (getrusage) beside the estimate.
TEST_CASE("calibrate the CalculiX memory estimate", "[.][calculix-calibrate]") {
  REQUIRE_CCX(ccx);
  for (double size : {5.0, 3.5, 2.5, 1.8, 1.4, 1.2}) {
    Scratch scratch;
    Document doc;
    auto box = std::make_unique<BoxFeature>("Column", glm::dvec3(0), 10.0, 10.0,
                                            200.0);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
    const Body& body = doc.GetBodies()[0];
    fem::Study& s = doc.AddStudy("Column");
    s.meshSize = size;
    s.loads.push_back(MakeLoad(
        doc, fem::LoadType::Fixed,
        FaceWhere(body, "plane", [](auto c) { return std::abs(c.z) < 1e-6; })));
    s.loads.push_back(
        MakeLoad(doc, fem::LoadType::Force,
                 FaceWhere(body, "plane",
                           [](auto c) { return std::abs(c.z - 200.0) < 1e-6; }),
                 100.0, glm::dvec3(1, 0, 0)));
    const fem::StudyJobInput input = Meshed(doc, s);
    std::string err;
    {
      std::ofstream deck(scratch.dir / "job.inp");
      REQUIRE(fem::WriteStudyDeck(input, 0, 0, deck, "calibration", &err));
    }
    proc::PipedChild child;
    proc::ChildOptions co;
    co.cwd = scratch.dir.string();
    co.stdoutPath = (scratch.dir / "job.out").string();
    const auto t0 = std::chrono::steady_clock::now();
    REQUIRE(child.Start(ccx->path, {"-i", "job"}, &err, co));
    REQUIRE(child.Finish(nullptr) == 0);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    const size_t dofs = 3 * (input.bodies[0].mesh.nodes.size() -
                             input.bodies[0].fixedNodes.size());
    std::printf("mesh %.2f mm: %zu dof, ccx peak %.1f MB in %.1f s, "
                "estimate %.1f MB\n",
                size, dofs, child.PeakBytes() / 1e6, secs,
                fem::CalculixStaticMemoryEstimate(dofs) / 1e6);
    CHECK(fem::CalculixStaticMemoryEstimate(dofs) >= child.PeakBytes());
  }
}
#endif
