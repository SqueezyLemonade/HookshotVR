// Hookshot VR: the vector and rotation maths the body and items use, and the palette's 3x4 matrices.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
constexpr float kPi = 3.14159265358979323846f, kDeg = kPi / 180.0f;

struct V3 { float x, y, z; };
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(std::max(dot(a, a), 0.0f)); }
V3 unit(V3 a, V3 fallback) { const float n = length(a); return n > 1e-6f ? mul(a, 1.0f / n) : fallback; }
V3 flat(V3 a) { return {a.x, 0, a.z}; }
V3 perp(V3 a, V3 axis) { return sub(a, mul(axis, dot(a, axis))); }   // (axis: a unit vector)
V3 v3(const float* p) { return {p[0], p[1], p[2]}; }
float smoothstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3 - 2 * t); }

struct M3 { float v[3][3]; };   // row-major
M3 identity() { return {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}}; }
M3 columns(V3 a, V3 b, V3 c) { return {{{a.x, b.x, c.x}, {a.y, b.y, c.y}, {a.z, b.z, c.z}}}; }
V3 column(const M3& m, int c) { return {m.v[0][c], m.v[1][c], m.v[2][c]}; }
V3 apply(const M3& r, V3 a) {
    return {r.v[0][0] * a.x + r.v[0][1] * a.y + r.v[0][2] * a.z, r.v[1][0] * a.x + r.v[1][1] * a.y + r.v[1][2] * a.z,
            r.v[2][0] * a.x + r.v[2][1] * a.y + r.v[2][2] * a.z};
}
M3 mul(const M3& a, const M3& b) {
    M3 r{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r.v[i][j] += a.v[i][k] * b.v[k][j];
    return r;
}
M3 transpose(const M3& a) {
    M3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.v[i][j] = a.v[j][i];
    return r;
}
M3 axis_angle(V3 axis, float angle) {
    axis = unit(axis, {0, 1, 0});
    const float x = axis.x, y = axis.y, z = axis.z, c = std::cos(angle), s = std::sin(angle), t = 1 - c;
    return {{{t * x * x + c, t * x * y - s * z, t * x * z + s * y}, {t * x * y + s * z, t * y * y + c, t * y * z - s * x},
             {t * x * z - s * y, t * y * z + s * x, t * z * z + c}}};
}
/// The shortest turn from one direction to another, scaled (amount 1: all of it)
M3 from_to(V3 from, V3 to, float amount = 1.0f) {
    from = unit(from, {1, 0, 0}), to = unit(to, from);
    const float c = std::clamp(dot(from, to), -1.0f, 1.0f);
    V3 ax = cross(from, to);
    const float s = length(ax);
    if (s > 1e-6f) return axis_angle(mul(ax, 1.0f / s), std::atan2(s, c) * amount);
    if (c > 0) return identity();
    ax = cross(from, std::fabs(from.x) < 0.7f ? V3{1, 0, 0} : V3{0, 1, 0});
    return axis_angle(ax, kPi * amount);
}
M3 local_euler(const float deg[3]) {   // pitch about x, then yaw about y, then roll about z (calibrate_wrists' decomposition)
    return mul(axis_angle({0, 0, 1}, deg[2] * kDeg), mul(axis_angle({0, 1, 0}, deg[1] * kDeg), axis_angle({1, 0, 0}, deg[0] * kDeg)));
}
float signed_angle(V3 from, V3 to, V3 axis) {
    from = unit(perp(from, axis), {0, 0, 1});
    to = unit(perp(to, axis), from);
    return std::atan2(dot(axis, cross(from, to)), std::clamp(dot(from, to), -1.0f, 1.0f));
}
/// A rotation's angle (radians) and axis
float rotation_angle(const M3& r, V3* axis) {
    const float c = std::clamp((r.v[0][0] + r.v[1][1] + r.v[2][2] - 1) * .5f, -1.0f, 1.0f);
    const float a = std::acos(c);
    *axis = unit({r.v[2][1] - r.v[1][2], r.v[0][2] - r.v[2][0], r.v[1][0] - r.v[0][1]}, {0, 1, 0});
    if (a > kPi - 1e-3f) {   // (half a turn: the axis from the diagonal)
        const V3 d{std::sqrt(std::max((r.v[0][0] + 1) * .5f, 0.0f)), std::sqrt(std::max((r.v[1][1] + 1) * .5f, 0.0f)),
                   std::sqrt(std::max((r.v[2][2] + 1) * .5f, 0.0f))};
        *axis = unit({d.x, std::copysign(d.y, r.v[0][1] + r.v[1][0]), std::copysign(d.z, r.v[0][2] + r.v[2][0])}, {0, 1, 0});
    }
    return a;
}
/// The part of a rotation about axis (swing-twist), in radians
float twist_about(const M3& r, V3 axis) {
    const float tr = r.v[0][0] + r.v[1][1] + r.v[2][2];
    float w, x, y, z;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1) * 2;
        w = .25f * s, x = (r.v[2][1] - r.v[1][2]) / s, y = (r.v[0][2] - r.v[2][0]) / s, z = (r.v[1][0] - r.v[0][1]) / s;
    } else if (r.v[0][0] > r.v[1][1] && r.v[0][0] > r.v[2][2]) {
        const float s = std::sqrt(1 + r.v[0][0] - r.v[1][1] - r.v[2][2]) * 2;
        w = (r.v[2][1] - r.v[1][2]) / s, x = .25f * s, y = (r.v[0][1] + r.v[1][0]) / s, z = (r.v[0][2] + r.v[2][0]) / s;
    } else if (r.v[1][1] > r.v[2][2]) {
        const float s = std::sqrt(1 + r.v[1][1] - r.v[0][0] - r.v[2][2]) * 2;
        w = (r.v[0][2] - r.v[2][0]) / s, x = (r.v[0][1] + r.v[1][0]) / s, y = .25f * s, z = (r.v[1][2] + r.v[2][1]) / s;
    } else {
        const float s = std::sqrt(1 + r.v[2][2] - r.v[0][0] - r.v[1][1]) * 2;
        w = (r.v[1][0] - r.v[0][1]) / s, x = (r.v[0][2] + r.v[2][0]) / s, y = (r.v[1][2] + r.v[2][1]) / s, z = .25f * s;
    }
    float a = 2 * std::atan2(x * axis.x + y * axis.y + z * axis.z, w);
    if (a > kPi) a -= 2 * kPi;
    if (a < -kPi) a += 2 * kPi;
    return a;
}

