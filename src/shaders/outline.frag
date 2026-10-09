// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#version 450 core

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec4 viewPos;
    vec4 outlineColor;
    float outlineWidth;
} ubo;

layout(location = 0) out vec4 fragColor;

void main() {
    fragColor = ubo.outlineColor;
}