// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// CalculiX as a study solver (docs/simulation/README.md#calculix): finding
// ccx, writing the deck, running it and reading its answer back.

#include "core/fem/Calculix.h"
#include "core/fem/FemThermal.h"

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
#include <optional>
#include <set>
#include <sstream>
#include <thread>

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

long ThisPid() { return proc::CurrentPid(); }

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

namespace {

// A load as the nodal forces its in-house traction gives, and their sum as
// one force and the moment about `about`: what a reference node carries.
bool LoadResultant(const FemMesh& mesh, const FaceLoad& load,
                   const glm::dvec3& about, glm::dvec3& force,
                   glm::dvec3& moment, std::string* error) {
  std::vector<glm::dvec3> f;
  if (!NodalForces(mesh, {Material{}}, {load}, glm::dvec3(0.0), f, error))
    return false;
  force = moment = glm::dvec3(0.0);
  for (size_t n = 0; n < f.size(); ++n) {
    force += f[n];
    moment += glm::cross(mesh.nodes[n] - about, f[n]);
  }
  return true;
}

// A tet's ten nodes in the order the deck writes them: ccx wants the
// corners positively oriented (Netgen gives them the other way), so a
// negative one has its corners 1 and 2 swapped, and the midsides with them.
// Ours: corners 0-3, then (0,1) (1,2) (0,2) (0,3) (1,3) (2,3), which is
// ccx's order; swapped: corners 0 2 1 3, then (0,2) (2,1) (1,0) (0,3)
// (2,3) (1,3).
std::array<int, 10> WrittenOrder(const FemMesh& mesh, size_t t) {
  const auto& q = mesh.tets[t];
  const glm::dvec3 &x0 = mesh.nodes[q[0]], &x1 = mesh.nodes[q[1]],
                   &x2 = mesh.nodes[q[2]], &x3 = mesh.nodes[q[3]];
  const bool positive = glm::dot(glm::cross(x1 - x0, x2 - x0), x3 - x0) > 0.0;
  return positive ? std::array<int, 10>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9}
                  : std::array<int, 10>{0, 2, 1, 3, 6, 5, 4, 7, 9, 8};
}

// Each boundary facet of `mesh` as the deck names it: (element, face) with
// the element numbered t + 1 + `elemOffset` and ccx's faces of a C3D10 in
// its written corners' order, S1 (1 2 3), S2 (1 4 2), S3 (2 4 3), S4
// (3 4 1). (0, 0) for a facet that is the face of no tet.
std::vector<std::pair<int, int>> FacetFaces(const FemMesh& mesh,
                                            int elemOffset) {
  static constexpr int kFace[4][3] = {
      {0, 1, 2}, {0, 3, 1}, {1, 3, 2}, {2, 3, 0}};
  std::map<std::array<int, 3>, std::pair<int, int>> faceOf;
  for (size_t t = 0; t < mesh.tets.size(); ++t) {
    const std::array<int, 10> order = WrittenOrder(mesh, t);
    for (int f = 0; f < 4; ++f) {
      std::array<int, 3> k = {mesh.tets[t][order[kFace[f][0]]],
                              mesh.tets[t][order[kFace[f][1]]],
                              mesh.tets[t][order[kFace[f][2]]]};
      std::sort(k.begin(), k.end());
      faceOf[k] = {static_cast<int>(t) + 1 + elemOffset, f + 1};
    }
  }
  std::vector<std::pair<int, int>> out(mesh.boundary.size(), {0, 0});
  for (size_t b = 0; b < mesh.boundary.size(); ++b) {
    const auto& tri = mesh.boundary[b];
    std::array<int, 3> k = {tri.nodes[0], tri.nodes[1], tri.nodes[2]};
    std::sort(k.begin(), k.end());
    if (const auto it = faceOf.find(k); it != faceOf.end())
      out[b] = it->second;
  }
  return out;
}

// A real number as *SPRING reads one: with a decimal point, which is how it
// tells a stiffness from a degree of freedom.
std::string Real(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.12E", v);
  return buf;
}

// Every node's temperature at `lambda` of the way from the stress-free one
// to the field's (*TEMPERATURE), for a step that takes the thermal load to
// that factor. The `partners` rigid partners (node sets P1..) stay at the
// stress-free temperature, and say so: they carry an initial temperature,
// and ccx 2.21 refuses a node with an initial one and no final one
// ("checktemp: no final temperature").
void WriteTemperatures(std::ostream& out, const NodalTemperature& thermal,
                       double lambda, bool opNew, size_t partners = 0) {
  out << "*TEMPERATURE" << (opNew ? ", OP=NEW" : "") << "\n";
  const double t0 = thermal.stressFreeC;
  for (size_t n = 0; n < thermal.temperature.size(); ++n)
    out << n + 1 << ", " << Num(t0 + lambda * (thermal.temperature[n] - t0))
        << "\n";
  for (size_t k = 0; k < partners; ++k)
    out << "P" << k + 1 << ", " << Num(t0) << "\n";
}

