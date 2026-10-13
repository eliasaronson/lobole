// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "remote/RpcErrors.h"

#include "remote/InspectionContracts.h"

#include <Standard_Failure.hxx>
#include <Standard_Type.hxx>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <typeinfo>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

namespace remote {
namespace {

std::string Demangle(const std::type_info& type) {
#if defined(__GNUG__)
  int status = 0;
  std::unique_ptr<char, void (*)(void*)> name(
      abi::__cxa_demangle(type.name(), nullptr, nullptr, &status), std::free);
  if (status == 0 && name)
    return name.get();
#endif
  return type.name();
}

void Walk(const nlohmann::json& j, std::string& path,
          std::vector<std::string>& out) {
  if (j.is_number_float()) {
    if (!std::isfinite(j.get<double>()))
      out.push_back(path); // "" is the whole document (RFC 6901)
    return;
  }
  if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++) {
      const size_t mark = path.size();
      path += "/" + std::to_string(i);
      Walk(j[i], path, out);
      path.resize(mark);
    }
    return;
  }
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      const size_t mark = path.size();
      path += '/';
      // RFC 6901: '~' is "~0" and '/' is "~1".
      for (char c : it.key()) {
        if (c == '~')
          path += "~0";
        else if (c == '/')
          path += "~1";
        else
          path += c;
      }
      Walk(it.value(), path, out);
      path.resize(mark);
    }
  }
}

} // namespace

std::string InternalErrorType(const std::exception& e) {
  if (dynamic_cast<const std::runtime_error*>(&e) != nullptr)
    return {};
  if (dynamic_cast<const nlohmann::json::type_error*>(&e) != nullptr ||
      dynamic_cast<const nlohmann::json::parse_error*>(&e) != nullptr)
    return {};
  return Demangle(typeid(e));
}

nlohmann::json InternalDiagnostic(const std::string& operation,
                                  const std::string& type) {
  return {{"operation", operation},
          {"code", "internal"},
          {"path", "/params"},
          {"expected", "a handler that checks its input before using it"},
          {"observed", type}};
}

CaughtError DescribeCurrentException(const std::string& operation) {
  try {
    throw;
  } catch (const ContractError& e) {
    nlohmann::json diagnostic = e.diagnostic;
    if (diagnostic.value("operation", "").empty())
      diagnostic["operation"] = operation;
    return {e.what(), diagnostic};
  } catch (const std::exception& e) {
    const std::string type = InternalErrorType(e);
    return {e.what(), type.empty() ? nlohmann::json()
                                   : InternalDiagnostic(operation, type)};
  } catch (const Standard_Failure& e) {
    const char* said = e.GetMessageString();
    return {std::string("OpenCASCADE error: ") +
                (said && *said ? said : "unspecified failure"),
            InternalDiagnostic(operation, e.DynamicType()->Name())};
  } catch (...) {
    return {"an exception of unknown type escaped the handler",
            InternalDiagnostic(operation, "unknown")};
  }
}

namespace {
bool FindNul(const nlohmann::json& j, std::string& path) {
  if (j.is_string())
    return j.get_ref<const std::string&>().find('\0') != std::string::npos;
  if (j.is_array()) {
    for (size_t i = 0; i < j.size(); i++) {
      const size_t mark = path.size();
      path += "/" + std::to_string(i);
      if (FindNul(j[i], path))
        return true;
      path.resize(mark);
    }
  } else if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      const size_t mark = path.size();
      path += "/" + it.key();
      if (it.key().find('\0') != std::string::npos || FindNul(*it, path))
        return true;
      path.resize(mark);
    }
  }
  return false;
}
} // namespace

std::optional<std::string> FirstNulString(const nlohmann::json& j) {
  std::string path;
  if (FindNul(j, path))
    return path;
  return std::nullopt;
}

std::vector<std::string> NonFinitePointers(const nlohmann::json& j) {
  std::vector<std::string> out;
  std::string path;
  Walk(j, path, out);
  return out;
}

} // namespace remote
