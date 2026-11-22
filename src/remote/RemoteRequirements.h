// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "remote/RemoteHelpers.h"
#include "remote/RemoteParking.h"
#include "remote/RpcParams.h"
#include <nlohmann/json.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>

class AssemblyDocument;
namespace ui {
struct AppState;
}

// Design requirements (docs/requirements/README.md): add_requirement,
// set_requirement, remove_requirement, list_requirements,
// check_requirements, design_sweep and design_sweep_result, and the
// clearance verbs that are now requirements (add_assertion,
// list_assertions, remove_assertion, verify_assertions).
// Returns the result json when `method` is one of them, std::nullopt
// otherwise.
namespace remote {

std::optional<nlohmann::json> HandleRequirementsRpc(ui::AppState& state,
                                                    const std::string& method,
                                                    const RpcParams& params);

std::span<const MethodDoc> RequirementMethodDocs();

// Everything an assembly sequence check reads
// (docs/assemblies/README.md#assembly-sequences), hashed: the assembly's
// content and every sub-assembly's, every placed part's bodies (shapes,
// thread marks, face names) and the tolerances and print process a fit is
// judged by, and the sequence as saved. The `assembly_sequence`
// requirement's staleness and the check's kept answer
// (remote::RunSequenceCheck) both read it, so what one calls fresh the
// other does too.
uint64_t SequenceInputKey(ui::AppState& state, const AssemblyDocument& asmDoc);

// Whether `method` is one of this module's: they answer requirement rows
// of their own.
[[nodiscard]] bool IsRequirementsMethod(const std::string& method);

// A synchronous `check_requirements` with something to measure waits
// instead of measuring on the frame
// (docs/requirements/README.md#checking-is-a-job): it starts the check's job,
// owned by the caller's connection, and is answered with what the synchronous
// form answers once the job is reaped. A synchronous `design_sweep` waits on
// its job the same way (docs/requirements/README.md#design-sweep). nullptr:
// dispatch it now (async, nothing to measure, or a request the handler
// refuses in its own words).
std::unique_ptr<Parking> ParkRequirementsRequest(ui::AppState& state,
                                                 const std::string& method,
                                                 const nlohmann::json& request);

// A batch's (or check_expect's) `expect` (docs/scripting/rpc.md#expect):
// {volume_mm3?, mass_g?, body_count?, bbox_mm?, tolerance_pct?, part?}.
// Read and checked before anything runs -- an unknown key, a negative
// number, a tolerance missing beside a number, a key the active tab cannot
// measure -- and refused (a ContractError naming the key) with nothing
// changed. Judged afterwards by the requirement code: one transient
// requirement per number, measured and judged as a stored one is, nothing
// stored.
struct Expectations {
  nlohmann::json spec;   // as given
  uint64_t document = 0; // the part or assembly it is judged on
  bool assembly = false;
};
Expectations ReadExpectations(ui::AppState& state, const nlohmann::json& spec,
                              const std::string& method);
// {ok, checks: [{quantity, expected, measured, unit, deviation_pct, pass}]},
// and `expect_failed` (one sentence per miss) beside them when one misses,
// which the server lifts to the top of the message.
nlohmann::json JudgeExpectations(ui::AppState& state, const Expectations& e);

// Every requirement's row with its state, as list_requirements answers it.
nlohmann::json RequirementRows(ui::AppState& state);

// Starts a check of every requirement as a job (the panel's Check). Returns
// the job id, 0 when every requirement was already current and nothing had
// to run.
uint64_t StartRequirementsCheck(ui::AppState& state, bool recompute);

// Tests only: every clearance measure holds `ms` before it measures, on
// whichever thread it runs (0: none). Also LOBOLE_TEST_REQUIREMENT_MS.
void SetTestRequirementHold(int ms);

// Tests only: every design sweep point waits `ms` first, cancellably, so a
// test can watch a sweep run and cancel it. Also LOBOLE_TEST_SWEEP_MS.
void SetTestSweepHold(int ms);

// Every `expression` requirement whose inputs moved, measured in place
// (docs/requirements/README.md#expression); the rows whose state that
// changed. The server attaches them to the response of the call that moved
// them, as `requirements`, the way `regen` reports a broken feature.
nlohmann::json RefreshExpressionRequirements(ui::AppState& state);

// Whether `method`'s answer may carry those rows: an untyped one, or one
// whose `returns` column names `requirements` (a typed answer is closed).
// remote/RemoteDispatch.cpp, which owns the rows.
[[nodiscard]] bool MayAttachRequirements(const std::string& method);

} // namespace remote