// The steps of a nonlinear deck (WriteDeck's `nonlinear`), from the
// boundaries on: first the model a contact adds (its rigid partners, its
// surfaces and pairs, the stabilizing springs), then the steps.
bool WriteNonlinearSteps(std::ostream& out, const FemMesh& mesh,
                         const std::vector<Material>& byDomain,
                         const std::vector<char>& fixed,
                         const std::vector<int>& frameOf,
                         const std::vector<int>& pinOf,
                         const std::vector<StudyJobBody::PinSupport>& pins,
                         const std::vector<glm::dvec3>& nodalForces,
                         const DeckNonlinear& nl, std::string* error,
                         const NodalTemperature* thermal) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  const int nNodes = static_cast<int>(mesh.nodes.size());
  const std::vector<std::pair<int, int>> faces = FacetFaces(mesh, 0);
  const auto facesOf = [&](const std::vector<int>& tris,
                           std::vector<std::pair<int, int>>& into) {
    for (int ti : tris) {
      if (ti < 0 || ti >= static_cast<int>(faces.size()) ||
          faces[ti].first == 0)
        return fail("a loaded boundary facet is the face of no tet");
      into.push_back(faces[ti]);
    }
    return true;
  };

  // A contact (docs/simulation/README.md#contact): each rigid partner's
  // nodes and tets after the mesh's, every node fixed.
  static const std::vector<StudyJobBody::ContactPair> kNoContacts;
  static const std::vector<FemMesh> kNoPartners;
  static const std::vector<StudyJobBody::Stabilized> kNoSprings;
  const auto& contacts = nl.contacts ? *nl.contacts : kNoContacts;
  const auto& partners = nl.partners ? *nl.partners : kNoPartners;
  const auto& springs = nl.stabilized ? *nl.stabilized : kNoSprings;
  int nextNode = nNodes + 1;
  int nextElem = static_cast<int>(mesh.tets.size()) + 1;
  std::vector<int> partnerNode(partners.size()), partnerElem(partners.size());
  if (!partners.empty()) {
    const Material& m = byDomain.front();
    out << "*MATERIAL, NAME=MR\n*ELASTIC\n"
        << Num(m.youngMPa) << ", " << Num(m.poisson) << "\n";
  }
  for (size_t k = 0; k < partners.size(); ++k) {
    const FemMesh& pm = partners[k];
    if (pm.nodes.empty() || pm.tets.empty())
      return fail("a contact's rigid partner meshed to nothing");
    partnerNode[k] = nextNode;
    partnerElem[k] = nextElem;
    out << "*NODE, NSET=P" << k + 1 << "\n";
    for (size_t n = 0; n < pm.nodes.size(); ++n)
      out << nextNode + static_cast<int>(n) << ", " << Num(pm.nodes[n].x)
          << ", " << Num(pm.nodes[n].y) << ", " << Num(pm.nodes[n].z) << "\n";
    out << "*ELEMENT, TYPE=C3D10, ELSET=R" << k + 1 << "\n";
    for (size_t t = 0; t < pm.tets.size(); ++t) {
      out << nextElem + static_cast<int>(t);
      for (int i : WrittenOrder(pm, t))
        out << ", " << nextNode + pm.tets[t][i];
      out << "\n";
    }
    out << "*SOLID SECTION, ELSET=R" << k + 1 << ", MATERIAL=MR\n";
    out << "*BOUNDARY\nP" << k + 1 << ", 1, 3\n";
    nextNode += static_cast<int>(pm.nodes.size());
    nextElem += static_cast<int>(pm.tets.size());
  }
  // Each pair: the body's facets the slave surface, the partner's the
  // master, face-to-face penalty contact.
  for (size_t k = 0; k < contacts.size(); ++k) {
    const StudyJobBody::ContactPair& c = contacts[k];
    if (c.slaveTris.empty() || c.masterTris.empty())
      return fail("contact " + std::to_string(c.id) + " (" + c.name +
                  ") has no facets on one of its sides");
    if (!(c.penalty > 0.0) || !(c.friction >= 0.0))
      return fail("contact " + std::to_string(c.id) +
                  " has no penalty or a negative friction");
    std::vector<std::pair<int, int>> slave, master;
    if (!facesOf(c.slaveTris, slave))
      return false;
    if (c.partner >= 0) {
      if (c.partner >= static_cast<int>(partners.size()))
        return fail("contact " + std::to_string(c.id) + " has no partner");
      const std::vector<std::pair<int, int>> pf =
          FacetFaces(partners[c.partner], partnerElem[c.partner] - 1);
      for (int ti : c.masterTris) {
        if (ti < 0 || ti >= static_cast<int>(pf.size()) || pf[ti].first == 0)
          return fail("a contact partner's facet is the face of no tet");
        master.push_back(pf[ti]);
      }
    } else if (!facesOf(c.masterTris, master)) {
      return false;
    }
    out << "*SURFACE, NAME=S" << k + 1 << "\n";
    for (const auto& [e, f] : slave)
      out << e << ", S" << f << "\n";
    out << "*SURFACE, NAME=M" << k + 1 << "\n";
    for (const auto& [e, f] : master)
      out << e << ", S" << f << "\n";
    out << "*SURFACE INTERACTION, NAME=I" << k + 1
        << "\n*SURFACE BEHAVIOR, PRESSURE-OVERCLOSURE=LINEAR\n"
        << Num(c.penalty) << "\n";
    if (c.friction > 0.0)
      out << "*FRICTION\n"
          << Num(c.friction) << ", " << Num(c.penalty / kStickSlopeRatio)
          << "\n";
    out << "*CONTACT PAIR, INTERACTION=I" << k + 1
        << ", TYPE=SURFACE TO SURFACE\nS" << k + 1 << ", M" << k + 1 << "\n";
  }
  // Weak springs to the ground, each direction, on every node of a part
  // nothing but a contact holds; and the settling step's stiffer ones.
  const bool settle = !nl.settlePerNode.empty();
  if (settle &&
      (nl.settlePerNode.size() != springs.size() || nl.steps.size() < 2))
    return fail("a settling step needs its springs and a step after it");
  std::string settleSets;
  for (size_t g = 0; g < springs.size(); ++g)
    for (int d = 1; d <= 3; ++d)
      for (int kind = 0; kind < (settle ? 2 : 1); ++kind) {
        const std::string set = std::string(kind ? "KS" : "K") +
                                std::to_string(g + 1) + "D" + std::to_string(d);
        out << "*ELEMENT, TYPE=SPRING1, ELSET=" << set << "\n";
        for (int n : springs[g].nodes)
          out << nextElem++ << ", " << n + 1 << "\n";
        out << "*SPRING, ELSET=" << set << "\n"
            << d << "\n"
            << Real(kind ? nl.settlePerNode[g] : springs[g].kPerNode) << "\n";
        if (kind)
          settleSets += (settleSets.empty() ? "" : "\n") + set;
      }

  // The pressures, and a reference node per torque or remote load.
  struct Pressure {
    double mpa;
    std::vector<std::pair<int, int>> faces;
  };
  struct Reference {
    int node;
    glm::dvec3 at, force, moment;
  };
  std::vector<Pressure> pressures;
  std::vector<Reference> refs;
  std::vector<std::vector<std::pair<int, int>>> refFaces;
  for (const FaceLoad& l : nl.loads) {
    std::vector<std::pair<int, int>> lf;
    if (!facesOf(l.tris, lf))
      return false;
    if (l.type == LoadType::Pressure) {
      pressures.push_back({l.magnitude, std::move(lf)});
    } else if (l.type == LoadType::Torque || l.type == LoadType::Remote) {
      Reference r;
      r.node = nextNode + static_cast<int>(refs.size());
      r.at = l.origin;
      if (!LoadResultant(mesh, l, r.at, r.force, r.moment, error))
        return false;
      refs.push_back(r);
      refFaces.push_back(std::move(lf));
    } else {
      return fail(std::string("a nonlinear deck writes a ") +
                  LoadTypeToString(l.type) + " as nodal forces");
    }
  }
  for (size_t k = 0; k < refs.size(); ++k) {
    out << "*NODE\n"
        << refs[k].node << ", " << Num(refs[k].at.x) << ", "
        << Num(refs[k].at.y) << ", " << Num(refs[k].at.z) << "\n";
    out << "*SURFACE, NAME=REF" << k + 1 << "\n";
    for (const auto& [e, f] : refFaces[k])
      out << e << ", S" << f << "\n";
    out << "*COUPLING, REF NODE=" << refs[k].node << ", SURFACE=REF" << k + 1
        << ", CONSTRAINT NAME=REF" << k + 1 << "\n*DISTRIBUTING\n1, 6\n";
  }
  // The supported nodes, whose reactions each increment prints.
  std::vector<int> held;
  for (int n = 0; n < nNodes; ++n)
    if (fixed[n] || frameOf[n] >= 0)
      held.push_back(n);
  if (held.empty() && contacts.empty())
    return fail("a nonlinear run needs a support");
  if (!held.empty()) {
    out << "*NSET, NSET=NSUP\n";
    WriteIdLines(out, held);
  }
  // A thermal load's initial temperature, every node: the body's (NALL)
  // and each rigid partner's, whose elements ccx checks too.
  if (thermal) {
    out << "*INITIAL CONDITIONS, TYPE=TEMPERATURE\nNALL, "
        << Num(thermal->stressFreeC) << "\n";
    for (size_t k = 0; k < partners.size(); ++k)
      out << "P" << k + 1 << ", " << Num(thermal->stressFreeC) << "\n";
  }
  double from = 0.0;
  for (size_t si = 0; si < nl.steps.size(); ++si) {
    const NonlinearStep& st = nl.steps[si];
    const double period = st.toFactor - from;
    if (!(period > 0.0) || !(st.maxIncrement > 0.0))
      return fail("a nonlinear step runs the load up");
    const double inc = std::min(st.maxIncrement, period);
    out << "*STEP" << (nl.nlgeom ? ", NLGEOM" : "")
        << ", INC=" << nl.maxIncrements << "\n*STATIC\n"
        << Num(inc) << ", " << Num(period) << ", "
        << Num(std::min(nl.minIncrement, inc)) << ", " << Num(inc) << "\n";
    out << "*CONTROLS, PARAMETERS=FIELD\n"
        << Num(kNonlinearResidual) << ", , , , " << Num(kNonlinearResidual)
        << "\n";
    if (settle && si == 1)
      out << "*MODEL CHANGE, TYPE=ELEMENT, REMOVE\n" << settleSets << "\n";
    const double lambda = st.toFactor;
    bool anyLoad = false;
    const auto cload = [&](int node, int dof, double v) {
      if (v == 0.0)
        return;
      if (!anyLoad)
        out << "*CLOAD\n";
      anyLoad = true;
      out << node << ", " << dof << ", " << Num(v) << "\n";
    };
    for (int n = 0; n < nNodes; ++n) {
      if (fixed[n])
        continue;
      glm::dvec3 f = lambda * nodalForces[n];
      if (frameOf[n] >= 0) {
        const auto& pin = pins[pinOf[n]];
        const auto cyl = CylFrame(mesh.nodes[n], pin.axisOrigin,
                                  glm::normalize(pin.axisDirection));
        f = glm::dvec3(glm::dot(f, cyl[0]), glm::dot(f, cyl[1]),
                       glm::dot(f, cyl[2]));
      }
      for (int k = 0; k < 3; ++k)
        cload(n + 1, k + 1, f[k]);
    }
    for (const Reference& r : refs)
      for (int k = 0; k < 3; ++k) {
        cload(r.node, k + 1, lambda * r.force[k]);
        cload(r.node, k + 4, lambda * r.moment[k]);
      }
    if (!pressures.empty()) {
      out << "*DLOAD\n";
      for (const Pressure& pr : pressures)
        for (const auto& [e, f] : pr.faces)
          out << e << ", P" << f << ", " << Num(lambda * pr.mpa) << "\n";
    }
    if (thermal)
      WriteTemperatures(out, *thermal, lambda, false, partners.size());
    out << "*NODE PRINT, NSET=NALL, GLOBAL=YES\nU\n";
    if (!held.empty())
      out << "*NODE PRINT, NSET=NSUP, GLOBAL=YES\nRF\n";
    for (size_t k = 0; k < partners.size(); ++k)
      out << "*NODE PRINT, NSET=P" << k + 1 << ", TOTALS=ONLY\nRF\n";
    for (size_t k = 0; k < contacts.size(); ++k)
      out << "*CONTACT PRINT, SLAVE=S" << k + 1 << ", MASTER=M" << k + 1
          << "\nCF\n";
    if (!contacts.empty())
      out << "*CONTACT PRINT\nCSTR\n";
    if (nl.plasticity)
      out << "*EL PRINT, ELSET=EBODY\nS, PEEQ\n";
    out << "*END STEP\n";
    from = st.toFactor;
  }
  // A plastic run takes its load off again, over as long as it took to put
  // on: what is left is the deformation the yield left behind. Every load
  // goes (*CLOAD and *DLOAD, OP=NEW, ramped down); the supports and
  // contacts stay.
  if (nl.unload && !nl.steps.empty()) {
    const double period = nl.steps.back().toFactor;
    const double inc = std::min(nl.steps.back().maxIncrement, period);
    out << "*STEP" << (nl.nlgeom ? ", NLGEOM" : "")
        << ", INC=" << nl.maxIncrements << "\n*STATIC\n"
        << Num(inc) << ", " << Num(period) << ", "
        << Num(std::min(nl.minIncrement, inc)) << ", " << Num(inc) << "\n";
    out << "*CONTROLS, PARAMETERS=FIELD\n"
        << Num(kNonlinearResidual) << ", , , , " << Num(kNonlinearResidual)
        << "\n";
    out << "*CLOAD, OP=NEW\n";
    if (!pressures.empty())
      out << "*DLOAD, OP=NEW\n";
    // Cooled back to where it was stress-free: what is left is what heating
    // and cooling left.
    if (thermal)
      WriteTemperatures(out, *thermal, 0.0, true, partners.size());
    out << "*NODE PRINT, NSET=NALL, GLOBAL=YES\nU\n";
    if (!held.empty())
      out << "*NODE PRINT, NSET=NSUP, GLOBAL=YES\nRF\n";
    for (size_t k = 0; k < partners.size(); ++k)
      out << "*NODE PRINT, NSET=P" << k + 1 << ", TOTALS=ONLY\nRF\n";
    out << "*END STEP\n";
  }
  if (!out)
    return fail("the deck could not be written");
  return true;
}

} // namespace

