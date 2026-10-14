// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace remote {

// The version of the RPC surface, which `describe` publishes as
// `api_version` (docs/scripting/rpc.md#the-api-version). It is raised when a
// public method or one of its parameters is REMOVED or RENAMED — the changes
// that break a caller. Adding one does not raise it; the new row says when it
// arrived with `MethodDoc::since`. tests/api_surface_gate.py holds the
// published surface (api/rpc-surface.json) and fails a removal made without
// raising this. The Python package is generated against one version
// (lobole.rpc.API_VERSION) and warns when the app reports another.
//
// Independent of the document format version: a file and a protocol change
// for different reasons.
inline constexpr int kApiVersion = 1;

} // namespace remote
