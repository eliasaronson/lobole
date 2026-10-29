// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// CalculiX as a study solver (docs/simulation/README.md#calculix): finding
// ccx, writing the deck, running it and reading its answer back.

#include "core/fem/Calculix.h"

#include "common/ChildProcess.h"
#include "common/RuntimePaths.h"

#ifdef HAVE_FEM_SOLVER
#include "core/fem/FemSolverInternal.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#if !defined(_WIN32)
#include <csignal>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace fem {

std::string CalculixInstall::Version() const {
  return std::to_string(major) + "." + std::to_string(minor);
}

std::string CalculixInstall::Name() const { return "calculix " + Version(); }

std::string CalculixInstallSentence() {
  return "CalculiX (ccx " + std::to_string(2) + "." +
         std::to_string(kCalculixOldestMinor) +
         " or later) is not installed: on Ubuntu or Debian `sudo apt install "
         "calculix-ccx`, with Nix the `calculix-ccx` package, on Windows the "
         "ccx_static.exe from dhondt.de's calculix_2.23_4win.zip, or point "
         "the calculix_path setting (Preferences > Simulation) at a ccx you "
         "have";
}

namespace {

long ThisPid() {
#if defined(_WIN32)
  return 0;
#else
  return static_cast<long>(::getpid());
#endif
}

std::string ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// One probe per (path, size, modification time): a ccx replaced in place
// is asked again, an unchanged one never is.
struct ProbeKey {
  std::string path;
  uintmax_t size = 0;
  int64_t mtime = 0;
  bool operator<(const ProbeKey& o) const {
    return std::tie(path, size, mtime) < std::tie(o.path, o.size, o.mtime);
  }
};
struct ProbeAnswer {
  std::optional<CalculixInstall> found;
  std::string why;
};

} // namespace

std::optional<CalculixInstall> ProbeCalculix(const std::string& path,
                                             std::string* why) {
  const auto fail =
      [&](const std::string& m) -> std::optional<CalculixInstall> {
    if (why)
      *why = m;
    return std::nullopt;
  };
  std::error_code ec;
  if (!fs::is_regular_file(path, ec))
    return fail(path + " is not a file");
  ProbeKey key{path, fs::file_size(path, ec), 0};
  const auto t = fs::last_write_time(path, ec);
  key.mtime = static_cast<int64_t>(t.time_since_epoch().count());
  static std::mutex mutex;
  static std::map<ProbeKey, ProbeAnswer> probed;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (auto it = probed.find(key); it != probed.end()) {
      if (!it->second.found && why)
        *why = it->second.why;
      return it->second.found;
    }
  }
  ProbeAnswer answer;
  // `ccx -v` prints its version and exits (2.23 with status 201): what it
  // printed is the answer, not how it left.
  const fs::path out =
      fs::temp_directory_path(ec) /
      ("lobole-ccx-v-" + std::to_string(ThisPid()) + "-" +
       std::to_string(std::hash<std::string>{}(path)) + ".txt");
  proc::PipedChild child;
  proc::ChildOptions opts;
  opts.stdoutPath = out.string();
  std::string err;
  if (!child.Start(path, {"-v"}, &err, opts)) {
    answer.why = path + " could not be run: " + err;
  } else {
    std::string tail;
    child.Finish(&tail);
    const std::string text = ReadFile(out) + "\n" + tail;
    fs::remove(out, ec);
    const size_t at = text.find("Version ");
    int major = 0, minor = 0;
    if (at == std::string::npos ||
        std::sscanf(text.c_str() + at + 8, "%d.%d", &major, &minor) != 2) {
      answer.why = path + " does not say it is CalculiX: `ccx -v` printed no "
                          "\"Version 2.NN\"";
    } else if (major < 2 || (major == 2 && minor < kCalculixOldestMinor)) {
      answer.why = path + " is CalculiX " + std::to_string(major) + "." +
                   std::to_string(minor) + ", older than 2." +
                   std::to_string(kCalculixOldestMinor) +
                   ", the oldest one this build is tested against";
    } else {
      CalculixInstall c;
      c.path = path;
      c.major = major;
      c.minor = minor;
      answer.found = c;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex);
    probed[key] = answer;
  }
  if (!answer.found && why)
    *why = answer.why;
  return answer.found;
}