bool WriteDeck(std::ostream& out, const FemMesh& mesh,
               const std::vector<Material>& byDomain, const Supports& supports,
               const std::vector<StudyJobBody::PinSupport>& pins,
               const std::vector<glm::dvec3>& nodalForces,
               const std::string& title, std::string* error, int buckleFactors,
               const DeckNonlinear* nonlinear,
               const NodalTemperature* thermal) {
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
  if (thermal && thermal->temperature.size() != mesh.nodes.size())
    return fail("one temperature per node is needed");

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
  // One ELSET per domain, each tet's corners positively oriented
  // (WrittenOrder).
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
      out << t + 1;
      for (int i : WrittenOrder(mesh, t))
        out << ", " << q[i] + 1;
      out << "\n";
    }
    if (!any)
      continue;
    const Material& m = byDomain[std::min(d, byDomain.size() - 1)];
    out << "*MATERIAL, NAME=M" << d + 1 << "\n*ELASTIC\n"
        << Num(m.youngMPa) << ", " << Num(m.poisson) << "\n";
    // A plastic run's hardening curve: von Mises stress against the
    // equivalent plastic strain, isotropic hardening (ccx's default).
    if (nonlinear && nonlinear->plasticity) {
      if (m.hardening.empty())
        return fail("a plastic deck needs every material's hardening curve");
      out << "*PLASTIC\n";
      for (const auto& [stress, strain] : m.hardening)
        out << Num(stress) << ", " << Num(strain) << "\n";
    }
    if (m.densityTPerMM3 > 0)
      out << "*DENSITY\n" << Num(m.densityTPerMM3) << "\n";
    // A thermal load's expansion, from the stress-free temperature.
    if (thermal)
      out << "*EXPANSION, ZERO=" << Num(thermal->stressFreeC) << "\n"
          << Num(m.expansionPerK) << "\n";
    out << "*SOLID SECTION, ELSET=D" << d + 1 << ", MATERIAL=M" << d + 1
        << "\n";
  }

  // The body's elements, whose stress and plastic strain a plastic run
  // prints at every integration point.
  if (nonlinear && nonlinear->plasticity) {
    std::vector<int> sets;
    for (size_t d = 0; d < nDomains; ++d)
      for (size_t t = 0; t < mesh.tets.size(); ++t)
        if ((mesh.tetDomain.empty() ? 0 : (size_t)mesh.tetDomain[t]) == d) {
          sets.push_back(static_cast<int>(d));
          break;
        }
    out << "*ELSET, ELSET=EBODY\n";
    for (size_t i = 0; i < sets.size(); ++i)
      out << "D" << sets[i] + 1
          << ((i + 1) % 8 == 0 || i + 1 == sets.size() ? "\n" : ", ");
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

  // A thermal load's nodes start where the part is stress-free (a
  // nonlinear deck writes it with its partners' nodes).
  if (thermal && !nonlinear)
    out << "*INITIAL CONDITIONS, TYPE=TEMPERATURE\nNALL, "
        << Num(thermal->stressFreeC) << "\n";
  if (nonlinear)
    return WriteNonlinearSteps(out, mesh, byDomain, fixed, frameOf, pinOf, pins,
                               nodalForces, *nonlinear, error, thermal);
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
  if (thermal)
    WriteTemperatures(out, *thermal, 1.0, false);
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
  std::error_code ec;
  if (!fs::is_directory(root, ec))
    return;
  for (const auto& entry : fs::directory_iterator(root, ec)) {
    const std::string name = entry.path().filename().string();
    char* end = nullptr;
    const long pid = std::strtol(name.c_str(), &end, 10);
    if (pid <= 0 || end == name.c_str() || *end != '-' || pid == ThisPid())
      continue;
    if (!proc::ProcessAlive(pid))
      fs::remove_all(entry.path(), ec);
  }
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
            const std::function<bool(std::string*)>& read,
            const std::function<bool()>& keepGoing = nullptr,
            std::string* ccxError = nullptr) {
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
    // A watcher that has what it wanted stops the run: what ccx wrote so
    // far is the answer.
    if (keepGoing && !keepGoing()) {
      child.Kill();
      std::string ignored;
      child.Finish(&ignored);
      if (!read(&err))
        return fail(err);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::string tail;
  child.Finish(&tail);
  const std::string said = ReadFile(dir / "job.out");
  const std::string first = FirstCalculixError(said + "\n" + tail);
  // A nonlinear run reads its increments whatever ccx said, and is told
  // ccx's error to judge.
  if (ccxError) {
    *ccxError = first;
    if (first.empty() && code != 0)
      *ccxError = "CalculiX exited with status " + std::to_string(code);
    if (!read(&err))
      return fail(err);
    return true;
  }
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
      if (!WriteDeck(deck, jb.mesh, mats, sups[k], jb.pins, forces, title, &err,
                     0, nullptr, options.thermal.get())) {
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

uint64_t CalculixHeatMemoryEstimate(size_t nodes) {
  // Fitted, as CalculixStaticMemoryEstimate is, to measured runs of ccx
  // 2.23 on a 100 x 20 x 20 bar held at its ends (docs/simulation/README.md
  // #thermal): 44 541 nodes, 187 MB; 337 881 nodes, 2.29 GB.
  constexpr double kBaseBytes = 55e6;
  constexpr double kCoefficient = 45.7; // bytes per node^1.39
  constexpr double kExponent = 1.39;
  return static_cast<uint64_t>(
      kBaseBytes +
      kCoefficient * std::pow(static_cast<double>(nodes), kExponent));
}

bool WriteHeatDeck(std::ostream& out, const FemMesh& mesh,
                   const std::vector<Material>& byDomain,
                   const HeatLoads& loads, const std::string& title,
                   std::string* error) {
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
  std::vector<double> q;
  std::string err;
  if (!HeatNodalLoads(mesh, loads, q, &err))
    return fail(err);
  out << "** " << title << "\n";
  out << "** Written by Loboele from its own study: the Netgen mesh, a "
         "material per domain, the assembled nodal heat.\n";
  out << "** Units: mm, mW, degrees C (a conductivity in W/(m K) is the same "
         "number in mW/(mm K)).\n";
  out << "*NODE, NSET=NALL\n";
  for (int n = 0; n < nNodes; ++n) {
    const glm::dvec3& p = mesh.nodes[n];
    out << n + 1 << ", " << Num(p.x) << ", " << Num(p.y) << ", " << Num(p.z)
        << "\n";
  }
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
      const auto& tet = mesh.tets[t];
      out << t + 1;
      for (int i : WrittenOrder(mesh, t))
        out << ", " << tet[i] + 1;
      out << "\n";
    }
    if (!any)
      continue;
    const Material& m = byDomain[std::min(d, byDomain.size() - 1)];
    if (!(m.conductivityWmK > 0.0))
      return fail("a heat deck needs every material's conductivity");
    out << "*MATERIAL, NAME=M" << d + 1 << "\n*CONDUCTIVITY\n"
        << Num(m.conductivityWmK) << "\n";
    out << "*SOLID SECTION, ELSET=D" << d + 1 << ", MATERIAL=M" << d + 1
        << "\n";
  }
  // The held nodes, each once at its first load's temperature (SolveHeat's
  // rule).
  std::vector<int> heldBy(nNodes, -1);
  for (size_t k = 0; k < loads.held.size(); ++k)
    for (int t : loads.held[k].tris)
      for (int v : mesh.boundary[t].nodes)
        if (heldBy[v] < 0)
          heldBy[v] = static_cast<int>(k);
  out << "*STEP\n*HEAT TRANSFER, STEADY STATE\n1., 1.\n";
  bool anyHeld = false;
  for (int n = 0; n < nNodes; ++n)
    if (heldBy[n] >= 0) {
      if (!anyHeld)
        out << "*BOUNDARY\n";
      anyHeld = true;
      out << n + 1 << ", 11, 11, " << Num(loads.held[heldBy[n]].temperatureC)
          << "\n";
    }
  bool anyFlux = false;
  for (int n = 0; n < nNodes; ++n)
    if (heldBy[n] < 0 && q[n] != 0.0) {
      if (!anyFlux)
        out << "*CFLUX\n";
      anyFlux = true;
      out << n + 1 << ", 11, " << Num(q[n]) << "\n";
    }
  if (!loads.films.empty()) {
    const std::vector<std::pair<int, int>> faces = FacetFaces(mesh, 0);
    out << "*FILM\n";
    for (const HeatLoads::Film& f : loads.films)
      for (int t : f.tris) {
        if (faces[t].first == 0)
          return fail("convection " + std::to_string(f.id) +
                      ": a facet of its face is the face of no element");
        out << faces[t].first << ", F" << faces[t].second << ", "
            << Num(f.ambientC) << ", " << Num(f.hMWmm2K) << "\n";
      }
  }
  out << "*NODE PRINT, NSET=NALL\nNT\n*NODE FILE\nNT\n*END STEP\n";
  if (!out)
    return fail("the deck could not be written");
  return true;
}

bool ReadDatTemperatures(const std::string& path, size_t nNodes,
                         std::vector<double>& out, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  std::ifstream in(path);
  if (!in)
    return fail("CalculiX wrote no " + fs::path(path).filename().string());
  out.assign(nNodes, 0.0);
  std::vector<char> seen(nNodes, 0);
  std::string line;
  bool inBlock = false, started = false;
  size_t count = 0;
  while (std::getline(in, line)) {
    if (!inBlock) {
      if (line.find("temperatures for set") != std::string::npos)
        inBlock = true;
      continue;
    }
    std::istringstream ls(line);
    std::string a, b;
    if (!(ls >> a)) {
      if (started)
        break;
      continue;
    }
    if (!(ls >> b))
      break;
    char* end = nullptr;
    const long node = std::strtol(a.c_str(), &end, 10);
    double t = 0.0;
    if (*end != '\0' || !ParseFortran(b, t))
      break;
    started = true;
    if (node < 1 || static_cast<size_t>(node) > nNodes)
      return fail("CalculiX printed a temperature for node " +
                  std::to_string(node) + ", which the deck does not have");
    out[node - 1] = t;
    if (!seen[node - 1])
      ++count;
    seen[node - 1] = 1;
  }
  if (!started)
    return fail("CalculiX's " + fs::path(path).filename().string() +
                " holds no temperatures");
  if (count != nNodes)
    return fail("CalculiX printed the temperature of " + std::to_string(count) +
                " of " + std::to_string(nNodes) + " nodes");
  return true;
}

bool HeatOnCalculix(const FemMesh& mesh, const std::vector<Material>& mats,
                    const HeatLoads& loads, const std::string& bodyName,
                    const SolveOptions& options,
                    std::vector<double>& temperature, std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!options.calculix)
    return fail("this study solves on CalculiX, and the run was given none");
  const CalculixRun& run = *options.calculix;
  const size_t nNodes = mesh.nodes.size();
  const uint64_t estimate = CalculixHeatMemoryEstimate(nNodes);
  if (run.estimated)
    run.estimated(estimate);
  const uint64_t available =
      run.availableBytes ? run.availableBytes : AvailableMemoryBytes();
  if (available > 0 && estimate > available)
    return fail("CalculiX would need about " + Gb(estimate) +
                " for a heat run of " + std::to_string(nNodes) +
                " nodes, and " + Gb(available) +
                " is free: its solver has no out-of-core mode, so the run was "
                "not started. A coarser mesh_size, or the in-house solver, "
                "solves it");
  std::string err;
  fs::path dir;
  if (!MakeRunDir(run, dir, &err))
    return fail(err);
  std::error_code ec;
  if (options.solving)
    options.solving("calculix: writing the heat deck");
  {
    std::ofstream deck(dir / "job.inp");
    if (!WriteHeatDeck(deck, mesh, mats, loads, "body '" + bodyName + "', heat",
                       &err)) {
      fs::remove_all(dir, ec);
      return fail(err);
    }
  }
  if (options.solving)
    options.solving("calculix: solving the heat");
  bool keep = true;
  if (!RunCcx(run, dir, options, keep, &err, [&](std::string* why) {
        return ReadDatTemperatures((dir / "job.dat").string(), nNodes,
                                   temperature, why);
      })) {
    if (!keep) {
      fs::remove_all(dir, ec);
      return fail(err);
    }
    return fail(err + " (the run is kept in " + dir.string() + ")");
  }
  fs::remove_all(dir, ec);
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
  if (in.type != AnalysisType::Static && in.type != AnalysisType::Buckling &&
      in.type != AnalysisType::Thermal)
    return fail(std::string("a CalculiX deck is written for a static, "
                            "buckling or thermal study; this one is ") +
                AnalysisTypeToString(in.type));
  if (body >= in.bodies.size())
    return fail("no such body");
  const StudyJobBody& jb = in.bodies[body];
  if (in.type == AnalysisType::Thermal) {
    if (caseIndex != 0)
      return fail("no such load case");
    const std::vector<Material> mats =
        jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
    return WriteHeatDeck(out, jb.mesh, mats, jb.heat, title, error);
  }
  const std::vector<std::vector<FaceLoad>> rhs =
      jb.caseLoads.empty() ? std::vector<std::vector<FaceLoad>>{jb.faceLoads}
                           : jb.caseLoads;
  if (caseIndex >= rhs.size())
    return fail("no such load case");
  const std::vector<Material> mats =
      jb.materials.empty() ? std::vector<Material>{jb.mat} : jb.materials;
  const glm::dvec3 g = in.gravity * 1000.0; // m/s^2 -> mm/s^2
  std::string err;
  // A thermal load's temperature: a uniform one as it is, a thermal study's
  // solved in-house on this mesh (the run solves it on CalculiX, and holds
  // that answer to the same K_T).
  SolveOptions opts;
  if (jb.thermalLoad.On()) {
    auto field = std::make_shared<NodalTemperature>();
    field->stressFreeC = jb.thermalLoad.stressFreeC;
    if (jb.thermalLoad.kind == ThermalLoad::Kind::Uniform) {
      field->temperature.assign(jb.mesh.nodes.size(), jb.thermalLoad.uniformC);
    } else {
      HeatSolution hs;
      if (!SolveHeat(jb.mesh, mats, jb.heat, SolveOptions{}, hs, &err))
        return fail("the thermal load: " + err);
      field->temperature = std::move(hs.temperature);
    }
    opts.thermal = field;
  }
  std::vector<Supports> sups;
  std::vector<std::vector<FemResults::SupportReaction>> pinRows;
  if (!StaticSupportsOf(jb, mats, {rhs[caseIndex]}, g, opts, sups, pinRows,
                        &err))
    return fail(err);
  if (in.nonlinear.On() || !jb.contacts.empty()) {
    // A large-deformation, plastic or contact run's deck
    // (docs/simulation/README.md#nonlinear-static, #plasticity, #contact):
    // what NonlinearOnCalculix's first run writes.
    if (in.nonlinear.snapThrough)
      return fail("a snap-through run's deck is seeded with the first "
                  "buckling mode and capped at twice its factor, both found "
                  "when the study runs; export_study_inp writes a "
                  "large-deformation deck: set nonlinear snap_through false");
    std::vector<FaceLoad> dead;
    DeckNonlinear deck;
    deck.nlgeom = in.nonlinear.largeDeformation;
    deck.contacts = &jb.contacts;
    deck.partners = &jb.rigidPartners;
    std::vector<StudyJobBody::Stabilized> weak;
    if (!WeakSprings(jb, mats, rhs[caseIndex], g, opts, weak, &err))
      return fail(err);
    deck.stabilized = &weak;
    deck.plasticity = in.nonlinear.plasticity;
    deck.unload = in.nonlinear.plasticity;
    for (const FaceLoad& l : rhs[caseIndex])
      (l.type == LoadType::Force || !deck.nlgeom ? dead : deck.loads)
          .push_back(l);
    std::vector<glm::dvec3> forces;
    if (!NodalForces(jb.mesh, mats, dead, g, forces, &err))
      return fail(err);
    const int increments = in.nonlinear.increments > 0 ? in.nonlinear.increments
                                                       : kNonlinearIncrements;
    deck.steps = {{1.0, 1.0 / increments}};
    deck.maxIncrements = 100 * increments + 1000;
    const double settleFactor =
        std::min(1.0 / increments, 1.0 / kNonlinearIncrements);
    deck.settlePerNode =
        SettleSprings(jb, mats, rhs[caseIndex], g, settleFactor);
    if (!deck.settlePerNode.empty())
      deck.steps.insert(deck.steps.begin(), {settleFactor, settleFactor});
    return WriteDeck(out, jb.mesh, mats, sups.front(), jb.pins, forces, title,
                     error, 0, &deck, opts.thermal.get());
  }
  std::vector<glm::dvec3> forces;
  if (!NodalForces(jb.mesh, mats, rhs[caseIndex], g, forces, &err))
    return fail(err);
  return WriteDeck(
      out, jb.mesh, mats, sups.front(), jb.pins, forces, title, error,
      in.type == AnalysisType::Buckling ? std::max(in.numModes, 1) : 0, nullptr,
      opts.thermal.get());
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
                   std::max(numFactors, 1), nullptr, options.thermal.get())) {
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

// ---- Nonlinear static -------------------------------------------------------
// (docs/simulation/README.md#nonlinear-static)

// Peak resident memory of ccx 2.23 running the same column's
// large-deformation deck (a 2 kN sideways pull, ten increments; the hidden
// "[calculix-nl-calibrate]" case): 5.7k DOF 57.8 MB, 16k 68.3 MB, 30k
// 124 MB, 66k 290 MB, 197k 1.24 GB, 241k 1.55 GB -- 1.02 to 1.10 times the
// linear run's, the tangent rebuilt each iteration in the same storage. The
// static estimate bounds them by as little as 7 % (197k), so this one is a
// tenth over it: 1.17 times the largest measured, or more.
uint64_t CalculixNonlinearMemoryEstimate(size_t dofs) {
  return static_cast<uint64_t>(
      1.1 * static_cast<double>(CalculixStaticMemoryEstimate(dofs)));
}

double ContactPenalty(double youngMPa, double edgeMm) {
  return edgeMm > 0.0 ? kContactPenalty * youngMPa / edgeMm : 0.0;
}

bool WeakSprings(const StudyJobBody& jb, const std::vector<Material>& mats,
                 const std::vector<FaceLoad>& loads,
                 const glm::dvec3& gravityAccel, const SolveOptions& options,
                 std::vector<StudyJobBody::Stabilized>& out,
                 std::string* error) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = "the weak springs that hold a part while its contact closes "
               "are scaled from its own stiffness, and " +
               m;
    return false;
  };
  out = jb.stabilized;
  if (out.empty())
    return true;
  // The case's own load; with none but a thermal one, a uniform body force
  // (every material a unit density, its scale cancelling in k) along
  // (1, 1, 1), so the softest direction answers.
  std::vector<Material> used = mats;
  std::vector<FaceLoad> probe = loads;
  glm::dvec3 g = gravityAccel;
  std::vector<glm::dvec3> f;
  std::string err;
  if (!NodalForces(jb.mesh, used, probe, g, f, &err))
    return fail(err);
  double total = 0.0;
  for (const glm::dvec3& v : f)
    total += glm::length(v);
  if (!(total > 0.0)) {
    for (Material& m : used)
      m.densityTPerMM3 = 1.0;
    probe.clear();
    g = glm::normalize(glm::dvec3(1.0));
    if (!NodalForces(jb.mesh, used, probe, g, f, &err))
      return fail(err);
    total = 0.0;
    for (const glm::dvec3& v : f)
      total += glm::length(v);
  }
  // Clamped where it can touch, as the contact holds it once closed: both
  // sides of a pair whose partner is meshed here.
  std::set<int> held(jb.fixedNodes.begin(), jb.fixedNodes.end());
  for (const auto& c : jb.contacts) {
    for (int t : c.slaveTris)
      for (int v : jb.mesh.boundary[t].nodes)
        held.insert(v);
    if (c.partner < 0) // the partner meshed here, not rigid
      for (int t : c.masterTris)
        if (t >= 0 && static_cast<size_t>(t) < jb.mesh.boundary.size())
          for (int v : jb.mesh.boundary[t].nodes)
            held.insert(v);
  }
  Supports sup;
  sup.fixedNodes.assign(held.begin(), held.end());
  SolveOptions own;
  own.cancelled = options.cancelled;
  own.makeExternal = options.makeExternal;
  auto r = SolveCases(jb.mesh, used, sup, {probe}, &err, g, own);
  if (!r)
    return fail("the part clamped where its contact closes did not solve: " +
                err);
  double work = 0.0;
  const std::vector<glm::dvec3>& u = r->front().displacement;
  for (size_t n = 0; n < f.size() && n < u.size(); ++n)
    work += glm::dot(f[n], u[n]);
  if (!(total > 0.0) || !(work > 0.0))
    return fail("its load does no work on it, clamped where its contact "
                "closes");
  const double k = total * total / work; // N/mm
  for (StudyJobBody::Stabilized& st : out)
    st.kPerNode = kStabilization * k /
                  static_cast<double>(std::max<size_t>(1, st.nodes.size()));
  return true;
}

