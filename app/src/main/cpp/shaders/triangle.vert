#version 450

// Positions/colors are hardcoded here via gl_VertexIndex rather than fed
// through a VkBuffer — deliberate scope control for a first Vulkan
// triangle: skips VkBuffer/VkDeviceMemory allocation and staging entirely
// while still exercising the full real pipeline (instance through present).
layout(location = 0) out vec3 fragColor;

vec2 positions[3] = vec2[](
    vec2(0.0, -0.6),
    vec2(0.3, 0.6),
    vec2(-0.3, 0.6)
);

vec3 colors[3] = vec3[](
    vec3(0.20, 0.85, 0.55),
    vec3(0.20, 0.85, 0.55),
    vec3(0.20, 0.85, 0.55)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}
