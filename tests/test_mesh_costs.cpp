// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// What recognition and the cradle's pieces cost, measured on the fixtures
// (docs/interchange/README.md#what-a-mesh-costs,
// docs/rendering/README.md#mesh-bodies,
// docs/modeling/README.md#what-a-cradle-costs). HIDDEN: these are the
// measurements the cost laws there were read from, not checks, and they
// take minutes. Run one by name:
//
//   ./tests/designer_tests "[mesh-cost-recognition]"
//   ./tests/designer_tests "[mesh-cost-cradle]"
//   ./tests/designer_tests "[mesh-cost-render]"
//
// LOBOLE_MESH_COST_SIZES (comma-separated triangle counts) narrows the
// sizes, LOBOLE_MESH_COST_FIXTURES the recognition's fixtures, and
// LOBOLE_MESH_COST_CAP_S sets the CPU cap (300 s). Every measurement runs in a
// forked child under a CPU cap, so one that runs away is reported as such, and
// its peak memory is its own.
//
// The cradle's distance field needs a closest-point query and a winding
// number, which src/core/mesh/ does not have yet (MeshBvh is stage 1). The
// tree here is a plain one, written only to measure how such queries scale
// with the triangle count; it is not the one the plan builds.

#include "mesh_fixtures.h"

#include "cad/FaceSelector.h"
#include "cad/MeshRebuild.h"
#include "core/mesh/MeshBoolean.h"
#include "core/mesh/MeshReaders.h"
#include "core/mesh/Segment.h"
#include "render/MeshRenderer.h"
#include "render/OutlineEdges.h"

#include <catch2/catch_test_macros.hpp>
#include <manifold/manifold.h>

#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

double Wall() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double Cpu() {
  timespec ts{};
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return double(ts.tv_sec) + 1e-9 * double(ts.tv_nsec);
}

// The process's resident peak so far, MB (VmHWM).
double PeakMb() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind("VmHWM:", 0) == 0)
      return std::atof(line.c_str() + 6) / 1024.0;
  return 0.0;
}
double RssMb() {
  std::ifstream in("/proc/self/status");
  std::string line;
  while (std::getline(in, line))
    if (line.rfind("VmRSS:", 0) == 0)
      return std::atof(line.c_str() + 6) / 1024.0;
  return 0.0;
}

int CapS() {
  const char* env = std::getenv("LOBOLE_MESH_COST_CAP_S");
  return env ? std::atoi(env) : 300;
}

std::vector<size_t> Sizes() {
  std::vector<size_t> out;
  const char* env = std::getenv("LOBOLE_MESH_COST_SIZES");
  std::stringstream ss(env ? env : "10000,30000,100000,300000,1000000,2000000");
  for (std::string s; std::getline(ss, s, ',');)
    out.push_back(std::stoul(s));
  return out;
}

// Runs `body` in a child capped at `cpuCapS` seconds of CPU and prints the
// line it returns, or why there is none.
void InChild(const std::string& label, int cpuCapS,
             const std::function<std::string()>& body) {
  std::fflush(stdout);
  int fd[2];
  REQUIRE(pipe(fd) == 0);
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    close(fd[0]);
    rlimit cap{rlim_t(cpuCapS), rlim_t(cpuCapS) + 5};
    setrlimit(RLIMIT_CPU, &cap);
    // And 6 GB of address space: a runaway allocation fails in the child
    // (std::bad_alloc, reported) instead of pressing on the machine.
    rlimit memory{rlim_t(6) << 30, rlim_t(6) << 30};
    setrlimit(RLIMIT_AS, &memory);
    std::string line;
    try {
      line = body();
    } catch (const std::exception& e) {
      line = std::string("threw: ") + e.what();
    }
    (void)!write(fd[1], line.data(), line.size());
    close(fd[1]);
    _exit(0);
  }
  close(fd[1]);
  std::string got;
  char buf[4096];
  for (ssize_t n; (n = read(fd[0], buf, sizeof buf)) > 0;)
    got.append(buf, size_t(n));
  close(fd[0]);
  int status = 0;
  rusage usage{};
  wait4(pid, &status, 0, &usage);
  if (WIFSIGNALED(status))
    got = "killed by signal " + std::to_string(WTERMSIG(status)) +
          (WTERMSIG(status) == SIGXCPU
               ? " (past the " + std::to_string(cpuCapS) + " s CPU cap)"
               : "") +
          "; peak " + std::to_string(usage.ru_maxrss / 1024) + " MB";
  std::printf("COST %s | %s\n", label.c_str(), got.c_str());
  std::fflush(stdout);
}