std::vector<double> SettleSprings(const StudyJobBody& jb,
                                  const std::vector<Material>& mats,
                                  const std::vector<FaceLoad>& loads,
                                  const glm::dvec3& gravityAccel,
                                  double firstFactor) {
  std::vector<double> out;
  if (jb.stabilized.empty() || jb.contacts.empty())
    return out;
  std::vector<glm::dvec3> f;
  std::string err;
  if (!NodalForces(jb.mesh, mats, loads, gravityAccel, f, &err))
    return out;
  double total = 0.0;
  for (const glm::dvec3& v : f)
    total += glm::length(v);
  double edge = 0.0;
  size_t n = 0;
  for (const auto& c : jb.contacts)
    for (int t : c.slaveTris) {
      const auto& tri = jb.mesh.boundary[t];
      for (int a = 0; a < 3; ++a)
        edge += glm::length(jb.mesh.nodes[tri.nodes[a]] -
                            jb.mesh.nodes[tri.nodes[(a + 1) % 3]]);
      n += 3;
    }
  if (!(total > 0.0) || n == 0 || !(edge > 0.0))
    return out;
  const double motion = kSettleMotion * edge / static_cast<double>(n);
  for (const auto& st : jb.stabilized)
    out.push_back(firstFactor * total / motion /
                  static_cast<double>(std::max<size_t>(1, st.nodes.size())));
  return out;
}

