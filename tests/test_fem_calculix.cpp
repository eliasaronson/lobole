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
#include "core/fem/FemThermal.h"
#include "core/io/Serialization.h"
#include "topo_testmesh.h"

#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <catch2/catch_approx.hpp>
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
#include <sstream>
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

TEST_CASE("CalculiX solves static and buckling studies only",
          "[fem][calculix]") {
  CHECK(fem::SolverRefusal(fem::Solver::Calculix, fem::AnalysisType::Static)
            .empty());
  CHECK(fem::SolverRefusal(fem::Solver::Calculix, fem::AnalysisType::Buckling)
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

// Used by the planning refusals below, which need no mesh, as well as by
// the runs.
namespace {

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
  plate->SetMaterial("aluminum_6061_t6");
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
  for (auto [d, mat] : {std::pair{lower.get(), "aluminum_6061_t6"},
                        std::pair{upper.get(), "steel"}}) {
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

// ---- Buckling on the same mesh
// -----------------------------------------------
//
// The same element and Gauss rule on both sides, so this measures K_G and
// the eigensolve (docs/simulation/README.md#buckling), not the physics:
// the first three factors agree to 1e-4.

namespace {

void BucklingBoth(fem::StudyJobInput input, const fem::CalculixInstall& ccx,
                  const char* what) {
  Scratch scratch;
  input.type = fem::AnalysisType::Buckling;
  input.numModes = fem::kBucklingFactorsDefault;
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
  REQUIRE(own.bodies.size() == theirs.bodies.size());
  for (size_t b = 0; b < own.bodies.size(); ++b) {
    const auto& a = own.bodies[b].results.modes;
    const auto& c = theirs.bodies[b].results.modes;
    REQUIRE(a.size() >= 3);
    REQUIRE(c.size() >= 3);
    double worst = 0.0;
    std::string line;
    for (size_t m = 0; m < 3; ++m) {
      // ccx's factor nearest ours: the two may order a pair of equal ones
      // differently.
      double best = 1e300;
      for (const auto& theirsMode : c)
        best = std::min(best, Rel(a[m].factor, theirsMode.factor));
      worst = std::max(worst, best);
      char buf[96];
      std::snprintf(buf, sizeof(buf), " %.9g/%.9g", a[m].factor, c[m].factor);
      line += buf;
    }
    std::printf("calculix buckling %s (%zu nodes): factors in-house/ccx%s, "
                "worst rel %.2e\n",
                what, own.bodies[b].mesh.stats.nodeCount, line.c_str(), worst);
    CHECK(worst < 1e-4);
    CHECK(theirs.bodies[b].buckling.solved);
  }
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

} // namespace

TEST_CASE("CalculiX and the in-house solver buckle a column at the same "
          "factors",
          "[fem][calculix][buckling][run]") {
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
  s.type = fem::AnalysisType::Buckling;
  s.meshSize = 4.0;
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(200.0)), 1000.0,
                             glm::dvec3(0.1, 0, -1)));
  s.gravity = glm::dvec3(0, 0, -9.81);
  BucklingBoth(Meshed(doc, s), *ccx, "column");
}

TEST_CASE("CalculiX and the in-house solver buckle a bracket on a pin at the "
          "same factors",
          "[fem][calculix][buckling][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto plate =
      std::make_unique<BoxFeature>("Plate", glm::dvec3(0), 80.0, 20.0, 4.0);
  plate->SetMaterial("aluminum_6061_t6");
  doc.AddFeature(std::move(plate));
  const uint64_t solid = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Hole", glm::dvec3(12.0, 10.0, -1.0), glm::dvec3(0, 0, 1), 4.0, 6.0));
  const uint64_t tool = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Bore", solid, tool,
                                                  BooleanOp::Subtract));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Bracket");
  s.type = fem::AnalysisType::Buckling;
  s.meshSize = 3.0;
  fem::Load pin = MakeLoad(
      doc, fem::LoadType::Pin,
      FaceWhere(body, "cylinder", [](const glm::dvec3&) { return true; }));
  pin.fixRotation = true;
  s.loads.push_back(pin);
  // Pushed along its length from the far end: onto the pin, and buckling
  // out of its plane.
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane",
                                       [](const glm::dvec3& c) {
                                         return std::abs(c.x - 80.0) < 1e-6;
                                       }),
                             200.0, glm::dvec3(-1, 0, 0)));
  BucklingBoth(Meshed(doc, s), *ccx, "bracket on a pin");
}

TEST_CASE("CalculiX and the in-house solver buckle a shaft under a torque "
          "and a remote load at the same factors",
          "[fem][calculix][buckling][run]") {
  REQUIRE_CCX(ccx);
  Document doc;
  auto cyl = std::make_unique<CylinderFeature>("Shaft", glm::dvec3(0, 0, 0),
                                               glm::dvec3(0, 0, 1), 3.0, 80.0);
  cyl->SetMaterial("steel");
  doc.AddFeature(std::move(cyl));
  const Body& body = doc.GetBodies()[0];
  const ElementRef side =
      FaceWhere(body, "cylinder", [](const glm::dvec3&) { return true; });
  const ElementRef bottom =
      FaceWhere(body, "plane", [](const glm::dvec3& c) { return c.z < 40.0; });
  const ElementRef top =
      FaceWhere(body, "plane", [](const glm::dvec3& c) { return c.z > 40.0; });
  fem::Study& s = doc.AddStudy("Twist");
  s.type = fem::AnalysisType::Buckling;
  s.meshSize = 1.5;
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed, bottom));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Torque, side, 2000.0));
  fem::Load remote =
      MakeLoad(doc, fem::LoadType::Remote, top, 100.0, glm::dvec3(0, 0.2, -1));
  remote.point = glm::dvec3(5.0, 0.0, 90.0);
  remote.hasPoint = true;
  s.loads.push_back(remote);
  BucklingBoth(Meshed(doc, s), *ccx, "torque and remote load");
}

TEST_CASE("CalculiX and the in-house solver buckle a steel column bonded to "
          "an aluminium one at the same factors",
          "[fem][calculix][buckling][run]") {
  REQUIRE_CCX(ccx);
  auto lower = std::make_unique<Document>();
  auto upper = std::make_unique<Document>();
  for (auto [d, mat] : {std::pair{lower.get(), "aluminum_6061_t6"},
                        std::pair{upper.get(), "steel"}}) {
    auto box = std::make_unique<BoxFeature>("Block", glm::vec3(0.0f), 10.0,
                                            10.0, 80.0);
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
  up.position = glm::vec3(0.0f, 0.0f, 80.0f);
  const uint64_t upperOcc = asmDoc.AddOccurrence(up);
  fem::AssemblyStudyContext ctx;
  ctx.placedBodies = [&](uint64_t occId) {
    std::vector<Body> out;
    const Document* part = occId == lowerOcc ? lower.get() : upper.get();
    const glm::mat4 world = glm::translate(
        glm::mat4(1.0f),
        occId == lowerOcc ? glm::vec3(0.0f) : glm::vec3(0.0f, 0.0f, 80.0f));
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
  s.type = fem::AnalysisType::Buckling;
  s.meshSize = 4.0;
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
  press.face = ElementRef{0, faceAtZ(*upper, 80.0).name};
  press.magnitude = 1000.0;
  press.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(press);
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanAssemblyStudy(asmDoc, s, ctx, plan, &err));
  fem::StudyJobInput input;
  REQUIRE(fem::MeshPlannedStudy(plan, input, &err));
  REQUIRE(input.bodies.size() == 1);
  REQUIRE(input.bodies[0].materials.size() == 2);
  BucklingBoth(input, *ccx, "bonded steel on aluminium");
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

// ---- Nonlinear static (docs/simulation/README.md#nonlinear-static) --------

namespace {

// A stand-in ccx that converges one increment, to `factor` of the load,
// and then stops converging: what it prints is what a real one does.
std::string StallingCcx(const fs::path& dir, double factor) {
  char t[32];
  std::snprintf(t, sizeof(t), "%.7E", factor);
  return FakeCcx(
      dir,
      std::string(
          "nodes=$(awk '/^\\*NODE, NSET=NALL/{on=1;next} /^\\*/{on=0} "
          "on{sub(/,.*/,\"\");print}' job.inp)\n"
          "sup=$(awk '/^\\*NSET, NSET=NSUP/{on=1;next} /^\\*/{on=0} "
          "on{gsub(/,/,\" \");print}' job.inp)\n"
          "{ echo; echo ' displacements (vx,vy,vz) for set NALL and time  ") +
          t +
          "'; echo; for n in $nodes; do echo \"  $n  0.0E+00  0.0E+00  "
          "0.0E+00\"; done; echo;\n"
          "  echo ' forces (fx,fy,fz) for set NSUP and time  " +
          t +
          "'; echo; for n in $sup; do echo \"  $n  0.0E+00  0.0E+00  "
          "0.0E+00\"; done; echo; } > job.dat\n"
          "printf 'SUMMARY OF JOB INFORMATION\\n  STEP INC ATT ITRS TOT STEP "
          "INC\\n"
          "     1  1  1  3  " +
          t + "  " + t + "  " + t + "\\n     1  2  1U  9  " + t + "  " + t +
          "  1.0E-06\\n' > job.sta\n"
          "echo ' *ERROR: increment size smaller than minimum'\n"
          "echo ' best solution and residuals are in the frd file'\n"
          "exit 201");
}

} // namespace

TEST_CASE("a nonlinear study is refused in-house, on another analysis, with "
          "load cases, and a snap-through run with converge always",
          "[fem][calculix][nonlinear]") {
  fem::Nonlinear nl;
  nl.largeDeformation = true;
  using fem::AnalysisType;
  using fem::ConvergeMode;
  using fem::Solver;
  CHECK(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, nl, 0,
                              ConvergeMode::Auto, false)
            .empty());
  fem::Study st;
  st.nonlinear = nl;
  CHECK(fem::EffectiveSolver(st) == Solver::Calculix);
  CHECK(fem::EffectiveSolver(fem::Study{}) == Solver::Auto);
  CHECK_THAT(fem::NonlinearRefusal(Solver::InHouse, AnalysisType::Static, nl, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("in-house solver is linear"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Modal, nl, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("a static study's"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, nl, 2,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("takes no load cases"));
  fem::Nonlinear snap = nl;
  snap.snapThrough = true;
  CHECK(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, snap, 0,
                              ConvergeMode::Never, false)
            .empty());
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, snap, 0,
                                   ConvergeMode::Always, false),
             ContainsSubstring("peak load factor"));
  fem::Nonlinear half;
  half.snapThrough = true;
  CHECK_THAT(fem::NonlinearRefusal(Solver::Calculix, AnalysisType::Static, half,
                                   0, ConvergeMode::Auto, false),
             ContainsSubstring("large_deformation: true"));
  fem::Nonlinear seeded = nl;
  seeded.hasImperfection = true;
  seeded.imperfectionMm = 0.1;
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, seeded,
                                   0, ConvergeMode::Auto, false),
             ContainsSubstring("snap_through: true"));

  // The run refuses it as set_study does: a file can carry it.
  fem::StudyJobInput in;
  in.type = AnalysisType::Static;
  in.solver = Solver::InHouse;
  in.nonlinear = nl;
  in.bodies.push_back(HandCantilever());
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err));
  CHECK_THAT(err, ContainsSubstring("in-house solver is linear"));
  CHECK(rt.bodies.empty());
}

TEST_CASE("a nonlinear run that stops converging is refused with the load "
          "it reached, and nothing is stored",
          "[fem][calculix][nonlinear]") {
  Scratch scratch;
  fem::CalculixInstall ccx;
  ccx.path = StallingCcx(scratch.dir, 0.4);
  ccx.major = 2;
  ccx.minor = 23;
  fem::StudyJobInput in;
  in.type = fem::AnalysisType::Static;
  in.solver = fem::Solver::Calculix;
  in.nonlinear.largeDeformation = true;
  in.bodies.push_back(HandCantilever());
  in.gravity = kHeavy / 1000.0; // m/s^2
  fem::SolveOptions options;
  options.calculix = RunWith(ccx, scratch.dir / "runs");
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err, options));
  INFO(err);
  CHECK_THAT(err, ContainsSubstring("stopped converging at 40 % of the load"));
  CHECK_THAT(err, ContainsSubstring("increment size smaller than minimum"));
  CHECK_THAT(err, ContainsSubstring("Nothing was stored"));
  CHECK(rt.bodies.empty());
}

