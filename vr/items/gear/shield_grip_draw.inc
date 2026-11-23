// Hookshot VR: the held shield's grip, moved across its back for the left VR hand: the vertex buffer the replay binds
// for it.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (NvnState, BufferRef, vr_bones, memory_read_as_submitted, create_buffer, logf). Not compiled on its own.
// The VR side (vr/items/gear/shield_grip.inc) says which shapes have a grip (vr::shield_grip_shape), checks the game's
// bytes against your game's model and moves the grip (vr::shield_grip_make), and says which draws want it: those whose
// VR palette it made for the left hand with the grip moved (vr::shield_grip_drawn), the same palette for the frame's
// colour, depth and shadow draws. Here: a buffer of the replay's own for each such shape, filled once from the game's
// buffer as first drawn and kept to the end (a few hundred kB in all), so a draw recorded with it never outlives it.
// A shape met at another address (the model loaded again) is checked again; the same bytes move the same way, so the
// buffer is not written again. draw_one binds it in vertex stream 0 in place of the game's buffer, and does not remember
// that set as the one bound, so the next draw (the right hand's, any other) binds its own streams again.
struct VrGripBuffer {
    uint64_t vb0 = 0, addr = 0;
    bool ok = false, written = false;
    int read_failures = 0;
    uint8_t* mapped = nullptr;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
};
static std::mutex vr_grip_mu;
static VrGripBuffer vr_grip_buffers[8];

static BufferRef vr_shield_grip(const NvnState& s) {   // (an empty ref: the game's own buffer)
    if (!vr_bones.palette || !s.vbuf[0].address) return {};
    const uint64_t vb0 = s.vbuf[0].size, addr = s.vbuf[0].address;
    if (!vr::shield_grip_shape(vb0, s.vstream[0].stride)) return {};
    std::lock_guard lk(vr_grip_mu);
    VrGripBuffer* g = nullptr;
    for (VrGripBuffer& x : vr_grip_buffers)
        if (x.vb0 == vb0) { g = &x; break; }
    if (!g)
        for (VrGripBuffer& x : vr_grip_buffers)
            if (!x.vb0) { x.vb0 = vb0, g = &x; break; }
    if (!g) return {};
    if (!g->buffer && g->read_failures < 64) {   // (made first: a shape is recognised only with a buffer to draw it from)
        void* mapped = nullptr;
        if (!create_buffer(vb0, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true, g->buffer, g->alloc, &mapped, false) || !mapped) {
            if (g->buffer || g->alloc) destroy_buffer(g->buffer, g->alloc);
            g->buffer = VK_NULL_HANDLE, g->alloc = nullptr, g->read_failures = 64;
            logf("vr: shield grip: no buffer for the %llu-byte shield shape: held as before", (unsigned long long)vb0);
            return {};
        }
        g->mapped = (uint8_t*)mapped;
    }
    if (g->buffer && g->addr != addr && g->read_failures < 64) {
        std::vector<uint8_t> game(vb0);
        if (!memory_read_as_submitted(addr, game.data(), vb0)) {
            vr::shield_grip_make(vb0, nullptr, 0, nullptr);   // (not known meanwhile: the left hand's placement as before)
            if (++g->read_failures == 64) logf("vr: shield grip: the %llu-byte shield shape could not be read: held as before",
                                               (unsigned long long)vb0);
            return {};
        }
        g->addr = addr, g->read_failures = 0;
        if (g->written) {   // (the fingerprint matched: the same bytes, moved the same way; not written again under a draw)
            std::vector<uint8_t> moved(vb0);
            g->ok = vr::shield_grip_make(vb0, game.data(), vb0, moved.data());
        } else if ((g->ok = vr::shield_grip_make(vb0, game.data(), vb0, g->mapped))) {
            flush_mapped_checked(g->alloc, vb0);
            g->written = true;
        }
    }
    if (g->addr != addr || !g->ok || !vr::shield_grip_drawn(vb0, vr_bones.palette.get())) return {};
    BufferRef r;
    r.buffer = g->buffer, r.offset = 0, r.size = vb0;
    return r;
}