// ---- the palette's 3x4 matrices -----------------------------------------------------------------------------------------------
V3 point(const float* m, V3 p) {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}
M3 rotation(const float* m) {   // orthonormalised (Gram-Schmidt on the columns)
    V3 x{m[0], m[4], m[8]}, y{m[1], m[5], m[9]};
    x = unit(x, {1, 0, 0});
    y = unit(perp(y, x), {0, 1, 0});
    return columns(x, y, cross(x, y));
}
void set(float* m, const M3& r, V3 t, float scale = 1.0f) {
    const float tv[3] = {t.x, t.y, t.z};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = r.v[i][j] * scale;
        m[i * 4 + 3] = tv[i];
    }
}
/// Turn a matrix rigidly by r about a pivot (palette space)
void turn(float* m, const M3& r, V3 pivot) {
    float b[12];
    std::memcpy(b, m, sizeof b);
    const V3 t = add(apply(r, sub({b[3], b[7], b[11]}, pivot)), pivot);
    const float tv[3] = {t.x, t.y, t.z};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = r.v[i][0] * b[j] + r.v[i][1] * b[4 + j] + r.v[i][2] * b[8 + j];
        m[i * 4 + 3] = tv[i];
    }
}
/// A scale of k along one direction (unit d), 1 across it
M3 stretch_along(V3 d, float k) {
    M3 r = identity();
    const float dv[3] = {d.x, d.y, d.z};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.v[i][j] += (k - 1) * dv[i] * dv[j];
    return r;
}
template <size_t N> void move_set(float* p, const int16_t (&set)[N], V3 by) {
    for (int16_t s : set) {
        if (s < 0) break;
        float* m = p + s * 12;
        m[3] += by.x, m[7] += by.y, m[11] += by.z;
    }
}
template <size_t N> void turn_set(float* p, const int16_t (&set)[N], const M3& r, V3 pivot, int skip = -1) {
    for (int16_t s : set) {
        if (s < 0) break;
        if (s != skip) turn(p + s * 12, r, pivot);
    }
}

// ---- general 3x4 transforms (palette matrices carry scale: never orthonormalised here) ---------------------------------------
struct Aff { M3 r; V3 t; };
Aff aff(const float* m) { return {{{{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}}}, {m[3], m[7], m[11]}}; }
void store(float* m, const Aff& a) {
    const float tv[3] = {a.t.x, a.t.y, a.t.z};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) m[i * 4 + j] = a.r.v[i][j];
        m[i * 4 + 3] = tv[i];
    }
}
Aff mul(const Aff& a, const Aff& b) { return {mul(a.r, b.r), add(apply(a.r, b.t), a.t)}; }
Aff shifted(Aff a, V3 by) { a.t = add(a.t, by); return a; }
float det(const M3& r) {
    return dot(column(r, 0), cross(column(r, 1), column(r, 2)));
}
bool invert(const Aff& a, Aff* out) {
    const V3 c0 = column(a.r, 0), c1 = column(a.r, 1), c2 = column(a.r, 2);
    const float d = dot(c0, cross(c1, c2));
    if (!std::isfinite(d) || std::fabs(d) < 1e-9f) return false;
    const V3 r0 = mul(cross(c1, c2), 1 / d), r1 = mul(cross(c2, c0), 1 / d), r2 = mul(cross(c0, c1), 1 / d);
    out->r = {{{r0.x, r0.y, r0.z}, {r1.x, r1.y, r1.z}, {r2.x, r2.y, r2.z}}};
    out->t = mul(apply(out->r, a.t), -1.0f);
    return true;
}