TEST_CASE("a contact is refused where a nonlinear study is: in-house, on "
          "another analysis, with load cases, beside a pin, with a negative "
          "friction",
          "[fem][calculix][contact]") {
  using fem::AnalysisType;
  using fem::Solver;
  fem::Study s;
  fem::Load c;
  c.id = 1;
  c.type = fem::LoadType::Contact;
  s.loads.push_back(c);
  CHECK(fem::ContactRefusal(s).empty());
  CHECK(fem::HasContact(s));
  CHECK(fem::IsNonlinear(s));
  // A contact on auto solves on CalculiX, and without large deformation.
  CHECK(fem::EffectiveSolver(s) == Solver::Calculix);
  {
    fem::Study t = s;
    t.solver = Solver::InHouse;
    CHECK_THAT(fem::ContactRefusal(t),
               ContainsSubstring("a study with a contact solves on CalculiX"));
  }
  for (AnalysisType type :
       {AnalysisType::Modal, AnalysisType::Buckling, AnalysisType::Topology}) {
    fem::Study t = s;
    t.type = type;
    CHECK_THAT(fem::ContactRefusal(t),
               ContainsSubstring("a contact is a static study's"));
  }
  {
    fem::Study t = s;
    t.loadCases.push_back({"up", 1.0, ""});
    CHECK_THAT(fem::ContactRefusal(t),
               ContainsSubstring("takes no load cases"));
  }
  {
    fem::Study t = s;
    t.converge = fem::ConvergeMode::Always;
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("converge: always"));
    t.converge = fem::ConvergeMode::Never;
    CHECK(fem::ContactRefusal(t).empty());
  }
  {
    fem::Study t = s;
    t.nonlinear.largeDeformation = true;
    CHECK(fem::ContactRefusal(t).empty());
    t.nonlinear.snapThrough = true;
    CHECK_THAT(fem::ContactRefusal(t),
               ContainsSubstring("cannot hold a contact"));
  }
  {
    // Increments alone belong to a contact; without one they are refused.
    fem::Study t = s;
    t.nonlinear.increments = 4;
    CHECK(fem::ContactRefusal(t).empty());
    t.loads.clear();
    CHECK_THAT(fem::ContactRefusal(t),
               ContainsSubstring("large_deformation: true"));
  }
  {
    fem::Study t = s;
    t.loads[0].friction = -0.1;
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("friction"));
    t.loads[0].friction = std::nan("");
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("friction"));
  }
  {
    fem::Study t = s;
    fem::Load pin;
    pin.id = 2;
    pin.type = fem::LoadType::Pin;
    t.loads.push_back(pin);
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("in-house solver"));
  }
  {
    fem::Study t;
    t.participants.push_back({7, fem::ParticipantRole::Meshed});
    fem::Participant touching;
    touching.occurrenceId = 8;
    touching.role = fem::ParticipantRole::Contact;
    touching.friction = -1.0;
    t.participants.push_back(touching);
    CHECK(fem::HasContact(t));
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("friction"));
    t.participants.back().friction = 0.2;
    CHECK(fem::ContactRefusal(t).empty());
    t.solver = Solver::InHouse;
    CHECK_THAT(fem::ContactRefusal(t), ContainsSubstring("CalculiX"));
  }

  // The run refuses it as set_study does: a file can carry it.
  fem::StudyJobInput in;
  in.type = AnalysisType::Static;
  in.solver = Solver::InHouse;
  in.bodies.push_back(HandCantilever());
  fem::StudyJobBody::ContactPair pair;
  pair.id = 1;
  in.bodies.back().contacts.push_back(pair);
  fem::StudyRuntime rt;
  std::string err;
  CHECK_FALSE(fem::SolvePreparedStudy(in, rt, &err));
  CHECK_THAT(err, ContainsSubstring("a study with a contact solves on"));
  CHECK(rt.bodies.empty());
}

TEST_CASE("a part rests on the ground under a flat face only",
          "[fem][calculix][contact]") {
  Document doc;
  auto roller = std::make_unique<CylinderFeature>(
      "Roller", glm::dvec3(0), glm::dvec3(0, 0, 1), 10.0, 30.0);
  roller->SetMaterial("steel");
  doc.AddFeature(std::move(roller));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Rests");
  fem::Load c = MakeLoad(
      doc, fem::LoadType::Contact,
      FaceWhere(body, "cylinder", [](const glm::dvec3&) { return true; }));
  s.loads.push_back(c);
  fem::StudyPlan plan;
  std::string err;
  CHECK_FALSE(fem::PlanStudy(doc, s, plan, &err));
  CHECK_THAT(err, ContainsSubstring("not flat"));
  // The flat end rests on a box under it: its face on the end's plane,
  // reaching past it.
  double low = 1e300;
  for (const auto& [index, name] : body.elementMap.names)
    if (const auto info = Measure::DescribeFaceAt(body, index);
        info && info->kind == "plane")
      low = std::min(low, info->centroid.z);
  s.loads[0].face = FaceWhere(body, "plane", [low](const glm::dvec3& c) {
    return std::abs(c.z - low) < 1e-6;
  });
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  REQUIRE(plan.bodies.size() == 1);
  REQUIRE(plan.bodies[0].contacts.size() == 1);
  const auto& ground = plan.bodies[0].partners[0];
  CHECK(ground.rigid);
  CHECK(ground.name == "ground");
  Bnd_Box box;
  BRepBndLib::Add(ground.shape, box);
  double x0, y0, z0, x1, y1, z1;
  box.Get(x0, y0, z0, x1, y1, z1);
  CHECK(x0 < -10.0);
  CHECK(x1 > 10.0);
  CHECK(z1 == Catch::Approx(low).margin(1e-6));
  CHECK(z0 < low);
}

#ifdef HAVE_NETGEN
namespace {

constexpr double kPiNl = 3.14159265358979323846;

// A study run as run_study runs one: planned, meshed and solved on ccx,
// its peak's convergence check off.
fem::StudyRuntime RunNonlinear(Document& doc, fem::Study& s,
                               const fem::CalculixInstall& ccx,
                               const Scratch& scratch, double* seconds) {
  fem::StudyPlan plan;
  std::string err;
  const bool planned = fem::PlanStudy(doc, s, plan, &err);
  INFO(err);
  REQUIRE(planned);
  fem::SolveOptions options;
  options.calculix = RunWith(ccx, scratch.dir / "runs");
  fem::ConvergeOptions converge;
  converge.mode = fem::ConvergeMode::Never;
  fem::StudyJobInput input;
  fem::StudyRuntime rt;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = fem::SolveConverged(plan, input, rt, converge, &err, options);
  if (seconds)
    *seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
  INFO(err);
  REQUIRE(ok);
  REQUIRE(rt.bodies.size() == 1);
  return rt;
}

// The elastica of a cantilever under a dead tip load P normal to its axis,
// k = P L^2 / EI: theta'' = -k cos(theta) on s in [0, 1], theta(0) = 0,
// theta'(1) = 0, shot on theta'(0) (Bisshopp and Drucker's elliptic
// integrals, evaluated). The tip's motion along the load and back along the
// axis, over L.
std::pair<double, double> Elastica(double k) {
  const auto shoot = [k](double d0, double* along, double* back) {
    const int n = 4000;
    const double h = 1.0 / n;
    double th = 0.0, w = d0, x = 0.0, y = 0.0;
    for (int i = 0; i < n; ++i) {
      const double k1t = w, k1w = -k * std::cos(th);
      const double k2t = w + h / 2 * k1w, k2w = -k * std::cos(th + h / 2 * k1t);
      const double k3t = w + h / 2 * k2w, k3w = -k * std::cos(th + h / 2 * k2t);
      const double k4t = w + h * k3w, k4w = -k * std::cos(th + h * k3t);
      const double a1 = th, a2 = th + h / 2 * k1t, a3 = th + h / 2 * k2t,
                   a4 = th + h * k3t;
      x += h / 6 *
           (std::cos(a1) + 2 * std::cos(a2) + 2 * std::cos(a3) + std::cos(a4));
      y += h / 6 *
           (std::sin(a1) + 2 * std::sin(a2) + 2 * std::sin(a3) + std::sin(a4));
      th += h / 6 * (k1t + 2 * k2t + 2 * k3t + k4t);
      w += h / 6 * (k1w + 2 * k2w + 2 * k3w + k4w);
    }
    if (along)
      *along = y;
    if (back)
      *back = 1.0 - x;
    return w; // theta'(1)
  };
  double lo = 0.0, hi = 2.0 * k + 1.0;
  for (int i = 0; i < 80; ++i) {
    const double mid = 0.5 * (lo + hi);
    (shoot(mid, nullptr, nullptr) > 0 ? hi : lo) = mid;
  }
  double along = 0.0, back = 0.0;
  shoot(0.5 * (lo + hi), &along, &back);
  return {along, back};
}

const CADMaterials::MaterialProperties& Steel() {
  return CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString("steel"));
}

} // namespace

TEST_CASE("a cantilever at P L^2 / EI = 1 bends as Bisshopp and Drucker's "
          "elastica, where linear theory is 10 % out",
          "[fem][calculix][nonlinear]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  Document doc;
  constexpr double L = 100.0, a = 5.0;
  auto box = std::make_unique<BoxFeature>("Beam", glm::dvec3(0), a, a, L);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  const double E = Steel().youngGPa * 1000.0;
  const double I = a * a * a * a / 12.0;
  const double P = E * I / (L * L); // k = 1
  fem::Study& s = doc.AddStudy("Elastica");
  s.meshSize = 2.5;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  const ElementRef tip = FaceWhere(body, "plane", isZ(L));
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Force, tip, P, glm::dvec3(1, 0, 0)));
  s.nonlinear.largeDeformation = true;
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunNonlinear(doc, s, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& b = rt.bodies[0];
  REQUIRE(b.nonlinear.solved);
  fem::ResolvedFace rf;
  std::string err;
  REQUIRE(fem::ResolveFace(doc, tip, "tip", rf, &err));
  const auto motion =
      fem::FitFaceMotion(b.mesh, b.results.displacement, rf.faceIndex);
  REQUIRE(motion.has_value());
  const auto [along, back] = Elastica(1.0);
  const double ux = motion->translation.x / L, uz = -motion->translation.z / L;
  std::printf("nonlinear elastica, PL^2/EI = 1, %zu nodes, %d increments, "
              "%.1f s: tip along %.5f L (elastica %.5f, %+.2f %%), back "
              "%.5f L (elastica %.5f, %+.2f %%); linear theory 1/3; "
              "imbalance %.2e\n",
              b.mesh.stats.nodeCount, b.nonlinear.increments, seconds, ux,
              along, 100.0 * (ux / along - 1.0), uz, back,
              100.0 * (uz / back - 1.0), b.nonlinear.imbalance);
  CHECK_THAT(along, Catch::Matchers::WithinRel(0.30172, 1e-4));
  CHECK_THAT(back, Catch::Matchers::WithinRel(0.05643, 1e-3));
  CHECK_THAT(ux, Catch::Matchers::WithinRel(along, 0.01));
  CHECK_THAT(uz, Catch::Matchers::WithinRel(back, 0.02));
  CHECK(b.nonlinear.factor == 1.0);
  CHECK(b.nonlinear.imbalance < fem::kNonlinearBalance);
  CHECK_FALSE(b.nonlinear.snapped);
  // The clamp carries the dead load back.
  REQUIRE(b.results.reactions.size() == 1);
  CHECK_THAT(b.results.reactions[0].forceN.x,
             Catch::Matchers::WithinRel(-P, 2e-3));
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
  // The Cauchy stress at the root: the moment P (L - u along the axis is
  // not it: the arm is the deflected tip's x from the root, L (1 - back)).
  // The root's peak sits on the clamp's edge, so the away value is read.
  CHECK(b.results.maxVonMises > 0.0);
}

TEST_CASE("at a load too small to deform it, a large-deformation run answers "
          "the linear one: a pressure, a torque, a remote load and a pin",
          "[fem][calculix][nonlinear]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  // A shaft under a torque and a remote load, a plate under a pressure on a
  // pin: every load the deck writes its own way.
  Document doc;
  auto cyl = std::make_unique<CylinderFeature>("Shaft", glm::dvec3(0, 0, 0),
                                               glm::dvec3(0, 0, 1), 6.0, 80.0);
  cyl->SetMaterial("steel");
  doc.AddFeature(std::move(cyl));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Small");
  s.meshSize = 2.5;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  fem::Load t = MakeLoad(doc, fem::LoadType::Torque,
                         FaceWhere(body, "plane", isZ(80.0)), 20.0);
  t.axisOrigin = glm::dvec3(0, 0, 80);
  t.axisDirection = glm::dvec3(0, 0, 1);
  s.loads.push_back(t);
  fem::Load r =
      MakeLoad(doc, fem::LoadType::Remote, FaceWhere(body, "plane", isZ(80.0)),
               1.0, glm::dvec3(1, 0, 0));
  r.point = glm::dvec3(0, 0, 100);
  r.hasPoint = true;
  s.loads.push_back(r);
  s.loads.push_back(MakeLoad(
      doc, fem::LoadType::Pressure,
      FaceWhere(body, "cylinder", [](const glm::dvec3&) { return true; }),
      0.001));
  fem::StudyRuntime own;
  {
    fem::ConvergeOptions never;
    never.mode = fem::ConvergeMode::Never;
    std::string err;
    REQUIRE(fem::RunStudy(doc, s, own, never, &err));
  }
  s.nonlinear.largeDeformation = true;
  const fem::StudyRuntime theirs = RunNonlinear(doc, s, *ccx, scratch, nullptr);
  const auto& a = own.bodies[0].results;
  const auto& b = theirs.bodies[0].results;
  REQUIRE(a.displacement.size() == b.displacement.size());
  double worst = 0.0;
  for (size_t n = 0; n < a.displacement.size(); ++n)
    worst = std::max(worst, glm::length(a.displacement[n] - b.displacement[n]));
  const double rel = worst / a.maxDisplacement;
  std::printf("nonlinear at a small load, shaft (%zu nodes): largest nodal "
              "difference %.3e of the peak %.6g mm; peak von Mises %.6g vs "
              "%.6g MPa; imbalance %.2e\n",
              a.displacement.size(), rel, a.maxDisplacement, b.maxVonMises,
              a.maxVonMises, theirs.bodies[0].nonlinear.imbalance);
  CHECK(rel < 1e-3);
  CHECK_THAT(b.maxVonMises, Catch::Matchers::WithinRel(a.maxVonMises, 1e-3));
}