std::optional<CalculixInstall> FindCalculix(const std::string& settingPath,
                                            std::string* why) {
  const auto named = [&](const std::string& path,
                         const char* who) -> std::optional<CalculixInstall> {
    std::string probe;
    if (auto c = ProbeCalculix(path, &probe))
      return c;
    if (why)
      *why = std::string(who) + " names " + path +
             ", which is not a ccx this "
             "build can run (" +
             probe + "). " + CalculixInstallSentence();
    return std::nullopt;
  };
  if (!settingPath.empty())
    return named(settingPath, "the calculix_path setting");
  if (const char* env = std::getenv("LOBOLE_CCX"); env != nullptr && *env)
    return named(env, "LOBOLE_CCX");
  // A copy shipped with the app: <prefix>/lib/lobole/ccx beside
  // <prefix>/bin/lobole, or ccx.exe beside lobole.exe.
  const fs::path exe = RuntimePaths::ExecutableDir();
#if defined(_WIN32)
  const fs::path bundled[] = {exe / "ccx.exe"};
#else
  const fs::path bundled[] = {exe.parent_path() / "lib" / "lobole" / "ccx"};
#endif
  std::error_code ec;
  for (const fs::path& b : bundled)
    if (fs::is_regular_file(b, ec))
      return named(b.string(), "the copy shipped beside Loböle,");
  const std::string onPath = proc::FindOnPath("ccx");
  if (!onPath.empty())
    return named(onPath, "PATH");
  if (why)
    *why = CalculixInstallSentence();
  return std::nullopt;
}

// Peak resident memory of `ccx` running a linear static C3D10 deck, measured
// (the child's getrusage ru_maxrss) with the nixpkgs ccx 2.23 (SPOOLES, one
// thread) on a 10 x 10 x 200 steel column meshed by our pipeline at 5 to
// 1.2 mm: 5.7k DOF 57 MB, 16k 63 MB, 30k 116 MB, 66k 272 MB, 197k 1.18 GB,
// 241k 1.48 GB
// (test_fem_calculix.cpp's hidden "[calculix-calibrate]" case prints them).
// A direct factor of a 3D mesh fills in faster than the DOF count grows:
// DOF^1.3 through the largest runs, plus what the process holds before the
// matrix (55 MB), and a tenth more, bounds every measured run from above
// (by 1.1-1.2x at 5.7k, 197k and 241k DOF, 1.7x at 16k).
uint64_t CalculixStaticMemoryEstimate(size_t dofs) {
  constexpr double kBaseBytes = 55e6;
  constexpr double kCoefficient = 166.0; // bytes per DOF^1.3
  constexpr double kExponent = 1.3;
  return static_cast<uint64_t>(
      kBaseBytes +
      kCoefficient * std::pow(static_cast<double>(dofs), kExponent));
}

uint64_t AvailableMemoryBytes() {
#if defined(__linux__)
  std::ifstream in("/proc/meminfo");
  std::string key;
  uint64_t kb = 0;
  std::string unit;
  while (in >> key >> kb) {
    std::getline(in, unit);
    if (key == "MemAvailable:")
      return kb * 1024;
  }
#endif
  return 0;
}

bool NodalForces(const FemMesh& mesh, const std::vector<Material>& byDomain,
                 const std::vector<FaceLoad>& loads,
                 const glm::dvec3& gravityAccel, std::vector<glm::dvec3>& out,
                 std::string* error) {
#ifdef HAVE_FEM_SOLVER
  using namespace internal;
  const size_t nNodes = mesh.nodes.size();
  if (byDomain.empty()) {
    if (error)
      *error = "no material";
    return false;
  }
  Eigen::VectorXd F = Eigen::VectorXd::Zero(3 * static_cast<int>(nNodes));
  // Gravity exactly as SolveCases integrates it: rho g N_i dV at the four
  // Gauss points of each tet.
  if (gravityAccel.x != 0 || gravityAccel.y != 0 || gravityAccel.z != 0) {
    const Vector3d gv(gravityAccel.x, gravityAccel.y, gravityAccel.z);
    for (size_t ti = 0; ti < mesh.tets.size(); ++ti) {
      const size_t d = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[ti];
      const Material& m = byDomain[std::min(d, byDomain.size() - 1)];
      if (m.densityTPerMM3 <= 0)
        continue;
      const Vector3d b = m.densityTPerMM3 * gv;
      const auto& tet = mesh.tets[ti];
      Vector3d X[10];
      for (int i = 0; i < 10; ++i)
        X[i] = Vector3d(mesh.nodes[tet[i]].x, mesh.nodes[tet[i]].y,
                        mesh.nodes[tet[i]].z);
      for (const auto& g : kTetGauss) {
        Eigen::Matrix<double, 6, 30> B;
        const double dV = kTetW * std::abs(StrainDisp(X, g[0], g[1], g[2], B));
        double N[10];
        TetShape(g[0], g[1], g[2], N);
        for (int i = 0; i < 10; ++i)
          F.segment<3>(3 * tet[i]) += (N[i] * dV) * b;
      }
    }
  }
  std::vector<int> allDofs(3 * nNodes);
  for (size_t d = 0; d < allDofs.size(); ++d)
    allDofs[d] = static_cast<int>(d);
  if (!AssembleFaceLoads(mesh, loads, allDofs, F, error))
    return false;
  out.resize(nNodes);
  for (size_t n = 0; n < nNodes; ++n)
    out[n] = glm::dvec3(F(3 * n), F(3 * n + 1), F(3 * n + 2));
  return true;
#else
  (void)mesh;
  (void)byDomain;
  (void)loads;
  (void)gravityAccel;
  (void)out;
  if (error)
    *error = "built without the FEM solver";
  return false;
#endif
}

