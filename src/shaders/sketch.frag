#version 450

layout(push_constant) uniform PushConstants {
    vec4 tint; // rgb unused, a = overall opacity for this draw
} pc;

layout(location = 0) in vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, pc.tint.a);
}
