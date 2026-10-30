// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "mesh_organic.h"

#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace fixtures {

namespace {

constexpr double kPi = 3.14159265358979323846;

uint64_t Mix(uint64_t x) { // splitmix64
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

// A deterministic value in [-1, 1] for `key`.
double Noise(uint64_t key) {
  return (double(Mix(key) >> 11) / double(1ull << 53)) * 2.0 - 1.0;
}

double LongestEdge(const mesh::TriMesh& m) {
  double h = 0.0;
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (int e = 0; e < 3; e++)
      h = std::max(h, glm::length(m.Corner(t, e) - m.Corner(t, (e + 1) % 3)));
  return h;
}

// Drops the vertices no triangle uses, keeping the order of the rest.
void Compact(mesh::TriMesh& m) {
  std::vector<uint32_t> remap(m.positions.size(), mesh::kNone);
  std::vector<glm::dvec3> kept;
  for (auto& t : m.triangles)
    for (uint32_t& v : t) {
      if (remap[v] == mesh::kNone) {
        remap[v] = static_cast<uint32_t>(kept.size());
        kept.push_back(m.positions[v]);
      }
      v = remap[v];
    }
  m.positions = std::move(kept);
}

// ------------------------------------------------------------ cube sphere --

// A star-shaped surface over the cube-sphere grid of side n: vertex
// direction d (equal-angle) placed at `radius(d, key)`. Wound outward.
template <class Radius> mesh::TriMesh CubeSphere(int n, Radius radius) {
  mesh::TriMesh m;
  std::unordered_map<uint64_t, uint32_t> index;
  std::vector<glm::dvec3> dirs;
  const auto vertex = [&](int a, int b, int c) {
    const uint64_t key = (uint64_t(a) * (n + 1) + b) * (n + 1) + c;
    auto [it, fresh] = index.try_emplace(key, uint32_t(m.positions.size()));
    if (fresh) {
      const auto u = [&](int i) {
        return std::tan((2.0 * i / n - 1.0) * kPi / 4.0);
      };
      const glm::dvec3 d = glm::normalize(glm::dvec3(u(a), u(b), u(c)));
      dirs.push_back(d);
      m.positions.push_back(radius(d, key));
    }
    return it->second;
  };
  for (int axis = 0; axis < 3; axis++)
    for (int side : {0, n})
      for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
          const auto at = [&](int di, int dj) {
            int c[3];
            c[axis] = side;
            c[(axis + 1) % 3] = i + di;
            c[(axis + 2) % 3] = j + dj;
            return vertex(c[0], c[1], c[2]);
          };
          const uint32_t p00 = at(0, 0), p10 = at(1, 0), p11 = at(1, 1),
                         p01 = at(0, 1);
          for (std::array<uint32_t, 3> t :
               {std::array{p00, p10, p11}, std::array{p00, p11, p01}}) {
            // Wound by the noise-free directions: the unit sphere is convex,
            // so the sign is exact whatever the radius does.
            if (glm::dot(dirs[t[0]], glm::cross(dirs[t[1]], dirs[t[2]])) < 0)
              std::swap(t[1], t[2]);
            m.triangles.push_back(t);
          }
        }
  mesh::BuildAdjacency(m);
  return m;
}

int CubeSide(size_t triangles) {
  return std::max(2, int(std::lround(std::sqrt(double(triangles) / 12.0))));
}

// ------------------------------------------------------------- the figure --

// A segment primitive of influence radius R: the field is
// (1 - (d/R)²)³ inside R, where d is the distance to the segment (a point
// when a == b), or to a horizontal disc of radius `disc` about `a`.
struct Blob {
  glm::dvec3 a, b;
  double R;
  double disc = 0.0;
};

const std::vector<Blob>& Figure() {
  static const std::vector<Blob> blobs = {
      {{0, 0, 5.5}, {0, 0, 5.5}, 12.0, 24.0}, // the base, a round plinth
      {{-8, 0, 9}, {-6, 1, 40}, 12.0},        // legs
      {{8, 0, 9}, {6, -1, 40}, 12.0},         //
      {{0, 0, 40}, {0, 2, 68}, 26.0},         // body
      {{-11, 0, 64}, {-27, -6, 46}, 11.0},    // arms, one forward
      {{11, 0, 64}, {28, 4, 50}, 11.0},       //
      {{0, 1, 84}, {0, 1, 84}, 23.0},         // head
      {{0, -9.5, 85}, {0, -9.5, 85}, 7.0},    // nose
  };
  return blobs;
}
constexpr double kIso = 0.5;

double Field(const glm::dvec3& p) {
  double f = 0.0;
  for (const Blob& b : Figure()) {
    double d2;
    if (b.disc > 0.0) {
      const double rxy = std::hypot(p.x - b.a.x, p.y - b.a.y);
      const double out = std::max(rxy - b.disc, 0.0);
      d2 = out * out + (p.z - b.a.z) * (p.z - b.a.z);
    } else {
      const glm::dvec3 ab = b.b - b.a;
      const double len2 = glm::dot(ab, ab);
      const double s =
          len2 > 0 ? std::clamp(glm::dot(p - b.a, ab) / len2, 0.0, 1.0) : 0.0;
      const glm::dvec3 q = b.a + s * ab - p;
      d2 = glm::dot(q, q);
    }
    const double x2 = d2 / (b.R * b.R);
    if (x2 < 1.0) {
      const double g = 1.0 - x2;
      f += g * g * g;
    }
  }
  return f;
}

// The figure's box, wide enough that the field is below the iso level on
// it: no blob reaches past half its radius at kIso alone, and blends only
// thicken where two overlap.
void FigureBox(glm::dvec3& lo, glm::dvec3& hi) {
  lo = glm::dvec3(1e30);
  hi = glm::dvec3(-1e30);
  for (const Blob& b : Figure()) {
    const double r = 0.75 * b.R + b.disc;
    lo = glm::min(lo, glm::min(b.a, b.b) - glm::dvec3(r, r, 0.75 * b.R));
    hi = glm::max(hi, glm::max(b.a, b.b) + glm::dvec3(r, r, 0.75 * b.R));
  }
}

// The solid's volume and box, integrated column by column: on an xy grid of
// `hc`, each vertical line's inside intervals found by sampling at `hc` and
// bisecting every crossing. Independent of any triangle.
struct Reference {
  double volume = 0.0;
  glm::dvec3 lo{1e30}, hi{-1e30};
  double hc = 0.0;
};

const Reference& FigureReference() {
  static Reference ref;
  static std::once_flag once;
  std::call_once(once, [] {
    glm::dvec3 lo, hi;
    FigureBox(lo, hi);
    const double hc = 0.25;
    ref.hc = hc;
    const int nx = int((hi.x - lo.x) / hc), ny = int((hi.y - lo.y) / hc),
              nz = int((hi.z - lo.z) / hc) + 1;
    double volume = 0.0;
    for (int i = 0; i < nx; i++)
      for (int j = 0; j < ny; j++) {
        const double x = lo.x + (i + 0.5) * hc, y = lo.y + (j + 0.5) * hc;
        const auto f = [&](double z) { return Field({x, y, z}) - kIso; };
        double z0 = lo.z, f0 = f(z0), start = 0.0;
        bool inside = f0 > 0;
        for (int k = 1; k <= nz; k++) {
          const double z1 = lo.z + k * hc, f1 = f(z1);
          if ((f1 > 0) != (f0 > 0)) {
            double a = z0, b = z1;
            for (int it = 0; it < 40; it++) {
              const double m = 0.5 * (a + b);
              ((f(m) > 0) == (f0 > 0) ? a : b) = m;
            }
            const double z = 0.5 * (a + b);
            if (!inside) {
              start = z;
            } else {
              volume += (z - start) * hc * hc;
              ref.lo = glm::min(ref.lo, glm::dvec3(x, y, start));
              ref.hi = glm::max(ref.hi, glm::dvec3(x, y, z));
            }
            inside = !inside;
          }
          z0 = z1;
          f0 = f1;
        }
      }
    ref.volume = volume;
    ref.lo -= glm::dvec3(hc, hc, 0);
    ref.hi += glm::dvec3(hc, hc, 0);
  });
  return ref;
}

// Marching tetrahedra (six Kuhn tetrahedra a cube, conforming across
// cubes) of the figure at grid spacing h. Every vertex is on a grid edge,
// shared by key, and kept at least 2% of the edge from its ends, so the
// float32 weld of a written STL cannot fold two into one.
mesh::TriMesh Contour(double h) {
  glm::dvec3 lo, hi;
  FigureBox(lo, hi);
  const int nx = int((hi.x - lo.x) / h) + 2, ny = int((hi.y - lo.y) / h) + 2,
            nz = int((hi.z - lo.z) / h) + 2;
  if (uint64_t(nx) * ny * nz >= (1ull << 32))
    throw std::runtime_error("figure grid too fine");
  const auto node = [&](int i, int j, int k) {
    return uint32_t(i + nx * (j + ny * k));
  };
  const auto at = [&](int i, int j, int k) {
    return lo + h * glm::dvec3(i, j, k);
  };
  mesh::TriMesh m;
  std::unordered_map<uint64_t, uint32_t> edgeVertex;
  std::vector<float> below(size_t(nx) * ny), above(size_t(nx) * ny);
  const auto fill = [&](std::vector<float>& layer, int k) {
    for (int j = 0; j < ny; j++)
      for (int i = 0; i < nx; i++)
        layer[size_t(j) * nx + i] = float(Field(at(i, j, k)));
  };
  fill(below, 0);
  // Cube corner c: bit 0 x, bit 1 y, bit 2 z.
  static constexpr int kTets[6][4] = {{0, 1, 3, 7}, {0, 1, 5, 7}, {0, 2, 3, 7},
                                      {0, 2, 6, 7}, {0, 4, 5, 7}, {0, 4, 6, 7}};
  for (int k = 0; k + 1 < nz; k++) {
    fill(above, k + 1);
    for (int j = 0; j + 1 < ny; j++)
      for (int i = 0; i + 1 < nx; i++) {
        float f[8];
        uint32_t id[8];
        glm::dvec3 p[8];
        bool any = false, all = true;
        for (int c = 0; c < 8; c++) {
          const int di = c & 1, dj = (c >> 1) & 1, dk = (c >> 2) & 1;
          f[c] = (dk ? above : below)[size_t(j + dj) * nx + i + di];
          id[c] = node(i + di, j + dj, k + dk);
          p[c] = at(i + di, j + dj, k + dk);
          any |= f[c] > kIso;
          all &= f[c] > kIso;
        }
        if (!any || all)
          continue;
        for (const auto& tet : kTets) {
          int in[4], out[4], ni = 0, no = 0;
          for (int c : tet)
            (f[c] > kIso ? in[ni++] : out[no++]) = c;
          if (ni == 0 || no == 0)
            continue;
          const auto vertexOn = [&](int a, int b) {
            if (id[a] > id[b])
              std::swap(a, b);
            const uint64_t key = (uint64_t(id[a]) << 32) | id[b];
            auto [it, fresh] =
                edgeVertex.try_emplace(key, uint32_t(m.positions.size()));
            if (fresh) {
              double t = (kIso - f[a]) / double(f[b] - f[a]);
              t = std::clamp(t, 0.02, 0.98);
              m.positions.push_back(p[a] + t * (p[b] - p[a]));
            }
            return it->second;
          };
          const auto mid = [&](int a, int b) { return 0.5 * (p[a] + p[b]); };
          // Wound from inside to outside, decided on the edges' MIDPOINTS:
          // there the cut is a plane section of the tetrahedron (a triangle
          // or a parallelogram) and the sign is far from zero, whatever
          // the interpolation does to the real vertices.
          glm::dvec3 inC(0.0), outC(0.0);
          for (int q = 0; q < ni; q++)
            inC += p[in[q]] / double(ni);
          for (int q = 0; q < no; q++)
            outC += p[out[q]] / double(no);
          std::array<std::pair<int, int>, 4> poly;
          int corners = 3;
          if (ni == 1)
            poly = {{{in[0], out[0]}, {in[0], out[1]}, {in[0], out[2]}, {}}};
          else if (ni == 3)
            poly = {{{in[0], out[0]}, {in[1], out[0]}, {in[2], out[0]}, {}}};
          else {
            poly = {{{in[0], out[0]},
                     {in[0], out[1]},
                     {in[1], out[1]},
                     {in[1], out[0]}}};
            corners = 4;
          }
          const glm::dvec3 m0 = mid(poly[0].first, poly[0].second),
                           m1 = mid(poly[1].first, poly[1].second),
                           m2 = mid(poly[2].first, poly[2].second);
          const bool reverse =
              glm::dot(glm::cross(m1 - m0, m2 - m0), outC - inC) < 0;
          uint32_t v[4];
          for (int q = 0; q < corners; q++)
            v[q] = vertexOn(poly[q].first, poly[q].second);
          const auto emit = [&](uint32_t a, uint32_t b, uint32_t c) {
            m.triangles.push_back(reverse ? std::array{a, c, b}
                                          : std::array{a, b, c});
          };
          emit(v[0], v[1], v[2]);
          if (corners == 4)
            emit(v[0], v[2], v[3]);
        }
      }
    std::swap(below, above);
  }
  mesh::BuildAdjacency(m);
  return m;
}

// The grid spacing that makes about `triangles`: a contour at a quarter of
// the count says how the count goes with h (as 1/h²).
double SpacingFor(size_t triangles) {
  static std::mutex mu;
  static std::unordered_map<size_t, double> known;
  std::lock_guard lock(mu);
  if (auto it = known.find(triangles); it != known.end())
    return it->second;
  // A first guess from the figure's surface (about 1.9e4 mm²) and marching
  // tetrahedra's ~3.5 triangles per h² of surface; the trial measures it.
  const double area = 1.9e4, perH2 = 3.5;
  const double trialCount = std::max<double>(2000.0, triangles / 4.0);
  const double h0 = std::sqrt(perH2 * area / trialCount);
  const double c0 = double(Contour(h0).triangles.size());
  const double h = h0 * std::sqrt(c0 / double(triangles));
  known[triangles] = h;
  return h;
}

} // namespace

