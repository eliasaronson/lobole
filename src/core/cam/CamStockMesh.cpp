#include "core/cam/CamStockMesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace cam {

namespace {

// Four corners, counter-clockwise as seen from the side `normal` points at.
void AddQuad(StockMesh& m, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d, const glm::vec3& normal) {
  const uint32_t base = static_cast<uint32_t>(m.vertices.size());
  m.vertices.push_back({a, normal});
  m.vertices.push_back({b, normal});
  m.vertices.push_back({c, normal});
  m.vertices.push_back({d, normal});
  m.indices.insert(m.indices.end(),
                   {base, base + 1, base + 2, base, base + 2, base + 3});
}

// A wall standing on the line x = X, between y0 and y1, from zLow to zHigh.
// `outwardPlusX` says which side the material is NOT on.
void AddWallX(StockMesh& m, double X, double y0, double y1, double zLow,
              double zHigh, bool outwardPlusX) {
  if (zHigh - zLow < 1e-9)
    return;
  const glm::vec3 n(outwardPlusX ? 1.0f : -1.0f, 0.0f, 0.0f);
  const glm::vec3 a(X, y0, zLow), b(X, y1, zLow), c(X, y1, zHigh),
      d(X, y0, zHigh);
  if (outwardPlusX)
    AddQuad(m, a, b, c, d, n);
  else
    AddQuad(m, d, c, b, a, n);
}

void AddWallY(StockMesh& m, double Y, double x0, double x1, double zLow,
              double zHigh, bool outwardPlusY) {
  if (zHigh - zLow < 1e-9)
    return;
  const glm::vec3 n(0.0f, outwardPlusY ? 1.0f : -1.0f, 0.0f);
  const glm::vec3 a(x1, Y, zLow), b(x0, Y, zLow), c(x0, Y, zHigh),
      d(x1, Y, zHigh);
  if (outwardPlusY)
    AddQuad(m, a, b, c, d, n);
  else
    AddQuad(m, d, c, b, a, n);
}

} // namespace

double StockMesh::Volume() const {
  // Divergence theorem over the closed surface: six times the volume is the
  // sum of the triangles' scalar triple products.
  double sixV = 0.0;
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const glm::dvec3 a(vertices[indices[i]].position);
    const glm::dvec3 b(vertices[indices[i + 1]].position);
    const glm::dvec3 c(vertices[indices[i + 2]].position);
    sixV += glm::dot(a, glm::cross(b, c));
  }
  return sixV / 6.0;
}

