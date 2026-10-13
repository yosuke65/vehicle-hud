// Dial geometry shared by the GLES and Vulkan paths. Every vertex comes from
// its index alone, so neither path needs a vertex buffer.
//   vertices  0-41: 7 tick marks, 6 vertices (two triangles) each
//   vertices 42-44: the needle
//   vertices 45-50: the hub
// Positions are in dial units (radius 1); the caller scales and places them.

const int kTicks = 7;
const int kVertexCount = kTicks * 6 + 3 + 6;
const float kStartAngle = 3.92699;  // 225 degrees: lower left
const float kSweep = 4.71239;       // 270 degrees, clockwise
const vec3 kTickColor = vec3(0.85, 0.88, 0.92);
const vec3 kNeedleColor = vec3(1.0, 0.62, 0.18);

vec2 quadCorner(int i) {
    // Two triangles covering (0,0)-(1,1).
    if (i == 0 || i == 3) return vec2(0.0, 0.0);
    if (i == 1) return vec2(1.0, 0.0);
    if (i == 2 || i == 4) return vec2(1.0, 1.0);
    return vec2(0.0, 1.0);
}

vec2 dialPoint(int id, float value, out vec3 color) {
    if (id < kTicks * 6) {
        vec2 q = quadCorner(id - (id / 6) * 6);
        float a = kStartAngle - kSweep * float(id / 6) / float(kTicks - 1);
        vec2 dir = vec2(cos(a), sin(a));
        vec2 side = vec2(-dir.y, dir.x);
        color = kTickColor;
        return dir * mix(0.78, 0.95, q.y) + side * mix(-0.025, 0.025, q.x);
    }
    if (id < kTicks * 6 + 3) {
        int v = id - kTicks * 6;
        float a = kStartAngle - kSweep * clamp(value, 0.0, 1.0);
        vec2 dir = vec2(cos(a), sin(a));
        vec2 side = vec2(-dir.y, dir.x);
        color = kNeedleColor;
        if (v == 0) return side * 0.045;
        if (v == 1) return -side * 0.045;
        return dir * 0.88;
    }
    color = kTickColor;
    return (quadCorner(id - kTicks * 6 - 3) - 0.5) * 0.12;
}