// -------------------------------------------------------------- fixtures --

Organic NoisySphere(size_t triangles, double radius, double noise) {
  Organic o;
  o.name = "sphere";
  o.mesh =
      CubeSphere(CubeSide(triangles), [&](const glm::dvec3& d, uint64_t key) {
        return d * (radius + noise * Noise(key));
      });
  const double h = LongestEdge(o.mesh);
  // A flat triangle stands off the sphere by at most its sagitta, h²/8R; the
  // noise moves each vertex by at most `noise`.
  const double standoff = h * h / (8.0 * (radius - noise)) + noise;
  o.area = 4.0 * kPi * radius * radius;
  o.volume = 4.0 / 3.0 * kPi * radius * radius * radius;
  o.volumeTolerance = o.area * standoff * 1.05;
  // A tilted facet is longer than its footprint: the noise across an edge
  // tilts it by about 2 noise / h, which lengthens it by half the square.
  const double tilt = 2.0 * noise / std::max(h / 2.0, 1e-12);
  o.areaTolerance = o.area * (h * h / (4.0 * radius * radius) + tilt * tilt);
  o.radius = radius;
  o.radiusTolerance = noise * (1.0 + 1e-9);
  o.lo = glm::dvec3(-radius - noise);
  o.hi = glm::dvec3(radius + noise);
  return o;
}