namespace {

// A number as ccx reads it: each field of a card is read into 20
// characters, so 13 significant digits (at most 19 with sign and exponent),
// far inside the residual check's tolerance.
std::string Num(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.13g", v);
  return buf;
}

// Ids in lines of at most 16, as ccx reads a set.
void WriteIdLines(std::ostream& out, const std::vector<int>& ids) {
  for (size_t i = 0; i < ids.size(); ++i) {
    out << ids[i] + 1;
    out << ((i + 1) % 16 == 0 || i + 1 == ids.size() ? "\n" : ", ");
  }
}

// ccx's cylindrical frame at `x` about the axis through `o` along unit `a`
// (*TRANSFORM, TYPE=C with points o and o + a): radial, tangential, axial.
std::array<glm::dvec3, 3> CylFrame(const glm::dvec3& x, const glm::dvec3& o,
                                   const glm::dvec3& a) {
  glm::dvec3 r = (x - o) - glm::dot(x - o, a) * a;
  r = glm::normalize(r);
  return {r, glm::cross(a, r), a};
}

} // namespace

bool WriteDeck(std::ostream& out, const FemMesh& mesh,
               const std::vector<Material>& byDomain, const Supports& supports,
               const std::vector<StudyJobBody::PinSupport>& pins,
               const std::vector<glm::dvec3>& nodalForces,
               const std::string& title, std::string* error,
               int buckleFactors) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  if (nNodes == 0 || mesh.tets.empty())
    return fail("empty mesh");
  if (byDomain.empty())
    return fail("no material");
  if (nodalForces.size() != mesh.nodes.size())
    return fail("one force per node is needed");

  // The supports as SolveCases reads them: a fixed node is fixed whatever
  // else holds it; a held node takes its first frame and every direction any
  // entry holds.
  std::vector<char> fixed(nNodes, 0);
  for (int n : supports.fixedNodes)
    if (n >= 0 && n < nNodes)
      fixed[n] = 1;
  std::vector<int> frameOf(nNodes, -1);
  std::vector<std::array<glm::dvec3, 3>> frames;
  std::vector<std::array<bool, 3>> held;
  for (const HeldNode& h : supports.held) {
    if (h.node < 0 || h.node >= nNodes || fixed[h.node])
      continue;
    if (frameOf[h.node] < 0) {
      frameOf[h.node] = static_cast<int>(frames.size());
      frames.push_back(h.frame);
      held.push_back({false, false, false});
    }
    for (int k = 0; k < 3; ++k)
      held[frameOf[h.node]][k] = held[frameOf[h.node]][k] || h.held[k];
  }
  // Which pin's frame each held node is in: the pin whose facets have it.
  std::vector<int> pinOf(nNodes, -1);
  for (size_t p = 0; p < pins.size(); ++p)
    for (int t : pins[p].tris)
      for (int v : mesh.boundary[t].nodes) {
        if (frameOf[v] < 0)
          continue;
        if (pinOf[v] >= 0 && pinOf[v] != static_cast<int>(p))
          return fail("node " + std::to_string(v + 1) + " is on pin " +
                      std::to_string(pins[pinOf[v]].id) + "'s face and pin " +
                      std::to_string(pins[p].id) +
                      "'s, and a CalculiX node is held in one frame: the "
                      "two bores touch, which the deck cannot express");
        pinOf[v] = static_cast<int>(p);
      }
  // Each held node's frame directions as ccx's cylindrical ones: direction
  // k is ccx's dof slot[k] times sign[k].
  std::vector<std::array<int, 3>> slot(frames.size());
  std::vector<std::array<double, 3>> sign(frames.size());
  for (int n = 0; n < nNodes; ++n) {
    const int f = frameOf[n];
    if (f < 0)
      continue;
    if (pinOf[n] < 0)
      return fail("node " + std::to_string(n + 1) +
                  " is held in a frame of its own but is on no pin's face, "
                  "which the deck has no transform for");
    const auto& pin = pins[pinOf[n]];
    const auto cyl = CylFrame(mesh.nodes[n], pin.axisOrigin,
                              glm::normalize(pin.axisDirection));
    for (int k = 0; k < 3; ++k) {
      slot[f][k] = -1;
      for (int j = 0; j < 3; ++j) {
        const double c = glm::dot(frames[f][k], cyl[j]);
        if (std::abs(std::abs(c) - 1.0) < 1e-9) {
          slot[f][k] = j;
          sign[f][k] = c > 0 ? 1.0 : -1.0;
        }
      }
      if (slot[f][k] < 0)
        return fail("node " + std::to_string(n + 1) +
                    " is held in a frame that is not its pin's cylinder");
    }
  }

  out << "** " << title << "\n";
  out << "** Written by Loboele from its own study: the Netgen mesh, a "
         "material per domain, the assembled nodal forces.\n";
  out << "** Units: mm, N, MPa, t/mm^3.\n";
  out << "*NODE, NSET=NALL\n";
  for (int n = 0; n < nNodes; ++n) {
    const glm::dvec3& p = mesh.nodes[n];
    out << n + 1 << ", " << Num(p.x) << ", " << Num(p.y) << ", " << Num(p.z)
        << "\n";
  }
  // One ELSET per domain. ccx wants the corners positively oriented (Netgen
  // gives them the other way): corners 1 and 2 swap, and the midsides with
  // them.
  size_t nDomains = 1;
  for (int d : mesh.tetDomain)
    nDomains = std::max(nDomains, static_cast<size_t>(d) + 1);
  for (size_t d = 0; d < nDomains; ++d) {
    bool any = false;
    for (size_t t = 0; t < mesh.tets.size(); ++t) {
      const size_t td = mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t];
      if (td != d)
        continue;
      if (!any) {
        out << "*ELEMENT, TYPE=C3D10, ELSET=D" << d + 1 << "\n";
        any = true;
      }
      const auto& q = mesh.tets[t];
      const glm::dvec3 &x0 = mesh.nodes[q[0]], &x1 = mesh.nodes[q[1]],
                       &x2 = mesh.nodes[q[2]], &x3 = mesh.nodes[q[3]];
      const bool positive =
          glm::dot(glm::cross(x1 - x0, x2 - x0), x3 - x0) > 0.0;
      // Ours: corners 0-3, then (0,1) (1,2) (0,2) (0,3) (1,3) (2,3), which
      // is ccx's order; swapped: corners 0 2 1 3, then (0,2) (2,1) (1,0)
      // (0,3) (2,3) (1,3).
      const std::array<int, 10> order =
          positive ? std::array<int, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
                   : std::array<int, 10>{0, 2, 1, 3, 6, 5, 4, 7, 9, 8};
      out << t + 1;
      for (int i : order)
        out << ", " << q[i] + 1;
      out << "\n";
    }
    if (!any)
      continue;
    const Material& m = byDomain[std::min(d, byDomain.size() - 1)];
    out << "*MATERIAL, NAME=M" << d + 1 << "\n*ELASTIC\n"
        << Num(m.youngMPa) << ", " << Num(m.poisson) << "\n";
    if (m.densityTPerMM3 > 0)
      out << "*DENSITY\n" << Num(m.densityTPerMM3) << "\n";
    out << "*SOLID SECTION, ELSET=D" << d + 1 << ", MATERIAL=M" << d + 1
        << "\n";
  }

  // A pin's nodes in its cylinder: one transform per pin.
  for (size_t p = 0; p < pins.size(); ++p) {
    std::vector<int> ids;
    for (int n = 0; n < nNodes; ++n)
      if (pinOf[n] == static_cast<int>(p))
        ids.push_back(n);
    if (ids.empty())
      continue;
    out << "*NSET, NSET=PIN" << p + 1 << "\n";
    WriteIdLines(out, ids);
    const glm::dvec3 a = pins[p].axisOrigin;
    const glm::dvec3 b = a + glm::normalize(pins[p].axisDirection);
    out << "*TRANSFORM, NSET=PIN" << p + 1 << ", TYPE=C\n"
        << Num(a.x) << ", " << Num(a.y) << ", " << Num(a.z) << ", " << Num(b.x)
        << ", " << Num(b.y) << ", " << Num(b.z) << "\n";
  }
  // The held directions.
  bool boundaryOpen = false;
  const auto boundary = [&]() {
    if (!boundaryOpen)
      out << "*BOUNDARY\n";
    boundaryOpen = true;
  };
  for (int n = 0; n < nNodes; ++n)
    if (fixed[n]) {
      boundary();
      out << n + 1 << ", 1, 3\n";
    }
  for (int n = 0; n < nNodes; ++n) {
    const int f = frameOf[n];
    if (f < 0)
      continue;
    for (int k = 0; k < 3; ++k)
      if (held[f][k]) {
        boundary();
        out << n + 1 << ", " << slot[f][k] + 1 << ", " << slot[f][k] + 1
            << "\n";
      }
  }
  // A tie: sum coef u_k = 0 over its entries' free directions, the first
  // term the largest (ccx eliminates the first). An entry on a held
  // direction is zero already and drops out; a tie left with one term holds
  // it, as SolveCases does.
  for (const Tie& tie : supports.ties) {
    std::vector<std::pair<int, double>> terms; // (node, coef) in ccx dofs
    std::vector<int> dofs;
    for (const Tie::Entry& e : tie.entries) {
      if (e.node < 0 || e.node >= nNodes || e.k < 0 || e.k > 2 || e.coef == 0.0)
        continue;
      const int f = frameOf[e.node];
      if (f < 0 || held[f][e.k])
        continue;
      terms.push_back({e.node, e.coef * sign[f][e.k]});
      dofs.push_back(slot[f][e.k] + 1);
    }
    if (terms.empty())
      continue;
    if (terms.size() == 1) {
      boundary();
      out << terms[0].first + 1 << ", " << dofs[0] << ", " << dofs[0] << "\n";
      continue;
    }
    size_t lead = 0;
    for (size_t i = 1; i < terms.size(); ++i)
      if (std::abs(terms[i].second) > std::abs(terms[lead].second))
        lead = i;
    std::swap(terms[0], terms[lead]);
    std::swap(dofs[0], dofs[lead]);
    // An *EQUATION has to follow the boundaries' block as its own keyword.
    boundaryOpen = false;
    out << "*EQUATION\n" << terms.size() << "\n";
    for (size_t i = 0; i < terms.size(); ++i) {
      out << terms[i].first + 1 << ", " << dofs[i] << ", "
          << Num(terms[i].second);
      out << ((i + 1) % 4 == 0 || i + 1 == terms.size() ? "\n" : ", ");
    }
  }

  // A buckling run's step: its factors to the accuracy the in-house
  // eigensolve holds (ccx's default is 1 %).
  if (buckleFactors > 0)
    out << "*STEP\n*BUCKLE\n" << buckleFactors << ", 1e-10\n";
  else
    out << "*STEP\n*STATIC\n";
  bool anyLoad = false;
  for (int n = 0; n < nNodes; ++n) {
    if (fixed[n])
      continue;
    glm::dvec3 f = nodalForces[n];
    if (frameOf[n] >= 0) {
      // A transformed node's load is given in its frame.
      const auto& pin = pins[pinOf[n]];
      const auto cyl = CylFrame(mesh.nodes[n], pin.axisOrigin,
                                glm::normalize(pin.axisDirection));
      f = glm::dvec3(glm::dot(f, cyl[0]), glm::dot(f, cyl[1]),
                     glm::dot(f, cyl[2]));
    }
    for (int k = 0; k < 3; ++k)
      if (f[k] != 0.0) {
        if (!anyLoad)
          out << "*CLOAD\n";
        anyLoad = true;
        out << n + 1 << ", " << k + 1 << ", " << Num(f[k]) << "\n";
      }
  }
  out << "*NODE PRINT, NSET=NALL, GLOBAL=YES\nU\n*NODE FILE\nU\n*END STEP\n";
  if (!out)
    return fail("the deck could not be written");
  return true;
}

