#pragma once

#include "core/cam/CamJob.h" // Tool

#include <glm/glm.hpp>

#include <string>
#include <vector>

// The MACHINE, as one physical thing (docs/cam/job-document.md, round two).
//
// Post dialect, arc fitting and the rotary axis lived on `cam::Setup` because
// there was nowhere else, and every new job re-answered "which machine" with
// a combo defaulting to mach3. They are claims about the machine in the shop,
// not about the job -- setup-frame.md said so when it split `frame` from
// `rotationA` -- so they live here, and the setup EMBEDS a copy of the machine
// it was set up on, named by `name`.
//
// Saved globally, one file per machine beside settings.json (Serialization.h
// LoadMachines/SaveMachine), the way every reference surveyed does it:
// FreeCAD's `.fcm` files, Kiri:Moto's device list, KiCad's library table. The
// job file carries the copy too (PrusaSlicer/Bambu embed theirs), so it opens
// on a machine that has never seen the profile -- and the panel SAYS which
// copy it is using when the two differ, which none of them do.
namespace cam {

// `struct Machine` itself is declared in CamJob.h, after Tool and before the
// Setup that embeds it; this header holds what is asked ABOUT machines.

// How a setup's embedded machine stands against the global store, by name.
enum class MachineMatch {
  Unnamed, // the job's own: no name, nothing to compare against
  Global,  // named, and equal to the global of that name
  Differs, // named, and the global of that name says something else
  Missing, // named, and no global of that name exists here
};

[[nodiscard]] const Machine* FindMachine(const std::vector<Machine>& globals,
                                         const std::string& name);
[[nodiscard]] MachineMatch MatchMachine(const std::vector<Machine>& globals,
                                        const Machine& embedded);

// The file a machine of this name is stored in, under the machines directory:
// the name lower-cased with every run of non-alphanumerics folded to one '-'.
[[nodiscard]] std::string MachineFileName(const std::string& name);

} // namespace cam
