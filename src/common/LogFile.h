// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <filesystem>
#include <string>
#include <vector>

// The log's second sink: this session's file under the state dir
// (`<state>/logs/<started>-<pid>.log`,
// docs/architecture.md#crashes-and-stalls). Same pattern and
// flush-on-every-message as stdout, so the file is complete up to the instant
// of a crash; stdout is unchanged.
//
// Its own header rather than a Logger.h addition: Logger.h is in nearly every
// translation unit, and only start-up needs this.
namespace logfile {

// Opens `path` (creating its folder) and starts copying every message there.
// False with `error` when it cannot be opened; the log carries on on stdout.
// A second call replaces the first file.
bool Open(const std::filesystem::path& path, std::string* error = nullptr);

// The file being written, or empty.
std::filesystem::path Current();

// Deletes all but the `keep` newest `*.log` in `dir` (by name, which starts
// with the session's start time), never one named in `keepAlways`.
void Prune(const std::filesystem::path& dir, size_t keep,
           const std::vector<std::filesystem::path>& keepAlways);

} // namespace logfile
