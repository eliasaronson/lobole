// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "cad/OccPatches.h"

#include <cstring>

// Defined for this file alone (CMakeLists.txt), so a kernel with another
// list rebuilds one file, not the tree.
#ifndef LOBOLE_OCC_PATCHES
#define LOBOLE_OCC_PATCHES ""
#endif

namespace occpatch {

bool Carries(const char* name) {
  return name != nullptr && *name != '\0' &&
         std::strstr(LOBOLE_OCC_PATCHES, name) != nullptr;
}

std::string List() { return LOBOLE_OCC_PATCHES; }

} // namespace occpatch
