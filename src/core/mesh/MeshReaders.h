// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// Mesh files read straight into a TriMesh, with no B-Rep built
// (docs/interchange/README.md#recognising-a-faceted-mesh).

#include "core/mesh/TriMesh.h"

#include <functional>
#include <string>

namespace mesh {

// Polled while reading; true stops the read, which then fails with
// "cancelled". May be empty.
using Cancelled = std::function<bool()>;

// A binary or ASCII STL: binary when the file's size is exactly what its
// header's triangle count says, ASCII when it starts with `solid` and is not,
// refused otherwise. The triangles are welded (FromSoup) at the precision
// the file was written at: float32 for binary, the digits printed for ASCII.
// False with `error` naming the file and what is wrong.
bool ReadStl(const std::string& path, TriMesh& out, std::string& error,
             const Cancelled& cancelled = {});

// The same from bytes already in memory, `name` used in the error.
bool ReadStlBytes(const std::string& bytes, const std::string& name,
                  TriMesh& out, std::string& error,
                  const Cancelled& cancelled = {});

} // namespace mesh