namespace {

// A Fortran E number, which drops the E before a three-digit exponent
// ("0.123456-100").
bool ParseFortran(const std::string& tok, double& v) {
  std::string s = tok;
  for (size_t i = 1; i < s.size(); ++i)
    if ((s[i] == '-' || s[i] == '+') && s[i - 1] != 'E' && s[i - 1] != 'e' &&
        s.find_first_of("Ee") == std::string::npos) {
      s.insert(i, "E");
      break;
    }
  char* end = nullptr;
  v = std::strtod(s.c_str(), &end);
  return end != s.c_str() && *end == '\0';
}

} // namespace

bool ReadDatDisplacements(const std::string& path, size_t nNodes,
                          std::vector<glm::dvec3>& out, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  std::ifstream in(path);
  if (!in)
    return fail("CalculiX wrote no " + fs::path(path).filename().string());
  out.assign(nNodes, glm::dvec3(0.0));
  std::vector<char> seen(nNodes, 0);
  std::string line;
  bool inBlock = false, started = false;
  size_t count = 0;
  while (std::getline(in, line)) {
    if (!inBlock) {
      if (line.find("displacements (vx,vy,vz)") != std::string::npos)
        inBlock = true;
      continue;
    }
    std::istringstream ls(line);
    std::string a, b, c, d;
    if (!(ls >> a)) {
      if (started)
        break; // the blank line after the block
      continue;
    }
    if (!(ls >> b >> c >> d))
      break;
    char* end = nullptr;
    const long node = std::strtol(a.c_str(), &end, 10);
    double x = 0, y = 0, z = 0;
    if (*end != '\0' || !ParseFortran(b, x) || !ParseFortran(c, y) ||
        !ParseFortran(d, z))
      break;
    started = true;
    if (node < 1 || static_cast<size_t>(node) > nNodes)
      return fail("CalculiX printed a displacement for node " +
                  std::to_string(node) + ", which the deck does not have");
    out[node - 1] = glm::dvec3(x, y, z);
    if (!seen[node - 1])
      ++count;
    seen[node - 1] = 1;
  }
  if (!started)
    return fail("CalculiX's " + fs::path(path).filename().string() +
                " holds no displacements");
  if (count != nNodes)
    return fail("CalculiX printed the displacement of " +
                std::to_string(count) + " of " + std::to_string(nNodes) +
                " nodes");
  return true;
}

