# Sourced, not run: the ASan/UBSan runtime options this project needs, in one
# place so the direct runner (tools/run-sanitized-tests.sh), the local CI
# script and the GitHub job cannot disagree about what is being suppressed.
#
#   detect_container_overflow=0  our std containers cross into
#     non-instrumented OpenCASCADE, which false-positives as
#     container-overflow. Nothing else is turned off — leak detection stays
#     ON (it is worth having; see the LSan line below for the one exception).
#   UBSan suppressions           the benign OCC alignment reports; every
#     other check still fails the run. See tools/ubsan-suppressions.txt.
#   LSan suppressions            OCC singletons allocated once and never
#     freed (XCAFApp_Application). Named frames only, so a leak of ours in
#     the same library still fails. See tools/lsan-suppressions.txt.
#
# Usage: source "$(dirname "$0")/sanitizer-env.sh"
_san_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export ASAN_OPTIONS="detect_container_overflow=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}"
export LSAN_OPTIONS="suppressions=$_san_root/tools/lsan-suppressions.txt${LSAN_OPTIONS:+:$LSAN_OPTIONS}"
export UBSAN_OPTIONS="suppressions=$_san_root/tools/ubsan-suppressions.txt:print_stacktrace=1${UBSAN_OPTIONS:+:$UBSAN_OPTIONS}"

# The tests that are excluded from a sanitized RUN, and why: the SIMP
# topology gates and the FEM/GPU numeric tests are minutes-long under ASan
# (the SIMP gate alone ran past 20 minutes) and exercise arithmetic, not
# memory. Everything else — including the app smokes, which launch the
# instrumented binary — is in.
SANITIZER_TEST_EXCLUDE='Topo:|femgpu|topo_smoke'