glm::dvec3 Post::Apex() const {
  return base + axis * (height + coneHeight * radius / (radius - topRadius));
}

double Post::HalfAngle() const {
  return std::atan((radius - topRadius) / coneHeight);
}

Post ScannedPost(size_t triangles, double noise, double lump) {
  Post post;
  post.noise = noise;
  post.axis = glm::normalize(glm::dvec3(0.3, -0.2, 1.0));
  post.base = {5.0, -3.0, 2.0};
  const double R = post.radius, H = post.height, h = post.coneHeight,
               r2 = post.topRadius;
  const double slant = std::hypot(R - r2, h);
  const double length = R + H + slant + r2;
  // 2 nTheta nProfile triangles at one edge length e along both.
  const double e = std::sqrt(2.0 * 2.0 * kPi * R * length /
                             double(std::max<size_t>(triangles, 200)));
  const int nTheta = std::max(12, int(std::lround(2.0 * kPi * R / e)));
  // The profile as (r, z) samples with their normals, corner to corner.
  struct Sample {
    double r, z;
    glm::dvec2 n; // (radial, axial)
  };
  std::vector<Sample> prof;
  const auto segment = [&](glm::dvec2 a, glm::dvec2 b, glm::dvec2 n,
                           bool last) {
    const int k = std::max(1, int(std::lround(glm::length(b - a) / e)));
    for (int i = 0; i < k + (last ? 1 : 0); i++) {
      const glm::dvec2 q = a + (b - a) * (double(i) / k);
      prof.push_back({q.x, q.y, n});
    }
  };
  const glm::dvec2 coneN = glm::normalize(glm::dvec2(h, R - r2));
  segment({0, 0}, {R, 0}, {0, -1}, false);
  segment({R, 0}, {R, H}, {1, 0}, false);
  segment({R, H}, {r2, H + h}, coneN, false);
  segment({r2, H + h}, {0, H + h}, {0, 1}, true);
  // The frame: local z along the axis.
  glm::dvec3 ex = glm::normalize(glm::cross(glm::dvec3(0, 1, 0), post.axis));
  const glm::dvec3 ey = glm::cross(post.axis, ex);
  const auto world = [&](double r, double z, double th) {
    return post.base + ex * (r * std::cos(th)) + ey * (r * std::sin(th)) +
           post.axis * z;
  };
  const auto normalAt = [&](const glm::dvec2& n, double th) {
    return ex * (n.x * std::cos(th)) + ey * (n.x * std::sin(th)) +
           post.axis * n.y;
  };
  // The lump: on the cylinder at theta 0, half way up.
  const double lumpZ = H / 2.0, sigma = 3.0;
  post.lumpSigma = sigma;
  post.onLump = world(R + lump, lumpZ, 0.0);
  mesh::TriMesh m;
  std::vector<std::vector<uint32_t>> ring(prof.size());
  uint64_t key = 0x5ca9;
  for (size_t i = 0; i < prof.size(); i++) {
    const Sample& sm = prof[i];
    const bool pole = sm.r < 1e-12;
    const int count = pole ? 1 : nTheta;
    for (int j = 0; j < count; j++) {
      const double th = 2.0 * kPi * j / nTheta;
      glm::dvec3 p = world(sm.r, sm.z, th);
      const glm::dvec3 n = normalAt(sm.n, th);
      p += n * (noise * Noise(key++));
      if (std::abs(sm.r - R) < 1e-12 && sm.z > 0.0 && sm.z < H) {
        const double arc = std::remainder(th, 2.0 * kPi) * R, dz = sm.z - lumpZ;
        p += n *
             (lump * std::exp(-(arc * arc + dz * dz) / (2.0 * sigma * sigma)));
      }
      ring[i].push_back(uint32_t(m.positions.size()));
      m.positions.push_back(p);
    }
  }
  for (size_t i = 0; i + 1 < prof.size(); i++) {
    const auto& a = ring[i];
    const auto& b = ring[i + 1];
    for (int j = 0; j < nTheta; j++) {
      const int k = (j + 1) % nTheta;
      if (a.size() == 1)
        m.triangles.push_back({a[0], b[size_t(j)], b[size_t(k)]});
      else if (b.size() == 1)
        m.triangles.push_back({a[size_t(j)], b[0], a[size_t(k)]});
      else {
        m.triangles.push_back({a[size_t(j)], b[size_t(j)], b[size_t(k)]});
        m.triangles.push_back({a[size_t(j)], b[size_t(k)], a[size_t(k)]});
      }
    }
  }
  mesh::BuildAdjacency(m);
  if (m.Volume() < 0.0) {
    for (auto& t : m.triangles)
      std::swap(t[1], t[2]);
    mesh::BuildAdjacency(m);
  }
  m.quantum = mesh::Float32Quantum(m.positions);
  post.onCylinder = world(R, H * 0.4, kPi);
  post.onCone = world(0.5 * (R + r2), H + 0.5 * h, kPi * 0.5);
  post.onTop = world(r2 * 0.4, H + h, kPi * 0.25);
  post.onBottom = world(R * 0.5, 0.0, kPi * 1.25);
  Organic& o = post.organic;
  o.name = "post";
  o.volume = kPi * R * R * H + kPi * h / 3.0 * (R * R + R * r2 + r2 * r2);
  o.area =
      kPi * R * R + 2.0 * kPi * R * H + kPi * (R + r2) * slant + kPi * r2 * r2;
  m.Bounds(o.lo, o.hi);
  o.mesh = std::move(m);
  return post;
}