std::string FirstCalculixError(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line))
    if (line.find("*ERROR") != std::string::npos) {
      const size_t a = line.find_first_not_of(" \t\r");
      const size_t b = line.find_last_not_of(" \t\r");
      return line.substr(a, b - a + 1);
    }
  return {};
}

void SweepDeadRunDirs(const std::string& root) {
#if !defined(_WIN32)
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    return;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    const std::string name = entry.path().filename().string();
    char* end = nullptr;
    const long pid = std::strtol(name.c_str(), &end, 10);
    if (pid <= 0 || end == name.c_str() || *end != '-' || pid == ThisPid())
      continue;
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH)
      fs::remove_all(entry.path(), ec);
  }
#else
  (void)root;
#endif
}

namespace {

std::atomic<unsigned> gRunCounter{0};

// A fresh run directory under the run root, <pid>-<n>; the dead ones
// swept first.
bool MakeRunDir(const CalculixRun& run, fs::path& dir, std::string* error) {
  std::error_code ec;
  fs::create_directories(run.runRoot, ec);
  SweepDeadRunDirs(run.runRoot);
  dir = fs::path(run.runRoot) / (std::to_string(ThisPid()) + "-" +
                                 std::to_string(gRunCounter.fetch_add(1)));
  fs::remove_all(dir, ec);
  if (!fs::create_directories(dir, ec)) {
    if (error)
      *error = "could not make the run directory " + dir.string() + ": " +
               ec.message();
    return false;
  }
  return true;
}

std::string Gb(uint64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / 1e9);
  return buf;
}