TEST_CASE("a shallow arch snaps through at its closed form's load, half its "
          "linear buckling factor",
          "[fem][calculix][nonlinear][slow]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  // A circular arch between two pins, a rectangular h x b section, rise e
  // over the span L between the pins' axes, under a uniform pressure on its
  // crown. Shallow-arch theory (Timoshenko and Gere, sec. 7.6, with a
  // sinusoidal arch's first harmonic): a circle of rise e is the sine
  // e1 sin(pi x / L) with e1 = 32 e / pi^3, a uniform load q its harmonic
  // 4 q / pi, and with r = h / sqrt(12), eps = e1 / r and k = pi / L the
  // load on the harmonic is EI k^4 r [(eps - xi) + (eps^2 - xi^2) xi / 4]
  // at crown motion (eps - xi) r: a limit point at xi^2 = (eps^2 - 4) / 3,
  // symmetric for 2 < eps < sqrt(22).
  constexpr double L = 200.0, h = 4.0, b = 4.0, d = 1.6, over = 3.0;
  const double r = h / std::sqrt(12.0);
  constexpr double eps = 4.0;
  const double e = eps * r * kPiNl * kPiNl * kPiNl / 32.0;
  const double R = (L * L / 4.0 + e * e) / (2.0 * e);
  const double zc = e - R; // the centre line's centre
  Document doc;
  auto outer = std::make_unique<CylinderFeature>(
      "Outer", glm::dvec3(L / 2, 0, zc), glm::dvec3(0, 1, 0), R + h / 2, b);
  outer->SetMaterial("steel");
  doc.AddFeature(std::move(outer));
  uint64_t solid = doc.GetFeature(0)->GetId();
  const auto cut = [&](std::unique_ptr<Feature> tool) {
    doc.AddFeature(std::move(tool));
    const uint64_t t = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
    doc.AddFeature(
        std::make_unique<BooleanFeature>("Cut", solid, t, BooleanOp::Subtract));
    solid = doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  };
  cut(std::make_unique<CylinderFeature>("Inner", glm::dvec3(L / 2, -1, zc),
                                        glm::dvec3(0, 1, 0), R - h / 2, b + 2));
  const double big = 4.0 * R;
  cut(std::make_unique<BoxFeature>("Left", glm::dvec3(-over - big, -1, -big),
                                   big, b + 2, 2 * big));
  cut(std::make_unique<BoxFeature>("Right", glm::dvec3(L + over, -1, -big), big,
                                   b + 2, 2 * big));
  cut(std::make_unique<BoxFeature>("Below", glm::dvec3(-big, -1, -big - 20),
                                   2 * big, b + 2, big));
  for (double x : {0.0, L})
    cut(std::make_unique<CylinderFeature>("Pin", glm::dvec3(x, -1, 0),
                                          glm::dvec3(0, 1, 0), d / 2, b + 2));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  // The crown is every face of the outer cylinder: its seam may split it.
  ElementRef pins[2];
  std::vector<ElementRef> crown;
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    REQUIRE(info.has_value());
    if (info->kind != "cylinder")
      continue;
    const ElementRef ref{body.createdBy, name};
    if (std::abs(info->radius - d / 2) < 1e-6)
      pins[std::abs(info->centroid.x) < 1.0 ? 0 : 1] = ref;
    else if (std::abs(info->radius - (R + h / 2)) < 1e-6)
      crown.push_back(ref);
  }
  REQUIRE_FALSE(pins[0].name.empty());
  REQUIRE_FALSE(pins[1].name.empty());
  REQUIRE_FALSE(crown.empty());
  fem::Study& s = doc.AddStudy("Arch");
  s.meshSize =
      std::getenv("ARCH_MESH") ? std::atof(std::getenv("ARCH_MESH")) : 3.0;
  for (const ElementRef& p : pins)
    s.loads.push_back(MakeLoad(doc, fem::LoadType::Pin, p));
  constexpr double p0 = 1.0; // MPa
  for (const ElementRef& c : crown)
    s.loads.push_back(MakeLoad(doc, fem::LoadType::Pressure, c, p0));
  s.nonlinear.largeDeformation = true;
  s.nonlinear.snapThrough = true;
  s.nonlinear.hasImperfection = true; // the perfect arch theory describes
  s.nonlinear.imperfectionMm = 0.0;
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunNonlinear(doc, s, *ccx, scratch, &seconds);
  const fem::StudyBodyResult::Nonlinear& n = rt.bodies[0].nonlinear;
  const double E = Steel().youngGPa * 1000.0;
  const double I = b * h * h * h / 12.0;
  const double k = kPiNl / L;
  const double xi = std::sqrt((eps * eps - 4.0) / 3.0);
  const double q1 = E * I * std::pow(k, 4) * r *
                    ((eps - xi) + (eps * eps - xi * xi) * xi / 4);
  const double closed = kPiNl / 4.0 * q1 / b / p0; // the factor on p0
  std::printf("nonlinear arch, eps %.1f (rise %.3f mm over %.0f), %zu nodes, "
              "%d ccx runs, %.1f s: peak %.5g (%s, [%.5g, %.5g]) against the "
              "closed form %.5g (%+.2f %%); linear buckling %.5g; imbalance "
              "%.2e\n",
              eps, e, L, rt.bodies[0].mesh.stats.nodeCount, n.ccxRuns, seconds,
              n.peakFactor, n.peakAt.c_str(), n.peakLow, n.peakHigh, closed,
              100.0 * (n.peakFactor / closed - 1.0), n.linearFactor,
              n.imbalance);
  REQUIRE(n.solved);
  CHECK((n.peakAt == "limit_point" || n.peakAt == "snap"));
  CHECK_THAT(n.peakFactor, Catch::Matchers::WithinRel(closed, 0.05));
  CHECK(n.peakHigh - n.peakLow < 0.02 * n.peakFactor);
  // What linear buckling cannot see: the arch snaps well below it.
  CHECK(n.peakFactor < 0.7 * n.linearFactor);
  CHECK(n.imbalance < fem::kNonlinearBalance);
}

TEST_CASE("a column seeded with its first mode peaks below its linear factor, "
          "and nearer it as the seed shrinks",
          "[fem][calculix][nonlinear][slow]") {
  REQUIRE_CCX(ccx);
  std::vector<double> peaks;
  double linear = 0.0;
  for (double seed : {1.0, 0.1}) {
    Scratch scratch;
    Document doc;
    auto box = std::make_unique<BoxFeature>("Column", glm::dvec3(0), 10.0, 10.0,
                                            200.0);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
    const Body& body = doc.GetBodies()[0];
    fem::Study& s = doc.AddStudy("Column");
    s.meshSize = 5.0;
    s.loads.push_back(MakeLoad(
        doc, fem::LoadType::Fixed,
        FaceWhere(body, "plane", [](auto c) { return std::abs(c.z) < 1e-6; })));
    s.loads.push_back(
        MakeLoad(doc, fem::LoadType::Force,
                 FaceWhere(body, "plane",
                           [](auto c) { return std::abs(c.z - 200.0) < 1e-6; }),
                 1000.0, glm::dvec3(0, 0, -1)));
    s.nonlinear.largeDeformation = true;
    s.nonlinear.snapThrough = true;
    s.nonlinear.hasImperfection = true;
    s.nonlinear.imperfectionMm = seed;
    double seconds = 0.0;
    const fem::StudyRuntime rt = RunNonlinear(doc, s, *ccx, scratch, &seconds);
    const fem::StudyBodyResult::Nonlinear& n = rt.bodies[0].nonlinear;
    REQUIRE(n.solved);
    std::printf("nonlinear column, seed %.2g mm, %zu nodes, %d ccx runs, %.1f "
                "s: peak %.5g (%s at %.3g mm, [%.5g, %.5g]), linear %.5g "
                "(%.2f %% below); imbalance %.2e\n",
                seed, rt.bodies[0].mesh.stats.nodeCount, n.ccxRuns, seconds,
                n.peakFactor, n.peakAt.c_str(), n.deflectionLimitMm, n.peakLow,
                n.peakHigh, n.linearFactor,
                100.0 * (1.0 - n.peakFactor / n.linearFactor), n.imbalance);
    CHECK(n.peakAt == "deflection_limit");
    CHECK(n.peakFactor < n.linearFactor);
    CHECK_THAT(n.deflectionLimitMm,
               Catch::Matchers::WithinRel(
                   0.1 * std::sqrt(200.0 * 200.0 + 200.0), 1e-3));
    CHECK(n.imbalance < fem::kNonlinearBalance);
    peaks.push_back(n.peakFactor);
    linear = n.linearFactor;
  }
  CHECK(peaks[1] > peaks[0]);
  CHECK(peaks[1] > 0.98 * linear);
}

// The nonlinear memory estimate's data: ccx's peak resident memory on the
// 10 x 10 x 200 column's large-deformation deck (a sideways pull, ten
// increments), by mesh size. Prints; the estimate must bound each.
TEST_CASE("calibrate the CalculiX nonlinear memory estimate",
          "[.][calculix-nl-calibrate]") {
  REQUIRE_CCX(ccx);
  const char* sizes = std::getenv("LOBOLE_CALIBRATE_SIZES");
  std::vector<double> list;
  for (std::istringstream in(sizes ? sizes : "5 3.5 2.5 1.8 1.4 1.2");;) {
    double v = 0;
    if (!(in >> v))
      break;
    list.push_back(v);
  }
  for (double size : list) {
    Scratch scratch;
    Document doc;
    auto box = std::make_unique<BoxFeature>("Column", glm::dvec3(0), 10.0, 10.0,
                                            200.0);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
    const Body& body = doc.GetBodies()[0];
    fem::Study& s = doc.AddStudy("Column");
    s.meshSize = size;
    s.nonlinear.largeDeformation = true;
    s.loads.push_back(MakeLoad(
        doc, fem::LoadType::Fixed,
        FaceWhere(body, "plane", [](auto c) { return std::abs(c.z) < 1e-6; })));
    s.loads.push_back(
        MakeLoad(doc, fem::LoadType::Force,
                 FaceWhere(body, "plane",
                           [](auto c) { return std::abs(c.z - 200.0) < 1e-6; }),
                 2000.0, glm::dvec3(1, 0, 0)));
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
    std::printf("nonlinear mesh %.2f mm: %zu dof, ccx peak %.1f MB in %.1f s, "
                "estimate %.1f MB\n",
                size, dofs, child.PeakBytes() / 1e6, secs,
                fem::CalculixNonlinearMemoryEstimate(dofs) / 1e6);
    CHECK(fem::CalculixNonlinearMemoryEstimate(dofs) >= child.PeakBytes());
  }
}

namespace {

// A contact study run as run_study runs one (docs/simulation/README.md
// #contact), the time it took in `seconds`.
fem::StudyRuntime RunContact(const fem::StudyPlan& plan,
                             const fem::CalculixInstall& ccx,
                             const Scratch& scratch, double* seconds) {
  fem::SolveOptions options;
  options.calculix = RunWith(ccx, scratch.dir / "runs");
  fem::ConvergeOptions converge;
  converge.mode = fem::ConvergeMode::Never;
  fem::StudyJobInput input;
  fem::StudyRuntime rt;
  std::string err;
  const auto t0 = std::chrono::steady_clock::now();
  const bool ok = fem::SolveConverged(plan, input, rt, converge, &err, options);
  *seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
          .count();
  INFO(err);
  REQUIRE(ok);
  REQUIRE(rt.bodies.size() == 1);
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
  return rt;
}

} // namespace

TEST_CASE("a block on the ground lifts off under a moment only past its "
          "kern, its contact carrying the load where statics says",
          "[fem][calculix][contact]") {
  REQUIRE_CCX(ccx);
  // A 20 x 20 x 40 steel block standing on a frictionless rigid floor, a
  // force P down at the top, e off the axis (a remote load: P/A plus the
  // linear traction of its moment, exactly). Plane sections in a prism
  // under end traction stay plane, and a frictionless floor holds only
  // normal to it, so while the whole base presses, the contact pressure
  // is linear: P/A (1 + 12 e x / b^2), its peak P/A (1 + 6 e / b), and the
  // base closes everywhere exactly when e <= b/6 (the kern). Past it the
  // base lifts off; wherever it lands, statics puts the floor's resultant
  // on the load's line, x = e.
  constexpr double b = 20.0, H = 40.0, P = 4000.0, A = b * b;
  for (const double e : {b / 12.0, b / 4.0}) {
    Scratch scratch;
    Document doc;
    auto box = std::make_unique<BoxFeature>(
        "Block", glm::dvec3(-b / 2, -b / 2, 0.0), b, b, H);
    box->SetMaterial("steel");
    doc.AddFeature(std::move(box));
    const Body& body = doc.GetBodies()[0];
    fem::Study& s = doc.AddStudy("Rocking");
    s.meshSize = 4.0;
    s.nonlinear.increments = 2;
    const auto isZ = [](double z) {
      return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
    };
    fem::Load floor = MakeLoad(doc, fem::LoadType::Contact,
                               FaceWhere(body, "plane", isZ(0.0)));
    s.loads.push_back(floor);
    fem::Load push =
        MakeLoad(doc, fem::LoadType::Remote, FaceWhere(body, "plane", isZ(H)),
                 P, glm::dvec3(0, 0, -1));
    push.point = glm::dvec3(e, 0.0, H);
    push.hasPoint = true;
    s.loads.push_back(push);
    fem::StudyPlan plan;
    std::string err;
    REQUIRE(fem::PlanStudy(doc, s, plan, &err));
    double seconds = 0.0;
    const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
    const fem::StudyBodyResult& r = rt.bodies[0];
    REQUIRE(r.contacts.size() == 1);
    const auto& c = r.contacts[0];
    const double arm = -c.momentNmm.y / c.forceN.z;
    const double peak = P / A * (1.0 + 6.0 * e / b);
    std::printf("contact: block on the ground, e = b/%.0f, %zu nodes, %.1f s: "
                "normal %.6g N (P %g), arm %.5f mm (e %.5f, %+.3f %%), closed "
                "%.2f of %.2f mm^2, max pressure %.4f MPa (linear %.4f, "
                "%+.2f %%), penalty %.4g MPa/mm, springs %.2e, imbalance "
                "%.2e\n",
                b / e, r.mesh.stats.nodeCount, seconds, c.normalN, P, arm, e,
                100.0 * (arm / e - 1.0), c.closedMm2, c.areaMm2,
                c.maxPressureMPa, peak, 100.0 * (c.maxPressureMPa / peak - 1.0),
                c.penaltyMPaPerMm, r.nonlinear.stabilization,
                r.nonlinear.imbalance);
    CHECK_THAT(c.areaMm2, Catch::Matchers::WithinRel(A, 1e-6));
    CHECK_THAT(c.normalN, Catch::Matchers::WithinRel(P, 1e-3));
    CHECK_THAT(c.forceN.z, Catch::Matchers::WithinRel(P, 1e-3));
    CHECK_THAT(arm, Catch::Matchers::WithinRel(e, 5e-3));
    CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
    CHECK(r.nonlinear.stabilization < 1e-4);
    CHECK_FALSE(r.nonlinear.largeDeformation);
    // The support's own row says the same.
    REQUIRE(r.results.reactions.size() == 1);
    CHECK(r.results.reactions[0].type == fem::LoadType::Contact);
    CHECK(r.results.reactions[0].contactAreaMm2 == c.closedMm2);
    if (e <= b / 6.0) {
      CHECK_THAT(c.closedMm2, Catch::Matchers::WithinRel(A, 1e-3));
      CHECK_THAT(c.maxPressureMPa, Catch::Matchers::WithinRel(peak, 0.03));
    } else {
      CHECK(c.closedMm2 < 0.9 * A);
      CHECK(c.closedMm2 > 0.3 * A);
    }
  }
}

