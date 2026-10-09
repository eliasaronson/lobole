# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Sourced, not run.
#
# The runtime OPTIONS now live in the binary (src/common/SanitizerDefaults.cpp,
# compiled in when ENABLE_SANITIZERS=ON), so a sanitized build behaves the same
# however it is launched — including when you run build-asan/lobole straight
# from a shell, which is how the real findings have actually turned up. This
# file keeps only what a binary cannot carry: which tests to skip.
#
# What the binary sets, for reference:
#   detect_container_overflow=0  our std containers cross into
#     non-instrumented OpenCASCADE, which false-positives as
#     container-overflow. Nothing else is off — leak detection stays ON.
#   UBSan suppressions           tools/ubsan-suppressions.txt (OCC alignment,
#     the ImGui present-mode sentinel).
#   LSan suppressions            dependency-owned allocations, named frame by
#     frame in SanitizerDefaults.cpp. Never a whole library.
#
# Setting any of those env vars still overrides the built-in default.
#
# Usage: source "$(dirname "$0")/sanitizer-env.sh"

# The tests that are excluded from a sanitized RUN, and why: the SIMP
# topology gates and the FEM/GPU numeric tests are minutes-long under ASan
# (the SIMP gate alone ran past 20 minutes) and exercise arithmetic, not
# memory. Everything else — including the app smokes, which launch the
# instrumented binary — is in.
SANITIZER_TEST_EXCLUDE='Topo:|femgpu|topo_smoke'