std::string Seconds(double s) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g s", s);
  return buf;
}

// Runs ccx on `dir`/job.inp; the displacement it printed into `u`. False
// with `error` (and `keep` saying whether the directory is worth keeping).
bool RunCcx(const CalculixRun& run, const fs::path& dir,
            const SolveOptions& options, bool& keep, std::string* error,
            const std::function<bool(std::string*)>& read) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  keep = true;
  proc::ChildOptions co;
  co.cwd = dir.string();
  co.stdoutPath = (dir / "job.out").string();
  const std::string threads = std::to_string(std::max(1, run.threads));
  co.env = {{"OMP_NUM_THREADS", threads},
            {"CCX_NPROC_EQUATION_SOLVER", threads},
            {"CCX_NPROC_STIFFNESS", threads},
            {"CCX_NPROC_RESULTS", threads}};
  proc::PipedChild child;
  std::string err;
  if (!child.Start(run.ccx.path, {"-i", "job"}, &err, co))
    return fail(err);
  const auto start = std::chrono::steady_clock::now();
  int code = -1;
  while (!child.Poll(&code)) {
    if (options.cancelled && options.cancelled()) {
      child.Kill();
      keep = false;
      return fail("cancelled");
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    if (run.timeoutSeconds > 0 && elapsed > run.timeoutSeconds) {
      child.Kill();
      return fail("CalculiX did not finish within timeout_s " +
                  Seconds(run.timeoutSeconds) + ", so it was stopped");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::string tail;
  child.Finish(&tail);
  const std::string said = ReadFile(dir / "job.out");
  const std::string first = FirstCalculixError(said + "\n" + tail);
  if (!first.empty())
    return fail(first);
  if (code != 0) {
    std::string last = said.empty() ? tail : said;
    if (last.size() > 400)
      last = "..." + last.substr(last.size() - 400);
    return fail("CalculiX exited with status " + std::to_string(code) +
                (last.empty() ? std::string() : ": " + last));
  }
  if (!read(&err))
    return fail(err);
  return true;
}

} // namespace

bool SolveBodyOnCalculix(const StudyJobBody& jb,
                         const std::vector<Material>& mats,
                         const std::vector<std::vector<FaceLoad>>& rhs,
                         const glm::dvec3& gravityAccel,
                         const SolveOptions& options,
                         std::vector<FemResults>& out, SolveReport& report,
                         std::string* error,
                         std::vector<Supports>* supportsOut) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!options.calculix)
    return fail("this study solves on CalculiX, and the run was given none");
  const CalculixRun& run = *options.calculix;
  // The in-house options for what is ours to solve: a pin's contact.
  SolveOptions own = options;
  own.calculix = nullptr;
  own.given = nullptr;
  std::vector<Supports> sups;
  std::vector<std::vector<FemResults::SupportReaction>> pinRows;
  std::string err;
  if (!StaticSupportsOf(jb, mats, rhs, gravityAccel, own, sups, pinRows, &err))
    return fail(err);
  if (supportsOut)
    *supportsOut = sups;
  const size_t nNodes = jb.mesh.nodes.size();
  out.clear();
  report = SolveReport{};
  report.solver = run.ccx.Name();
  for (size_t k = 0; k < rhs.size(); ++k) {
    if (options.cancelled && options.cancelled())
      return fail("cancelled");
    std::vector<glm::dvec3> forces;
    if (!NodalForces(jb.mesh, mats, rhs[k], gravityAccel, forces, &err))
      return fail(err);
    // SPOOLES factorises in core: a run larger than the machine has free
    // would take the host with it, so it is refused before it starts.
    const size_t dofs =
        3 * (nNodes - std::min(nNodes, sups[k].fixedNodes.size()));
    const uint64_t estimate = CalculixStaticMemoryEstimate(dofs);
    if (run.estimated)
      run.estimated(estimate);
    const uint64_t available =
        run.availableBytes ? run.availableBytes : AvailableMemoryBytes();
    if (available > 0 && estimate > available)
      return fail("CalculiX would need about " + Gb(estimate) + " for " +
                  std::to_string(dofs) + " degrees of freedom, and " +
                  Gb(available) +
                  " is free: its solver has no out-of-core mode, so the run "
                  "was not started. A coarser mesh_size, or the in-house "
                  "solver, solves it");
    std::error_code ec;
    fs::path dir;
    if (!MakeRunDir(run, dir, &err))
      return fail(err);
    if (options.solving)
      options.solving("calculix: writing the deck");
    {
      std::ofstream deck(dir / "job.inp");
      std::string title = "body '" + jb.bodyName + "'";
      if (rhs.size() > 1)
        title += ", load case " + std::to_string(k + 1);
      if (!WriteDeck(deck, jb.mesh, mats, sups[k], jb.pins, forces, title,
                     &err)) {
        fs::remove_all(dir, ec);
        return fail(err);
      }
    }
    if (options.solving)
      options.solving("calculix: solving");
    std::vector<glm::dvec3> u;
    bool keep = true;
    if (!RunCcx(run, dir, options, keep, &err, [&](std::string* why) {
          return ReadDatDisplacements((dir / "job.dat").string(), nNodes, u,
                                      why);
        })) {
      if (!keep) {
        fs::remove_all(dir, ec);
        return fail(err);
      }
      return fail(err + " (the run is kept in " + dir.string() + ")");
    }
    if (options.solving)
      options.solving("calculix: checking the answer");
    // Our K holds the answer to the residual rule, and everything after the
    // displacement is computed as it is for an in-house answer.
    SolveOptions check = own;
    check.makeExternal = nullptr;
    check.solving = nullptr;
    check.given = std::make_shared<const std::vector<std::vector<glm::dvec3>>>(
        std::vector<std::vector<glm::dvec3>>{std::move(u)});
    check.givenPrecision = kCalculixDatPrecision;
    check.givenName = run.ccx.Name();
    Supports sup = sups[k];
    sup.reactions = true;
    SolveReport r;
    auto res =
        SolveCases(jb.mesh, mats, sup, {rhs[k]}, &err, gravityAccel, check, &r);
    if (!res)
      return fail(err + " (the run is kept in " + dir.string() + ")");
    FemResults& one = res->front();
    one.reactions = k < pinRows.size()
                        ? pinRows[k]
                        : std::vector<FemResults::SupportReaction>{};
    report.worstResidual = std::max(report.worstResidual, r.worstResidual);
    report.worstResidualFloor =
        std::max(report.worstResidualFloor, r.worstResidualFloor);
    report.worstEnergyResidual =
        std::max(report.worstEnergyResidual, r.worstEnergyResidual);
    out.push_back(std::move(one));
    fs::remove_all(dir, ec);
  }
  return true;
}