TEST_CASE("a thin bridge held by its contact alone is held by its contact, "
          "not by the weak springs",
          "[fem][calculix][contact]") {
  REQUIRE_CCX(ccx);
  // A 1 mm steel strip 60 x 6 on two 4 mm feet, standing on a
  // frictionless rigid floor, pressed down by 1 N over its top. Its span
  // bends at about 57 N/mm (384 E I / (5 L^3) over the 52 mm between the
  // feet), not far above the 1e-6 E L = 12.7 N/mm the weak springs once
  // were: they carried 7.8 % of the load and the study was refused. Scaled
  // from the part's own stiffness under its load they carry 4.5e-5, and
  // the floor carries the load.
  constexpr double L = 60.0, B = 6.0, t = 1.0, F = 4.0, P = 1.0;
  Scratch scratch;
  Document doc;
  auto strip =
      std::make_unique<BoxFeature>("Strip", glm::dvec3(0, 0, F), L, B, t);
  strip->SetMaterial("steel");
  doc.AddFeature(std::move(strip));
  const uint64_t s0 = doc.GetFeature(0)->GetId();
  doc.AddFeature(
      std::make_unique<BoxFeature>("Foot A", glm::dvec3(0, 0, 0), F, B, F));
  const uint64_t a = doc.GetFeature(1)->GetId();
  doc.AddFeature(
      std::make_unique<BoxFeature>("Foot B", glm::dvec3(L - F, 0, 0), F, B, F));
  const uint64_t b = doc.GetFeature(2)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("A", s0, a, BooleanOp::Union));
  const uint64_t sa = doc.GetFeature(3)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Bridge", sa, b, BooleanOp::Union));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Bridge");
  s.meshSize = t;
  s.nonlinear.increments = 2;
  const auto at = [](double x, double z) {
    return [x, z](const glm::dvec3& c) {
      return std::abs(c.z - z) < 1e-6 && std::abs(c.x - x) < 1e-6;
    };
  };
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Contact,
                             FaceWhere(body, "plane", at(F / 2, 0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Contact,
                             FaceWhere(body, "plane", at(L - F / 2, 0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", at(L / 2, F + t)), P,
                             glm::dvec3(0, 0, -1)));
  for (const fem::Load& l : s.loads)
    REQUIRE_FALSE(l.face.name.empty());
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  REQUIRE(r.contacts.size() == 2);
  double normal = 0.0;
  for (const auto& c : r.contacts)
    normal += c.normalN;
  std::printf("contact: thin bridge on the ground, %zu nodes, %.1f s: floor "
              "%.6g N (P %g), springs %.2e, imbalance %.2e\n",
              r.mesh.stats.nodeCount, seconds, normal, P,
              r.nonlinear.stabilization, r.nonlinear.imbalance);
  CHECK_THAT(normal, Catch::Matchers::WithinRel(P, 1e-3));
  CHECK(r.nonlinear.stabilization < 1e-4);
  CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
}

TEST_CASE("a part on a frictionless contact pushed sideways is refused: the "
          "weak springs would hold it",
          "[fem][calculix][contact]") {
  REQUIRE_CCX(ccx);
  // What the equilibrium check on the weak springs exists for: nothing
  // but they resist the push along a frictionless floor, so they carry it
  // all, and the answer is refused rather than reported.
  Scratch scratch;
  Document doc;
  auto box = std::make_unique<BoxFeature>("Block", glm::dvec3(-10, -10, 0),
                                          20.0, 20.0, 20.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Sliding");
  s.meshSize = 5.0;
  s.nonlinear.increments = 2;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Contact,
                             FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(20.0)), 1000.0,
                             glm::normalize(glm::dvec3(0.2, 0, -1))));
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  fem::SolveOptions options;
  options.calculix = RunWith(*ccx, scratch.dir / "runs");
  fem::ConvergeOptions converge;
  converge.mode = fem::ConvergeMode::Never;
  fem::StudyJobInput input;
  fem::StudyRuntime rt;
  CHECK_FALSE(fem::SolveConverged(plan, input, rt, converge, &err, options));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("weak springs"));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("friction"));
}

namespace {

// Two parts placed as they are, for an assembly study.
struct TwoParts {
  std::unique_ptr<Document> a = std::make_unique<Document>();
  std::unique_ptr<Document> b = std::make_unique<Document>();
  AssemblyDocument asmDoc;
  uint64_t occA = 0, occB = 0;
  fem::AssemblyStudyContext ctx;
  void Place(const char* nameA, const char* nameB) {
    Occurrence oa;
    oa.name = nameA;
    oa.partId = 1;
    oa.fixed = true;
    occA = asmDoc.AddOccurrence(oa);
    Occurrence ob;
    ob.name = nameB;
    ob.partId = 2;
    occB = asmDoc.AddOccurrence(ob);
    ctx.placedBodies = [this](uint64_t occ) {
      return (occ == occA ? a : b)->GetBodies();
    };
  }
};

// A ball's cap under a stem: a sphere of radius R touching z = `at` from
// above, cut to its lowest `cap` mm, and a cylinder of the cap's rim
// radius `stem` mm tall on it -- so the curved face is only the cap, and
// refining it refines no more than the contact needs.
void AddBall(Document& doc, double R, double at, double cap, double stem) {
  auto sphere =
      std::make_unique<SphereFeature>("Ball", glm::dvec3(0, 0, at + R), R);
  sphere->SetMaterial("steel");
  doc.AddFeature(std::move(sphere));
  const uint64_t ball = doc.GetFeature(0)->GetId();
  doc.AddFeature(std::make_unique<BoxFeature>("Cap", glm::dvec3(-R, -R, at),
                                              2 * R, 2 * R, cap));
  const uint64_t box = doc.GetFeature(1)->GetId();
  doc.AddFeature(std::make_unique<BooleanFeature>("Keep", ball, box,
                                                  BooleanOp::Intersect));
  const uint64_t kept = doc.GetFeature(2)->GetId();
  doc.AddFeature(std::make_unique<CylinderFeature>(
      "Stem", glm::dvec3(0, 0, at + cap), glm::dvec3(0, 0, 1),
      std::sqrt(2.0 * R * cap - cap * cap), stem));
  const uint64_t st = doc.GetFeature(3)->GetId();
  doc.AddFeature(
      std::make_unique<BooleanFeature>("Join", kept, st, BooleanOp::Union));
}

} // namespace

TEST_CASE("a steel ball pressed on a rigid flat touches over Hertz's "
          "circle",
          "[fem][calculix][contact][slow]") {
  REQUIRE_CCX(ccx);
  // Hertz: a sphere of radius R pressed with F on a rigid flat touches over
  // a circle of radius a = (3 F R / 4 E*)^(1/3), E* = E / (1 - nu^2), its
  // pressure peaking at 3 F / (2 pi a^2) in the middle. The ball (meshed)
  // rests on a rigid block (a contact support on it); F is chosen for
  // a = R / 10, and the cap reaches out to 2a.
  constexpr double R = 10.0, top = 4.0;
  const double E = Steel().youngGPa * 1000.0, nu = Steel().poisson;
  const double Estar = E / (1.0 - nu * nu);
  const double aWant = R / 10.0;
  const double F = 4.0 * Estar * aWant * aWant * aWant / (3.0 * R);
  const double cap = 4.0 * aWant * aWant / (2.0 * R);
  Scratch scratch;
  TwoParts parts;
  {
    auto box = std::make_unique<BoxFeature>("Block", glm::dvec3(-6, -6, 0),
                                            12.0, 12.0, top);
    box->SetMaterial("steel");
    parts.a->AddFeature(std::move(box));
  }
  AddBall(*parts.b, R, top, cap, 3.0);
  REQUIRE(parts.b->GetBodies().size() == 1);
  parts.Place("Block", "Ball");
  const Body& ball = parts.b->GetBodies()[0];
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& p) { return std::abs(p.z - z) < 1e-6; };
  };
  fem::Study s;
  s.id = 2;
  s.name = "Hertz";
  s.meshSize = 1.0;
  s.participants.push_back({parts.occB, fem::ParticipantRole::Meshed});
  s.participants.push_back({parts.occA, fem::ParticipantRole::Rigid});
  fem::Load rest;
  rest.id = 20;
  rest.type = fem::LoadType::Contact;
  rest.contactWith = parts.occA;
  s.loads.push_back(rest);
  fem::Load press;
  press.id = 21;
  press.type = fem::LoadType::Force;
  press.occurrenceId = parts.occB;
  press.face = FaceWhere(ball, "plane", isZ(top + cap + 3.0));
  press.magnitude = F;
  press.direction = glm::dvec3(0, 0, -1);
  s.loads.push_back(press);
  s.refinements.push_back(
      {FaceWhere(ball, "sphere", [](const glm::dvec3&) { return true; }),
       aWant / 6.0, "", parts.occB});
  fem::StudyPlan plan;
  std::string err;
  const bool planned =
      fem::PlanAssemblyStudy(parts.asmDoc, s, parts.ctx, plan, &err);
  INFO(err);
  REQUIRE(planned);
  REQUIRE(plan.bodies.size() == 1);
  REQUIRE(plan.bodies[0].contacts.size() == 1);
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  REQUIRE(r.contacts.size() == 1);
  const auto& k = r.contacts[0];
  CHECK_FALSE(k.participant);
  const double a = std::sqrt(k.closedMm2 / kPiNl);
  const double p0 = 3.0 * F / (2.0 * kPiNl * aWant * aWant);
  std::printf("contact: Hertz, a steel ball R %g on a rigid flat, F %.5g N, "
              "%zu nodes, %.1f s: contact radius %.4f mm (Hertz %.4f, %+.2f "
              "%%), normal %.6g N, max pressure %.5g MPa (Hertz %.5g, %+.2f "
              "%%), penalty %.4g MPa/mm, springs %.2e, imbalance %.2e\n",
              R, F, r.mesh.stats.nodeCount, seconds, a, aWant,
              100.0 * (a / aWant - 1.0), k.normalN, k.maxPressureMPa, p0,
              100.0 * (k.maxPressureMPa / p0 - 1.0), k.penaltyMPaPerMm,
              r.nonlinear.stabilization, r.nonlinear.imbalance);
  CHECK_THAT(k.normalN, Catch::Matchers::WithinRel(F, 1e-3));
  CHECK_THAT(a, Catch::Matchers::WithinRel(aWant, 0.05));
  CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
  CHECK(r.nonlinear.stabilization < 1e-4);
  // The support's row carries the force the flat pressed back with.
  REQUIRE(r.results.reactions.size() == 1);
  CHECK(r.results.reactions[0].type == fem::LoadType::Contact);
  CHECK_THAT(r.results.reactions[0].forceN.z,
             Catch::Matchers::WithinRel(F, 1e-3));
}