// The plate tessellated to about `triangles`: its holes take nearly all of
// them, as many sides as the count needs (its own angular deflection).
std::filesystem::path PlateStl(size_t triangles) {
  const fixtures::Part plate = fixtures::Plate();
  // Each of the 24 holes' walls is two triangles a side, and its two rims
  // fan into the plate's faces about as many again: ~4 triangles a side.
  double sides = std::max(8.0, double(triangles) / 96.0);
  std::filesystem::path path;
  for (int round = 0; round < 4; round++) {
    // The sagitta of a side on the smallest hole (r 2.5), so the linear
    // deflection asks for the same sides the angular one does.
    const double angular = 2.0 * kPi / sides;
    const double linear = 2.5 * (1.0 - std::cos(angular / 2.0));
    path = fixtures::WriteStl(plate, linear, angular,
                              "cost" + std::to_string(triangles));
    const double n = double(std::filesystem::file_size(path) - 84) / 50.0;
    if (std::abs(n - double(triangles)) < 0.08 * double(triangles))
      break;
    sides *= double(triangles) / n;
  }
  return path;
}

// A bored cylinder (outer radius 20, bore 8, 30 tall) of `triangles`,
// built straight as triangles: four rings of s vertices, 8 s triangles. A
// mechanical part at any size without BRepMesh, whose cost at a fine
// deflection is its own (the plate past 100k ran out of 6 GB in it).
mesh::TriMesh Tube(size_t triangles) {
  const int sides = std::max(8, int(triangles / 8));
  mesh::TriMesh m;
  const double radii[2] = {20.0, 8.0}, heights[2] = {0.0, 30.0};
  for (double r : radii)
    for (double z : heights)
      for (int k = 0; k < sides; k++) {
        const double a = 2.0 * kPi * k / sides;
        m.positions.emplace_back(r * std::cos(a), r * std::sin(a), z);
      }
  // ring index: (radius 0 outer / 1 inner) * 2 + (height 0 bottom / 1 top)
  const auto v = [&](int ring, int k) {
    return uint32_t(ring * sides + (k % sides));
  };
  const auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    m.triangles.push_back({a, b, c});
    m.triangles.push_back({a, c, d});
  };
  for (int k = 0; k < sides; k++) {
    quad(v(0, k), v(0, k + 1), v(1, k + 1), v(1, k)); // outer wall, out
    quad(v(2, k), v(3, k), v(3, k + 1), v(2, k + 1)); // bore, facing in
    quad(v(1, k), v(1, k + 1), v(3, k + 1), v(3, k)); // top annulus, up
    quad(v(0, k), v(2, k), v(2, k + 1), v(0, k + 1)); // bottom, down
  }
  m.quantum = mesh::Float32Quantum(m.positions);
  mesh::BuildAdjacency(m);
  REQUIRE(m.Closed());
  REQUIRE(m.Volume() > 0);
  return m;
}

// Area of the planes that are facets of something unrecognised (a plane
// region with a plane neighbour turning by less than a 12-gon's side, the
// segmenter's own facetsLeft test), as a share of the whole.
double FacetShare(const mesh::TriMesh& m, const mesh::Segmentation& s) {
  const double limit = std::cos(2.0 * kPi / 12.0);
  std::vector<char> facet(s.regions.size(), 0);
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    const uint32_t r = s.regionOf[t];
    if (s.regions[r].fit.kind != mesh::Kind::Plane || facet[r])
      continue;
    for (int e = 0; e < 3; e++) {
      const uint32_t u = m.neighbour[t][e];
      if (u == mesh::kNone)
        continue;
      const uint32_t q = s.regionOf[u];
      if (q != r && s.regions[q].fit.kind == mesh::Kind::Plane &&
          glm::dot(s.regions[r].fit.axis, s.regions[q].fit.axis) > limit) {
        facet[r] = 1;
        break;
      }
    }
  }
  double all = 0.0, facets = 0.0;
  for (size_t r = 0; r < s.regions.size(); r++) {
    all += s.regions[r].area;
    if (facet[r])
      facets += s.regions[r].area;
  }
  return all > 0 ? facets / all : 0.0;
}

