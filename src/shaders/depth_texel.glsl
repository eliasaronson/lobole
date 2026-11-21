// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

float ReadCenteredDepth(sampler2D depth, vec2 uv) {
#ifdef LOBOLE_STABLE_DEPTH_TEXEL
    return texture(depth, uv + 0.25 / vec2(textureSize(depth, 0))).r;
#else
    return texture(depth, uv).r;
#endif
}