TEST_CASE("a block standing on a slab, each meshed, presses where statics "
          "says",
          "[fem][calculix][contact]") {
  REQUIRE_CCX(ccx);
  // A contact participant: a 20 x 20 x 30 steel block (meshed apart, held
  // by its contact alone) standing on a 40 x 40 x 10 aluminium slab clamped
  // at its base, pushed down off its axis. Whatever the two deform to,
  // statics puts the slab's push on the load's line.
  constexpr double b = 20.0, H = 30.0, T = 10.0, P = 3000.0, e = 3.0;
  Scratch scratch;
  TwoParts parts;
  {
    auto slab = std::make_unique<BoxFeature>("Slab", glm::dvec3(-20, -20, 0),
                                             40.0, 40.0, T);
    slab->SetMaterial("aluminum_6061_t6");
    parts.a->AddFeature(std::move(slab));
    auto block = std::make_unique<BoxFeature>(
        "Block", glm::dvec3(-b / 2, -b / 2, T), b, b, H);
    block->SetMaterial("steel");
    parts.b->AddFeature(std::move(block));
  }
  parts.Place("Slab", "Block");
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& p) { return std::abs(p.z - z) < 1e-6; };
  };
  fem::Study s;
  s.id = 2;
  s.name = "Standing";
  s.meshSize = 5.0;
  s.nonlinear.increments = 2;
  s.participants.push_back({parts.occA, fem::ParticipantRole::Meshed});
  fem::Participant standing;
  standing.occurrenceId = parts.occB;
  standing.role = fem::ParticipantRole::Contact;
  standing.friction = 0.2;
  s.participants.push_back(standing);
  fem::Load fix;
  fix.id = 20;
  fix.type = fem::LoadType::Fixed;
  fix.occurrenceId = parts.occA;
  fix.face = FaceWhere(parts.a->GetBodies()[0], "plane", isZ(0.0));
  s.loads.push_back(fix);
  fem::Load push;
  push.id = 21;
  push.type = fem::LoadType::Remote;
  push.occurrenceId = parts.occB;
  push.face = FaceWhere(parts.b->GetBodies()[0], "plane", isZ(T + H));
  push.magnitude = P;
  push.direction = glm::dvec3(0, 0, -1);
  push.point = glm::dvec3(e, 0.0, T + H);
  push.hasPoint = true;
  s.loads.push_back(push);
  fem::StudyPlan plan;
  std::string err;
  const bool planned =
      fem::PlanAssemblyStudy(parts.asmDoc, s, parts.ctx, plan, &err);
  INFO(err);
  REQUIRE(planned);
  REQUIRE(plan.bodies.size() == 1);
  REQUIRE(plan.bodies[0].contacts.size() == 1);
  CHECK(plan.bodies[0].contacts[0].participant);
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  REQUIRE(r.contacts.size() == 1);
  const auto& k = r.contacts[0];
  // The force on the slab's side, from the block: down, at x = e.
  const double arm = -k.momentNmm.y / k.forceN.z;
  std::printf("contact: a block standing on a slab, %zu nodes, %.1f s: "
              "normal %.6g N (P %g), arm %.5f mm (e %g, %+.3f %%), closed "
              "%.2f of %.2f mm^2, max pressure %.4f MPa, springs %.2e, "
              "imbalance %.2e\n",
              r.mesh.stats.nodeCount, seconds, k.normalN, P, arm, e,
              100.0 * (arm / e - 1.0), k.closedMm2, k.areaMm2, k.maxPressureMPa,
              r.nonlinear.stabilization, r.nonlinear.imbalance);
  CHECK(k.participant);
  CHECK(k.name == "Block");
  CHECK_THAT(k.normalN, Catch::Matchers::WithinRel(P, 1e-3));
  CHECK_THAT(-k.forceN.z, Catch::Matchers::WithinRel(P, 1e-3));
  CHECK_THAT(arm, Catch::Matchers::WithinRel(e, 1e-2));
  CHECK(k.closedMm2 > 0.0);
  CHECK(k.closedMm2 <= k.areaMm2 * (1.0 + 1e-6));
  CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
  CHECK(r.nonlinear.stabilization < 1e-4);
  // One body: the slab's domain and the block's, each its own material.
  REQUIRE(r.mesh.stats.nodeCount > 0);
}

TEST_CASE("a nonlinear result is saved and read back with its run",
          "[fem][calculix][nonlinear]") {
  fem::StudyRuntime rt;
  fem::StudyBodyResult b;
  b.bodyName = "Arch";
  b.mesh = BoxMesh(2, 1, 1, 5.0);
  b.results.displacement.assign(b.mesh.nodes.size(), glm::dvec3(0.01));
  b.results.vonMises.assign(b.mesh.nodes.size(), 1.0);
  b.results.stress.assign(b.mesh.nodes.size(), fem::Stress{});
  b.nonlinear.solved = true;
  b.nonlinear.snapThrough = true;
  b.nonlinear.factor = 0.48;
  b.nonlinear.maxFactor = 1.9;
  b.nonlinear.increments = 17;
  b.nonlinear.ccxRuns = 2;
  b.nonlinear.curve = {{0.1, 0.2}, {0.48, 2.1}};
  b.nonlinear.linearFactor = 0.95;
  b.nonlinear.imperfectionMm = 0.4;
  b.nonlinear.deflectionLimitMm = 20.0;
  b.nonlinear.peakAt = "snap";
  b.nonlinear.peakFactor = 0.48;
  b.nonlinear.peakLow = 0.48;
  b.nonlinear.peakHigh = 0.483;
  b.nonlinear.firstYieldFactor = 0.3;
  b.nonlinear.residualCriterion = fem::kNonlinearResidual;
  b.nonlinear.imbalance = 2e-6;
  b.nonlinear.largeDeformation = false;
  b.nonlinear.stabilization = 3e-6;
  fem::StudyBodyResult::ContactResult contact;
  contact.id = 9;
  contact.name = "ground";
  contact.friction = 0.2;
  contact.penaltyMPaPerMm = 2.5e6;
  contact.areaMm2 = 400.0;
  contact.closedMm2 = 212.5;
  contact.forceN = glm::dvec3(0.5, 0.0, 4000.0);
  contact.momentNmm = glm::dvec3(0.0, -2.0e4, 0.0);
  contact.normalN = 4000.0;
  contact.shearN = 0.5;
  contact.maxPressureMPa = 31.0;
  contact.meanPressureMPa = 18.8;
  b.contacts.push_back(contact);
  rt.bodies.push_back(std::move(b));
  rt.solver = "calculix 2.23";
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  const fem::StudyRuntime back = DocumentIO::StudyResultFromJson(j);
  REQUIRE(back.bodies.size() == 1);
  const auto& n = back.bodies[0].nonlinear;
  CHECK(n.solved);
  CHECK(n.snapThrough);
  CHECK(n.peakAt == "snap");
  CHECK(n.peakFactor == 0.48);
  CHECK(n.peakHigh == 0.483);
  CHECK(n.curve.size() == 2);
  CHECK(n.firstYieldFactor == 0.3);
  CHECK(n.ccxRuns == 2);
  CHECK(n.linearFactor == 0.95);
  CHECK_FALSE(n.largeDeformation);
  CHECK(n.stabilization == 3e-6);
  REQUIRE(back.bodies[0].contacts.size() == 1);
  const auto& k = back.bodies[0].contacts[0];
  CHECK(k.id == 9);
  CHECK(k.name == "ground");
  CHECK(k.friction == 0.2);
  CHECK(k.closedMm2 == 212.5);
  CHECK(k.momentNmm.y == -2.0e4);
  CHECK(k.maxPressureMPa == 31.0);
  // A summary on a result that is not static is refused.
  j["kind"] = "modal";
  CHECK_THROWS(DocumentIO::StudyResultFromJson(j));
}

// ---- Plasticity (docs/simulation/README.md#plasticity) ---------------------

TEST_CASE("a plastic study is refused where a nonlinear one is, and on a "
          "material without a hardening curve",
          "[fem][calculix][plasticity]") {
  using fem::AnalysisType;
  using fem::ConvergeMode;
  using fem::Solver;
  fem::Nonlinear nl;
  nl.plasticity = true;
  // Plasticity alone is a nonlinear study, at small deformation.
  CHECK(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, nl, 0,
                              ConvergeMode::Auto, false)
            .empty());
  fem::Study st;
  st.nonlinear = nl;
  CHECK(fem::IsNonlinear(st));
  CHECK(fem::EffectiveSolver(st) == Solver::Calculix);
  CHECK_THAT(fem::NonlinearRefusal(Solver::InHouse, AnalysisType::Static, nl, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("a plastic study solves on CalculiX"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Modal, nl, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("a static study's"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Buckling, nl, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("a static study's"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, nl, 2,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("takes no load cases"));
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, nl, 0,
                                   ConvergeMode::Always, false),
             ContainsSubstring("plastic strain"));
  fem::Nonlinear snap = nl;
  snap.snapThrough = true;
  CHECK_THAT(fem::NonlinearRefusal(Solver::Auto, AnalysisType::Static, snap, 0,
                                   ConvergeMode::Auto, false),
             ContainsSubstring("large_deformation: true"));

  // The library's steel carries EN 1993-1-5's linear hardening, E/100 past
  // yield, to 5 %: in plastic strain the modulus E/99.
  const auto& steel = Steel();
  REQUIRE(steel.hardening.size() == 2);
  const double E = steel.youngGPa * 1000.0;
  CHECK(steel.hardening[0].first == steel.yieldMPa);
  CHECK(steel.hardening[0].second == 0.0);
  CHECK(steel.hardening[1].second == 0.05);
  CHECK_THAT((steel.hardening[1].first - steel.hardening[0].first) / 0.05,
             Catch::Matchers::WithinRel(E / 99.0, 1e-12));
  fem::Material m;
  m.youngMPa = E;
  m.yieldMPa = steel.yieldMPa;
  m.hardening = steel.hardening;
  CHECK(fem::HardeningRefusal(m, "steel", "steel").empty());
  // A curve that does not start at the yield, or falls, is refused.
  fem::Material off = m;
  off.hardening[0].first = 240.0;
  CHECK_THAT(fem::HardeningRefusal(off, "steel", "steel"),
             ContainsSubstring("does not start at its yield"));
  fem::Material falls = m;
  falls.hardening[1].first = 200.0;
  CHECK_THAT(fem::HardeningRefusal(falls, "steel", "steel"),
             ContainsSubstring("does not fall"));
  // A printed plastic has none: refused, naming the materials that do.
  CHECK(CADMaterials::MaterialLibrary::HardeningSlugList() == "steel");
  const auto& pla = CADMaterials::MaterialLibrary::GetMaterial(
      CADMaterials::MaterialLibrary::MaterialTypeFromString("pla"));
  CHECK(pla.hardening.empty());

  // A run on a body of it is refused before anything is meshed.
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 10.0, 10.0, 50.0);
  box->SetMaterial("pla");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Yield");
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(50.0)), 1000.0));
  s.nonlinear.plasticity = true;
  fem::StudyPlan plan;
  std::string err;
  CHECK_FALSE(fem::PlanStudy(doc, s, plan, &err));
  CHECK_THAT(err, ContainsSubstring("has no hardening curve"));
  CHECK_THAT(err, ContainsSubstring("PLA"));
  CHECK_THAT(err, ContainsSubstring("the library has one for: steel"));
  CHECK(plan.bodies.empty());
}

