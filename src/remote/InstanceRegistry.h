#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Running instances announce themselves, so a client can FIND the app instead
// of being told which port to assume.
//
// The MCP server used to default to port 8437 while this repo's .mcp.json
// hardcoded 8799 (dev-watch's port), and any other port produced "Lobole is
// not reachable" while the app was running perfectly. Nobody's app was
// missing; the client just could not find it.
//
// Each instance that opens a remote port writes
//
//   $XDG_STATE_HOME/lobole/instances/<port>.json
//   (%LOCALAPPDATA%\Lobole\state\instances\<port>.json on Windows)
//     { "app": "lobole", "pid": …, "port": …,
//       "mode": "gui"|"headless"|"offscreen", "started": <unix seconds> }
//
// and removes it when the port closes. A crash leaves the file behind, so a
// reader must treat the pid as the truth (`kill(pid, 0)`) and every writer
// sweeps dead entries on startup. `mode` is the capability split a client
// actually cares about: "gui" has a renderer and a test engine (screenshots
// and ui_action work), "offscreen" has a renderer and no GUI to drive,
// "headless" has neither.
//
// Deliberately NOT here: the open project and whether it has unsaved changes.
// Those change constantly, and a file rewritten on every edit is a worse
// source of truth than asking — `ping` reports them live.
namespace remote {

struct InstanceInfo {
  int pid = 0;
  uint16_t port = 0;
  std::string mode; // "gui" | "headless" | "offscreen"
  int64_t started = 0;
};

// Announces this process on `port`, and prunes entries whose process is gone.
// Failure is never fatal: an unwritable state dir costs discovery, not the
// remote interface, so this logs and returns false rather than refusing.
bool AnnounceInstance(uint16_t port, const std::string& mode);

// Withdraws this process's announcement for `port`. Safe to call twice.
void WithdrawInstance(uint16_t port);

// Live instances, newest first, with dead entries removed from disk as they
// are found. Used by tests; the MCP server reads the same directory itself
// (a python client cannot link this).
std::vector<InstanceInfo> LiveInstances();

// The directory the files live in (created on demand by AnnounceInstance).
std::string InstancesDir();

// This process's id, as announced and as `ping` reports it: a launcher checks
// that the instance answering on a port is the one it started, not one that
// already held the port.
int CurrentPid();

} // namespace remote
