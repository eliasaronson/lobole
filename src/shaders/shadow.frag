#version 450

// Depth-only: no color output. Exists solely to apply the section-clip
// discard (mesh.frag's fragClipDist convention) so cut-away material does
// not cast shadows.
layout(location = 0) in float fragClipDist;

void main() {
    if (fragClipDist > 0.0) {
        discard;
    }
}