TEST_CASE("under its yield a plastic run reads CalculiX's own stress, which "
          "is the elastic one",
          "[fem][calculix][plasticity]") {
  REQUIRE_CCX(ccx);
  // The stress of a plastic run is read at the Gauss points, not computed
  // from U: below yield it must be the elastic stress of the same U, node
  // for node -- which holds every point's corner and component to its own.
  Scratch scratch;
  Document doc;
  auto box =
      std::make_unique<BoxFeature>("Beam", glm::dvec3(0), 6.0, 8.0, 60.0);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Under yield");
  s.meshSize = 3.0;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(60.0)), 50.0,
                             glm::dvec3(1, 0.5, 0.2)));
  s.nonlinear.plasticity = true;
  s.nonlinear.increments = 2;
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunNonlinear(doc, s, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& b = rt.bodies[0];
  REQUIRE(b.nonlinear.solved);
  REQUIRE_FALSE(b.mesh.tets.empty());
  fem::Material steel;
  steel.youngMPa = Steel().youngGPa * 1000.0;
  steel.poisson = Steel().poisson;
  const fem::FemResults own =
      fem::SmallDeformationFields(b.mesh, {steel}, b.results.displacement);
  double worst = 0.0;
  for (size_t n = 0; n < own.stress.size(); ++n)
    for (int k = 0; k < 6; ++k)
      worst =
          std::max(worst, std::abs(own.stress[n][k] - b.results.stress[n][k]));
  std::printf("plastic run under yield, %zu nodes, %.1f s: largest stress "
              "difference %.3e MPa of a peak von Mises %.6g (elastic %.6g); "
              "plastic strain %.3g, residual %.3g mm\n",
              b.mesh.stats.nodeCount, seconds, worst, b.results.maxVonMises,
              own.maxVonMises, b.results.maxPlasticStrain,
              b.results.maxResidualDisplacement);
  CHECK(b.results.maxVonMises < Steel().yieldMPa);
  // U is read back at the seven digits ccx prints, and a stress computed
  // from it carries that rounding amplified by the element's stiffness
  // (docs/simulation/README.md#reading-the-answer-back); measured 3.8e-4
  // of the peak here. A point read at the wrong corner, or xz read as yz,
  // is over 1e-2 (the shear is 2.5 % of the peak).
  CHECK(worst < 1e-3 * own.maxVonMises);
  CHECK_THAT(b.results.maxVonMises,
             Catch::Matchers::WithinRel(own.maxVonMises, 1e-5));
  CHECK(b.results.maxPlasticStrain == 0.0);
  CHECK(b.nonlinear.unloaded);
  // Elastic all the way: nothing is left once the load is off.
  CHECK(b.results.maxResidualDisplacement < 1e-6 * b.results.maxDisplacement);
  CHECK(b.nonlinear.firstYieldFactor == 0.0);
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

TEST_CASE("a bar pressed past its yield yields uniformly, as its hardening "
          "line says, and is left shorter by its plastic strain",
          "[fem][calculix][plasticity]") {
  REQUIRE_CCX(ccx);
  // A 10 x 10 x 40 steel bar standing on a frictionless ground, pressed
  // down on its top face to sigma = 300 MPa. Nothing holds its sides or
  // its base against spreading, so the stress is uniaxial and the same
  // everywhere: von Mises sigma, and past f_y the plastic strain on the
  // library's line, (sigma - f_y) / H with H = E/99, exactly. Unloaded,
  // the bar keeps that strain: shorter by L eps_p, wider by b eps_p / 2
  // (plastic flow keeps the volume).
  constexpr double b = 10.0, L = 40.0, sigma = 300.0;
  Scratch scratch;
  Document doc;
  auto box = std::make_unique<BoxFeature>(
      "Bar", glm::dvec3(-b / 2, -b / 2, 0.0), b, b, L);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Past yield");
  s.meshSize = 5.0;
  s.nonlinear.plasticity = true;
  s.nonlinear.increments = 6;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Contact,
                             FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane", isZ(L)), sigma * b * b,
                             glm::dvec3(0, 0, -1)));
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  const auto& steel = Steel();
  const double E = steel.youngGPa * 1000.0, fy = steel.yieldMPa;
  const double H = (steel.hardening[1].first - steel.hardening[0].first) /
                   steel.hardening[1].second;
  const double epsP = (sigma - fy) / H;
  double peMin = 1e300, peMax = 0.0, vmMin = 1e300, vmMax = 0.0;
  double topZ = 0.0, botZ = 0.0, sideX = 0.0;
  int nTop = 0, nBot = 0, nSide = 0;
  const auto& m = r.mesh;
  for (size_t n = 0; n < m.nodes.size(); ++n) {
    peMin = std::min(peMin, r.results.plasticStrain[n]);
    peMax = std::max(peMax, r.results.plasticStrain[n]);
    vmMin = std::min(vmMin, r.results.vonMises[n]);
    vmMax = std::max(vmMax, r.results.vonMises[n]);
    const glm::dvec3 u = r.results.residualDisplacement[n];
    if (std::abs(m.nodes[n].z - L) < 1e-6) {
      topZ += u.z;
      ++nTop;
    } else if (std::abs(m.nodes[n].z) < 1e-6) {
      botZ += u.z;
      ++nBot;
    }
    if (std::abs(m.nodes[n].x - b / 2) < 1e-6) {
      sideX += u.x;
      ++nSide;
    } else if (std::abs(m.nodes[n].x + b / 2) < 1e-6) {
      sideX -= u.x;
      ++nSide;
    }
  }
  REQUIRE(nTop > 0);
  REQUIRE(nBot > 0);
  REQUIRE(nSide > 0);
  const double shorter = botZ / nBot - topZ / nTop;
  const double wider = 2.0 * sideX / nSide;
  std::printf("plastic bar, sigma %.0f MPa, %zu nodes, %d increments, %.1f "
              "s: plastic strain %.7f..%.7f (closed form %.7f, %+.2e), von "
              "Mises %.4f..%.4f; unloaded %.6f mm shorter (L eps_p %.6f, "
              "%+.2e), %.6f mm wider (b eps_p / 2 %.6f, %+.2e); springs "
              "%.1e, imbalance %.1e\n",
              sigma, r.mesh.stats.nodeCount, r.nonlinear.increments, seconds,
              peMin, peMax, epsP, peMax / epsP - 1.0, vmMin, vmMax, shorter,
              L * epsP, shorter / (L * epsP) - 1.0, wider, b * epsP / 2.0,
              wider / (b * epsP / 2.0) - 1.0, r.nonlinear.stabilization,
              r.nonlinear.imbalance);
  (void)E;
  REQUIRE(r.nonlinear.plasticity);
  CHECK(r.nonlinear.unloaded);
  CHECK_FALSE(r.nonlinear.collapsed);
  CHECK(r.nonlinear.factor == 1.0);
  // Exact but for what the weak springs that hold a part on a contact
  // alone carry (stabilization, 1.5e-4 of the load measured): taken from
  // the stress near the base, 0.05 MPa there, it is 2.6e-5 of plastic
  // strain on a hardening modulus of 2121 MPa (E/99) -- 1e-3 of it.
  CHECK(r.nonlinear.stabilization < 3e-4);
  CHECK_THAT(peMin, Catch::Matchers::WithinRel(epsP, 2e-3));
  CHECK_THAT(peMax, Catch::Matchers::WithinRel(epsP, 2e-3));
  CHECK_THAT(r.results.maxPlasticStrain,
             Catch::Matchers::WithinRel(epsP, 2e-3));
  CHECK_THAT(vmMin, Catch::Matchers::WithinRel(sigma, 3e-4));
  CHECK_THAT(vmMax, Catch::Matchers::WithinRel(sigma, 3e-4));
  CHECK_THAT(shorter, Catch::Matchers::WithinRel(L * epsP, 2e-3));
  CHECK_THAT(wider, Catch::Matchers::WithinRel(b * epsP / 2.0, 2e-3));
  CHECK(r.nonlinear.firstYieldFactor > 0.0);
  CHECK_THAT(r.nonlinear.firstYieldFactor,
             Catch::Matchers::WithinRel(fy / sigma, 1e-3));
  CHECK(r.nonlinear.strainLimit == 0.05);
  CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
}

TEST_CASE("a rectangular bar bent past its yield collapses at its plastic "
          "moment, M_p = f_y b h^2 / 4",
          "[fem][calculix][plasticity][slow]") {
  REQUIRE_CCX(ccx);
  // A b x h x L bar clamped at one end and bent by a moment M about x at
  // the other (a torque about a transverse axis: the linear traction of
  // pure bending, exactly), put in through a head twice its width and
  // depth: at the bar's own end face that traction yields the face itself
  // from 2/3 M_p on, and the run collapsed there, 17 % low. A perfectly plastic
  // section carries at most M_p = f_y b h^2 / 4, 1.5 times its first yield, and
  // the moment is the same all along, so the bar collapses at M_p where it is
  // free to: away from the clamp, whose face holds the section from spreading
  // across its width and so raises its strength (a tip force, whose moment
  // peaks at the clamp, collapsed 13 % high at L = 10 h and 6 % at 30 h).
  // Loaded to 1.25 M_p, the run stops converging there -- the collapse load is
  // that factor. The curve is stated flat here: the closed form is for perfect
  // plasticity, and the library's steel hardens, which at small
  // deformation has no collapse.
  constexpr double b = 10.0, h = 10.0, L = 60.0, over = 1.25;
  Scratch scratch;
  Document doc;
  auto box = std::make_unique<BoxFeature>("Bar", glm::dvec3(0), b, h, L);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const uint64_t bar = doc.GetFeature(0)->GetId();
  auto head = std::make_unique<BoxFeature>(
      "Head", glm::dvec3(-b / 2, -h / 2, L), 2 * b, 2 * h, h);
  head->SetMaterial("steel");
  doc.AddFeature(std::move(head));
  const uint64_t top = doc.GetFeature(1)->GetId();
  auto joined =
      std::make_unique<BooleanFeature>("Join", bar, top, BooleanOp::Union);
  joined->SetMaterial("steel");
  doc.AddFeature(std::move(joined));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  const double fy = Steel().yieldMPa;
  const double Mp = fy * b * h * h / 4.0;
  fem::Study& s = doc.AddStudy("Collapse");
  s.meshSize = 3.4;
  s.nonlinear.plasticity = true;
  s.nonlinear.increments = 10;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", isZ(0.0))));
  fem::Load m = MakeLoad(doc, fem::LoadType::Torque,
                         FaceWhere(body, "plane", isZ(L + h)), over * Mp);
  m.axisOrigin = glm::dvec3(b / 2, h / 2, L + h);
  m.axisDirection = glm::dvec3(1, 0, 0);
  s.loads.push_back(m);
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  REQUIRE(plan.bodies.size() == 1);
  plan.bodies[0].mat.hardening = {{fy, 0.0}, {fy, 1.0}};
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  const double Mc = r.nonlinear.factor * over * Mp;
  std::printf("plastic collapse, %g x %g x %g bar in bending, %zu nodes, %d "
              "increments, %.1f s: collapses at factor %.5f [%.5f, %.5f] of "
              "1.25 M_p, M = %.0f N mm (M_p = f_y b h^2 / 4 = %.0f N mm, "
              "%+.2f %%); first yield at %.4f (2/3 / 1.25 = %.4f); peak "
              "plastic strain %.3g at z = %.1f; imbalance %.1e\n",
              b, h, L, r.mesh.stats.nodeCount, r.nonlinear.increments, seconds,
              r.nonlinear.factor, r.nonlinear.factor, r.nonlinear.collapseHigh,
              Mc, Mp, 100.0 * (Mc / Mp - 1.0), r.nonlinear.firstYieldFactor,
              (2.0 / 3.0) / over, r.results.maxPlasticStrain,
              r.results.maxPeAt.z, r.nonlinear.imbalance);
  REQUIRE(r.nonlinear.collapsed);
  CHECK_FALSE(r.nonlinear.unloaded);
  CHECK(r.results.residualDisplacement.empty());
  CHECK(r.nonlinear.collapseHigh > r.nonlinear.factor);
  // At 2.5 mm it read +0.42 % (6 670 nodes, 70 s).
  CHECK_THAT(Mc, Catch::Matchers::WithinRel(Mp, 0.02));
  // It yields first where the head's corner concentrates the stress, below
  // the bar's own first yield at 2/3 M_p, and then nowhere near collapse.
  CHECK(r.nonlinear.firstYieldFactor > 0.0);
  CHECK(r.nonlinear.firstYieldFactor < (2.0 / 3.0) / over);
  // The hinge forms in the bar, away from its clamp and its head.
  CHECK(r.results.maxPeAt.z > h);
  CHECK(r.results.maxPeAt.z < L - h);
  CHECK(r.nonlinear.imbalance < fem::kNonlinearBalance);
}

TEST_CASE("a plastic result is saved and read back with its plastic strain "
          "and what unloading left",
          "[fem][calculix][plasticity]") {
  fem::StudyRuntime rt;
  fem::StudyBodyResult b;
  b.bodyName = "Bar";
  b.mesh = BoxMesh(2, 1, 1, 5.0);
  const size_t n = b.mesh.nodes.size();
  b.results.displacement.assign(n, glm::dvec3(0.01));
  b.results.vonMises.assign(n, 300.0);
  b.results.stress.assign(n, fem::Stress{});
  b.results.plasticStrain.assign(n, 0.02475);
  b.results.maxPlasticStrain = 0.02475;
  b.results.maxPeNode = 3;
  b.results.maxPeAt = b.mesh.nodes[3];
  b.results.residualDisplacement.assign(n, glm::dvec3(0.0, 0.0, -0.99));
  b.results.maxResidualDisplacement = 0.99;
  b.nonlinear.solved = true;
  b.nonlinear.factor = 0.8;
  b.nonlinear.plasticity = true;
  b.nonlinear.collapsed = true;
  b.nonlinear.collapseHigh = 0.80002;
  b.nonlinear.unloaded = false;
  b.nonlinear.strainLimit = 0.05;
  b.nonlinear.largeDeformation = false;
  const glm::dvec3 peakAt = b.mesh.nodes[3];
  rt.bodies.push_back(std::move(b));
  rt.solver = "calculix 2.23";
  nlohmann::json j;
  REQUIRE(DocumentIO::StudyResultToJson(rt, j));
  const fem::StudyRuntime back = DocumentIO::StudyResultFromJson(j);
  REQUIRE(back.bodies.size() == 1);
  const auto& nl = back.bodies[0].nonlinear;
  CHECK(nl.plasticity);
  CHECK(nl.collapsed);
  CHECK(nl.collapseHigh == 0.80002);
  CHECK_FALSE(nl.unloaded);
  CHECK(nl.strainLimit == 0.05);
  // Kept as the rest of the fields are: the surface's nodes and the peaks'
  // (this hand-made mesh has no surface, so the peaks' alone).
  const auto& r = back.bodies[0].results;
  const size_t kept = back.bodies[0].mesh.nodes.size();
  REQUIRE(kept > 0);
  REQUIRE(r.plasticStrain.size() == kept);
  REQUIRE(r.displacement.size() == kept);
  CHECK_THAT(r.plasticStrain[0], Catch::Matchers::WithinRel(0.02475, 1e-6));
  CHECK(r.maxPlasticStrain == 0.02475);
  REQUIRE(r.maxPeNode >= 0);
  REQUIRE(static_cast<size_t>(r.maxPeNode) < kept);
  CHECK(glm::length(back.bodies[0].mesh.nodes[r.maxPeNode] - peakAt) < 1e-5);
  REQUIRE(r.residualDisplacement.size() == kept);
  CHECK_THAT(r.residualDisplacement[0].z,
             Catch::Matchers::WithinRel(-0.99, 1e-6));
  CHECK(r.maxResidualDisplacement == 0.99);
}
#endif // HAVE_NETGEN

// ---- Thermal (docs/simulation/README.md#thermal) ----------------------------
// Each heat run is held to our own K_T and Q (SolveHeat with the answer
// given), and its temperatures and flows compared with the in-house solve of
// the same mesh.

