// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// What a refusal was caused by, and what an answer cannot say in JSON
// (docs/scripting/rpc.md#structured-errors, #what-a-response-carries).
//
// A deliberate refusal throws std::runtime_error (or a subclass:
// remote::ContractError, filesystem_error). Anything else that reaches the
// dispatcher's catch ladder is the handler falling over -- indexing past what
// it checked (std::out_of_range, nlohmann's out_of_range), an empty optional,
// a bad cast, an allocation, a kernel throw nobody guarded -- and it is
// labelled `internal`, so a caller (and the hostile sweep,
// tests/hostile_params_smoke.py) can tell a handler that checked its input
// from one that did not. The sentence stays what the exception said, and the
// call is rolled back like any other refusal.

#include <nlohmann/json.hpp>

#include <exception>
#include <optional>
#include <string>
#include <vector>

namespace remote {

// The exception's type name (demangled) when it means the handler fell over;
// empty for a deliberate refusal: std::runtime_error and its subclasses, and
// nlohmann's type_error (the designed wrong-type path) and parse_error.
std::string InternalErrorType(const std::exception& e);

// The structured diagnostic of an internal error raised in `operation`.
nlohmann::json InternalDiagnostic(const std::string& operation,
                                  const std::string& type);

// The exception in flight, for a `catch (...)` block: its sentence, and its
// diagnostic (a ContractError's own, an internal error's, or null). A kernel
// Standard_Failure reads "OpenCASCADE error: ..." and is internal; so is an
// exception of no known type.
struct CaughtError {
  std::string message;
  nlohmann::json diagnostic; // null when the refusal carries none
};
CaughtError DescribeCurrentException(const std::string& operation);

// The JSON pointers of every non-finite number in `j`. nlohmann writes NaN and
// infinity as `null`, so an answer that held one would otherwise reach the
// caller as a missing value; RemoteServer::Send lists them under `nonfinite`.
std::vector<std::string> NonFinitePointers(const nlohmann::json& j);

// The JSON pointer of the first string in `j` holding a NUL byte, if any.
// The dispatcher refuses one before any handler runs: every C API below a
// handler reads a string up to its first NUL, so "nul\0byte" as a path
// created the directory "nul" and then failed (hostile_params_smoke).
std::optional<std::string> FirstNulString(const nlohmann::json& j);

} // namespace remote
