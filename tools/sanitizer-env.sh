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
# instrumented binary — is in, except what carries the `asan_slow` label.
SANITIZER_TEST_EXCLUDE='Topo:|femgpu|topo_smoke'
# Labels a sanitized run leaves out (ctest -LE). `asan_slow` goes on a test,
# where it is written ([asan_slow] in Catch, LABELS in tests/CMakeLists.txt),
# that ASan makes many times longer while quicker tests already put the same
# code under it: the CAM clearing cases that ran 11 and 20 minutes, and the
# app smokes that ran past their timeouts.
SANITIZER_LABEL_EXCLUDE='gui|asan_slow'
# What one sanitized test process costs, in MB, for the suite's -j
# (tools/build-jobs "$SANITIZER_TEST_MB", as compiles take theirs): memory,
# not cores, is what runs out. At -j$(nproc) (22) the suite peaked at
# 26.6 GB on a 31 GB machine. Measured by sampling every process's RSS
# through a whole sanitized run at -j8: 20.2 GB at once at the peak, which
# is 2.5 GB a job; one designer_tests process is 1.4 GB at the median, 2.2 GB
# at the 99th percentile and 2.9 GB at most, a headless lobole smoke up to
# 2.0 GB, and LeakSanitizer's addr2line, symbolizing lobole at a smoke's exit
# to match its suppressions, 6.6 GB once. 2500 is the peak's share.
SANITIZER_TEST_MB=2500