namespace {

// The fin of the plan's measurements: a 100 x 10 x 10 bar at 5 mm, k = 50,
// its base held at 100 C, every other face cooled at 25 W/(m^2 K) to 20 C;
// and a source of 1 W through it.
void HeatFin(fem::FemMesh& mesh, fem::HeatLoads& loads,
             std::vector<fem::Material>& mats) {
  mesh = GenerateBoxMesh(20, 2, 2, 100.0, 10.0, 10.0);
  loads = {};
  loads.held.push_back({1, TrisAtPlane(mesh, 0, 0.0, 1e-9), 100.0});
  fem::HeatLoads::Film film;
  film.id = 2;
  film.hMWmm2K = 25.0 * fem::kWm2ToMWmm2;
  film.ambientC = 20.0;
  for (int t = 0; t < static_cast<int>(mesh.boundary.size()); ++t)
    if (mesh.boundary[t].faceIndex != 0 && mesh.boundary[t].faceIndex != 1)
      film.tris.push_back(t);
  loads.films.push_back(film);
  fem::HeatLoads::Flux flux;
  flux.id = 3;
  flux.tris = TrisAtPlane(mesh, 0, 100.0, 1e-9);
  flux.total = true;
  flux.totalMW = 500.0;
  loads.fluxes.push_back(flux);
  loads.sources.push_back({4, {}, 1000.0});
  fem::Material m;
  m.conductivityWmK = 50.0;
  mats = {m};
}

} // namespace

TEST_CASE("CalculiX and the in-house solver agree on a fin's heat",
          "[fem][calculix][thermal]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  fem::FemMesh mesh;
  fem::HeatLoads loads;
  std::vector<fem::Material> mats;
  HeatFin(mesh, loads, mats);
  fem::HeatSolution own;
  std::string err;
  REQUIRE(fem::SolveHeat(mesh, mats, loads, {}, own, &err));
  fem::SolveOptions options;
  options.calculix = RunWith(*ccx, scratch.dir / "runs");
  std::vector<double> T;
  REQUIRE(fem::HeatOnCalculix(mesh, mats, loads, "Fin", options, T, &err));
  INFO(err);
  fem::GivenTemperatures given;
  given.temperature = &T;
  given.precision = fem::kCalculixDatPrecision;
  given.name = ccx->Name();
  fem::HeatSolution theirs;
  REQUIRE(fem::SolveHeat(mesh, mats, loads, {}, theirs, &err, &given));
  double worstT = 0.0;
  for (size_t n = 0; n < T.size(); ++n)
    worstT = std::max(worstT, std::abs(T[n] - own.temperature[n]) /
                                  std::abs(own.temperature[n]));
  double worstFlow = 0.0;
  for (size_t k = 0; k < own.flows.size(); ++k)
    worstFlow = std::max(worstFlow,
                         std::abs(theirs.flows[k].watts - own.flows[k].watts) /
                             std::max(own.inW, own.outW));
  std::printf("calculix heat: fin of %zu nodes, temperatures within %.2e, "
              "flows within %.2e of the throughput; residual %.2e, floor "
              "%.2e, energy %.2e, imbalance %.2e W (floor %.2e)\n",
              mesh.nodes.size(), worstT, worstFlow, theirs.residual,
              theirs.residualFloor, theirs.energyResidual, theirs.imbalanceW,
              theirs.imbalanceFloorW);
  CHECK(worstT < 1e-6);
  CHECK(worstFlow < 1e-5);
  CHECK(EntriesIn(scratch.dir / "runs") == 0);
}

TEST_CASE("a heat deck a tenth of a percent away from its study is refused",
          "[fem][calculix][thermal]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  fem::FemMesh mesh;
  fem::HeatLoads loads;
  std::vector<fem::Material> mats;
  HeatFin(mesh, loads, mats);
  std::vector<fem::Material> off = mats;
  off[0].conductivityWmK *= 1.001;
  fem::SolveOptions options;
  options.calculix = RunWith(*ccx, scratch.dir / "runs");
  std::vector<double> T;
  std::string err;
  REQUIRE(fem::HeatOnCalculix(mesh, off, loads, "Fin", options, T, &err));
  fem::GivenTemperatures given;
  given.temperature = &T;
  given.precision = fem::kCalculixDatPrecision;
  given.name = ccx->Name();
  fem::HeatSolution theirs;
  CHECK_FALSE(fem::SolveHeat(mesh, mats, loads, {}, theirs, &err, &given));
  CHECK_THAT(err, Catch::Matchers::ContainsSubstring("differ"));
}

#ifdef HAVE_NETGEN

namespace {

// A steel bar 100 x 10 x 10 along x, its ends held at 100 and 20 C (thermal
// study `heat`), and a static study clamping both ends under its temperature,
// stress-free at 20 C.
struct HeatedBar {
  Document doc;
  uint64_t heat = 0, clamped = 0;
  HeatedBar(double mesh) {
    auto bar =
        std::make_unique<BoxFeature>("Bar", glm::dvec3(0), 100.0, 10.0, 10.0);
    bar->SetMaterial("steel");
    doc.AddFeature(std::move(bar));
    const Body& body = doc.GetBodies()[0];
    const auto atX = [](double x) {
      return [x](const glm::dvec3& c) { return std::abs(c.x - x) < 1e-6; };
    };
    fem::Study& h = doc.AddStudy("Heat");
    h.type = fem::AnalysisType::Thermal;
    h.meshSize = mesh;
    fem::Load hot = MakeLoad(doc, fem::LoadType::Temperature,
                             FaceWhere(body, "plane", atX(0.0)));
    hot.temperatureC = 100.0;
    fem::Load cold = MakeLoad(doc, fem::LoadType::Temperature,
                              FaceWhere(body, "plane", atX(100.0)));
    cold.temperatureC = 20.0;
    h.loads = {hot, cold};
    heat = h.id;
    fem::Study& st = doc.AddStudy("Clamped");
    st.meshSize = mesh;
    st.loads = {
        MakeLoad(doc, fem::LoadType::Fixed, FaceWhere(body, "plane", atX(0.0))),
        MakeLoad(doc, fem::LoadType::Fixed,
                 FaceWhere(body, "plane", atX(100.0)))};
    st.thermalLoad.kind = fem::ThermalLoad::Kind::FromStudy;
    st.thermalLoad.fromStudy = heat;
    st.thermalLoad.hasStressFree = true;
    st.thermalLoad.stressFreeC = 20.0;
    clamped = st.id;
  }
  fem::StudyRuntime Run(uint64_t id, fem::Solver solver,
                        const fem::CalculixInstall* ccx,
                        const Scratch* scratch) {
    fem::Study* s = doc.StudyForRun(id);
    s->solver = solver;
    fem::StudyPlan plan;
    std::string err;
    REQUIRE(fem::PlanStudy(doc, *s, plan, &err));
    fem::SolveOptions options;
    if (ccx)
      options.calculix = RunWith(*ccx, scratch->dir / "runs");
    fem::ConvergeOptions converge;
    converge.mode = fem::ConvergeMode::Never;
    fem::StudyJobInput input;
    fem::StudyRuntime rt;
    const bool ok =
        fem::SolveConverged(plan, input, rt, converge, &err, options);
    INFO(err);
    REQUIRE(ok);
    return rt;
  }
};

} // namespace

TEST_CASE("CalculiX and the in-house solver agree on a thermal study and a "
          "static one under its temperature",
          "[fem][calculix][thermal]") {
  REQUIRE_CCX(ccx);
  Scratch scratch;
  HeatedBar bar(5.0);
  const fem::StudyRuntime own =
      bar.Run(bar.heat, fem::Solver::InHouse, nullptr, nullptr);
  const fem::StudyRuntime theirs =
      bar.Run(bar.heat, fem::Solver::Calculix, &*ccx, &scratch);
  CHECK(theirs.solver == ccx->Name());
  const auto& a = own.bodies[0];
  const auto& b = theirs.bodies[0];
  REQUIRE(a.results.temperature.size() == b.results.temperature.size());
  double worst = 0.0;
  for (size_t n = 0; n < a.results.temperature.size(); ++n)
    worst = std::max(
        worst, std::abs(a.results.temperature[n] - b.results.temperature[n]) /
                   a.results.temperature[n]);
  double flow = 0.0;
  for (size_t k = 0; k < a.thermal.flows.size(); ++k)
    flow = std::max(
        flow, std::abs(a.thermal.flows[k].watts - b.thermal.flows[k].watts) /
                  std::abs(a.thermal.flows[k].watts));
  std::printf("calculix thermal study: %zu nodes, temperatures within %.2e, "
              "flows within %.2e (%.6f W in-house, %.6f W ccx)\n",
              a.mesh.stats.nodeCount, worst, flow, a.thermal.flows[0].watts,
              b.thermal.flows[0].watts);
  CHECK(worst < 1e-6);
  CHECK(flow < 1e-5);

  const fem::StudyRuntime sOwn =
      bar.Run(bar.clamped, fem::Solver::InHouse, nullptr, nullptr);
  const fem::StudyRuntime sTheirs =
      bar.Run(bar.clamped, fem::Solver::Calculix, &*ccx, &scratch);
  const double dRel = Rel(sTheirs.bodies[0].results.maxDisplacement,
                          sOwn.bodies[0].results.maxDisplacement);
  const double vRel = Rel(sTheirs.bodies[0].results.maxVonMises,
                          sOwn.bodies[0].results.maxVonMises);
  std::printf("calculix static under a thermal study's temperature: max "
              "displacement %.9g vs %.9g (rel %.2e), max von Mises %.9g vs "
              "%.9g (rel %.2e), residual %.2e floor %.2e energy %.2e\n",
              sTheirs.bodies[0].results.maxDisplacement,
              sOwn.bodies[0].results.maxDisplacement, dRel,
              sTheirs.bodies[0].results.maxVonMises,
              sOwn.bodies[0].results.maxVonMises, vRel, sTheirs.residual,
              sTheirs.residualFloor, sTheirs.energyResidual);
  CHECK(dRel < 1e-5);
  CHECK(vRel < 1e-5);
  CHECK(sTheirs.bodies[0].thermal.load);
}

TEST_CASE("a steel bar clamped along its axis, heated past yield and cooled, "
          "keeps the plastic strain its hardening line says",
          "[fem][calculix][thermal][plasticity]") {
  REQUIRE_CCX(ccx);
  // A 10 x 10 x 40 steel bar between two frictionless grounds, heated
  // uniformly by 200 K: held along its axis and free across it, its strain
  // is alpha dT, which past f_y / E is taken up plastically on the
  // library's line, (alpha dT - f_y / E) / (1 + H / E). Cooled back to where
  // it was stress-free, it keeps that strain.
  constexpr double b = 10.0, L = 40.0, dT = 200.0;
  Scratch scratch;
  Document doc;
  auto box = std::make_unique<BoxFeature>(
      "Bar", glm::dvec3(-b / 2, -b / 2, 0.0), b, b, L);
  box->SetMaterial("steel");
  doc.AddFeature(std::move(box));
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Heated past yield");
  s.meshSize = 5.0;
  s.nonlinear.plasticity = true;
  s.nonlinear.increments = 8;
  const auto isZ = [](double z) {
    return [z](const glm::dvec3& c) { return std::abs(c.z - z) < 1e-6; };
  };
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Contact,
                             FaceWhere(body, "plane", isZ(0.0))));
  s.loads.push_back(
      MakeLoad(doc, fem::LoadType::Contact, FaceWhere(body, "plane", isZ(L))));
  s.thermalLoad.kind = fem::ThermalLoad::Kind::Uniform;
  s.thermalLoad.uniformC = 20.0 + dT;
  s.thermalLoad.hasStressFree = true;
  s.thermalLoad.stressFreeC = 20.0;
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanStudy(doc, s, plan, &err));
  double seconds = 0.0;
  const fem::StudyRuntime rt = RunContact(plan, *ccx, scratch, &seconds);
  const fem::StudyBodyResult& r = rt.bodies[0];
  const auto& steel = Steel();
  const double E = steel.youngGPa * 1000.0, fy = steel.yieldMPa;
  const double H = (steel.hardening[1].first - steel.hardening[0].first) /
                   steel.hardening[1].second;
  const double alpha = steel.expansionPerK;
  // The grounds are penalty contacts: each end overcloses by sigma / k_p
  // (k_p the pair's pressure per overclosure), which the bar's length gives
  // back -- so the closed form with that compliance in series:
  // eps_p (1 + H/E + 2H/(k_p L)) = alpha dT - f_y/E - 2 f_y/(k_p L).
  REQUIRE(r.contacts.size() == 2);
  const double kp = r.contacts[0].penaltyMPaPerMm;
  REQUIRE(kp > 0.0);
  const double epsP = (alpha * dT - fy / E - 2.0 * fy / (kp * L)) /
                      (1.0 + H / E + 2.0 * H / (kp * L));
  double peMin = 1e300, peMax = 0.0, vmMax = 0.0;
  for (size_t n = 0; n < r.mesh.nodes.size(); ++n) {
    peMin = std::min(peMin, r.results.plasticStrain[n]);
    peMax = std::max(peMax, r.results.plasticStrain[n]);
    vmMax = std::max(vmMax, r.results.vonMises[n]);
  }
  std::printf("calculix thermal plasticity: %zu nodes, %.1f s, plastic "
              "strain %.7f..%.7f (closed form %.7f, %+.2e), von Mises max "
              "%.4f (f_y + H eps_p %.4f), springs %.1e, imbalance %.1e\n",
              r.mesh.stats.nodeCount, seconds, peMin, peMax, epsP,
              peMax / epsP - 1.0, vmMax, fy + H * epsP,
              r.nonlinear.stabilization, r.nonlinear.imbalance);
  REQUIRE(r.nonlinear.plasticity);
  CHECK(r.nonlinear.unloaded);
  CHECK_THAT(peMin, Catch::Matchers::WithinRel(epsP, 5e-3));
  CHECK_THAT(peMax, Catch::Matchers::WithinRel(epsP, 5e-3));
  CHECK_THAT(vmMax, Catch::Matchers::WithinRel(fy + H * epsP, 5e-3));
  // Cooled, it pulls away from the grounds, which only push: free, it is
  // left shorter by L eps_p.
  double topZ = 0.0, botZ = 0.0;
  int nTop = 0, nBot = 0;
  for (size_t n = 0; n < r.mesh.nodes.size(); ++n) {
    const double uz = r.results.residualDisplacement[n].z;
    if (std::abs(r.mesh.nodes[n].z - L) < 1e-6) {
      topZ += uz;
      ++nTop;
    } else if (std::abs(r.mesh.nodes[n].z) < 1e-6) {
      botZ += uz;
      ++nBot;
    }
  }
  REQUIRE(nTop > 0);
  REQUIRE(nBot > 0);
  const double shorter = botZ / nBot - topZ / nTop;
  std::printf("calculix thermal plasticity: cooled, %.6f mm shorter (L eps_p "
              "%.6f, %+.2e)\n",
              shorter, L * epsP, shorter / (L * epsP) - 1.0);
  CHECK_THAT(shorter, Catch::Matchers::WithinRel(L * epsP, 5e-3));
}