namespace {

// One block of a ccx .dat, as *NODE PRINT writes it: "displacements
// (vx,vy,vz) for set NALL and time  0.25", a blank line, a row per node.
struct DatBlock {
  std::string kind; // displacements, forces, total, statistics, contact
  std::string set;
  double time = 0.0;
  // A node's row (a total's has node 0); a contact stress row is its slave
  // element * 10 + face, and (pressure, shear 1, shear 2).
  std::vector<std::pair<long, glm::dvec3>> rows;
  // A contact pair's statistics: the force and moment on the slave surface,
  // its centre and mean normal, the moment about that centre, and its area
  // in contact, normal force (+ = tension) and shear force.
  std::vector<double> values;
  // *EL PRINT rows (kind "stresses" or "peeq"): an element, its integration
  // point (1-4) and the values -- the stress as ccx prints it, xx yy zz xy
  // xz yz, or the equivalent plastic strain first.
  struct IpRow {
    long elem = 0;
    int ip = 0;
    std::array<double, 6> v{};
  };
  std::vector<IpRow> ip;
};

std::string Trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r");
  if (a == std::string::npos)
    return {};
  const size_t b = s.find_last_not_of(" \t\r");
  return s.substr(a, b - a + 1);
}

// The .dat read as ccx writes it, a whole block at a time: a block is
// complete at the blank line after its rows (or at the end of the run).
class DatStream {
public:
  explicit DatStream(fs::path path) : path_(std::move(path)) {}
  void Poll(std::vector<DatBlock>& out, bool final) {
    std::ifstream in(path_, std::ios::binary);
    if (in) {
      in.seekg(0, std::ios::end);
      const std::streamoff end = in.tellg();
      if (end > offset_) {
        in.seekg(offset_);
        std::string chunk(static_cast<size_t>(end - offset_), '\0');
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        chunk.resize(static_cast<size_t>(in.gcount()));
        offset_ += static_cast<std::streamoff>(chunk.size());
        buf_ += chunk;
      }
    }
    size_t start = 0, nl = 0;
    while ((nl = buf_.find('\n', start)) != std::string::npos) {
      Line(buf_.substr(start, nl - start), out);
      start = nl + 1;
    }
    buf_.erase(0, start);
    if (final) {
      if (!buf_.empty())
        Line(buf_, out);
      buf_.clear();
      Close(out);
    }
  }

private:
  void Line(const std::string& line, std::vector<DatBlock>& out) {
    const size_t time = line.find(" and time ");
    const size_t stats = line.find("statistics for slave set ");
    const size_t cstr = line.find("contact stress (slave element+face");
    // ccx writes no space before this one's "for set".
    const size_t peeq = line.find("equivalent plastic strain (elem");
    if (peeq != std::string::npos &&
        line.find(" and time ") != std::string::npos) {
      Close(out);
      cur_ = DatBlock{};
      cur_.kind = "peeq";
      const size_t fs = line.find("for set ");
      const size_t at = line.find(" and time ");
      if (fs != std::string::npos && fs < at)
        cur_.set = Trim(line.substr(fs + 8, at - fs - 8));
      ParseFortran(Trim(line.substr(at + 10)), cur_.time);
      open_ = true;
      return;
    }
    const size_t set = line.find(" for set ");
    if (time != std::string::npos &&
        (stats != std::string::npos || cstr != std::string::npos ||
         (set != std::string::npos && time > set))) {
      Close(out);
      cur_ = DatBlock{};
      if (stats != std::string::npos) {
        cur_.kind = "statistics";
        const size_t from = stats + 25;
        const size_t comma = line.find(',', from);
        cur_.set = Trim(line.substr(from, comma - from));
      } else if (cstr != std::string::npos) {
        cur_.kind = "contact";
      } else {
        std::istringstream ls(line);
        ls >> cur_.kind;
        cur_.set = Trim(line.substr(set + 9, time - set - 9));
      }
      ParseFortran(Trim(line.substr(time + 10)), cur_.time);
      open_ = true;
      return;
    }
    if (!open_)
      return;
    std::istringstream ls(line);
    std::vector<std::string> tok;
    for (std::string t; ls >> t;)
      tok.push_back(t);
    if (cur_.kind == "statistics") {
      // Lines of numbers between lines of words, 18 numbers in all.
      std::vector<double> v(tok.size());
      bool numbers = !tok.empty();
      for (size_t i = 0; i < tok.size() && numbers; ++i)
        numbers = ParseFortran(tok[i], v[i]);
      if (numbers)
        cur_.values.insert(cur_.values.end(), v.begin(), v.end());
      if (cur_.values.size() >= 18)
        Close(out);
      return;
    }
    if (tok.empty()) {
      if (!cur_.rows.empty() || !cur_.ip.empty())
        Close(out);
      return;
    }
    if (cur_.kind == "stresses" || cur_.kind == "peeq") {
      const size_t want = cur_.kind == "stresses" ? 8 : 3;
      DatBlock::IpRow r;
      char *e1 = nullptr, *e2 = nullptr;
      bool ok = tok.size() == want;
      if (ok) {
        r.elem = std::strtol(tok[0].c_str(), &e1, 10);
        r.ip = static_cast<int>(std::strtol(tok[1].c_str(), &e2, 10));
        ok = *e1 == '\0' && *e2 == '\0';
      }
      for (size_t i = 2; ok && i < want; ++i)
        ok = ParseFortran(tok[i], r.v[i - 2]);
      if (!ok) {
        Close(out);
        return;
      }
      cur_.ip.push_back(r);
      return;
    }
    glm::dvec3 v(0.0);
    long node = 0;
    bool ok = false;
    if (cur_.kind == "total" && tok.size() == 3) {
      ok = ParseFortran(tok[0], v.x) && ParseFortran(tok[1], v.y) &&
           ParseFortran(tok[2], v.z);
    } else if (cur_.kind == "contact" && tok.size() == 5) {
      char *e1 = nullptr, *e2 = nullptr;
      const long elem = std::strtol(tok[0].c_str(), &e1, 10);
      const long face = std::strtol(tok[1].c_str(), &e2, 10);
      node = elem * 10 + face;
      ok = *e1 == '\0' && *e2 == '\0' && ParseFortran(tok[2], v.x) &&
           ParseFortran(tok[3], v.y) && ParseFortran(tok[4], v.z);
    } else if (tok.size() == 4) {
      char* end = nullptr;
      node = std::strtol(tok[0].c_str(), &end, 10);
      ok = *end == '\0' && ParseFortran(tok[1], v.x) &&
           ParseFortran(tok[2], v.y) && ParseFortran(tok[3], v.z);
    }
    if (!ok) {
      Close(out);
      return;
    }
    cur_.rows.push_back({node, v});
  }
  void Close(std::vector<DatBlock>& out) {
    if (open_ &&
        (!cur_.rows.empty() || !cur_.ip.empty() || cur_.values.size() >= 18))
      out.push_back(std::move(cur_));
    cur_ = DatBlock{};
    open_ = false;
  }
  fs::path path_;
  std::streamoff offset_ = 0;
  std::string buf_;
  DatBlock cur_;
  bool open_ = false;
};

// The smallest increment ccx tried and gave up on after its last converged
// one (the .sta's lines whose attempt ends in U), 0 when none.
double LastFailedIncrement(const fs::path& sta) {
  std::ifstream in(sta);
  std::string line;
  double failed = 0.0;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string step, inc, att, itrs, tot, stepTime, incTime;
    if (!(ls >> step >> inc >> att >> itrs >> tot >> stepTime >> incTime))
      continue;
    double v = 0.0;
    if (!ParseFortran(incTime, v))
      continue;
    if (!att.empty() && att.back() == 'U')
      failed = failed == 0.0 ? v : std::min(failed, v);
    else
      failed = 0.0; // converged since
  }
  return failed;
}

// The total time of the last converged increment in the .sta, 0 when none.
double StaReached(const fs::path& sta) {
  std::ifstream in(sta);
  std::string line;
  double reached = 0.0;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string step, inc, att, itrs, tot;
    if (!(ls >> step >> inc >> att >> itrs >> tot) || att.empty() ||
        att.back() == 'U')
      continue;
    double v = 0.0;
    if (ParseFortran(tot, v))
      reached = std::max(reached, v);
  }
  return reached;
}

// One converged increment: its factor (ccx's total time), every node's
// displacement, and the supported nodes' RF in the order the deck lists
// them.
struct Increment {
  double factor = 0.0;
  double maxU = 0.0;
  std::vector<glm::dvec3> u;
  std::vector<glm::dvec3> rf; // per node, zero off the supports
  // A contact run's: each rigid partner's total reaction, each pair's
  // statistics (DatBlock::values) and its largest contact pressure.
  std::vector<glm::dvec3> partnerRf;
  std::vector<std::vector<double>> stats;
  std::vector<double> maxPressure;
  // A plastic run's: each tet's stress and equivalent plastic strain at its
  // four Gauss points, in its own corner order (IntegrationPointFields).
  std::vector<std::array<Stress, 4>> ipStress;
  std::vector<std::array<double, 4>> ipPeeq;
  bool HasIp() const { return !ipStress.empty(); }
};

// How one ccx run of a nonlinear deck went.
struct PassOutcome {
  enum class End { Completed, Stopped, Jump, Deflection } end = End::Completed;
  std::string ccxError; // a Stopped run's: ccx's words
  double failedIncrement = 0.0;
  Increment before;       // the last increment before the end (or the last)
  double afterFactor = 0; // Jump, Deflection: the increment that ended it
  double afterMaxU = 0;
  bool haveBefore = false;
  // A plastic run's last increment of its unloading step: the deformation
  // the load left behind (haveUnloaded false when it did not finish).
  Increment unloaded;
  bool haveUnloaded = false;
  // A large-deformation run's jump on the way, kept going.
  bool snapped = false;
  double snappedFrom = 0.0, snappedTo = 0.0;
  double firstYield = 0.0;
  std::vector<std::pair<double, double>> curve; // (factor, max motion)
  bool stoppedEarly = false;
};

