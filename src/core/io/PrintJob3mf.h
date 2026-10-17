// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A print job as a 3MF Bambu Studio opens and slices
// (docs/tolerances/README.md#print-jobs): the 3MF core model, one object
// per piece, Bambu's per-object settings (Metadata/model_settings.config)
// and one plate per plate.
//
// What Bambu needs, measured with its CLI (02.08):
//  - per-object settings are read from model_settings.config in any 3MF;
//  - PLATES are kept only in a Bambu PROJECT (Application metadata and a
//    Metadata/project_settings.config), and Bambu applies that config over
//    its built-in defaults, so it must be COMPLETE: a partial one slices on
//    a 200 mm bed with a generic start G-code. Loböle has no slicer preset
//    of its own, so a project is written only from a config it is handed
//    (`projectConfig`); without one the job is the plain MODEL form, which
//    Bambu opens with the user's own presets, on one plate.
//  - plate k sits at Bambu's plate grid: columns ceil-ish sqrt(n), each
//    plate a bed and a fifth apart.
// No geometry here: the caller meshes and places.
namespace printjob {

struct Object {
  std::string name;
  std::vector<glm::dvec3> vertices; // mm, the plate's (bed's) frame
  std::vector<std::array<uint32_t, 3>> triangles;
  // Bambu's per-object keys (printset::BambuObjectSettings).
  std::vector<std::pair<std::string, std::string>> settings;
};

struct Plate {
  std::string name;
  std::vector<Object> objects;
};

struct Job {
  std::vector<Plate> plates;
  glm::dvec2 bedMm{0.0}; // the bed's x and y: the plate grid's pitch
  // A complete Bambu config (a project's project_settings.config): written
  // whole, which makes the 3MF a project. Unset = the model form.
  std::optional<nlohmann::json> projectConfig;
  // Loböle's own record of the job, Metadata/lobole_job.json.
  nlohmann::json record;
};

// Where plate `index` of `count` sits in Bambu's plate grid, mm.
[[nodiscard]] glm::dvec2 PlateOrigin(int index, int count,
                                     const glm::dvec2& bedMm);

// The parts of the archive, for tests: path -> content.
[[nodiscard]] std::vector<std::pair<std::string, std::string>>
Entries(const Job& job);

// The 3MF's bytes (a zip, every entry deflated).
[[nodiscard]] std::string Archive(const Job& job);

// The keys a project config must carry for Bambu to load it, and whether
// `config` does: empty, or what is missing.
[[nodiscard]] std::string MissingProjectKeys(const nlohmann::json& config);

} // namespace printjob
