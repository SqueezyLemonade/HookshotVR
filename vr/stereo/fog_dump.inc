// (OoT VR, research, STE-36) vr/stereo/fog_dump.inc: the True stereo fog dump, included by gpu/vk/vk_draw.cpp after
// inputs_draw.inc (Program, NvnState, Targets, handle_of, texture_view_of, ...). Not compiled on its own.
//
// RX_VR_FOG_DUMP=<folder>: off unless set. A tool asks for one frame by writing a label ("in", "exit") into
// <folder>/fogdump.request; the frame after the request is seen is dumped into <folder>/fogdump-<label>.txt:
//  A  every draw whose program samples a 3D texture (the fog grid): per program and target, how many, how it was drawn for
//     the eyes (0 one view, 1 both, 2 moved), routed per eye or not, the fragment patch, and the reasons a camera rule did
//     or did not apply (its vertex buffer 0's size, size field and P00; the particle camera's P00), its learned copies
//  B  the textures of a few fog programs: kind, size, format, levels, layers, and the program that last wrote it this frame
//  C  every dispatch: grid, run once or twice, the images it writes; the storage buffers of the depth-bounds computes
//     (0ba2f076e4a94acf, 592079442d7129da) and which later draws and dispatches bind them
//  D  raw numbers: the scene camera, the fog blocks, the 14848-byte fog buffer, the particle camera
// (The stereo fog investigation, its section 7.) The pictures (its E) are the headless test's own
// (RX_VR_EYES_TEST): not repeated here.
#ifdef HOOKSHOT_VR_RESEARCH   // (research builds only: a player's build has the stubs at the end)
namespace fogdump {

struct DispatchRow {
    int count = 0;
    bool twice = false;
    std::string line;
};
struct State {
    std::mutex mu;
    bool active = false;
    uint64_t frame = 0, checked = 0;
    std::string label;
    std::map<std::string, int> draws;                          // (A: the line -> draws)
    std::map<uint64_t, DispatchRow> dispatches;                // (C)
    std::vector<std::string> lines;                            // (B, C's readers, D: in the order seen)
    std::unordered_map<const Texture*, uint64_t> writer;       // (this frame's last writer of each texture)
    std::vector<std::pair<uint64_t, uint64_t>> watched;        // (C: the depth-bounds computes' storage buffers)
    std::unordered_set<uint64_t> told;                         // (B, D and readers: once per program and kind)
};
State& state() {
    static State s;
    return s;
}
const char* folder() {
    static const char* d = vr_research_getenv("RX_VR_FOG_DUMP");
    return d && *d ? d : nullptr;
}
bool once(State& s, uint64_t prog, uint64_t kind) { return s.told.insert(prog * 31 + kind).second; }
thread_local const NvnState* dump_state = nullptr;   // (the draw's state, for ns_ubo)
/// a stage's uniform buffer by its binding (a UBO's number plus 3), or an empty one
const nvn_p_bind_buf& ns_ubo(int ns, int binding) {
    static const nvn_p_bind_buf none{};
    return dump_state && binding >= 3 && binding - 3 < 16 ? dump_state->ubo[ns][binding - 3] : none;
}

void flush(State& s) {
    const std::string path = std::string(folder()) + "/fogdump-" + s.label + ".txt";
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return void(logf("vr: fog dump: %s could not be written", path.c_str()));
    fprintf(f, "# True stereo fog dump '%s', frame %llu\n",
            s.label.c_str(), (unsigned long long)s.frame);
    fprintf(f, "## A: draws sampling a 3D texture (draws | program, target, eyes, routing, patch, why)\n");
    for (const auto& [line, n] : s.draws) fprintf(f, "A %4d | %s\n", n, line.c_str());
    fprintf(f, "## C: dispatches (count, once or twice | program, grid, images written, two-layer textures read, storage)\n");
    for (const auto& [key, row] : s.dispatches)
        fprintf(f, "C %4d %s | %s\n", row.count, row.twice ? "twice" : "once ", row.line.c_str());
    fprintf(f, "## B, D and the storage readers, in the order seen\n");
    for (const std::string& l : s.lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
    logf("vr: fog dump: '%s' written (%zu draw rows, %zu dispatch programs, %zu other lines): %s", s.label.c_str(),
         s.draws.size(), s.dispatches.size(), s.lines.size(), path.c_str());
}
/// (State's lock held) at each draw and dispatch: the last dump frame written when a new frame starts; a request taken
bool begin(State& s) {
    const uint64_t now = texture_frame_no();
    if (s.active && now != s.frame) flush(s), s.active = false;
    if (!s.active && now != s.checked) {
        s.checked = now;
        const std::string req = std::string(folder()) + "/fogdump.request";
        if (FILE* f = fopen(req.c_str(), "r")) {
            char buf[64] = {};
            if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
            fclose(f);
            remove(req.c_str());
            std::string label;
            for (const char* c = buf; *c; c++)
                if (isalnum((unsigned char)*c) || *c == '-' || *c == '_') label += *c;
            s.draws.clear(), s.dispatches.clear(), s.lines.clear(), s.writer.clear(), s.watched.clear(), s.told.clear();
            s.label = label.empty() ? "dump" : label, s.active = true, s.frame = now;
        }
    }
    return s.active;
}
std::string floats(const char* name, uint64_t addr, uint64_t size, uint32_t from, uint32_t to) {
    if (!addr || size < to) return std::string("D ") + name + ": the buffer is too small or unbound";
    std::vector<float> v((to - from) / 4);
    if (!memory_read_as_submitted(addr + from, v.data(), to - from)) return std::string("D ") + name + ": not read";
    std::string out;
    char b[64];
    for (size_t i = 0; i < v.size(); i++) {
        if (i % 16 == 0) {
            snprintf(b, sizeof b, "%sD %s %#x:", out.empty() ? "" : "\n", name, (unsigned)(from + i * 4));
            out += b;
        }
        snprintf(b, sizeof b, " %.6g", v[i]);
        out += b;
    }
    return out;
}
std::string texture_text(const Texture* t) {
    if (!t) return "none";
    char b[420];
    snprintf(b, sizeof b, "%ux%ux%u vk %d, %u levels, %u layers, %s%s, written by the GPU %d | id %u image %#llx, uploaded %d gen %u, "
             "deferred at %llu (asks %u), astc %d reuse-retire %d freed-listed %d, last used %llu",
             t->width, t->height, t->depth, (int)t->format, t->levels, t->layers, t->vr_eyes ? "two-layer" : "one-layer",
             t->vr_twin ? " + twin" : "", (int)t->gpu_written, t->id, (unsigned long long)(uintptr_t)t->image, (int)t->uploaded,
             t->upload_gen, (unsigned long long)t->defer_frame, t->defer_asks, (int)t->astc, (int)t->reuse_retire,
             (int)t->freed_listed, (unsigned long long)t->last_frame);
    return b;
}
std::string writer_text(State& s, const Texture* t) {
    auto it = t ? s.writer.find(t) : s.writer.end();
    if (it == s.writer.end()) return "not written yet this frame";
    char b[48];
    snprintf(b, sizeof b, "written this frame by %016llx", (unsigned long long)it->second);
    return b;
}
/// C's readers: a draw or dispatch binding a storage buffer that overlaps one of the depth-bounds computes'
void readers(State& s, uint64_t prog, const NvnState& ns, const char* kind) {
    for (int st = 0; st < kNvnStages; st++)
        for (int i = 0; i < 16; i++) {
            const nvn_p_bind_buf& u = ns.ssbo[st][i];
            if (!u.addr) continue;
            for (const auto& [a, n] : s.watched)
                if (u.addr < a + n && a < u.addr + u.size && once(s, prog, 1000 + st * 16 + i)) {
                    char b[200];
                    snprintf(b, sizeof b, "C reader: %s %016llx stage %d ssbo %d binds %#llx (+%llu bytes) of a depth-bounds "
                             "compute's storage", kind, (unsigned long long)prog, st, i, (unsigned long long)u.addr,
                             (unsigned long long)u.size);
                    s.lines.push_back(b);
                }
        }
}
/// B: every texture a program samples or writes, with its writer this frame
void textures(State& s, const Program& p, const NvnState& ns) {
    // (and STE-30: the eight foliage programs, each sampled binding's texture with its upload state, to compare a clean run
    // with a white one: a placeholder or a texture not yet uploaded kept bound in a white run)
    static const uint64_t keys[] = {0x1a7482c0ec9991eaull, 0x01dfc7c88064d24dull, 0x03a487667f9abcb4ull, 0x00b00daa74d50fb9ull,
                                    0x22ffdff3b3f254ebull, 0xd905f52b7517c681ull, 0x739acd678ae08655ull, 0x6b4dea601798c255ull,
                                    0x68642bb810ee3e7cull, 0x89ca750f9852b7caull, 0xc36a4f3f667c886aull, 0xd8e59341cf4f629cull,
                                    0xd4abbcb4c2140609ull};
    bool want = false;
    for (const uint64_t k : keys) want |= k == p.key;
    if (!want || !once(s, p.key, 1)) return;
    for (const Stage& st : p.stages) {
        for (const auto& d : st.info.texture_descriptors)
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(ns, st, d, k);
                Texture* tex = nullptr;
                const VkImageView view = handle ? texture_view_of(handle, view_type(d.type), ViewUse::Sample, VK_FORMAT_UNDEFINED, &tex)
                                                : VK_NULL_HANDLE;
                char b[800];
                snprintf(b, sizeof b, "B %016llx stage %#x texture handle c%u:%#x [%u] = %#x (view type %d, view %#llx): %s; %s",
                         (unsigned long long)p.key, (unsigned)st.vk_stage, d.cbuf_index, d.cbuf_offset, k, handle, (int)view_type(d.type),
                         (unsigned long long)(uintptr_t)view, texture_text(tex).c_str(), writer_text(s, tex).c_str());
                s.lines.push_back(b);
            }
        for (const auto& d : st.info.image_descriptors)
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(ns, st, d, k);
                Texture* tex = nullptr;
                if (handle) texture_view_of(handle, view_type(d.type), ViewUse::Storage, image_format(d.format), &tex);
                char b[400];
                snprintf(b, sizeof b, "B %016llx stage %#x image handle c%u:%#x [%u]%s: %s; %s", (unsigned long long)p.key,
                         (unsigned)st.vk_stage, d.cbuf_index, d.cbuf_offset, k, d.is_written ? " (written)" : "", texture_text(tex).c_str(),
                         writer_text(s, tex).c_str());
                s.lines.push_back(b);
            }
    }
}
/// D: the numbers, once per dump each
void values(State& s, const Program& p, const NvnState& ns) {
    dump_state = &ns;
    const nvn_p_bind_buf& v0 = ns.ubo[0][0];
    if (v0.addr && v0.size >= 0x980 && vr::eyes_scene_camera(v0.addr, v0.size) && once(s, 0, 2)) {
        s.lines.push_back(floats("scene camera", v0.addr, v0.size, 0, 0x2a0));
        s.lines.push_back(floats("scene camera", v0.addr, v0.size, 0x940, 0x980));
    }
    if (p.key == 0x1a7482c0ec9991eaull || p.key == 0x01dfc7c88064d24dull)   // (the fog block of their fragment buffers)
        for (const Stage& st : p.stages)
            if (st.vk_stage & VK_SHADER_STAGE_FRAGMENT_BIT)
                for (int i = 0; i < 16; i++) {
                    const nvn_p_bind_buf& u = ns.ubo[std::clamp(st.nvn_stage, 0, kNvnStages - 1)][i];
                    if (u.addr && u.size >= 0x690 && once(s, p.key, 10 + i)) {
                        char name[64];
                        snprintf(name, sizeof name, "fog block of %016llx ubo %d", (unsigned long long)p.key, i);
                        s.lines.push_back(floats(name, u.addr, u.size, 0x5e0, 0x690));
                    }
                }
    for (int st = 0; st < kNvnStages; st++)   // (the 14848-byte fog buffer, wherever it is bound first)
        for (int i = 0; i < 16; i++) {
            const nvn_p_bind_buf& u = ns.ubo[st][i];
            if (u.addr && u.size == 14848 && once(s, 0, 3)) s.lines.push_back(floats("fog buffer (14848 bytes)", u.addr, u.size, 0, 0x2d0));
        }
    // (STE-30: the grass blades' mesh stage, d905f52b7517c681 / 739acd678ae08655: bindings 4 and 5, which nothing moves per eye;
    // binding 5 holds a camera-like position (0x40), the blade width's scalar (0x15c) and height cuts (0x180, 0x184, 0x1f4).
    // As guest memory and with the stream's updates over it (ub_overlay into a buffer of the dump's own): which bytes the draw
    // gets. The investigation session's suggestion, to compare a clean run with a white one)
    if ((p.key == 0xd905f52b7517c681ull || p.key == 0x739acd678ae08655ull) && once(s, p.key, 20))
        for (const Stage& st : p.stages) {
            if (!(st.vk_stage & VK_SHADER_STAGE_MESH_BIT_EXT)) continue;
            const int ns = std::clamp(st.nvn_stage, 0, kNvnStages - 1);
            static const struct { int binding; uint32_t from, to; } ranges[] = {
                {4, 0x50, 0x5c}, {4, 0x220, 0x228}, {4, 0x380, 0x388},
                {5, 0x40, 0x50}, {5, 0x150, 0x160}, {5, 0x180, 0x188}, {5, 0x1f0, 0x1f8}};
            for (const auto& r : ranges) {
                const nvn_p_bind_buf& u = ns_ubo(ns, r.binding);
                char name[160];   // (96 cut the name short)
                snprintf(name, sizeof name, "blades %016llx mesh binding %d (%#llx, %llu bytes) as guest memory",
                         (unsigned long long)p.key, r.binding, (unsigned long long)u.addr, (unsigned long long)u.size);
                s.lines.push_back(floats(name, u.addr, u.size, r.from, r.to));
                std::vector<float> v((r.to - r.from) / 4);
                const bool upd = u.addr && u.size >= r.to &&
                                 ub_overlay(u.addr + r.from, r.to - r.from, (uint8_t*)v.data(),
                                            [](uint64_t, uint64_t) { return (uint64_t)0; }, 0) != UINT64_MAX;
                std::string line = std::string("D blades ") + (upd ? "with the stream's updates over it" : "no stream update over it") + ":";
                char b[32];
                for (float f : v) snprintf(b, sizeof b, " %.6g", upd ? f : 0.0f), line += b;
                s.lines.push_back(upd ? line : "D blades: no stream update over that range");
            }
        }
    if (p.key == 0x00b00daa74d50fb9ull && once(s, 0, 4)) {   // (the effects layout's particle camera, vertex buffer 10)
        const nvn_p_bind_buf& u = ns.ubo[0][10];
        s.lines.push_back(floats("particle camera", u.addr, u.size, 0, 0xc0));
        s.lines.push_back(floats("particle camera", u.addr, u.size, 0x1c0, 0x200));
    }
}
bool samples_3d(const Program& p) {
    thread_local std::unordered_map<uint64_t, bool> known;
    if (auto it = known.find(p.key); it != known.end()) return it->second;
    bool any = false;
    for (const Stage& st : p.stages)
        for (const auto& d : st.info.texture_descriptors) any |= view_type(d.type) == VK_IMAGE_VIEW_TYPE_3D;
    return known[p.key] = any;
}

}  // namespace fogdump