bool WriteStudyDeck(const StudyJobInput& in, size_t body, size_t caseIndex,
                    std::ostream& out, const std::string& title,
                    std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (in.type != AnalysisType::Static && in.type != AnalysisType::Buckling)
    return fail(std::string("a CalculiX deck is written for a static study "
                            "or a buckling one; this one is ") +
                AnalysisTypeToString(in.type));
  if (body >= in.bodies.size())
    return fail("no such body");
  const StudyJobBody& jb = in.bodies[body];
  const std::vector<std::vector<FaceLoad>> rhs =
      jb.caseLoads.empty() ? std::vector<std::vector<FaceLoad>>{jb.faceLoads}
                           : jb.caseLoads;
  if (caseIndex >= rhs.size())
    return fail("no such load case");
  const std::vector<Material> mats =
      jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
  const glm::dvec3 g = in.gravity * 1000.0; // m/s^2 -> mm/s^2
  std::vector<Supports> sups;
  std::vector<std::vector<FemResults::SupportReaction>> pinRows;
  std::string err;
  if (!StaticSupportsOf(jb, mats, {rhs[caseIndex]}, g, SolveOptions{}, sups,
                        pinRows, &err))
    return fail(err);
  std::vector<glm::dvec3> forces;
  if (!NodalForces(jb.mesh, mats, rhs[caseIndex], g, forces, &err))
    return fail(err);
  return WriteDeck(
      out, jb.mesh, mats, sups.front(), jb.pins, forces, title, error,
      in.type == AnalysisType::Buckling ? std::max(in.numModes, 1) : 0);
}

