// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// How far a body is from a scan, on a document
// (docs/inspect/README.md#how-far-a-body-is-from-a-scan): what `compare_scan`,
// `set_scan_deviation` and the Compare to scan tool share. The bodies are
// gathered on the main thread -- a B-Rep tessellated on a copy, finer than
// the tolerance, each triangle keeping its face -- so that the comparison
// (mesh::CompareToBody) can run on a worker with nothing of the document.

#include "core/Receipt.h"
#include "core/mesh/Deviation.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Document;
struct Body;

namespace scancompare {

struct Request {
  uint64_t scanId = 0, bodyId = 0; // the features that made them
  std::optional<double> toleranceMm;
};

// One face of the body as the reply names it.
struct FaceRef {
  int body = -1;       // the body's index in the document's bodies
  int face = -1;       // its face index on that body
  std::string element; // its stable name, when it has one
};

struct Input {
  Request request;
  std::string scanName, bodyName;
  std::shared_ptr<const mesh::TriMesh> scan, body;
  std::vector<int> scanBodies;     // the scan's bodies, by document index
  std::vector<int> bodyBodies;     // the body's
  std::vector<uint32_t> bodyFaces; // per body triangle, into `faces`
  std::vector<FaceRef> faces;
  double chordMm = 0.0; // a B-Rep's tessellation; 0 for a mesh body
};

// Every input checked and gathered, before anything runs: "" and `out`
// filled, or why not -- a scan that is no mesh body, a body that is the
// scan, a feature that made nothing, a tolerance that is not a positive
// number. Throws nothing.
std::string Gather(const Document& doc, const Request& request, Input& out);

// The surface of `bodies` (each with its index among the document's) as
// `out.body`, `out.bodyFaces`, `out.faces` and `out.chordMm`: a mesh body's
// own triangles, a B-Rep tessellated on a copy at a quarter of `toleranceMm`
// (never coarser than a thousandth of its size), each triangle keeping its
// face. What Gather calls; a test calls it on bodies of its own.
void AddBodies(const std::vector<std::pair<const Body*, int>>& bodies,
               double toleranceMm, Input& out);

struct Outcome {
  mesh::Deviation deviation;
  nlohmann::json reply; // what compare_scan answers, but its receipt
  receipt::Receipt receipt;
};

// The comparison and its reply. `cancel` and `fraction` may be null.
Outcome Run(const Input& input, const std::atomic<bool>* cancel,
            std::atomic<double>* fraction);

// The reply for `d` (Run's): {scan, body, tolerance_mm, tolerance_from,
// noise_mm, floor_mm, within, mean_mm, rms_mm, p95_mm, min_mm, max_mm,
// no_scan, unmatched, hotspot_count, hotspots[]}; the receipt beside it.
nlohmann::json ReplyJson(const Input& input, const mesh::Deviation& d);
// What it can and cannot say: no feature past the tolerance (pass or
// fail), and the body's area no scan lies under, which nothing checked.
receipt::Receipt ReceiptOf(const Input& input, const mesh::Deviation& d);

// How many hotspots a reply lists (the worst); hotspot_count says how many
// there are.
inline constexpr size_t kReplyHotspots = 50;

} // namespace scancompare