Organic Torus(size_t triangles) {
  const double R = 30.0, r = 10.0;
  const int nv = std::max(3, int(std::lround(std::sqrt(triangles / 6.0))));
  const int nu = std::max(3, int(std::lround(triangles / (2.0 * nv))));
  Organic o;
  o.name = "torus";
  mesh::TriMesh& m = o.mesh;
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      const double u = 2.0 * kPi * i / nu, v = 2.0 * kPi * j / nv;
      m.positions.emplace_back((R + r * std::cos(v)) * std::cos(u),
                               (R + r * std::cos(v)) * std::sin(u),
                               r * std::sin(v));
    }
  const auto id = [&](int i, int j) {
    return uint32_t((i % nu) * nv + (j % nv));
  };
  for (int i = 0; i < nu; i++)
    for (int j = 0; j < nv; j++) {
      // (u, v) is right-handed with the outward normal on a torus about +Z.
      m.triangles.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1)});
      m.triangles.push_back({id(i, j), id(i + 1, j + 1), id(i, j + 1)});
    }
  mesh::BuildAdjacency(m);
  const double h = LongestEdge(m);
  // The tighter of its curvatures is the tube's, r.
  o.area = 4.0 * kPi * kPi * R * r;
  o.volume = 2.0 * kPi * kPi * R * r * r;
  o.volumeTolerance = o.area * h * h / (8.0 * r);
  o.areaTolerance = o.area * h * h / (2.0 * r * r);
  o.radius = r;
  o.radiusTolerance = h * h / (8.0 * r) + 1e-9;
  o.lo = {-R - r, -R - r, -r};
  o.hi = {R + r, R + r, r};
  return o;
}