/// VR HOOK (research, STE-36) a draw, after its eyes and routing are decided (vk_draw.cpp draw_one)
void vr_fog_dump_draw(const Program& p, const NvnState& s, const DrawArgs& d, const Targets& at) {
    if (!fogdump::folder()) return;
    fogdump::State& S = fogdump::state();
    std::lock_guard lk(S.mu);
    if (!fogdump::begin(S)) return;
    for (int i = 0; i < at.n && i < 8; i++)
        if (at.color[i]) S.writer[at.color[i]] = p.key;
    if (at.depth && s.ds.dw) S.writer[at.depth] = p.key;
    fogdump::readers(S, p.key, s, "draw");
    fogdump::textures(S, p, s);
    fogdump::values(S, p, s);
    if (!fogdump::samples_3d(p)) return;
    // (why the camera rules did or did not move it: vertex buffer 0, as vr::eyes_camera reads it; the particle camera)
    char why[200] = "vertex buffer 0 unbound";
    const nvn_p_bind_buf& v0 = s.ubo[0][0];
    float c[0x130 / 4] = {};
    if (v0.addr && v0.size >= 0x130 && memory_read_as_submitted(v0.addr, c, sizeof c))
        snprintf(why, sizeof why, "vbuf0 %llu bytes, size field %.0fx%.0f, w row %s, P00 %.4f (scene %.4f)",
                 (unsigned long long)v0.size, c[0x120 / 4], c[0x124 / 4],
                 c[0xa0 / 4] == 0 && c[0xa4 / 4] == 0 && c[0xa8 / 4] == -1 && c[0xac / 4] == 0 ? "ok" : "not a projection",
                 std::fabs(c[0x70 / 4]), vr::eyes_scene_p00());
    else if (v0.addr)
        snprintf(why, sizeof why, "vbuf0 %llu bytes (under 0x130)", (unsigned long long)v0.size);
    char particle[64] = "";
    float pc[0xa0 / 4] = {};
    if (vr_vertex_reads(p, 13) && s.ubo[0][10].addr && s.ubo[0][10].size >= 0xa0 &&
        memory_read_as_submitted(s.ubo[0][10].addr, pc, sizeof pc))
        snprintf(particle, sizeof particle, ", particle camera P00 %.4f", std::fabs(pc[0x80 / 4]));
    char line[900];
    snprintf(line, sizeof line, "%016llx into %ux%u (%s), %d vertices x%d%s%s, blend %d, depth test %d write %d | eyes %d, %s, "
             "patch %s | %s%s | copies %s",
             (unsigned long long)p.key, at.width, at.height, at.n && at.color[0] && at.color[0]->vr_eyes ? "two-layer" : "one-layer",
             d.count, d.instances, d.mesh ? " mesh" : "", d.indirect ? " indirect" : "", (int)(s.color.blend_en & 1),
             (int)s.ds.dt, (int)s.ds.dw, (int)at.eyes, vr_split ? "routed per eye" : "one draw", at.eyes_cam ? "on" : "off", why,
             particle, vr::eyes_copies_text(p.key).c_str());
    S.draws[line]++;
}