bool ReadDatBuckling(const std::string& path, size_t nNodes,
                     std::vector<double>& factors,
                     std::vector<std::vector<glm::dvec3>>& shapes,
                     std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  std::ifstream in(path);
  if (!in)
    return fail("CalculiX wrote no " + fs::path(path).filename().string());
  factors.clear();
  shapes.clear();
  std::string line;
  // The factors' table, then a displacement block per mode, each after its
  // "E I G E N V A L U E   N U M B E R" line.
  enum class At { Before, Factors, Mode, Shape } at = At::Before;
  std::vector<char> seen;
  size_t count = 0;
  const auto endShape = [&]() {
    if (at == At::Shape && count != nNodes)
      return false;
    return true;
  };
  while (std::getline(in, line)) {
    if (line.find("B U C K L I N G   F A C T O R") != std::string::npos) {
      at = At::Factors;
      continue;
    }
    if (line.find("E I G E N V A L U E    N U M B E R") != std::string::npos) {
      if (!endShape())
        return fail("CalculiX printed buckling mode " +
                    std::to_string(shapes.size()) + " at " +
                    std::to_string(count) + " of " + std::to_string(nNodes) +
                    " nodes");
      at = At::Mode;
      continue;
    }
    std::istringstream ls(line);
    std::string a, b, c, d;
    if (at == At::Factors) {
      if (!(ls >> a >> b))
        continue;
      char* end = nullptr;
      std::strtol(a.c_str(), &end, 10);
      double f = 0.0;
      if (*end == '\0' && ParseFortran(b, f))
        factors.push_back(f);
      continue;
    }
    if (at == At::Mode) {
      if (line.find("displacements (vx,vy,vz)") != std::string::npos) {
        shapes.emplace_back(nNodes, glm::dvec3(0.0));
        seen.assign(nNodes, 0);
        count = 0;
        at = At::Shape;
      }
      continue;
    }
    if (at != At::Shape || !(ls >> a >> b >> c >> d))
      continue;
    char* end = nullptr;
    const long node = std::strtol(a.c_str(), &end, 10);
    double x = 0, y = 0, z = 0;
    if (*end != '\0' || !ParseFortran(b, x) || !ParseFortran(c, y) ||
        !ParseFortran(d, z))
      continue;
    if (node < 1 || static_cast<size_t>(node) > nNodes)
      return fail("CalculiX printed a mode shape at node " +
                  std::to_string(node) + ", which the deck does not have");
    shapes.back()[node - 1] = glm::dvec3(x, y, z);
    if (!seen[node - 1])
      ++count;
    seen[node - 1] = 1;
  }
  if (!endShape())
    return fail("CalculiX printed buckling mode " +
                std::to_string(shapes.size()) + " at " + std::to_string(count) +
                " of " + std::to_string(nNodes) + " nodes");
  if (factors.empty())
    return fail("CalculiX's " + fs::path(path).filename().string() +
                " holds no buckling factors");
  if (shapes.size() != factors.size())
    return fail("CalculiX printed " + std::to_string(factors.size()) +
                " buckling factors and " + std::to_string(shapes.size()) +
                " mode shapes");
  return true;
}

bool BucklingOnCalculix(
    const StudyJobBody& jb, const std::vector<Material>& mats,
    const Supports& supports, const std::vector<FaceLoad>& loads,
    const glm::dvec3& gravityAccel, int numFactors, const SolveOptions& options,
    std::vector<FemResults::ModeShape>& modes, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!options.calculix)
    return fail("this study solves on CalculiX, and the run was given none");
  const CalculixRun& run = *options.calculix;
  const size_t nNodes = jb.mesh.nodes.size();
  std::string err;
  std::vector<glm::dvec3> forces;
  if (!NodalForces(jb.mesh, mats, loads, gravityAccel, forces, &err))
    return fail(err);
  fs::path dir;
  if (!MakeRunDir(run, dir, &err))
    return fail(err);
  std::error_code ec;
  {
    std::ofstream deck(dir / "job.inp");
    if (!WriteDeck(deck, jb.mesh, mats, supports, jb.pins, forces,
                   "body '" + jb.bodyName + "', buckling", &err,
                   std::max(numFactors, 1))) {
      fs::remove_all(dir, ec);
      return fail(err);
    }
  }
  std::vector<double> factors;
  std::vector<std::vector<glm::dvec3>> shapes;
  bool keep = true;
  if (!RunCcx(run, dir, options, keep, &err, [&](std::string* why) {
        return ReadDatBuckling((dir / "job.dat").string(), nNodes, factors,
                               shapes, why);
      })) {
    if (!keep) {
      fs::remove_all(dir, ec);
      return fail(err);
    }
    return fail(err + " (the run is kept in " + dir.string() + ")");
  }
  modes.clear();
  for (size_t m = 0; m < factors.size(); ++m) {
    FemResults::ModeShape mode;
    mode.factor = factors[m];
    mode.displacement = std::move(shapes[m]);
    double peak = 0.0;
    for (const glm::dvec3& u : mode.displacement)
      peak = std::max(peak, glm::length(u));
    if (peak > 0)
      for (glm::dvec3& u : mode.displacement)
        u /= peak;
    modes.push_back(std::move(mode));
  }
  std::stable_sort(modes.begin(), modes.end(),
                   [](const auto& a, const auto& b) {
                     return std::abs(a.factor) < std::abs(b.factor);
                   });
  fs::remove_all(dir, ec);
  return true;
}

} // namespace fem