Organic Figurine(size_t triangles) {
  const Reference& ref = FigureReference();
  const double h = SpacingFor(triangles);
  Organic o;
  o.name = "figurine";
  o.mesh = Contour(h);
  o.area = 0.0; // not known apart from the triangles
  o.volume = ref.volume;
  const double surface = o.mesh.SurfaceArea();
  // Each vertex sits on its grid edge within 2% of h of the linear
  // crossing (the clamp), and the linear crossing within h²/8ρ of the true
  // surface for a curvature radius ρ no smaller than the figure's tightest
  // blend (3 mm); the reference's own column sampling adds 0.1%.
  o.volumeTolerance =
      surface * (0.02 * h + h * h / (8.0 * 3.0)) + 1e-3 * ref.volume;
  o.lo = ref.lo;
  o.hi = ref.hi;
  return o;
}

Organic CutFigurine(size_t triangles) {
  Organic o = Figurine(triangles);
  o.name = "cut_figurine";
  const double cut = o.lo.z + 3.0;
  mesh::TriMesh& m = o.mesh;
  std::vector<std::array<uint32_t, 3>> kept;
  for (const auto& t : m.triangles)
    if (std::min({m.positions[t[0]].z, m.positions[t[1]].z,
                  m.positions[t[2]].z}) >= cut)
      kept.push_back(t);
  m.triangles = std::move(kept);
  Compact(m);
  mesh::BuildAdjacency(m);
  o.volume = o.volumeTolerance = 0.0;
  o.boundaryLoops = 1;
  return o;
}

