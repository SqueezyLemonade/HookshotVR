// Hookshot VR: the held shield's grip, moved across its back for the left VR hand.
//
// A piece of vr/items/gear/held_gear.inc (so of vr/body/vk_vr_body.cpp), included there before anchored_skin. Not compiled
// on its own.
// The shields are made for Link's right forearm: their grip sits at one edge of the plate's back, the plate and strap
// reach from there toward the elbow. In the left hand, the placement mirrored, the plate reached forward past the fist
// (the handle on the wrong side); the model mirrored showed inside out (headset 17 Nov). Now, in the left hand, only the
// grip moves across the back, to its mirror image across the plate's middle (a translation: every triangle keeps its
// winding), and the placement moves back by the opposite amount, so the grip stays in the fingers and the plate and strap
// lie over the forearm (Astra's design, 22 Nov). The
// grip goes up to 1.5 cm further out, toward the right arm's room between the handle and the strap (the Deku Shield's;
// headset 22 Nov), and the plate's back has its texture mirrored so the mount holes painted on it lie under the moved
// mounts (they showed where the mounts had been). Which vertices, how far: read from your game's models at build time
// (vk_vr_shield_grip.h, vr/tools/generate/shield_grip.py).
// One decision for the geometry and the placement: the left hand's placement moves back only for a shield whose grip
// shape the renderer has recognised (its buffer's bytes match the fingerprint taken of your game's file: shield_grip_edit),
// and the palette it makes then is listed here; the replay binds the moved grip's buffer for a draw only when that draw's
// palette is listed (vr/items/gear/shield_grip_draw.inc). The equipment cache hands one palette to the frame's colour,
// depth and shadow draws alike, so all of them agree. A shield not recognised is held as before, geometry and placement.
const rig::ShieldGrip* shield_grip_of(uint64_t vb0) {   // (a shape with a grip of its own)
    for (const rig::ShieldGrip& g : rig::shield_grips)
        if (g.vb0 && g.vb0 == vb0) return &g;
    return nullptr;
}
const rig::ShieldGrip* shield_grip_model(uint64_t vb0) {   // (the grip shape a shape is placed with: its own, or its model's)
    if (const rig::ShieldGrip* g = shield_grip_of(vb0)) return g;
    for (const rig::ShieldPart& p : rig::shield_parts)
        if (p.vb0 && p.vb0 == vb0) return shield_grip_of(p.grip_vb0);
    return nullptr;
}
constexpr int kShieldGrips = int(sizeof rig::shield_grips / sizeof rig::shield_grips[0]);
std::atomic<int> grip_known[kShieldGrips];   // (0 not yet seen by the renderer, 1 recognised, 2 not: held as before)
bool grip_recognised(const rig::ShieldGrip* g) {
    return g && grip_known[g - rig::shield_grips].load(std::memory_order_relaxed) == 1;
}

float from_half(uint16_t h) {
    const int e = h >> 10 & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? INFINITY : std::ldexp((float)(m | 1024), e - 25);
    return h & 0x8000 ? -v : v;
}
uint16_t to_half(float f) {   // (nearest; the grip's coordinates are well inside half's normal range)
    const uint16_t sign = std::signbit(f) ? 0x8000 : 0;
    f = std::fabs(f);
    if (!(f >= std::ldexp(1.0f, -14))) return sign | (uint16_t)std::lround(std::ldexp(f, 24));   // (subnormal, or zero)
    int e;
    const float m = std::frexp(f, &e);   // (f = m 2^e, m in [.5, 1))
    long bits = std::lround(std::ldexp(m, 11));   // (11 bits, 1024..2048)
    if (bits == 2048) bits = 1024, e++;
    if (e + 14 >= 31) return sign | 0x7c00;
    return sign | (uint16_t)((e + 14) << 10 | (bits - 1024));
}

// The game's vertex buffer for a shape with a grip (n bytes) copied into out with the grip moved; false, out untouched,
// when the bytes are not the ones the fingerprint was taken of (another game version, another model of the same size).
// No bytes (game null): the renderer could not read them; not known until it can
bool shield_grip_edit(uint64_t vb0, const uint8_t* game, uint64_t n, uint8_t* out) {
    const rig::ShieldGrip* g = shield_grip_of(vb0);
    if (g && !game) grip_known[g - rig::shield_grips].store(0);
    if (!g || !game || n != g->vb0 || (uint64_t)g->stride * g->vertices > n) return false;
    uint64_t h = 0xcbf29ce484222325ull;   // (FNV-1a, as shield_grip.py takes it)
    for (uint64_t i = 0; i < n; i++) h = (h ^ game[i]) * 0x100000001b3ull;
    const bool ok = h == g->fingerprint;
    if (grip_known[g - rig::shield_grips].exchange(ok ? 1 : 2) != (ok ? 1 : 2))
        logf("vr: shield grip: the %llu-byte shield shape %s", (unsigned long long)vb0,
             ok ? "matches your game's model: in the left hand its grip moves across the back"
                : "does not match your game's model: held in the left hand as before");
    if (!ok) return false;
    std::memcpy(out, game, n);
    for (uint32_t r = g->first; r < g->first + g->runs; r++)
        for (uint32_t v = rig::shield_grip_runs[r][0]; v < rig::shield_grip_runs[r][0] + rig::shield_grip_runs[r][1]; v++) {
            uint8_t* x = out + (uint64_t)v * g->stride;   // (the position, half floats x4, first in the vertex: its x)
            uint16_t hx;
            std::memcpy(&hx, x, 2);
            hx = to_half(from_half(hx) + g->shift);
            std::memcpy(x, &hx, 2);
        }
    // (the plate's back with its texture mirrored, so the old mount holes painted there lie under the moved mounts:
    // texture coordinates and tangent as shield_grip.py worked them out; the front, the emblem, is not touched)
    for (uint32_t b = g->back_first; b < g->back_first + g->backs; b++) {
        const uint32_t* e = rig::shield_grip_backs[b];
        if (e[0] >= g->vertices) continue;
        uint8_t* v = out + (uint64_t)e[0] * g->stride;
        std::memcpy(v + g->uv_at, &e[1], 4), std::memcpy(v + g->tangent_at, &e[2], 4);
    }
    return true;
}

// The palettes anchored_skin made for a shield held with its grip moved (kept alive here, so an address is not another
// palette's while it is listed). More than a frame's worth: the replay of a frame asks after the next one has made its own.
std::mutex grip_palette_mu;
std::shared_ptr<const std::vector<float>> grip_palettes[16];
int grip_palette_next = 0;
void grip_palette_add(std::shared_ptr<const std::vector<float>> p) {
    std::lock_guard lk(grip_palette_mu);
    grip_palettes[grip_palette_next] = std::move(p);
    grip_palette_next = (grip_palette_next + 1) % 16;
}
bool grip_palette_listed(const void* p) {
    if (!p) return false;
    std::lock_guard lk(grip_palette_mu);
    for (const auto& q : grip_palettes)
        if (q.get() == p) return true;
    return false;
}
