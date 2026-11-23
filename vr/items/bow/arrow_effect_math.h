// Hookshot VR: a held arrow's effects drawn on the VR arrow: the maths on a private copy of their particle camera (vertex
// UBO 10, "C13", 0x210 bytes, as the fire arrow flame shaders read it: 0x00 the camera's place, 0x10 its forward, 0x20
// near/far, 0x40 view and 0x80 projection, column-major), never game memory. Fields are changed only when they agree with
// inverse(view). Used by vr/body/palette_draw.inc and skin_draw.inc; tested by vr/tools/test.py arrow_effect_camera.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstring>

namespace mkwvk::vr::arrow_fx {
constexpr size_t kView = 0x40 / 4, kProjection = 0x80 / 4, kInverseView = 0x1c0 / 4;
constexpr float kSourceReach = .8f;   // metres from the native held arrow; still a proximity heuristic
constexpr float kCameraMargin = .04f; // metres: a camera placement is not an emitter placement
struct Camera {
    float eye[3]{};
    bool position_at_zero = false, inverse_at_1c0 = false;
};

inline bool close(float a, float b, float tolerance = 1e-3f) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= tolerance;
}
inline bool affine(const float* m) {
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    return close(m[3], 0) && close(m[7], 0) && close(m[11], 0) && close(m[15], 1);
}
inline void multiply(const float* a, const float* b, float* out) {
    float c[16]{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k) c[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    std::memcpy(out, c, sizeof c);
}
// General affine inverse: the VR bow may be scaled. Transposing its rotation is not an inverse then.
inline bool inverse(const float* m, float* out) {
    if (!affine(m)) return false;
    const double a = m[0], b = m[4], c = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
    const double det = a * (e*i-f*h) - b * (d*i-f*g) + c * (d*h-e*g);
    if (!std::isfinite(det) || std::fabs(det) < 1e-9) return false;
    float r[16] = {
        float((e*i-f*h)/det), float((f*g-d*i)/det), float((d*h-e*g)/det), 0,
        float((c*h-b*i)/det), float((a*i-c*g)/det), float((b*g-a*h)/det), 0,
        float((b*f-c*e)/det), float((c*d-a*f)/det), float((a*e-b*d)/det), 0,
        0, 0, 0, 1};
    for (int row = 0; row < 3; ++row)
        r[12 + row] = -(r[row] * m[12] + r[4 + row] * m[13] + r[8 + row] * m[14]);
    for (float v : r) if (!std::isfinite(v)) return false;
    std::memcpy(out, r, sizeof r);
    return true;
}
inline bool inspect_camera(const float* data, size_t bytes, Camera& out) {
    out = {};
    if (bytes < 0xc0) return false;
    const float* p = data + kProjection;
    for (int i = 0; i < 16; ++i) if (!std::isfinite(p[i])) return false;
    // Perspective projection's last ROW, in column-major storage. 0xb0 is NOT a camera position here.
    if (!close(p[3], 0) || !close(p[7], 0) || !close(std::fabs(p[11]), 1) || !close(p[15], 0) ||
        std::fabs(p[0]) < 1e-5f || std::fabs(p[5]) < 1e-5f) return false;
    float iv[16];
    if (!inverse(data + kView, iv)) return false;
    out.position_at_zero = true;
    for (int i = 0; i < 3; ++i) {
        out.eye[i] = iv[12 + i];
        out.position_at_zero &= close(data[i], out.eye[i], .02f);
    }
    out.inverse_at_1c0 = bytes >= 0x200;
    if (out.inverse_at_1c0)
        for (int i = 0; i < 16; ++i) out.inverse_at_1c0 &= close(data[kInverseView + i], iv[i], .02f);
    return true;
}
inline bool source_matches(const float* effect, const float game[3], const float eye[3]) {
    float inv[16];
    if (!inverse(effect, inv)) return false; // rejects empty/stale/non-affine placements, not scale 1
    float arrow_distance = 0, camera_distance = 0;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(game[i]) || !std::isfinite(eye[i])) return false;
        const float a = effect[12 + i] - game[i], c = effect[12 + i] - eye[i];
        arrow_distance += a*a; camera_distance += c*c;
    }
    return arrow_distance <= kSourceReach*kSourceReach && camera_distance > kCameraMargin*kCameraMargin;
}
// The fire arrow's flame fades itself out near the camera: its vertex stage (programs 8eb603fa8c2d8fe6 / 09c11fa5575d1b3c,
// read from the program cache, 17 Nov) multiplies its alpha by clamp(0.4 * d - 1, 0, 1), d the particle's depth in front of
// the camera (the view's third row, words 18/22/26/30): gone nearer than 2.5 m, whole from 5 m. On the VR string the arrow is
// half a metre from the eyes. So a moved arrow effect's camera gets its view depth stretched by kNearFadeStretch and its
// projection's depth column shrunk by the same: the clip position (screen, depth buffer, both eyes) is exactly what it was,
// and only what reads the view depth itself (that fade) sees the effect that much further away.
constexpr float kNearFadeStretch = 10.0f;
// The stretch that makes that fade come out at an opacity (0..1) for an effect depth metres in front of the camera: the
// VR tab's Elemental arrow effects. Whole (1 or more): kNearFadeStretch, at any depth the VR string puts the arrow.
inline float fade_stretch(float depth, float opacity) {
    if (!(opacity < 1.0f) || !(depth > 1e-3f)) return kNearFadeStretch;
    return (1.0f + std::fmax(opacity, 0.0f)) / (0.4f * depth);
}
inline void stretch_view_depth(float* data, size_t bytes, float k) {
    if (bytes < 0xc0 || !(k > 0)) return;   // (only a camera whose projection is there to take it back)
    for (int col = 0; col < 4; ++col) data[kView + col * 4 + 2] *= k;           // (the view's third row)
    for (int row = 0; row < 4; ++row) data[kProjection + 2 * 4 + row] /= k;     // (the projection's third column)
}
// Make the draw see D*p without writing particle storage buffers: V' = V*D, P unchanged.
// Camera position and inverse-view/billboard axes use D^-1, including scale. Unknown fields stay untouched.
inline bool transform_camera(float* data, size_t bytes, const float move[12], const Camera& info) {
    float d[16]{}, inv[16]; d[15] = 1;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 4; ++col) d[col*4 + row] = move[row*4 + col];
    if (!inverse(d, inv)) return false;
    bool changed = false;
    if (bytes >= 0x80) {
        multiply(data + kView, d, data + kView);
        changed = true;
    }
    if (bytes >= 12 && info.position_at_zero) {
        const float eye[3] = {data[0], data[1], data[2]};
        for (int row = 0; row < 3; ++row)
            data[row] = inv[row]*eye[0] + inv[4+row]*eye[1] + inv[8+row]*eye[2] + inv[12+row];
        changed = true;
        // (the camera's forward at 0x10 (seen in the headset log, 17 Nov: a unit vector, the view's look direction): turned
        // by D^-1 too. Left as it was, the particles were judged against the old direction: the game's arrow is behind the
        // first-person eye (0.19 m behind it along that forward), and the moved flame was still rejected)
        if (bytes >= 0x1c) {
            const float f[3] = {data[4], data[5], data[6]};
            float g[3], n = 0;
            for (int row = 0; row < 3; ++row) g[row] = inv[row]*f[0] + inv[4+row]*f[1] + inv[8+row]*f[2], n += g[row]*g[row];
            n = std::sqrt(n);
            if (n > 1e-6f) for (int row = 0; row < 3; ++row) data[4 + row] = g[row] / n;
        }
    }
    if (bytes >= 0x200 && info.inverse_at_1c0) {
        multiply(inv, data + kInverseView, data + kInverseView);
        changed = true;
    }
    return changed;
}
} // namespace mkwvk::vr::arrow_fx