DirtySoup Dirty() {
  DirtySoup d;
  mesh::TriMesh m = NoisySphere(4800, 30.0, 0.02).mesh;
  // Slivers: an edge split a five-hundredth of the way along, in both its
  // triangles, so each makes two needles and the mesh stays closed.
  constexpr int kSplits = 5;
  for (int s = 0; s < kSplits; s++) {
    const uint32_t t = uint32_t(s * m.triangles.size() / kSplits + 7);
    const uint32_t u = m.neighbour[t][0];
    const int ue = m.neighbourEdge[t][0];
    const uint32_t a = m.triangles[t][0], b = m.triangles[t][1],
                   c = m.triangles[t][2];
    const uint32_t dvx = m.triangles[u][(ue + 2) % 3];
    const auto mid = uint32_t(m.positions.size());
    m.positions.push_back(m.positions[a] +
                          0.002 * (m.positions[b] - m.positions[a]));
    // t = (a, b, c), u = (b, a, d).
    m.triangles[t] = {a, mid, c};
    m.triangles.push_back({mid, b, c});
    m.triangles[u] = {b, mid, dvx};
    m.triangles.push_back({mid, a, dvx});
    d.slivers += 2;
  }
  mesh::BuildAdjacency(m);
  // Flipped: triangles far apart, none next to another or to a split.
  constexpr int kFlips = 7;
  for (int f = 0; f < kFlips; f++) {
    const uint32_t t = uint32_t((2 * f + 1) * m.triangles.size() / 15 + 17);
    std::swap(m.triangles[t][1], m.triangles[t][2]);
  }
  d.flipped = kFlips;
  const mesh::TriMesh small =
      Translated(NoisySphere(108, 5.0, 0.0).mesh, {100.0, 0.0, 0.0});
  d.quantum = 1e-4;
  d.vertices = m.positions.size() + small.positions.size();
  d.triangles = m.triangles.size() + small.triangles.size();
  size_t corner = 0;
  const mesh::TriMesh* parts[] = {&m, &small};
  for (const mesh::TriMesh* part : parts)
    for (uint32_t t = 0; t < part->triangles.size(); t++)
      for (int c = 0; c < 3; c++, corner++) {
        glm::dvec3 p = part->Corner(t, c);
        // Every 97th corner written a little off its vertex, as a file
        // that rounded the same point two ways.
        if (corner % 97 == 5) {
          p.x += 0.3 * d.quantum;
          d.nearDuplicates++;
        }
        d.soup.push_back(p);
      }
  return d;
}