std::string Recognise(const std::filesystem::path& path) {
  char line[1024];
  const double rss0 = RssMb();
  double w = Wall(), c = Cpu();
  mesh::TriMesh m;
  std::string error;
  if (!mesh::ReadStl(path.string(), m, error))
    return "unread: " + error;
  const double readW = Wall() - w, readC = Cpu() - c, readMb = PeakMb() - rss0;
  w = Wall();
  c = Cpu();
  mesh::SegmentOptions so;
  mesh::Segmentation seg = mesh::SegmentAll(m, so);
  const double segW = Wall() - w, segC = Cpu() - c, segMb = PeakMb() - rss0;
  const double facets = FacetShare(m, seg);
  size_t curved = 0;
  for (const auto& r : seg.regions)
    curved += r.fit.kind != mesh::Kind::Plane;
  std::snprintf(line, sizeof line,
                "%zu tris | read %.2f s wall %.2f cpu, %.0f MB | segment "
                "%.2f s wall %.2f cpu, %.0f MB | %zu regions, %zu curved, "
                "facets %.1f%% of area",
                m.TriangleCount(), readW, readC, readMb, segW, segC, segMb,
                seg.regions.size(), curved, 100.0 * facets);
  std::printf("COST-PARTIAL %s\n", line);
  std::fflush(stdout);
  w = Wall();
  c = Cpu();
  cad::Rebuilt built;
  int rounds = 0;
  for (;; rounds++) {
    built = cad::RebuildFromRegions(m, seg);
    if (built.failed.empty() || built.badRegions.empty() || rounds == 6)
      break;
    mesh::Demote(seg, m, built.badRegions);
  }
  const double rebW = Wall() - w, rebC = Cpu() - c, rebMb = PeakMb() - rss0;
  char tail[512];
  std::snprintf(tail, sizeof tail,
                " | rebuild %.2f s wall %.2f cpu, %.0f MB peak, %d rounds: %s",
                rebW, rebC, rebMb, rounds + 1,
                built.failed.empty() ? "a solid" : built.failed.c_str());
  return std::string(line) + tail;
}

// ------------------------------------------------- a plain BVH, to measure --

struct Node {
  glm::dvec3 lo, hi;
  uint32_t left = 0, count = 0; // leaf: first triangle and count
  glm::dvec3 centroid{0.0}, normalArea{0.0};
  double radius = 0.0;
};

struct Bvh {
  const mesh::TriMesh* m = nullptr;
  std::vector<uint32_t> order;
  std::vector<Node> nodes;

