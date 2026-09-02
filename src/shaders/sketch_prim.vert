#version 450

// Screen-space-expanded sketch primitives: thick anti-aliased lines and
// constant-size point dots. Each vertex sits on `posThis`; we expand it in
// pixel space so width/size stay constant regardless of zoom.
layout(binding = 0) uniform UniformBufferObject {
    mat4 view;
    mat4 proj;
    vec4 viewport; // xy = viewport size in pixels
} ubo;

layout(location = 0) in vec3 inPosThis;
layout(location = 1) in vec3 inPosOther;
layout(location = 2) in vec2 inCorner; // line: (side, 0); dot: (±1, ±1)
layout(location = 3) in vec2 inParams; // x = half px, y = shape (0 line, 1 dot)
layout(location = 4) in vec4 inColor;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 vLocal;  // cross-section coord in [-1, 1]
layout(location = 2) out float vHalfPx;
layout(location = 3) out float vShape;

void main() {
    vec2 vp = ubo.viewport.xy;
    vec4 clipA = ubo.proj * ubo.view * vec4(inPosThis, 1.0);
    vec4 clipB = ubo.proj * ubo.view * vec4(inPosOther, 1.0);
    vec2 screenA = (clipA.xy / clipA.w * 0.5 + 0.5) * vp;
    vec2 screenB = (clipB.xy / clipB.w * 0.5 + 0.5) * vp;

    float halfPx = inParams.x;
    vec2 offsetPx;
    if (inParams.y < 0.5) {
        // Line: offset along the screen-space normal of the segment. The
        // normal is CANONICAL — the same for a vertex at either end — so
        // corner.x = +1 is the same side of the line at A and at B, and the
        // fragment's cross-section coordinate is consistent across the quad.
        // Before this, the far end's normal was the near end's negated
        // (its posThis→posOther runs the other way); the vertex data undid
        // that for the position but vLocal kept the raw sign, so the
        // anti-aliased core of every stroke was a twisted hourglass — a
        // pinched rope at 4 px, invisible at the 3 px sketch lines wear.
        vec2 dir = screenB - screenA;
        float len = length(dir);
        dir = len > 1e-4 ? dir / len : vec2(1.0, 0.0);
        if (dir.x < 0.0 || (dir.x == 0.0 && dir.y < 0.0))
            dir = -dir;
        vec2 nrm = vec2(-dir.y, dir.x);
        offsetPx = nrm * (inCorner.x * halfPx);
        vLocal = vec2(inCorner.x, 0.0);
    } else {
        // Dot: offset to a square corner.
        offsetPx = inCorner * halfPx;
        vLocal = inCorner;
    }

    vec2 screenPos = screenA + offsetPx;
    vec2 ndcPos = screenPos / vp * 2.0 - 1.0;
    // Keep the anchor's depth/w so perspective and z-order are unchanged.
    gl_Position = vec4(ndcPos * clipA.w, clipA.z, clipA.w);

    fragColor = inColor;
    vHalfPx = halfPx;
    vShape = inParams.y;
}