size_t BoundaryLoops(const mesh::TriMesh& m, double* longest) {
  std::unordered_multimap<uint32_t, uint32_t> next; // from -> to
  for (uint32_t t = 0; t < m.triangles.size(); t++)
    for (int e = 0; e < 3; e++)
      if (m.neighbour[t][e] == mesh::kNone)
        next.emplace(m.triangles[t][(e + 1) % 3], m.triangles[t][e]);
  size_t loops = 0;
  double best = 0.0;
  while (!next.empty()) {
    auto it = next.begin();
    const uint32_t start = it->first;
    uint32_t at = start;
    double length = 0.0;
    do {
      const uint32_t to = it->second;
      length += glm::length(m.positions[to] - m.positions[at]);
      next.erase(it);
      at = to;
      it = next.find(at);
    } while (at != start && it != next.end());
    loops++;
    best = std::max(best, length);
  }
  if (longest)
    *longest = best;
  return loops;
}

size_t Slivers(const mesh::TriMesh& m, double ratio) {
  size_t n = 0;
  for (uint32_t t = 0; t < m.triangles.size(); t++) {
    double edge = 0.0;
    for (int e = 0; e < 3; e++)
      edge = std::max(edge,
                      glm::length(m.Corner(t, e) - m.Corner(t, (e + 1) % 3)));
    // height on the longest edge = 2 area / edge
    if (edge > 0 && 2.0 * m.Area(t) / edge < ratio * edge)
      n++;
  }
  return n;
}