  explicit Bvh(const mesh::TriMesh& mesh) : m(&mesh) {
    order.resize(mesh.triangles.size());
    std::iota(order.begin(), order.end(), 0u);
    nodes.reserve(2 * order.size() / 4 + 1);
    Build(0, uint32_t(order.size()));
  }
  glm::dvec3 Centre(uint32_t t) const {
    return (m->Corner(t, 0) + m->Corner(t, 1) + m->Corner(t, 2)) / 3.0;
  }
  uint32_t Build(uint32_t begin, uint32_t end) {
    const auto id = uint32_t(nodes.size());
    nodes.push_back({});
    Node n;
    n.lo = glm::dvec3(1e300);
    n.hi = glm::dvec3(-1e300);
    double area = 0.0;
    for (uint32_t i = begin; i < end; i++) {
      const uint32_t t = order[i];
      for (int c = 0; c < 3; c++) {
        n.lo = glm::min(n.lo, m->Corner(t, c));
        n.hi = glm::max(n.hi, m->Corner(t, c));
      }
      const double a = m->Area(t);
      area += a;
      n.centroid += a * Centre(t);
      n.normalArea += 0.5 * m->AreaNormal(t);
    }
    n.centroid /= std::max(area, 1e-300);
    n.radius = glm::length(n.hi - n.lo) / 2.0 +
               glm::length((n.hi + n.lo) / 2.0 - n.centroid);
    if (end - begin <= 4) {
      n.left = begin;
      n.count = end - begin;
    } else {
      const glm::dvec3 ext = n.hi - n.lo;
      const int axis =
          ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
      const uint32_t mid = (begin + end) / 2;
      std::nth_element(order.begin() + begin, order.begin() + mid,
                       order.begin() + end, [&](uint32_t a, uint32_t b) {
                         return Centre(a)[axis] < Centre(b)[axis];
                       });
      Build(begin, mid);
      n.left = Build(mid, end);
      n.count = 0;
    }
    nodes[id] = n;
    return id;
  }
  static glm::dvec3 ClosestOnTri(const glm::dvec3& p, const glm::dvec3& a,
                                 const glm::dvec3& b, const glm::dvec3& c) {
    // Ericson, Real-Time Collision Detection 5.1.5.
    const glm::dvec3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0 && d2 <= 0)
      return a;
    const glm::dvec3 bp = p - b;
    const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0 && d4 <= d3)
      return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
      return a + ab * (d1 / (d1 - d3));
    const glm::dvec3 cp = p - c;
    const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0 && d5 <= d6)
      return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
      return a + ac * (d2 / (d2 - d6));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
      return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const double denom = 1.0 / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
  }
  static double BoxDist2(const Node& n, const glm::dvec3& p) {
    const glm::dvec3 d = glm::max(glm::max(n.lo - p, p - n.hi), 0.0);
    return glm::dot(d, d);
  }
  double Closest(const glm::dvec3& p) const {
    double best = 1e300;
    uint32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top) {
      const Node& n = nodes[stack[--top]];
      if (BoxDist2(n, p) >= best)
        continue;
      if (n.count) {
        for (uint32_t i = n.left; i < n.left + n.count; i++) {
          const uint32_t t = order[i];
          const glm::dvec3 q = ClosestOnTri(p, m->Corner(t, 0), m->Corner(t, 1),
                                            m->Corner(t, 2));
          best = std::min(best, glm::dot(q - p, q - p));
        }
      } else {
        const uint32_t a = uint32_t(&n - nodes.data()) + 1;
        const uint32_t b = n.left;
        // nearer child last, so it is popped first
        if (BoxDist2(nodes[a], p) < BoxDist2(nodes[b], p)) {
          stack[top++] = b;
          stack[top++] = a;
        } else {
          stack[top++] = a;
          stack[top++] = b;
        }
      }
    }
    return std::sqrt(best);
  }
  // Fast winding number (Barill et al. 2018), first order: a far node is
  // its area-weighted dipole; a near leaf is its triangles' exact solid
  // angles (Van Oosterom & Strackee).
  double Winding(const glm::dvec3& p) const {
    double w = 0.0;
    uint32_t stack[128];
    int top = 0;
    stack[top++] = 0;
    while (top) {
      const Node& n = nodes[stack[--top]];
      const glm::dvec3 r = n.centroid - p;
      const double dist = glm::length(r);
      if (dist > 2.0 * n.radius) {
        w += glm::dot(r, n.normalArea) / (4.0 * kPi * dist * dist * dist);
        continue;
      }
      if (n.count) {
        for (uint32_t i = n.left; i < n.left + n.count; i++) {
          const uint32_t t = order[i];
          const glm::dvec3 a = m->Corner(t, 0) - p, b = m->Corner(t, 1) - p,
                           c = m->Corner(t, 2) - p;
          const double la = glm::length(a), lb = glm::length(b),
                       lc = glm::length(c);
          const double num = glm::dot(a, glm::cross(b, c));
          const double den = la * lb * lc + glm::dot(a, b) * lc +
                             glm::dot(b, c) * la + glm::dot(c, a) * lb;
          w += std::atan2(num, den) / (2.0 * kPi);
        }
      } else {
        stack[top++] = uint32_t(&n - nodes.data()) + 1;
        stack[top++] = n.left;
      }
    }
    return w;
  }
};

} // namespace