StockMesh BuildStockMesh(const HeightMap& map, double floorZ) {
  StockMesh mesh;
  if (map.nx <= 0 || map.ny <= 0 || map.z.empty())
    return mesh;

  // A through cut with a sacrificial pass goes BELOW the stock bottom. The
  // floor follows it rather than clipping it away — a solid that ends above
  // its own lowest cut is not a solid.
  double floor = floorZ;
  float lowest = std::numeric_limits<float>::max();
  float highest = -std::numeric_limits<float>::max();
  for (const float z : map.z) {
    lowest = std::min(lowest, z);
    highest = std::max(highest, z);
  }
  floor = std::min(floor, static_cast<double>(lowest) - 1e-6);

  const double res = map.res;
  const auto X = [&](int i) { return map.x0 + i * res; };
  const auto Y = [&](int j) { return map.y0 + j * res; };
  const auto Z = [&](int i, int j) {
    return static_cast<double>(map.At(i, j));
  };

  // --- the machined top, as merged rectangles -----------------------------
  //
  // Greedy: run east while the height matches, then run north while the whole
  // run matches. A flat block comes out as ONE quad; a pocket floor as one
  // more. Only genuinely curved surfaces (a ball nose) degrade to per-cell.
  std::vector<uint8_t> used(map.z.size(), 0);
  const auto Used = [&](int i, int j) -> uint8_t& {
    return used[static_cast<size_t>(j) * static_cast<size_t>(map.nx) +
                static_cast<size_t>(i)];
  };
  for (int j = 0; j < map.ny; j++) {
    for (int i = 0; i < map.nx; i++) {
      if (Used(i, j))
        continue;
      const float z = map.At(i, j);
      int w = 1;
      while (i + w < map.nx && !Used(i + w, j) && map.At(i + w, j) == z)
        w++;
      int h = 1;
      while (j + h < map.ny) {
        bool wholeRow = true;
        for (int k = 0; k < w && wholeRow; k++)
          wholeRow = !Used(i + k, j + h) && map.At(i + k, j + h) == z;
        if (!wholeRow)
          break;
        h++;
      }
      for (int jj = j; jj < j + h; jj++)
        for (int ii = i; ii < i + w; ii++)
          Used(ii, jj) = 1;
      AddQuad(mesh, {X(i), Y(j), z}, {X(i + w), Y(j), z},
              {X(i + w), Y(j + h), z}, {X(i), Y(j + h), z}, {0, 0, 1});
    }
  }

  // --- walls between neighbouring cells ------------------------------------
  //
  // Merged along the edge while both heights repeat, so a pocket's straight
  // side is one quad and not one per cell.
  for (int i = 0; i + 1 < map.nx; i++) {
    for (int j = 0; j < map.ny;) {
      const double za = Z(i, j), zb = Z(i + 1, j);
      if (std::abs(za - zb) < 1e-9) {
        j++;
        continue;
      }
      int run = 1;
      while (j + run < map.ny && Z(i, j + run) == za && Z(i + 1, j + run) == zb)
        run++;
      // The wall faces whichever side is LOWER — that is where you stand to
      // see it.
      AddWallX(mesh, X(i + 1), Y(j), Y(j + run), std::min(za, zb),
               std::max(za, zb), za > zb);
      j += run;
    }
  }
  for (int j = 0; j + 1 < map.ny; j++) {
    for (int i = 0; i < map.nx;) {
      const double za = Z(i, j), zb = Z(i, j + 1);
      if (std::abs(za - zb) < 1e-9) {
        i++;
        continue;
      }
      int run = 1;
      while (i + run < map.nx && Z(i + run, j) == za && Z(i + run, j + 1) == zb)
        run++;
      AddWallY(mesh, Y(j + 1), X(i), X(i + run), std::min(za, zb),
               std::max(za, zb), za > zb);
      i += run;
    }
  }

  // --- the four sides of the block, down to the floor ----------------------
  for (int j = 0; j < map.ny;) {
    int run = 1;
    while (j + run < map.ny && Z(0, j + run) == Z(0, j))
      run++;
    AddWallX(mesh, X(0), Y(j), Y(j + run), floor, Z(0, j), false);
    j += run;
  }
  for (int j = 0; j < map.ny;) {
    int run = 1;
    while (j + run < map.ny && Z(map.nx - 1, j + run) == Z(map.nx - 1, j))
      run++;
    AddWallX(mesh, X(map.nx), Y(j), Y(j + run), floor, Z(map.nx - 1, j), true);
    j += run;
  }
  for (int i = 0; i < map.nx;) {
    int run = 1;
    while (i + run < map.nx && Z(i + run, 0) == Z(i, 0))
      run++;
    AddWallY(mesh, Y(0), X(i), X(i + run), floor, Z(i, 0), false);
    i += run;
  }
  for (int i = 0; i < map.nx;) {
    int run = 1;
    while (i + run < map.nx && Z(i + run, map.ny - 1) == Z(i, map.ny - 1))
      run++;
    AddWallY(mesh, Y(map.ny), X(i), X(i + run), floor, Z(i, map.ny - 1), true);
    i += run;
  }

  // --- the floor -----------------------------------------------------------
  AddQuad(mesh, {X(0), Y(0), floor}, {X(0), Y(map.ny), floor},
          {X(map.nx), Y(map.ny), floor}, {X(map.nx), Y(0), floor}, {0, 0, -1});

  mesh.bboxMin = glm::vec3(X(0), Y(0), floor);
  mesh.bboxMax = glm::vec3(X(map.nx), Y(map.ny), highest);
  return mesh;
}

