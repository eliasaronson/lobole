// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450

// Depth-only: no color output. Exists solely to apply the section-clip
// discard (mesh.frag's fragClipDist convention) so cut-away material does
// not cast shadows.
layout(location = 0) in vec4 fragClipDist;

void main() {
    if (any(greaterThan(fragClipDist, vec4(0.0)))) {
        discard;
    }
}