void WriteStlTo(const mesh::TriMesh& m, const std::filesystem::path& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out)
    throw std::runtime_error("cannot write " + path.string());
  char header[80] = "binary STL written by tests/mesh_organic.cpp";
  out.write(header, sizeof header);
  const auto count = uint32_t(m.triangles.size());
  out.write(reinterpret_cast<const char*>(&count), 4);
  std::vector<char> buffer;
  buffer.reserve(size_t(50) * 65536);
  for (uint32_t t = 0; t < count; t++) {
    const glm::dvec3 n = glm::normalize(m.AreaNormal(t));
    float record[12] = {float(n.x), float(n.y), float(n.z)};
    for (int c = 0; c < 3; c++) {
      const glm::dvec3 p = m.Corner(t, c);
      record[3 + 3 * c] = float(p.x);
      record[4 + 3 * c] = float(p.y);
      record[5 + 3 * c] = float(p.z);
    }
    const char* bytes = reinterpret_cast<const char*>(record);
    buffer.insert(buffer.end(), bytes, bytes + sizeof record);
    buffer.push_back(0);
    buffer.push_back(0);
    if (buffer.size() >= size_t(50) * 65536) {
      out.write(buffer.data(), std::streamsize(buffer.size()));
      buffer.clear();
    }
  }
  out.write(buffer.data(), std::streamsize(buffer.size()));
  if (!out)
    throw std::runtime_error("writing " + path.string() + " failed");
}

std::filesystem::path WriteStl(const mesh::TriMesh& m, const std::string& tag) {
  const auto path = std::filesystem::temp_directory_path() /
                    ("lobole_mesh_organic_" + std::to_string(::getpid()) + "_" +
                     tag + ".stl");
  WriteStlTo(m, path);
  return path;
}

mesh::TriMesh Translated(const mesh::TriMesh& m, const glm::dvec3& offset) {
  mesh::TriMesh out = m;
  for (glm::dvec3& p : out.positions)
    p += offset;
  return out;
}

bool OrganicByName(const std::string& name, size_t triangles, Organic& out) {
  if (name == "sphere")
    out = NoisySphere(triangles);
  else if (name == "torus")
    out = Torus(triangles);
  else if (name == "figurine")
    out = Figurine(triangles);
  else if (name == "cut_figurine")
    out = CutFigurine(triangles);
  else
    return false;
  return true;
}

} // namespace fixtures