namespace {

// One turn of a profile polyline (radius, z) about +Z. Normals come from the
// profile's own 2D normal, so a ball nose reads as a sphere and a flat mill's
// shoulder stays a hard edge.
// The outward normal of one profile segment, in the (radius, z) plane.
glm::dvec2 SegmentNormal(const glm::dvec2& from, const glm::dvec2& to) {
  const glm::dvec2 t = to - from;
  glm::dvec2 n(t.y, -t.x);
  const double len = glm::length(n);
  n = len > 1e-12 ? n / len : glm::dvec2(1.0, 0.0);
  return n.x < 0.0 ? -n : n;
}

void Revolve(StockMesh& m, const std::vector<glm::dvec2>& profile,
             int segments) {
  if (profile.size() < 2)
    return;
  const double twoPi = 6.283185307179586;
  const int rings = static_cast<int>(profile.size());

  // One normal per SEGMENT, not per profile point. The old form took a
  // central difference across each point, which averages the two faces
  // meeting there into one normal — fine on the tip's sampled curve, wrong
  // at every hard corner. A tool profile has three: flute-to-shoulder,
  // shoulder-to-holder and holder-to-cap. The flat shoulder came out shaded
  // as a 45-degree cone, which reads as a lit facet that does not answer to
  // the camera the way the rest of the glyph does.
  //
  // Points are shared (smooth) only where the two segments actually agree to
  // within the crease angle, so the tip stays round and the corners stay
  // corners.
  constexpr double kCreaseCos = 0.86; // ~30 degrees
  const auto emitRing = [&](const glm::dvec2& p, const glm::dvec2& n2) {
    for (int s = 0; s <= segments; s++) {
      const double a = twoPi * s / segments;
      const double c = std::cos(a), sn = std::sin(a);
      m.vertices.push_back({glm::vec3(p.x * c, p.x * sn, p.y),
                            glm::vec3(n2.x * c, n2.x * sn, n2.y)});
    }
  };

  const uint32_t stride = static_cast<uint32_t>(segments + 1);
  for (int k = 0; k + 1 < rings; k++) {
    const glm::dvec2 nSeg = SegmentNormal(profile[k], profile[k + 1]);
    // Blend with the neighbour across each end, but only across a soft joint.
    glm::dvec2 nLo = nSeg, nHi = nSeg;
    if (k > 0) {
      const glm::dvec2 prev = SegmentNormal(profile[k - 1], profile[k]);
      if (glm::dot(prev, nSeg) > kCreaseCos)
        nLo = glm::normalize(prev + nSeg);
    }
    if (k + 2 < rings) {
      const glm::dvec2 next = SegmentNormal(profile[k + 1], profile[k + 2]);
      if (glm::dot(next, nSeg) > kCreaseCos)
        nHi = glm::normalize(next + nSeg);
    }
    const uint32_t base = static_cast<uint32_t>(m.vertices.size());
    emitRing(profile[k], nLo);
    emitRing(profile[k + 1], nHi);
    for (int s = 0; s < segments; s++) {
      const uint32_t a = base + s;
      const uint32_t b = a + 1;
      const uint32_t c = a + stride;
      const uint32_t d = c + 1;
      // Wound to AGREE with the outward normals above. It was {a,c,b, b,c,d}
      // — the opposite — so every face of the tool glyph was back-facing from
      // outside and front-facing from within: the near wall culled, the far
      // wall's interior drawn, which reads as a transparent tube. The mirror
      // material and a 49 mm holder hid it; a matte collar did not.
      m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
    }
  }
}

} // namespace

