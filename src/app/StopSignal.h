// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

// SIGTERM and SIGINT for the modes that stop cleanly, --headless and
// --offscreen (docs/architecture.md#stopping). The handler only sets a flag,
// which the loop reads once an iteration; a main thread inside one long call
// (a boolean in an RPC, a batch's import) never reads it, and the shutdown
// after the loop joins every background job, which a worker inside an
// uninterruptible OpenCASCADE call holds. So the first signal also starts a
// clock on a thread of its own: if the process is still alive
// kStopGraceSeconds later it says what the main thread is doing, runs the
// forced-exit step (the instance's registry entry), ends the crash session
// -- a process asked to stop has not crashed -- and _exits with 128 + the
// signal. A second signal ends it the same way at once.

#include <functional>

namespace stopsignal {

// The ordinary shutdown's allowance: well over what a clean stop takes
// (milliseconds: stop the server, cancel the jobs), and under the 5 s
// tests/smoke_app.py gives a terminated app before it kills it.
inline constexpr double kStopGraceSeconds = 3.0;

// Installs the handlers and starts the watcher. Call before anything long
// (the --file import): a signal before it is the default's, and kills.
void Install();

// Whether a stop was asked for. The loop's condition.
bool Requested();

// What the forced exit does first, from the watcher's thread (keep it to
// removing files: the main thread may be anywhere). Replaces the last one.
void OnForcedExit(std::function<void()> step);

} // namespace stopsignal
