#pragma once

// Where this build's source tree is (the goldens, the fixtures, the sources a
// convention test reads) and where the test binary itself is. Defined in
// TestPaths.cpp, the one file compiled with the paths: every other test source
// then compiles the same in every tree and shares ccache
// (docs/architecture.md#precompiled-headers).
const char* TestSourceDir();
const char* TestBinaryPath();
