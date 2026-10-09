# SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
# SPDX-License-Identifier: AGPL-3.0-or-later

# Embeds a compiled SPIR-V binary as a C++ byte array header, so fem_gpu and
# its tests need no runtime shader path (the renderer .spv files are loaded
# relative to the app executable, which test binaries don't share).
#
# Usage: cmake -DINPUT=<file.spv> -DOUTPUT=<file.h> -DNAME=<identifier> -P EmbedSpirv.cmake
#
# Emits: alignas(4) inline constexpr unsigned char k_<NAME>_spv[] = {...};
# The 4-byte alignment lets vkCreateShaderModule consume it via a uint32_t
# reinterpret without copying (SPIR-V is a little-endian 32-bit word stream;
# byte order in the file is already what the host expects on x86).

file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
file(WRITE "${OUTPUT}" "// Generated from ${INPUT} by EmbedSpirv.cmake — do not edit.
#pragma once
#include <cstddef>
alignas(4) inline constexpr unsigned char k_${NAME}_spv[] = {${bytes}};
inline constexpr std::size_t k_${NAME}_spv_size = sizeof(k_${NAME}_spv);
")