StockMesh BuildToolMesh(const Tool& tool) {
  StockMesh mesh;
  const double r = std::max(0.05, tool.diameter * 0.5);
  // A thread mill's cutting part is its head and the neck above it
  // (ReachLength): the shank starts where the neck ends.
  const double flutes = std::max(r, ReachLength(tool));
  // A holder wide enough to read as one, and SHORT. It used to run
  // `flutes + max(20, r*6)` — 49 mm of Ø16 slug for an 8 mm cutter, longer
  // than most of what it was cutting. Its length carries no information (the
  // FLUTE's length is the depth capability, and that is drawn at full size);
  // what it cost was occlusion, and a long near object under a wide
  // perspective FOV that swings its apparent axis far more than it turns —
  // which is what made the glyph read as a shape lying on its side that
  // half-followed the camera.
  //
  // ...unless the tool STATES one, in which case the glyph draws the collar
  // the verifier is checking against. A holder collision reported on line
  // 4102 is a number; the same collar visibly buried in a clamp while the
  // simulation plays is the answer. One source for both, as with everything
  // else here.
  const bool stated = HasHolder(tool);
  const double shankR = tool.shankDiameter > 0.0 ? tool.shankDiameter * 0.5 : r;
  // Unstated, NO holder: nothing says where the nut is, and one drawn on top
  // of the flutes was a claim the verifier never checks -- the chess
  // knights' 1.5 mm ball, 6 mm of flute, showed a 5.5 mm holder buried in
  // the part wherever the rest pass reached deeper than 6 mm, a collision
  // that was not there on a cutter with a long 3.175 mm shank. So the shank,
  // at its stated width, for a length that reads as a shank and not a stub.
  const double holderR = stated ? tool.collarDiameter * 0.5 : shankR;
  // Where the collar's underside sits: stated, the SHANK runs between it and
  // the flutes, which is the gap the whole check is about -- a stickout is
  // only worth typing because it can be longer than the cutting part.
  const double collarZ = stated ? std::max(flutes, tool.overallLength)
                                : flutes + std::max(12.0, 8.0 * shankR);
  const double holderZ = stated ? collarZ + std::max(4.0, r * 1.5) : collarZ;

  std::vector<glm::dvec2> profile;
  profile.push_back({0.0, 0.0});
  // The tip, sampled from the same function the removal uses — but only when
  // there is a shape to sample. A FLAT end mill's tip is one straight step to
  // the rim; sampling it in twelve put twelve segments at the same height,
  // 24 coincident rings of 33 vertices each, for a disc two triangles could
  // draw. It was three quarters of the mesh.
  const bool shaped = tool.type == ToolType::Ball ||
                      tool.type == ToolType::VBit ||
                      tool.type == ToolType::Drill;
  if (tool.type == ToolType::ThreadMill && tool.neckDiameter > 0.0 &&
      tool.neckDiameter < tool.diameter) {
    // The head as it is (docs/cam/operations.md#the-thread-mill-cutter): out
    // along its flat bottom to the neck, up the lower flank to the rim, the
    // flat, the upper flank back in, and the neck up to the shank.
    const double neckR = tool.neckDiameter * 0.5;
    const double rise =
        ProfileDepth(tool) * std::tan(glm::radians(tool.tipAngleDeg * 0.5));
    profile.push_back({neckR, 0.0});
    profile.push_back({r, rise});
    profile.push_back({r, rise + tool.tipFlat});
    profile.push_back({neckR, 2.0 * rise + tool.tipFlat});
    profile.push_back({neckR, flutes});
  } else {
    const int tipSteps = shaped ? 12 : 1;
    for (int i = 1; i <= tipSteps; i++) {
      const double d = r * i / tipSteps;
      profile.push_back({d, TipHeight(tool, d)});
    }
    profile.push_back({r, flutes});
  }
  // Above the flutes, the shank as stated: the 6 mm a 3 mm bit steps out to
  // is the thing the reach check refuses to drive into a wall
  // (docs/cam/setup.md#reach). Unstated, the cutter's width, as always.
  if (std::abs(shankR - r) > 1e-9)
    profile.push_back({shankR, flutes});
  if (collarZ > flutes)
    profile.push_back({shankR, collarZ});
  if (stated) {
    profile.push_back({holderR, collarZ});
    profile.push_back({holderR, holderZ});
  }
  profile.push_back({0.0, holderZ}); // cap, so the top is closed

  Revolve(mesh, profile, 32);

  mesh.bboxMin = glm::vec3(-holderR, -holderR, 0.0f);
  mesh.bboxMax = glm::vec3(holderR, holderR, holderZ);
  return mesh;
}

