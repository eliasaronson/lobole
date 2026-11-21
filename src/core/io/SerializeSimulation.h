// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// A simulation document saved and opened
// (docs/simulation/README.md#how-a-simulation-is-saved), discriminated by
// `kind: "simulation"` (v223). Two forms, dispatched on the path as a job's
// are:
//
// - the directory form (a project's `simulations/<folder>/`): `index.json`
//   holds the subject, the roles, the mesh settings and the studies; each
//   stored result is `results/<study id>.result.{json,bin}` and each stored
//   mesh `meshes/<key>.mesh.{json,bin}`, so a re-save of an unchanged mesh
//   writes nothing;
// - the single file: the same body, with `results` and `meshes` inline, their
//   bytes base64.
//
// Every mesh the document keeps (SimDocument::KeptMeshKeys) is saved;
// ClearStorage() removes them, and the results, from the next save. A result
// or mesh is DERIVED: one that does not read is dropped and reported, and the
// study runs again. The document itself is refused, naming what is accepted,
// on an unknown key, subject kind, participant role or study field.
//
// A loaded simulation's results are stale until the caller hands it its
// subject (fem::RestampSimByInputs), since only the subject says whether the
// inputs still hash as they did.

#include "core/fem/SimDocument.h"

#include <nlohmann/json_fwd.hpp>

#include <expected>
#include <memory>
#include <string>

namespace DocumentIO {

[[nodiscard]] std::expected<void, std::string>
SaveSimulation(const fem::SimDocument& sim, const std::string& path);
// The document alone, as one file without its stored results and meshes:
// what an autosave sidecar keeps (docs/versioning/README.md
// #autosave-and-recovery). Both are derived, and a big mesh is tens of
// megabytes written on every autosave.
[[nodiscard]] std::expected<void, std::string>
SaveSimulationBody(const fem::SimDocument& sim, const std::string& path);
[[nodiscard]] std::expected<std::unique_ptr<fem::SimDocument>, std::string>
LoadSimulation(const std::string& path);
// The body as text, without results or meshes: two simulations with the same
// fingerprint are the same document (what an undo guard compares).
[[nodiscard]] std::string SimulationFingerprint(const fem::SimDocument& sim);

// One stored mesh as a json whose arrays ride under kResultData (positions
// f64: a mesh is solved on again, not only drawn). FromJson throws on a
// malformed one: counts that disagree, an index past the nodes.
nlohmann::json StoredMeshToJson(uint64_t key, const fem::StoredMesh& mesh);
fem::StoredMesh StoredMeshFromJson(const nlohmann::json& j, uint64_t& key);

} // namespace DocumentIO
