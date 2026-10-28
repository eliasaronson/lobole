// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

// Writes one organic fixture (tests/mesh_organic.h) as a binary STL, with
// its known answers beside it as JSON: the bench's mesh workloads read
// these (tools/bench_workloads.py, docs/performance/README.md).
//
//   mesh_fixture_writer NAME TRIANGLES OUT.stl
//
// writes OUT.stl and OUT.stl.json. NAME is sphere, torus, figurine or
// cut_figurine; any other is refused, naming them.

#include "mesh_organic.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s NAME TRIANGLES OUT.stl\n", argv[0]);
    return 2;
  }
  const std::string name = argv[1], out = argv[3];
  const size_t triangles = std::strtoull(argv[2], nullptr, 10);
  fixtures::Organic o;
  if (triangles == 0 || !fixtures::OrganicByName(name, triangles, o)) {
    std::fprintf(stderr,
                 "mesh_fixture_writer: unknown fixture '%s' or count '%s'; "
                 "the fixtures are sphere, torus, figurine, cut_figurine\n",
                 argv[1], argv[2]);
    return 2;
  }
  fixtures::WriteStlTo(o.mesh, out);
  std::ofstream json(out + ".json");
  char buf[1024];
  std::snprintf(
      buf, sizeof buf,
      "{\"name\": \"%s\", \"triangles\": %zu, \"closed\": %s, "
      "\"volume_mm3\": %.6f, \"volume_tolerance_mm3\": %.6f, "
      "\"mesh_volume_mm3\": %.6f, \"area_mm2\": %.6f, "
      "\"bbox_min_mm\": [%.6f, %.6f, %.6f], "
      "\"bbox_max_mm\": [%.6f, %.6f, %.6f], \"boundary_loops\": %zu}\n",
      o.name.c_str(), o.mesh.TriangleCount(),
      o.mesh.Closed() ? "true" : "false", o.volume, o.volumeTolerance,
      o.mesh.Closed() ? o.mesh.Volume() : 0.0, o.area, o.lo.x, o.lo.y, o.lo.z,
      o.hi.x, o.hi.y, o.hi.z, o.boundaryLoops);
  json << buf;
  return json ? 0 : 1;
}