/// VR HOOK (research, STE-36) a dispatch, after its eyes are decided (vk_draw.cpp dispatch_one); vr_split 2: its second run
void vr_fog_dump_dispatch(const Program& p, const NvnState& s) {
    if (!fogdump::folder()) return;
    fogdump::State& S = fogdump::state();
    std::lock_guard lk(S.mu);
    if (!fogdump::begin(S)) return;
    fogdump::readers(S, p.key, s, "dispatch");
    fogdump::values(S, p, s);
    fogdump::DispatchRow& row = S.dispatches[p.key];
    row.count++;
    row.twice |= vr_split == 2;
    std::string written, read, storage;
    for (const Stage& st : p.stages) {
        for (const auto& d : st.info.image_descriptors)
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                Texture* tex = nullptr;
                if (!d.is_written || !handle ||
                    !texture_view_of(handle, view_type(d.type), ViewUse::Storage, image_format(d.format), &tex) || !tex)
                    continue;
                S.writer[tex] = p.key;
                if (row.line.empty()) written += " [" + fogdump::texture_text(tex) + "]";
            }
        for (const auto& d : st.info.texture_descriptors)
            for (uint32_t k = 0; k < d.count; k++) {
                const uint32_t handle = handle_of(s, st, d, k);
                Texture* tex = nullptr;
                if (row.line.empty() && handle && texture_view_of(handle, view_type(d.type), ViewUse::Sample, VK_FORMAT_UNDEFINED, &tex) &&
                    tex && tex->vr_eyes)
                    read += " [" + fogdump::texture_text(tex) + "]";
            }
    }
    const bool bounds = p.key == 0x0ba2f076e4a94acfull || p.key == 0x592079442d7129daull;
    const int ns = p.stages.empty() ? 5 : std::clamp(p.stages[0].nvn_stage, 0, kNvnStages - 1);
    for (int i = 0; i < 16; i++) {
        const nvn_p_bind_buf& u = s.ssbo[ns][i];
        if (!u.addr) continue;
        if (bounds) S.watched.push_back({u.addr, u.size});
        if (row.line.empty()) {
            char b[64];
            snprintf(b, sizeof b, " ssbo%d %#llx+%llu", i, (unsigned long long)u.addr, (unsigned long long)u.size);
            storage += b;
        }
    }
    if (row.line.empty()) {
        char b[96];
        snprintf(b, sizeof b, "%016llx grid %ux%ux%u", (unsigned long long)p.key, s.grid[0], s.grid[1], s.grid[2]);
        row.line = std::string(b) + " | writes" + (written.empty() ? " -" : written) + " | two-layer read" +
                   (read.empty() ? " -" : read) + " |" + storage;
    }
}
#else
inline void vr_fog_dump_draw(const Program&, const NvnState&, const DrawArgs&, const Targets&) {}
inline void vr_fog_dump_dispatch(const Program&, const NvnState&) {}
#endif