TEST_CASE("Mesh costs: recognition on the fixtures, by size",
          "[.][mesh-cost-recognition]") {
  // LOBOLE_MESH_COST_FIXTURES narrows the fixtures (comma-separated: plate,
  // tube, sphere, torus, figurine).
  const char* only = std::getenv("LOBOLE_MESH_COST_FIXTURES");
  const auto wanted = [&](const std::string& name) {
    return only == nullptr ||
           ("," + std::string(only) + ",").find("," + name + ",") !=
               std::string::npos;
  };
  for (size_t n : Sizes()) {
    if (wanted("plate")) {
      // Tessellated in the child too: at a fine enough deflection BRepMesh
      // is itself the cost, and its memory is the child's.
      InChild("recognize plate " + std::to_string(n), CapS(), [&] {
        const double w = Wall();
        const auto plate = PlateStl(n);
        const double meshS = Wall() - w;
        const std::string line = Recognise(plate);
        std::filesystem::remove(plate);
        char tail[96];
        std::snprintf(tail, sizeof tail, " | (BRepMesh and STL write %.1f s)",
                      meshS);
        return line + tail;
      });
    }
    if (wanted("tube")) {
      const auto path = fixtures::WriteStl(Tube(n), "costtube");
      InChild("recognize tube " + std::to_string(n), CapS(),
              [&] { return Recognise(path); });
      std::filesystem::remove(path);
    }
    for (const char* name : {"sphere", "torus", "figurine"}) {
      if (!wanted(name))
        continue;
      fixtures::Organic o;
      REQUIRE(fixtures::OrganicByName(name, n, o));
      const auto path = fixtures::WriteStl(o.mesh, std::string("cost") + name);
      InChild(std::string("recognize ") + name + " " + std::to_string(n),
              CapS(), [&] { return Recognise(path); });
      std::filesystem::remove(path);
    }
  }
}