// A bimetal strip (docs/simulation/README.md#a-thermal-load-on-an-assembly):
// a library steel layer under a C26000 brass one, two parts bonded in an
// assembly, clamped across one end and heated from 20 to 100 C. The tip
// turns by L / rho with Timoshenko's curvature, in-house and on ccx, on the
// one mesh the study made.
TEST_CASE("a steel and brass strip bonded in an assembly curls as "
          "Timoshenko's bimetal, in-house and on CalculiX",
          "[fem][calculix][thermal]") {
  REQUIRE_CCX(ccx);
  const double L = 40.0, b = 5.0, t = 0.5;
  auto lower = std::make_unique<Document>();
  auto upper = std::make_unique<Document>();
  for (auto [d, mat] :
       {std::pair{lower.get(), "steel"}, std::pair{upper.get(), "brass"}}) {
    auto box = std::make_unique<BoxFeature>("Layer", glm::vec3(0.0f), L, b, t);
    box->SetMaterial(mat);
    d->AddFeature(std::move(box));
  }
  AssemblyDocument asmDoc;
  Occurrence lo;
  lo.name = "Steel";
  lo.partId = 1;
  lo.fixed = true;
  const uint64_t lowerOcc = asmDoc.AddOccurrence(lo);
  Occurrence up;
  up.name = "Brass";
  up.partId = 2;
  up.position = glm::vec3(0.0f, 0.0f, static_cast<float>(t));
  const uint64_t upperOcc = asmDoc.AddOccurrence(up);
  fem::AssemblyStudyContext ctx;
  ctx.placedBodies = [&](uint64_t occId) {
    std::vector<Body> out;
    const Document* part = occId == lowerOcc ? lower.get() : upper.get();
    const glm::mat4 world = glm::translate(
        glm::mat4(1.0f), occId == lowerOcc
                             ? glm::vec3(0.0f)
                             : glm::vec3(0.0f, 0.0f, static_cast<float>(t)));
    for (const Body& body : part->GetBodies()) {
      Body nb = body;
      nb.shape = body.shape.Transformed(world);
      out.push_back(std::move(nb));
    }
    return out;
  };
  const auto endAt = [](const Document& d, double x) {
    return FaceWhere(d.GetBodies()[0], "plane", [x](const glm::dvec3& c) {
      return std::abs(c.x - x) < 1e-6;
    });
  };
  fem::Study s;
  s.id = 2;
  s.name = "bimetal";
  s.meshSize = t;
  s.participants.push_back({lowerOcc, fem::ParticipantRole::Meshed, 0.0, ""});
  s.participants.push_back({upperOcc, fem::ParticipantRole::Bonded, 0.0, ""});
  for (auto [occ, d] :
       {std::pair{lowerOcc, lower.get()}, std::pair{upperOcc, upper.get()}}) {
    fem::Load fix;
    fix.id = 20 + occ;
    fix.type = fem::LoadType::Fixed;
    fix.occurrenceId = occ;
    fix.face = ElementRef{0, endAt(*d, 0.0).name};
    s.loads.push_back(fix);
  }
  s.thermalLoad.kind = fem::ThermalLoad::Kind::Uniform;
  s.thermalLoad.uniformC = 100.0;
  s.thermalLoad.hasStressFree = true;
  s.thermalLoad.stressFreeC = 20.0;
  fem::StudyPlan plan;
  std::string err;
  REQUIRE(fem::PlanAssemblyStudy(asmDoc, s, ctx, plan, &err));
  INFO(err);
  fem::StudyJobInput input;
  REQUIRE(fem::MeshPlannedStudy(plan, input, &err));
  REQUIRE(input.bodies.size() == 1);
  REQUIRE(input.bodies[0].materials.size() == 2);
  const Agreement a = SolveBoth(input, *ccx, "steel/brass bimetal");
  CHECK(a.residual < 10.0 * std::max(a.floor, 1e-6));

  // The tip, read the way probe_study reads a face: the free end's facets
  // (both layers'), fitted as a rigid body.
  Scratch scratch;
  fem::StudyJobInput onCcx = input;
  onCcx.solver = fem::Solver::Calculix;
  fem::StudyRuntime own, theirs;
  REQUIRE(fem::SolvePreparedStudy(input, own, &err));
  fem::SolveOptions options;
  options.calculix = RunWith(*ccx, scratch.dir / "runs");
  REQUIRE(fem::SolvePreparedStudy(onCcx, theirs, &err, options));
  const fem::FemMesh& mesh = own.bodies[0].mesh;
  std::vector<int> tipFaces;
  for (const auto& tri : mesh.boundary) {
    bool all = true;
    for (int n : tri.nodes)
      all = all && std::abs(mesh.nodes[n].x - L) < 1e-6;
    if (all && std::find(tipFaces.begin(), tipFaces.end(), tri.faceIndex) ==
                   tipFaces.end())
      tipFaces.push_back(tri.faceIndex);
  }
  REQUIRE(tipFaces.size() == 2);
  const double kappaRaw = [&] {
    const double m = 1.0, n = 210.0 / 110.3, h = 2 * t;
    return 6.0 * (19.2e-6 - 12e-6) * 80.0 * (1 + m) * (1 + m) /
           (h *
            (3.0 * (1 + m) * (1 + m) + (1 + m * n) * (m * m + 1.0 / (m * n))));
  }();
  for (const fem::StudyRuntime* rt : {&own, &theirs}) {
    const auto fm = fem::FitFaceMotion(
        rt->bodies[0].mesh, rt->bodies[0].results.displacement, tipFaces);
    REQUIRE(fm);
    REQUIRE(fm->rotationDetermined);
    std::printf("bimetal (%s, %zu nodes): tip rotation %.6f rad against "
                "Timoshenko's %.6f (%+.3f %%), drop %.6f mm against %.6f "
                "(%+.3f %%)\n",
                rt == &own ? "in-house" : "calculix", mesh.nodes.size(),
                fm->rotation.y, kappaRaw * L,
                100.0 * (fm->rotation.y / (kappaRaw * L) - 1),
                -fm->translation.z, kappaRaw * L * L / 2,
                100.0 * (-fm->translation.z / (kappaRaw * L * L / 2) - 1));
    CHECK_THAT(fm->rotation.y, Catch::Matchers::WithinRel(kappaRaw * L, 0.02));
    // The clamp turns the root a little (test_fem_thermal.cpp's strip:
    // the free span curves as the closed form, the end 1.9 % more).
    CHECK_THAT(-fm->translation.z,
               Catch::Matchers::WithinRel(kappaRaw * L * L / 2, 0.04));
  }
}
#endif // HAVE_NETGEN

#ifdef HAVE_NETGEN
TEST_CASE("an exact answer printed to ccx's seven digits balances a thin bent "
          "sheet's stiffness, and a deck that differs still does not",
          "[fem][calculix]") {
  // A 0.5 mm 1.4301 sheet with a 90 degree bend at R 0.75 (1.5 t), each
  // leg 20 mm, meshed at its thickness and its bend at half: a soft part,
  // little energy for how stiff its elements are. Its exact answer
  // rounded to the seven digits ccx prints read 8e-6 on the energy check,
  // whose floor counted only rounding's first-order term (2e-6): it was
  // refused as "the deck and the study differ", and so was the tea
  // alarm's clip on CalculiX, whose answer was right.
  constexpr double t = 0.5, Ri = 1.5 * t, Ro = Ri + t, W = 6.0, Lg = 20.0;
  Document doc;
  auto a = std::make_unique<BoxFeature>("A", glm::dvec3(-Lg, 0, 0), Lg, W, t);
  a->SetMaterial("stainless_steel");
  doc.AddFeature(std::move(a));
  uint64_t solid = doc.GetFeature(0)->GetId();
  const auto add = [&](std::unique_ptr<Feature> f) {
    doc.AddFeature(std::move(f));
    return doc.GetFeature(doc.FeatureCount() - 1)->GetId();
  };
  const uint64_t co = add(std::make_unique<CylinderFeature>(
      "Co", glm::dvec3(0, -1, Ro), glm::dvec3(0, 1, 0), Ro, W + 2));
  const uint64_t ci = add(std::make_unique<CylinderFeature>(
      "Ci", glm::dvec3(0, -2, Ro), glm::dvec3(0, 1, 0), Ri, W + 4));
  const uint64_t ring = add(
      std::make_unique<BooleanFeature>("Ring", co, ci, BooleanOp::Subtract));
  const uint64_t q =
      add(std::make_unique<BoxFeature>("Q", glm::dvec3(0, 0, 0), Ro, W, Ro));
  const uint64_t bend = add(
      std::make_unique<BooleanFeature>("Bend", ring, q, BooleanOp::Intersect));
  solid = add(
      std::make_unique<BooleanFeature>("AB", solid, bend, BooleanOp::Union));
  const uint64_t legB =
      add(std::make_unique<BoxFeature>("B", glm::dvec3(Ri, 0, Ro), t, W, Lg));
  solid = add(
      std::make_unique<BooleanFeature>("Sheet", solid, legB, BooleanOp::Union));
  REQUIRE(doc.GetBodies().size() == 1);
  const Body& body = doc.GetBodies()[0];
  fem::Study& s = doc.AddStudy("Bent");
  s.meshSize = t;
  s.converge = fem::ConvergeMode::Never;
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Fixed,
                             FaceWhere(body, "plane", [&](const glm::dvec3& c) {
                               return std::abs(c.x + Lg) < 1e-6;
                             })));
  s.loads.push_back(MakeLoad(doc, fem::LoadType::Force,
                             FaceWhere(body, "plane",
                                       [&](const glm::dvec3& c) {
                                         return std::abs(c.z - (Ro + Lg)) <
                                                1e-6;
                                       }),
                             0.5, glm::dvec3(-1, 0, 0)));
  for (const auto& [index, name] : body.elementMap.names) {
    const auto info = Measure::DescribeFaceAt(body, index);
    if (info && info->kind == "cylinder")
      s.refinements.push_back({ElementRef{body.createdBy, name}, t / 2, ""});
  }
  REQUIRE(s.refinements.size() == 2);
  const fem::StudyJobInput in = Meshed(doc, s);
  const fem::StudyJobBody& jb = in.bodies[0];
  const std::vector<fem::Material> mats{jb.mat};
  fem::Supports sup;
  sup.fixedNodes = jb.fixedNodes;
  const auto printed = [&](double stiffer) {
    std::vector<fem::Material> m = mats;
    m[0].youngMPa *= stiffer;
    fem::SolveOptions direct;
    direct.iterativeAboveDofs = 1 << 30;
    std::string err;
    auto exact = fem::SolveCases(jb.mesh, m, sup, {jb.faceLoads}, &err,
                                 glm::dvec3(0), direct);
    REQUIRE(exact);
    std::vector<glm::dvec3> u = exact->front().displacement;
    for (glm::dvec3& d : u)
      for (int k = 0; k < 3; ++k) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.6E", d[k]); // ccx's E13.6
        d[k] = std::atof(buf);
      }
    return u;
  };
  const auto check = [&](const std::vector<glm::dvec3>& u, std::string* err,
                         fem::SolveReport* rep) {
    fem::SolveOptions o;
    o.given = std::make_shared<const std::vector<std::vector<glm::dvec3>>>(
        std::vector<std::vector<glm::dvec3>>{u});
    o.givenPrecision = fem::kCalculixDatPrecision;
    o.givenName = "the printed answer";
    return fem::SolveCases(jb.mesh, mats, sup, {jb.faceLoads}, err,
                           glm::dvec3(0), o, rep)
        .has_value();
  };
  std::string err;
  fem::SolveReport rep;
  const bool ok = check(printed(1.0), &err, &rep);
  INFO(err);
  std::printf("calculix: thin bent sheet, %zu nodes: the exact answer printed "
              "to seven digits balances to %.2e\n",
              jb.mesh.nodes.size(), rep.worstEnergyResidual);
  CHECK(ok);
  CHECK(rep.worstEnergyResidual > 2e-6); // past the first-order floor alone
  // The control: an answer from a material 0.1 % stiffer than the study's.
  CHECK_FALSE(check(printed(1.001), &err, nullptr));
  CHECK_THAT(err, ContainsSubstring("does not balance"));
}
#endif