// Watches a run's increments as they land: the curve, the progress, the
// first yield, and the events that end a snap-through run. Events count
// only above `above` (a second run's first step re-walks the path).
struct PathWatch {
  const FemMesh* mesh = nullptr;
  const std::vector<Material>* mats = nullptr;
  const std::vector<int>* held = nullptr; // supported nodes
  const NonlinearRequest* req = nullptr;
  double above = 0.0;
  PassOutcome* out = nullptr;
  // A contact run: its partners and pairs, and which pair each slave
  // element face (element * 10 + face) belongs to. Its increments are
  // taken whole, once the next one starts (contact blocks follow the
  // nodes'), and it looks for no snaps: a contact that closes or opens
  // changes the stiffness by design.
  size_t partners = 0, pairs = 0;
  std::map<long, int> pairOf;
  // A plastic run (docs/simulation/README.md#plasticity): its increments
  // carry the stress and plastic strain ccx printed at each Gauss point,
  // and are taken whole as a contact run's are. `corner[t][k]` is the
  // corner of tet t (ours) nearest the deck's integration point k + 1.
  // Increments past `cap` are the unloading step's.
  bool plastic = false;
  double cap = 1.0;
  std::vector<std::array<int, 4>> corner;
  std::optional<Increment> pendingInc;
  Increment prev, prev2;
  bool havePrev = false;
  double prevCompliance = -1.0;
  bool candidate = false;
  double candidateCompliance = 0.0, candidateFactor = 0.0, candidateMaxU = 0.0;
  double prevYieldRatio = 0.0;
  std::optional<DatBlock> pendingU;
  bool ended = false;

  bool SameTime(double a, double b) const {
    return std::abs(a - b) <= 1e-9 * std::max(1.0, std::abs(b));
  }
  // A contact run's increment, filled as its blocks land.
  void FeedContact(DatBlock& b) {
    const size_t n = mesh->nodes.size();
    if (b.kind == "displacements" && b.set == "NALL") {
      if (pendingInc)
        TakePending();
      Increment inc;
      inc.factor = b.time;
      inc.u.assign(n, glm::dvec3(0.0));
      for (const auto& [node, v] : b.rows)
        if (node >= 1 && static_cast<size_t>(node) <= n)
          inc.u[node - 1] = v;
      for (const glm::dvec3& u : inc.u)
        inc.maxU = std::max(inc.maxU, glm::length(u));
      inc.rf.assign(n, glm::dvec3(0.0));
      inc.partnerRf.assign(partners, glm::dvec3(0.0));
      inc.stats.assign(pairs, {});
      inc.maxPressure.assign(pairs, 0.0);
      if (plastic) {
        inc.ipStress.assign(corner.size(), {});
        inc.ipPeeq.assign(corner.size(), {});
      }
      pendingInc = std::move(inc);
      return;
    }
    if (!pendingInc || !SameTime(pendingInc->factor, b.time))
      return;
    Increment& inc = *pendingInc;
    if (b.kind == "forces" && b.set == "NSUP") {
      for (const auto& [node, v] : b.rows)
        if (node >= 1 && static_cast<size_t>(node) <= n)
          inc.rf[node - 1] = v;
    } else if (b.kind == "total" && b.set.size() > 1 && b.set[0] == 'P') {
      const size_t k = std::strtoul(b.set.c_str() + 1, nullptr, 10);
      if (k >= 1 && k <= partners && !b.rows.empty())
        inc.partnerRf[k - 1] = b.rows.front().second;
    } else if (b.kind == "statistics" && b.set.size() > 1 && b.set[0] == 'S') {
      const size_t k = std::strtoul(b.set.c_str() + 1, nullptr, 10);
      if (k >= 1 && k <= pairs)
        inc.stats[k - 1] = b.values;
    } else if ((b.kind == "stresses" || b.kind == "peeq") && plastic &&
               b.set == "EBODY") {
      const bool stress = b.kind == "stresses";
      for (const DatBlock::IpRow& r : b.ip) {
        if (r.elem < 1 || static_cast<size_t>(r.elem) > corner.size() ||
            r.ip < 1 || r.ip > 4)
          continue;
        const size_t t = static_cast<size_t>(r.elem - 1);
        const int c = corner[t][r.ip - 1];
        if (stress) // ccx: xx yy zz xy xz yz; ours: xx yy zz xy yz zx
          inc.ipStress[t][c] = {r.v[0], r.v[1], r.v[2], r.v[3], r.v[5], r.v[4]};
        else
          inc.ipPeeq[t][c] = r.v[0];
      }
    } else if (b.kind == "contact") {
      for (const auto& [code, v] : b.rows)
        if (const auto it = pairOf.find(code); it != pairOf.end())
          inc.maxPressure[it->second] =
              std::max(inc.maxPressure[it->second], v.x);
    }
  }
  void TakePending() {
    Increment inc = std::move(*pendingInc);
    pendingInc.reset();
    Take(std::move(inc));
  }

  // Feeds blocks; false once an event says the run has what it needs.
  bool Feed(std::vector<DatBlock>& blocks) {
    for (DatBlock& b : blocks) {
      if (ended)
        break;
      if (pairs > 0 || plastic) {
        FeedContact(b);
        continue;
      }
      if (b.kind == "displacements" && b.set == "NALL") {
        pendingU = std::move(b);
      } else if (b.kind == "forces" && b.set == "NSUP" && pendingU &&
                 std::abs(pendingU->time - b.time) <=
                     1e-9 * std::max(1.0, std::abs(b.time))) {
        Increment inc;
        inc.factor = b.time;
        const size_t n = mesh->nodes.size();
        inc.u.assign(n, glm::dvec3(0.0));
        for (const auto& [node, v] : pendingU->rows)
          if (node >= 1 && static_cast<size_t>(node) <= n)
            inc.u[node - 1] = v;
        inc.rf.assign(n, glm::dvec3(0.0));
        for (const auto& [node, v] : b.rows)
          if (node >= 1 && static_cast<size_t>(node) <= n)
            inc.rf[node - 1] = v;
        for (const glm::dvec3& u : inc.u)
          inc.maxU = std::max(inc.maxU, glm::length(u));
        pendingU.reset();
        Take(std::move(inc));
      }
    }
    blocks.clear();
    return !ended;
  }

  void Take(Increment inc) {
    // A plastic run's unloading step: kept apart from the path.
    if (plastic && inc.factor > cap * (1.0 + 1e-9)) {
      out->unloaded = std::move(inc);
      out->haveUnloaded = true;
      return;
    }
    out->curve.push_back({inc.factor, inc.maxU});
    // The first yield on the path, interpolated in the factor: on a
    // plastic run from the stress ccx printed, which is elastic up to it.
    if (!req->yieldAt.empty() && out->firstYield == 0.0) {
      const FemResults f =
          inc.HasIp() ? IntegrationPointFields(*mesh, inc.u, inc.ipStress, {})
                      : LargeDeformationFields(*mesh, *mats, inc.u);
      double ratio = 0.0;
      for (size_t n = 0; n < f.vonMises.size(); ++n)
        if (req->yieldAt[n] > 0)
          ratio = std::max(ratio, f.vonMises[n] / req->yieldAt[n]);
      if (ratio >= 1.0) {
        const double f0 = havePrev ? prev.factor : 0.0;
        const double r0 = havePrev ? prevYieldRatio : 0.0;
        out->firstYield =
            ratio > r0 ? f0 + (1.0 - r0) / (ratio - r0) * (inc.factor - f0)
                       : inc.factor;
      }
      prevYieldRatio = ratio;
    }
    // Its compliance: the largest nodal motion over the load it took.
    const double df = inc.factor - (havePrev ? prev.factor : 0.0);
    double du = 0.0;
    for (size_t n = 0; n < inc.u.size(); ++n)
      du = std::max(
          du, glm::length(inc.u[n] - (havePrev ? prev.u[n] : glm::dvec3(0.0))));
    const double compliance = df > 0 ? du / df : 0.0;
    const bool counts = inc.factor > above * (1.0 + 1e-12) && pairs == 0;
    // A snap is an increment far more compliant than the one before it
    // AND than the one after: the part jumped, and landed on a stiffer
    // branch. A path that only steepens (an imperfect column nearing its
    // Euler load) is not one -- its compliance keeps growing.
    if (candidate && counts) {
      if (compliance * kSnapJumpRatio < candidateCompliance) {
        if (req->snapThrough) {
          End(PassOutcome::End::Jump, std::move(prev2), candidateFactor,
              candidateMaxU);
          return;
        }
        if (!out->snapped) {
          out->snapped = true;
          out->snappedFrom = prev2.factor;
          out->snappedTo = candidateFactor;
        }
      }
      candidate = false;
    }
    if (req->snapThrough && counts && havePrev && req->deflectionLimitMm > 0 &&
        inc.maxU >= req->deflectionLimitMm) {
      End(PassOutcome::End::Deflection, std::move(prev), inc.factor, inc.maxU);
      return;
    }
    if (counts && havePrev && prevCompliance > 0.0 &&
        compliance > kSnapJumpRatio * prevCompliance) {
      candidate = true;
      candidateCompliance = compliance;
      candidateFactor = inc.factor;
      candidateMaxU = inc.maxU;
    }
    prevCompliance = counts ? compliance : -1.0;
    prev2 = std::move(prev);
    prev = std::move(inc);
    havePrev = true;
  }

  void End(PassOutcome::End how, Increment before, double afterFactor,
           double afterMaxU) {
    out->end = how;
    out->before = std::move(before);
    out->haveBefore = true;
    out->afterFactor = afterFactor;
    out->afterMaxU = afterMaxU;
    ended = true;
  }

  // At the end of the run: a jump on its last increment, with nothing
  // after it to land on, still counts.
  void Finish() {
    if (!ended && pendingInc)
      TakePending();
    // A plastic run's last increment that turned compliant is the yield
    // spreading, not a jump: one counts only with an increment after it.
    if (ended || !candidate || plastic)
      return;
    if (req->snapThrough)
      End(PassOutcome::End::Jump, std::move(prev2), candidateFactor,
          candidateMaxU);
    else if (!out->snapped) {
      out->snapped = true;
      out->snappedFrom = prev2.factor;
      out->snappedTo = candidateFactor;
    }
  }
};