TEST_CASE("Mesh costs: the cradle's pieces, by triangles and by cells",
          "[.][mesh-cost-cradle]") {
  // The subtraction: a block minus the figure, the cradle's last step, at
  // each size of the figure.
  for (size_t n : Sizes()) {
    InChild("manifold subtract figurine " + std::to_string(n), CapS(), [&] {
      const auto figure = fixtures::Figurine(n);
      // A block round its lower half, so the cut crosses the whole surface
      // there.
      mesh::TriMesh block;
      const glm::dvec3 lo = figure.lo - glm::dvec3(5, 5, -2),
                       hi = {figure.hi.x + 5, figure.hi.y + 5, 50.0};
      for (int c = 0; c < 8; c++)
        block.positions.push_back(
            {c & 1 ? hi.x : lo.x, c & 2 ? hi.y : lo.y, c & 4 ? hi.z : lo.z});
      block.triangles = {{0, 2, 3}, {0, 3, 1}, {4, 5, 7}, {4, 7, 6},
                         {0, 1, 5}, {0, 5, 4}, {2, 6, 7}, {2, 7, 3},
                         {0, 4, 6}, {0, 6, 2}, {1, 3, 7}, {1, 7, 5}};
      mesh::BuildAdjacency(block);
      const double rss0 = RssMb();
      const double w = Wall(), c = Cpu();
      mesh::TriMesh out;
      std::string error;
      const bool ok = mesh::Boolean(block, figure.mesh,
                                    mesh::BooleanOp::Subtract, out, error);
      char line[512];
      std::snprintf(line, sizeof line,
                    "%zu tris in | %.3f s wall %.3f cpu, %.0f MB | %s, %zu "
                    "tris out, closed %d",
                    figure.mesh.TriangleCount(), Wall() - w, Cpu() - c,
                    PeakMb() - rss0, ok ? "ok" : error.c_str(),
                    out.TriangleCount(), int(out.Closed()));
      return std::string(line);
    });
  }
  // The iso-surface: manifold's LevelSet over a sampled grid (trilinear
  // from an array, as the cradle samples its field), by cell count.
  for (int cells : {64, 128, 256, 400}) {
    InChild("manifold levelset " + std::to_string(cells) + "^3", CapS(), [&] {
      const double size = 100.0, h = size / cells;
      const int g = cells + 1;
      std::vector<float> field(size_t(g) * g * g);
      // A figure-like field: a sphere of 40 mm, enough surface to contour.
      for (int k = 0; k < g; k++)
        for (int j = 0; j < g; j++)
          for (int i = 0; i < g; i++)
            field[(size_t(k) * g + j) * g + i] =
                float(40.0 - glm::length(glm::dvec3(i, j, k) * h -
                                         glm::dvec3(size / 2)));
      const auto sample = [&](manifold::vec3 p) {
        const double x = std::clamp(p.x / h, 0.0, double(cells) - 1e-9),
                     y = std::clamp(p.y / h, 0.0, double(cells) - 1e-9),
                     z = std::clamp(p.z / h, 0.0, double(cells) - 1e-9);
        const int i = int(x), j = int(y), k = int(z);
        const double fx = x - i, fy = y - j, fz = z - k;
        const auto at = [&](int a, int b, int c) {
          return double(field[(size_t(k + c) * g + j + b) * g + i + a]);
        };
        const double c00 = at(0, 0, 0) * (1 - fx) + at(1, 0, 0) * fx,
                     c10 = at(0, 1, 0) * (1 - fx) + at(1, 1, 0) * fx,
                     c01 = at(0, 0, 1) * (1 - fx) + at(1, 0, 1) * fx,
                     c11 = at(0, 1, 1) * (1 - fx) + at(1, 1, 1) * fx;
        return (c00 * (1 - fy) + c10 * fy) * (1 - fz) +
               (c01 * (1 - fy) + c11 * fy) * fz;
      };
      const double rss0 = RssMb();
      const double w = Wall(), c = Cpu();
      const manifold::Manifold m = manifold::Manifold::LevelSet(
          sample, {manifold::vec3(0.0), manifold::vec3(size)}, h);
      char line[512];
      std::snprintf(line, sizeof line,
                    "%d^3 cells, %.0f MB field | %.3f s wall %.3f cpu, %.0f "
                    "MB | %zu tris, status %d, volume %.0f (sphere %.0f)",
                    cells, double(field.size()) * 4 / 1048576.0, Wall() - w,
                    Cpu() - c, PeakMb() - rss0, m.NumTri(), int(m.Status()),
                    m.Volume(), 4.0 / 3.0 * kPi * 40 * 40 * 40);
      return std::string(line);
    });
  }
  // The field's queries: closest point and winding number per grid point,
  // by the scan's triangle count, on a band of points near the surface and
  // on points spread through its box.
  for (size_t n : Sizes()) {
    InChild("field queries figurine " + std::to_string(n), CapS(), [&] {
      const auto figure = fixtures::Figurine(n);
      double w = Wall();
      const Bvh bvh(figure.mesh);
      const double buildS = Wall() - w;
      constexpr int kQueries = 20000;
      std::vector<glm::dvec3> band, spread;
      for (int q = 0; q < kQueries; q++) {
        const glm::dvec3 v =
            figure.mesh
                .positions[size_t(q) * 7919 % figure.mesh.positions.size()];
        band.push_back(v + glm::dvec3(0.3, -0.2, 0.25));
        const double s = (q + 0.5) / kQueries;
        spread.push_back(figure.lo + (figure.hi - figure.lo) *
                                         glm::dvec3(std::fmod(s * 7.13, 1.0),
                                                    std::fmod(s * 3.71, 1.0),
                                                    s));
      }
      double sink = 0.0;
      w = Wall();
      for (const auto& p : band)
        sink += bvh.Closest(p);
      const double closestBand = (Wall() - w) / kQueries;
      w = Wall();
      for (const auto& p : spread)
        sink += bvh.Closest(p);
      const double closestSpread = (Wall() - w) / kQueries;
      w = Wall();
      double inside = 0;
      for (const auto& p : band)
        inside += bvh.Winding(p) > 0.5;
      const double windBand = (Wall() - w) / kQueries;
      w = Wall();
      for (const auto& p : spread)
        sink += bvh.Winding(p);
      const double windSpread = (Wall() - w) / kQueries;
      char line[512];
      std::snprintf(line, sizeof line,
                    "%zu tris | tree %.3f s | closest %.2f us band, %.2f us "
                    "spread | winding %.2f us band, %.2f us spread | "
                    "(%.0f, %.0f)",
                    figure.mesh.TriangleCount(), buildS, 1e6 * closestBand,
                    1e6 * closestSpread, 1e6 * windBand, 1e6 * windSpread,
                    inside, sink);
      return std::string(line);
    });
  }
}