StockMesh BuildRotaryStockMesh(const RadiusMap& map) {
  StockMesh mesh;
  const int nx = map.nx, n = map.nTheta;
  if (nx < 2 || n < 3 || map.r.size() != static_cast<size_t>(nx) * n)
    return mesh;
  const double dTheta = 2.0 * 3.14159265358979323846 / n;

  // The tube: one vertex per cell, normals from the tangent plane. With the
  // surface written r(x, theta), the two tangents are
  //   dP/dx     = (1, r_x cos, r_x sin)
  //   dP/dtheta = (0, r_t cos - r sin, r_t sin + r cos)
  // and their cross product, theta first, points OUTWARD -- checked against a
  // plain cylinder, where it is the radial direction.
  mesh.vertices.reserve(static_cast<size_t>(nx) * n + 2 * (n + 1));
  for (int j = 0; j < n; j++) {
    const double th = map.CellTheta(j);
    const double c = std::cos(th), sn = std::sin(th);
    const int jm = (j + n - 1) % n, jp = (j + 1) % n;
    for (int i = 0; i < nx; i++) {
      const double r = map.At(i, j);
      const int im = std::max(0, i - 1), ip = std::min(nx - 1, i + 1);
      const double rx = (map.At(ip, j) - map.At(im, j)) / ((ip - im) * map.res);
      const double rt = (map.At(i, jp) - map.At(i, jm)) / (2.0 * dTheta);
      const glm::dvec3 px(1.0, rx * c, rx * sn);
      const glm::dvec3 pt(0.0, rt * c - r * sn, rt * sn + r * c);
      glm::dvec3 nrm = glm::cross(pt, px);
      const double len = glm::length(nrm);
      // On the axis (a cell turned to nothing) the tangent in theta vanishes;
      // the radial direction is the honest answer there.
      nrm = len > 1e-12 ? nrm / len : glm::dvec3(0.0, c, sn);
      mesh.vertices.push_back(
          {glm::vec3(map.CellX(i), r * c, r * sn), glm::vec3(nrm)});
    }
  }
  const auto at = [nx](int i, int j) {
    return static_cast<uint32_t>(j * nx + i);
  };
  // Two triangles per cell, wound so their geometric normal agrees with the
  // outward vertex normals: for corners A=(i,j) B=(i+1,j) C=(i+1,j+1)
  // D=(i,j+1), (A, D, B) and (B, D, C). The other order draws the tube
  // inside out -- the mistake the tool glyph made for a month, which is why
  // the winding gate exists.
  mesh.indices.reserve(static_cast<size_t>(nx - 1) * n * 6 + 6 * n);
  for (int j = 0; j < n; j++) {
    const int jn = (j + 1) % n;
    for (int i = 0; i + 1 < nx; i++) {
      const uint32_t a = at(i, j), b = at(i + 1, j), cc = at(i + 1, jn),
                     d = at(i, jn);
      mesh.indices.insert(mesh.indices.end(), {a, d, b, b, d, cc});
    }
  }

  // The two caps, with their OWN vertices: a cap is flat and faces along the
  // axis, and sharing the tube's radial normals would shade the end face as
  // though it were round.
  const auto cap = [&](int i, bool high) {
    const float x = static_cast<float>(map.CellX(i));
    const glm::vec3 nrm(high ? 1.0f : -1.0f, 0.0f, 0.0f);
    const uint32_t centre = static_cast<uint32_t>(mesh.vertices.size());
    mesh.vertices.push_back({glm::vec3(x, 0.0f, 0.0f), nrm});
    const uint32_t ring0 = static_cast<uint32_t>(mesh.vertices.size());
    for (int j = 0; j < n; j++)
      mesh.vertices.push_back({mesh.vertices[at(i, j)].position, nrm});
    for (int j = 0; j < n; j++) {
      const uint32_t v0 = ring0 + j, v1 = ring0 + (j + 1) % n;
      // Increasing theta runs anticlockwise seen from +X, so (centre, j, j+1)
      // faces +X and the low end takes the reverse.
      if (high)
        mesh.indices.insert(mesh.indices.end(), {centre, v0, v1});
      else
        mesh.indices.insert(mesh.indices.end(), {centre, v1, v0});
    }
  };
  cap(0, false);
  cap(nx - 1, true);

  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const StockVertex& v : mesh.vertices) {
    lo = glm::min(lo, v.position);
    hi = glm::max(hi, v.position);
  }
  mesh.bboxMin = lo;
  mesh.bboxMax = hi;
  return mesh;
}

} // namespace cam