bool IsNonConvergence(const std::string& e) {
  return e.find("increment size smaller than minimum") != std::string::npos ||
         e.find("solution seems to diverge") != std::string::npos;
}

} // namespace

bool NonlinearOnCalculix(
    const StudyJobBody& jb, const std::vector<Material>& mats,
    const Supports& supports, const std::vector<FaceLoad>& loads,
    const glm::dvec3& gravityAccel, const NonlinearRequest& req,
    const SolveOptions& options, FemResults& result,
    StudyBodyResult::Nonlinear& summary, std::string* error,
    std::vector<StudyBodyResult::ContactResult>* contactsOut) {
  const auto fail = [&](const std::string& m) {
    if (error)
      *error = m;
    return false;
  };
  if (!options.calculix)
    return fail("this study solves on CalculiX, and the run was given none");
  const CalculixRun& run = *options.calculix;
  const FemMesh& mesh = jb.mesh;
  const size_t nNodes = mesh.nodes.size();
  std::string err;
  // Dead loads as nodal forces; under large deformation a pressure, a
  // torque and a remote load through the deck's own keywords, and at small
  // deformation (a contact run) as the nodal forces the in-house solve
  // applies.
  std::vector<FaceLoad> dead, other;
  for (const FaceLoad& l : loads)
    (l.type == LoadType::Force || !req.nlgeom ? dead : other).push_back(l);
  std::vector<glm::dvec3> forces;
  if (!NodalForces(mesh, mats, dead, gravityAccel, forces, &err))
    return fail(err);
  std::vector<char> isHeld(nNodes, 0);
  for (int n : supports.fixedNodes)
    if (n >= 0 && static_cast<size_t>(n) < nNodes)
      isHeld[n] = 1;
  for (const HeldNode& h : supports.held)
    if (h.node >= 0 && static_cast<size_t>(h.node) < nNodes)
      isHeld[h.node] = 1;
  std::vector<int> held;
  for (size_t n = 0; n < nNodes; ++n)
    if (isHeld[n])
      held.push_back(static_cast<int>(n));

  const size_t dofs = 3 * (nNodes - std::min(nNodes, held.size()));
  const uint64_t estimate = CalculixNonlinearMemoryEstimate(dofs);
  if (run.estimated)
    run.estimated(estimate);
  const uint64_t available =
      run.availableBytes ? run.availableBytes : AvailableMemoryBytes();
  if (available > 0 && estimate > available)
    return fail("a nonlinear CalculiX run would need about " + Gb(estimate) +
                " for " + std::to_string(dofs) + " degrees of freedom, and " +
                Gb(available) +
                " is free: its solver has no out-of-core mode, so the run was "
                "not started. A coarser mesh_size solves it");

  // Which pair each slave element face is (the contact stress rows).
  std::map<long, int> pairOf;
  if (!jb.contacts.empty()) {
    const std::vector<std::pair<int, int>> faces = FacetFaces(mesh, 0);
    for (size_t k = 0; k < jb.contacts.size(); ++k)
      for (int t : jb.contacts[k].slaveTris)
        if (t >= 0 && t < static_cast<int>(faces.size()))
          pairOf[faces[t].first * 10L + faces[t].second] = static_cast<int>(k);
  }
  const double cap = req.maxFactor;
  const int increments = std::max(1, req.increments);
  // One ccx run over `steps`, watched.
  const double settleFactor =
      std::min(cap / increments, cap / kNonlinearIncrements);
  const std::vector<double> settle =
      req.snapThrough
          ? std::vector<double>{}
          : SettleSprings(jb, mats, loads, gravityAccel, settleFactor);
  std::vector<StudyJobBody::Stabilized> weak;
  if (!WeakSprings(jb, mats, loads, gravityAccel, options, weak, &err))
    return fail(err);
  const auto pass = [&](const std::vector<NonlinearStep>& steps, double above,
                        PassOutcome& po) -> bool {
    DeckNonlinear deck;
    deck.loads = other;
    deck.steps = steps;
    deck.minIncrement = 1e-5 * cap;
    deck.maxIncrements = 100 * increments + 1000;
    deck.nlgeom = req.nlgeom;
    deck.contacts = &jb.contacts;
    deck.partners = &jb.rigidPartners;
    deck.stabilized = &weak;
    deck.plasticity = req.plasticity;
    deck.unload = req.plasticity && !req.snapThrough;
    if (steps.size() >= 2 && !settle.empty())
      deck.settlePerNode = settle;
    fs::path dir;
    if (!MakeRunDir(run, dir, &err))
      return fail(err);
    std::error_code ec;
    {
      std::ofstream f(dir / "job.inp");
      if (!WriteDeck(f, mesh, mats, supports, jb.pins, forces,
                     "body '" + jb.bodyName + "', nonlinear", &err, 0, &deck,
                     options.thermal.get())) {
        fs::remove_all(dir, ec);
        return fail(err);
      }
    }
    if (options.solving)
      options.solving("calculix: nonlinear");
    DatStream dat(dir / "job.dat");
    PathWatch watch;
    watch.mesh = &mesh;
    watch.mats = &mats;
    watch.held = &held;
    watch.req = &req;
    watch.above = above;
    watch.out = &po;
    watch.partners = jb.rigidPartners.size();
    watch.pairs = jb.contacts.size();
    watch.pairOf = pairOf;
    watch.plastic = req.plasticity;
    watch.cap = cap;
    if (req.plasticity) {
      watch.corner.resize(mesh.tets.size());
      for (size_t t = 0; t < mesh.tets.size(); ++t) {
        const std::array<int, 10> w = WrittenOrder(mesh, t);
        for (int k = 0; k < 4; ++k)
          watch.corner[t][k] = w[k];
      }
    }
    std::vector<DatBlock> blocks;
    auto lastPoll = std::chrono::steady_clock::now();
    const auto keepGoing = [&]() {
      const auto now = std::chrono::steady_clock::now();
      if (now - lastPoll < std::chrono::milliseconds(200))
        return true;
      lastPoll = now;
      if (run.progress)
        run.progress(
            std::min(1.0, StaReached(dir / "job.sta") /
                              std::max(steps.back().toFactor, 1e-300)));
      dat.Poll(blocks, false);
      return watch.Feed(blocks);
    };
    bool keep = true;
    std::string ccxError;
    const bool ran = RunCcx(
        run, dir, options, keep, &err,
        [&](std::string*) {
          dat.Poll(blocks, true);
          watch.Feed(blocks);
          if (!watch.ended)
            watch.Finish();
          return true;
        },
        keepGoing, &ccxError);
    if (!ran) {
      if (!keep) {
        fs::remove_all(dir, ec);
        return fail(err);
      }
      return fail(err + " (the run is kept in " + dir.string() + ")");
    }
    po.stoppedEarly = watch.ended;
    if (po.end == PassOutcome::End::Completed && !watch.ended) {
      if (!ccxError.empty() && !IsNonConvergence(ccxError))
        return fail(ccxError + " (the run is kept in " + dir.string() + ")");
      po.end = ccxError.empty() ? PassOutcome::End::Completed
                                : PassOutcome::End::Stopped;
      po.ccxError = ccxError;
      po.failedIncrement = LastFailedIncrement(dir / "job.sta");
      if (watch.havePrev) {
        po.before = std::move(watch.prev);
        po.haveBefore = true;
      }
    }
    fs::remove_all(dir, ec);
    return true;
  };

  summary = StudyBodyResult::Nonlinear{};
  summary.solved = true;
  summary.snapThrough = req.snapThrough;
  summary.maxFactor = cap;
  summary.residualCriterion = kNonlinearResidual;
  const double coarse = cap / increments;
  const auto pct = [](double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3g %%", 100.0 * v);
    return std::string(buf);
  };
  const auto fmt = [](double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4g", v);
    return std::string(buf);
  };
  // A snap-through run's jump or deflection limit is bracketed again: the
  // run repeated with that increment cut in ten, and on past it, so a
  // candidate the finer increments do not bear out lets the run go on.
  constexpr int kMaxRuns = 4;
  std::vector<PassOutcome> runs;
  double windowLo = 0.0, windowHi = 0.0;
  // A part held by a contact alone settles onto it through the first
  // increment, held by stiffer springs (SettleSprings).
  for (int k = 0; k < kMaxRuns; ++k) {
    std::vector<NonlinearStep> steps;
    if (!settle.empty())
      steps.push_back({settleFactor, settleFactor});
    if (windowHi > windowLo) {
      steps.push_back({windowLo, coarse});
      steps.push_back({windowHi, (windowHi - windowLo) / 10.0});
      if (windowHi < cap * (1.0 - 1e-12))
        steps.push_back({cap, coarse});
    } else {
      steps.push_back({cap, coarse});
    }
    runs.emplace_back();
    if (!pass(steps, windowHi > windowLo ? windowLo : 0.0, runs.back()))
      return false;
    const PassOutcome& po = runs.back();
    if (!req.snapThrough || (po.end != PassOutcome::End::Jump &&
                             po.end != PassOutcome::End::Deflection))
      break;
    // Ended inside the fine window: that is the bracket.
    if (windowHi > windowLo && po.afterFactor <= windowHi * (1.0 + 1e-9))
      break;
    windowLo = po.before.factor;
    windowHi = po.afterFactor;
  }
  summary.ccxRuns = static_cast<int>(runs.size());
  PassOutcome* reported = &runs.back();
  if (!reported->haveBefore)
    return fail("CalculiX converged no increment of the load" +
                (reported->ccxError.empty() ? std::string()
                                            : " (" + reported->ccxError + ")"));
  // A plastic run that carried its load and then stopped converging was
  // taking it off again.
  const bool unloadStopped = req.plasticity && !req.snapThrough &&
                             reported->end == PassOutcome::End::Stopped &&
                             reported->haveBefore &&
                             reported->before.factor >= cap * (1.0 - 1e-9);
  if (unloadStopped)
    return fail("the part carried its load, and taking the load off again "
                "stopped converging at " +
                (reported->haveUnloaded
                     ? pct(reported->unloaded.factor / cap - 1.0)
                     : std::string("the start")) +
                " of it (CalculiX: " + reported->ccxError +
                "), so the deformation it leaves is not known: more "
                "increments may carry it. Nothing was stored");
  // A plastic run that stopped converging as its stiffness ran out has
  // collapsed there: load control cannot go past the peak (ccx 2.23 has no
  // arc-length method), so the factor it stopped at is the collapse load,
  // as a snap-through run's limit point is.
  const auto stiffnessRanOut = [&](const PassOutcome& r) {
    double c0 = 0.0, c1 = 0.0;
    if (r.curve.size() >= 2) {
      c0 = r.curve[0].second / std::max(r.curve[0].first, 1e-300);
      const auto& a = r.curve[r.curve.size() - 2];
      const auto& b = r.curve.back();
      c1 = (b.second - a.second) / std::max(b.first - a.first, 1e-300);
    }
    return c1 > kSnapJumpRatio * c0;
  };
  const bool collapsed = req.plasticity && !req.snapThrough &&
                         reported->end == PassOutcome::End::Stopped &&
                         stiffnessRanOut(*reported);
  if (!req.snapThrough && reported->end == PassOutcome::End::Stopped &&
      !collapsed)
    return fail(
        "the nonlinear solve stopped converging at " +
        pct(reported->before.factor / cap) + " of the load (factor " +
        fmt(reported->before.factor) + "; CalculiX: " + reported->ccxError +
        "): the part may lose its stiffness there -- a snap-through run "
        "(nonlinear snap_through: true) reports that peak -- or the load "
        "steps too far, which more increments would show. Nothing was "
        "stored");
  PassOutcome& r = *reported;
  const Increment& at = r.before;
  summary.increments = static_cast<int>(r.curve.size());
  summary.curve = r.curve;
  summary.factor = at.factor;
  summary.firstYieldFactor =
      r.firstYield > 0 && r.firstYield <= at.factor ? r.firstYield : 0.0;
  summary.snapped = r.snapped;
  summary.snappedFrom = r.snappedFrom;
  summary.snappedTo = r.snappedTo;
  summary.plasticity = req.plasticity;
  if (req.plasticity) {
    summary.strainLimit = 0.0;
    for (const Material& m : mats)
      if (!m.hardening.empty() &&
          (summary.strainLimit == 0.0 ||
           m.hardening.back().second < summary.strainLimit))
        summary.strainLimit = m.hardening.back().second;
  }
  if (collapsed) {
    summary.collapsed = true;
    summary.collapseHigh =
        at.factor + (r.failedIncrement > 0 ? r.failedIncrement : 1e-5 * cap);
  }
  if (req.snapThrough) {
    switch (r.end) {
    case PassOutcome::End::Stopped: {
      // A limit point: the solve stops converging where the stiffness runs
      // out. Its last increment is far more compliant than its first.
      if (!stiffnessRanOut(r))
        return fail("the nonlinear solve stopped converging at factor " +
                    fmt(at.factor) + " (" + pct(at.factor / cap) +
                    " of the run's cap; CalculiX: " + r.ccxError +
                    ") while the part still had its stiffness, which is not "
                    "a limit point: more increments, or a finer mesh, may "
                    "carry it on. Nothing was stored");
      summary.peakAt = "limit_point";
      summary.peakFactor = at.factor;
      summary.peakLow = at.factor;
      summary.peakHigh =
          at.factor + (r.failedIncrement > 0 ? r.failedIncrement : 1e-5 * cap);
      break;
    }
    case PassOutcome::End::Jump:
      summary.peakAt = "snap";
      summary.peakFactor = at.factor;
      summary.peakLow = at.factor;
      summary.peakHigh = r.afterFactor;
      break;
    case PassOutcome::End::Deflection: {
      const double t =
          r.afterMaxU > at.maxU
              ? (req.deflectionLimitMm - at.maxU) / (r.afterMaxU - at.maxU)
              : 1.0;
      summary.peakAt = "deflection_limit";
      summary.peakFactor =
          at.factor + std::clamp(t, 0.0, 1.0) * (r.afterFactor - at.factor);
      summary.peakLow = at.factor;
      summary.peakHigh = r.afterFactor;
      break;
    }
    case PassOutcome::End::Completed:
      summary.peakAt = "none";
      summary.none = "the load reached the run's cap, factor " + fmt(cap) +
                     " (twice the linear buckling factor), without a peak "
                     "and with the motion under the deflection limit";
      break;
    }
  }

  // Equilibrium at the factor reported: the supports' reaction against the
  // load there -- the dead loads scaled, a pressure on the deformed faces,
  // each reference node's force.
  std::vector<glm::dvec3> applied(nNodes, glm::dvec3(0.0));
  for (size_t n = 0; n < nNodes; ++n)
    applied[n] = at.factor * forces[n];
  glm::dvec3 refTotal(0.0);
  {
    FemMesh moved = mesh;
    for (size_t n = 0; n < nNodes; ++n)
      moved.nodes[n] += at.u[n];
    std::vector<FaceLoad> pressures;
    for (const FaceLoad& l : other)
      if (l.type == LoadType::Pressure)
        pressures.push_back(l);
      else {
        glm::dvec3 f, m;
        if (!LoadResultant(mesh, l, l.origin, f, m, &err))
          return fail(err);
        refTotal += at.factor * f;
      }
    if (!pressures.empty()) {
      std::vector<glm::dvec3> p;
      if (!NodalForces(moved, mats, pressures, glm::dvec3(0.0), p, &err))
        return fail(err);
      for (size_t n = 0; n < nNodes; ++n)
        applied[n] += at.factor * p[n];
    }
  }
  glm::dvec3 balance = refTotal;
  double scale = glm::length(refTotal);
  double carried = 0.0;
  // A plastic run's stress depends on its path, so it is not recomputed
  // from U: ccx's own, at each Gauss point, carried to the nodes.
  if (req.plasticity && !at.HasIp())
    return fail("CalculiX printed no stress for the plastic run's factor " +
                fmt(at.factor) + ", which its fields are read from");
  result =
      req.plasticity
          ? IntegrationPointFields(mesh, at.u, at.ipStress, at.ipPeeq)
      : req.nlgeom
          ? LargeDeformationFields(mesh, mats, at.u, options.thermal.get())
          : SmallDeformationFields(mesh, mats, at.u, options.thermal.get());
  if (req.plasticity && !req.snapThrough && !collapsed) {
    if (r.end != PassOutcome::End::Completed || !r.haveUnloaded ||
        r.unloaded.factor < 2.0 * cap * (1.0 - 1e-9))
      return fail("the plastic run carried its load, and CalculiX did not "
                  "report taking it off again, so the deformation it leaves "
                  "is not known. Nothing was stored");
    result.residualDisplacement = r.unloaded.u;
    result.residualDisplacement.resize(nNodes, glm::dvec3(0.0));
    for (size_t n = 0; n < nNodes; ++n) {
      const double d = glm::length(result.residualDisplacement[n]);
      if (d > result.maxResidualDisplacement) {
        result.maxResidualDisplacement = d;
        result.maxResidualAt = mesh.nodes[n];
      }
    }
    summary.unloaded = true;
  }
  result.nodeReaction.assign(nNodes, glm::dvec3(0.0));
  // A contact's rigid partners carry what they press back with, and the
  // weak springs what they hold.
  for (const glm::dvec3& r : at.partnerRf) {
    balance += r;
    carried += glm::length(r);
  }
  glm::dvec3 springs(0.0);
  double springCarried = 0.0;
  for (const StudyJobBody::Stabilized& st : weak)
    for (int n : st.nodes)
      if (n >= 0 && static_cast<size_t>(n) < nNodes) {
        springs -= st.kPerNode * at.u[n];
        springCarried += st.kPerNode * glm::length(at.u[n]);
      }
  balance += springs;
  for (size_t n = 0; n < nNodes; ++n) {
    if (isHeld[n]) {
      balance += at.rf[n];
      carried += glm::length(at.rf[n]);
      result.nodeReaction[n] = at.rf[n] - applied[n];
    } else {
      balance += applied[n];
      scale += glm::length(applied[n]);
    }
  }
  scale = std::max(scale, carried);
  summary.imbalance = scale > 0 ? glm::length(balance) / scale : 0.0;
  summary.largeDeformation = req.nlgeom;
  summary.stabilization = scale > 0 ? springCarried / scale : 0.0;
  if (!(summary.imbalance <= kNonlinearBalance))
    return fail("CalculiX's answer at factor " + fmt(at.factor) +
                " does not balance: the supports carry the applied load to " +
                pct(summary.imbalance) + ", past the " +
                pct(kNonlinearBalance) +
                " its residual criterion allows, so the deck and the study "
                "it was written from differ. Nothing was stored");
  if (summary.stabilization > kNonlinearBalance)
    return fail("the weak springs that hold a part while its contact closes "
                "carry " +
                pct(summary.stabilization) + " of the load at factor " +
                fmt(at.factor) +
                ", so nothing else holds it there: a frictionless contact "
                "leaves a part free to slide along it and turn on it. Give "
                "the contact friction, or the part another support. Nothing "
                "was stored");
  if (contactsOut) {
    contactsOut->clear();
    for (size_t k = 0; k < jb.contacts.size(); ++k) {
      const StudyJobBody::ContactPair& c = jb.contacts[k];
      StudyBodyResult::ContactResult cr;
      cr.id = c.id;
      cr.participant = c.participant;
      cr.name = c.name;
      cr.friction = c.friction;
      cr.penaltyMPaPerMm = c.penalty;
      for (int t : c.slaveTris) {
        const auto& tri = mesh.boundary[t];
        cr.areaMm2 +=
            0.5 * glm::length(glm::cross(
                      mesh.nodes[tri.nodes[1]] - mesh.nodes[tri.nodes[0]],
                      mesh.nodes[tri.nodes[2]] - mesh.nodes[tri.nodes[0]]));
      }
      if (k < at.stats.size() && at.stats[k].size() >= 18) {
        const std::vector<double>& v = at.stats[k];
        cr.forceN = glm::dvec3(v[0], v[1], v[2]);
        cr.momentNmm = glm::dvec3(v[3], v[4], v[5]);
        cr.closedMm2 = v[15];
        cr.normalN = -v[16]; // ccx: + = tension
        cr.shearN = v[17];
      }
      if (k < at.maxPressure.size())
        cr.maxPressureMPa = at.maxPressure[k];
      cr.meanPressureMPa = cr.closedMm2 > 0 ? cr.normalN / cr.closedMm2 : 0.0;
      contactsOut->push_back(cr);
    }
  }
  return true;
}

} // namespace fem