namespace {

// The figure as the renderer would get it: `shared` keeps the mesh's own
// vertex sharing (a mesh body, stage 1); unshared gives every triangle its
// own three vertices, which is what a faceted import's one face per
// triangle tessellates to today (CADShape::GenerateMesh shares nothing
// across faces).
std::string RenderCosts(const mesh::TriMesh& m, bool shared) {
  std::vector<Vertex3D> verts;
  std::vector<uint32_t> idx;
  if (shared) {
    std::vector<glm::dvec3> normal(m.positions.size(), glm::dvec3(0.0));
    for (uint32_t t = 0; t < m.triangles.size(); t++)
      for (int c = 0; c < 3; c++)
        normal[m.triangles[t][c]] += m.AreaNormal(t);
    for (size_t v = 0; v < m.positions.size(); v++)
      verts.push_back(
          {glm::vec3(m.positions[v]), glm::vec3(glm::normalize(normal[v]))});
    for (const auto& t : m.triangles)
      idx.insert(idx.end(), t.begin(), t.end());
  } else {
    for (uint32_t t = 0; t < m.triangles.size(); t++) {
      const glm::vec3 n(glm::normalize(m.AreaNormal(t)));
      for (int c = 0; c < 3; c++) {
        idx.push_back(uint32_t(verts.size()));
        verts.push_back({glm::vec3(m.Corner(t, c)), n});
      }
    }
  }
  const double gpuMb =
      double(verts.size() * sizeof(Vertex3D) + idx.size() * 4) / 1048576.0;
  const double rss0 = RssMb();
  double w = Wall();
  const outline::EdgeAdjacency adj =
      outline::BuildEdgeAdjacency(&verts[0].position, verts.size(),
                                  sizeof(Vertex3D), idx.data(), idx.size());
  const double adjS = Wall() - w, adjMb = PeakMb() - rss0;
  outline::SilhouetteScratch scratch;
  std::vector<uint32_t> lines;
  w = Wall();
  constexpr int kViews = 10;
  for (int v = 0; v < kViews; v++) {
    lines.clear();
    const double a = 2.0 * kPi * v / kViews;
    outline::AppendSilhouetteIndices(
        adj, glm::vec3(300 * std::cos(a), 300 * std::sin(a), 150), scratch,
        lines);
  }
  const double silS = (Wall() - w) / kViews;
  FaceSelector selector;
  w = Wall();
  selector.AddMesh(1, verts, idx, {}, glm::mat4(1.0f), {}, true);
  const double addS = Wall() - w;
  w = Wall();
  constexpr int kPicks = 20;
  int hits = 0;
  for (int p = 0; p < kPicks; p++) {
    Ray ray{glm::vec3(0, -300, 10 + 4 * p), glm::vec3(0, 1, 0)};
    hits += selector.PickFace(ray).hit;
  }
  const double pickS = (Wall() - w) / kPicks;
  char line[640];
  std::snprintf(
      line, sizeof line,
      "%zu tris, %s, %zu verts | vertex+index buffers %.0f MB (x2 with the "
      "outline asset) | edge adjacency %.3f s, %.0f MB peak; %zu static "
      "line indices, %zu smooth edges | silhouette pass %.2f ms, %zu "
      "indices | picker AddMesh %.3f s, PickFace %.2f ms (%d/%d hit)",
      m.TriangleCount(), shared ? "shared vertices" : "unshared (faceted)",
      verts.size(), gpuMb, adjS, adjMb, adj.staticIndices.size(),
      adj.smoothEdges.size(), 1e3 * silS, lines.size(), addS, 1e3 * pickS, hits,
      kPicks);
  return line;
}

} // namespace

TEST_CASE("Mesh costs: what the renderer and the picker do with a big mesh",
          "[.][mesh-cost-render]") {
  for (size_t n : Sizes())
    for (bool shared : {true, false})
      InChild(std::string("render figurine ") + std::to_string(n) +
                  (shared ? " shared" : " unshared"),
              CapS(),
              [&] { return RenderCosts(fixtures::Figurine(n).mesh, shared); });
}
