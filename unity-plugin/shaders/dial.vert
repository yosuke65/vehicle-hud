#version 450
#extension GL_GOOGLE_include_directive : require

layout(push_constant) uniform Dial {
    vec2 center;   // NDC, y up
    float radius;  // NDC units, vertical
    float aspect;  // width / height
    float value;   // 0..1
    float yFlip;   // -1 on Vulkan, whose NDC y points down
} dial;

layout(location = 0) out vec3 vColor;

#include "dial_body.glsl"

void main() {
    vec3 color;
    vec2 p = dialPoint(gl_VertexIndex, dial.value, color);
    vColor = color;
    gl_Position = vec4(dial.center.x + p.x * dial.radius / dial.aspect,
                       (dial.center.y + p.y * dial.radius) * dial.yFlip, 0.0, 1.0);
}
