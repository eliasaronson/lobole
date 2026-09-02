#include "ui/MachineActions.h"

#include "common/Logger.h"
#include "core/io/Serialization.h"
#include "core/io/Settings.h"

#include <filesystem>

namespace ui {

std::string MachineDir() {
  return (std::filesystem::path(settings::ConfigDir()) / "machines").string();
}

void ReloadMachines(AppState& state) {
  std::string problems;
  state.machines = DocumentIO::LoadMachines(MachineDir(), &problems);
  if (!problems.empty())
    LOG_WARN("machines: {}", problems);
}

bool SaveMachine(AppState& state, const cam::Machine& machine,
                 std::string* error) {
  if (!DocumentIO::SaveMachine(MachineDir(), machine, error))
    return false;
  ReloadMachines(state);
  return true;
}

bool DeleteMachine(AppState& state, const std::string& name,
                   std::string* error) {
  if (!DocumentIO::DeleteMachine(MachineDir(), name, error))
    return false;
  ReloadMachines(state);
  return true;
}

void ApplyMachine(cam::Setup& setup, const cam::Machine& machine) {
  setup.machine = machine;
  // Seeded, not bound: a capped rapid is a per-job choice on the controllers
  // that need one, so the job keeps its own number after this.
  setup.rapidFeed = machine.rapidFeed;
  // A rotary index on a machine without the axis is an A word nobody can
  // run. Zero it rather than carry it silently into the post.
  if (!machine.hasRotaryAxis)
    setup.rotationA = 0.0;
}

} // namespace ui
