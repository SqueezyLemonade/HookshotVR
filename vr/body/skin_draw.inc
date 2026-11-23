// Hookshot VR: Link's first-person pose in the draw replay: a draw that reads the skin palette gets the VR
// body's (vr_link_skin), and first-person hair and head are left out.
//
// A piece of gpu/vk/vk_draw.cpp (the draw replay), included there at its VR HOOK: it uses that file's types and helpers
// (Targets, NvnState, Program, stream_copy, logf, ...). Not compiled on its own.
static bool vr_reads_palette(const Program& p) {   // (OoT VR: its vertex stage reads uniform buffer 3, the skin palette)
    for (const Stage& st : p.stages)
        if (st.nvn_stage == 0)
            for (const auto& d : st.info.constant_buffer_descriptors)
                if (d.index <= 6 && 6 < d.index + d.count) return true;
    return false;
}
// (OoT VR) A held arrow's effect draw (the fire arrow's flames and glow): its vertex shader reads the effect matrix (UBO 11,
// "C14": axes and place, 16 floats) and the particle camera (UBO 10, "C13": view at 0x40, projection at 0x80, column-major,
// arrow_effect_math.h), the camera passes that layout's check, and the effect matrix sits on the game's held arrow but not
// at the camera (vr::arrow_effect_move). That camera is then bound moved onto the VR arrow (palette_draw.inc).
static bool vr_vertex_reads(const Program& p, uint32_t index) {   // (the binding index: a UBO's number plus 3)
    for (const Stage& st : p.stages)
        if (st.nvn_stage == 0)
            for (const auto& d : st.info.constant_buffer_descriptors)
                if (d.index <= index && index < d.index + d.count) return true;
    return false;
}
static bool vr_read_as_drawn(uint64_t addr, void* dst, uint64_t n) {
    return ub_overlay(addr, n, (uint8_t*)dst, [](uint64_t, uint64_t) { return (uint64_t)0; }, 16) != UINT64_MAX ||
           memory_read_as_submitted(addr, dst, n);
}
// Returns true for a held arrow's effect draw to leave out (the VR tab's Elemental arrow effects at 0)
// (And the hookshot's: its red aiming laser is such an effect, emitted on the game's hookshot: left out while a VR hand holds
// the hookshot; its reticle, at the hit point, is not emitted there and stays. And the Deku Stick's flame, emitted on the
// game's stick: drawn on the VR stick, whole, whatever the arrow row says. And the sword's charge glow, emitted on the game's
// sword: drawn on the VR sword, whole.)
static bool vr_arrow_effect(NvnState& s, const Program& p) {
    const bool arrow = vr::arrow_effects_live(), hook = !arrow && vr::hook_effects_live(),
               stick = !arrow && !hook && vr::stick_effects_live(), sword = !arrow && !hook && !stick && vr::sword_effects_live();
    if (!arrow && !hook && !stick && !sword) return false;   // (no arrow on the VR string, no hookshot, stick or sword in a VR hand)
    if (!vr_vertex_reads(p, 10 + 3) || !vr_vertex_reads(p, 11 + 3)) return false;
    const nvn_p_bind_buf& u = s.ubo[0][11];
    const nvn_p_bind_buf& cam = s.ubo[0][10];
    if (!u.addr || u.size < 64 || u.size > 0x10000 || !cam.addr || cam.size < 0xc0 || cam.size > 0x10000) return false;
    float m[16], c[0x200 / 4];
    const uint64_t cam_bytes = std::min<uint64_t>(cam.size, sizeof c);
    if (!vr_read_as_drawn(u.addr, m, sizeof m) || !vr_read_as_drawn(cam.addr, c, cam_bytes)) return false;
    vr::arrow_fx::Camera info;
    if (!vr::arrow_fx::inspect_camera(c, cam_bytes, info)) {
        static std::atomic<int> told{0};   // (bounded: a sword in a VR hand, an effect-like draw whose camera is not the layout)
        if (sword && told.fetch_add(1) < 20)
            logf("vr: sword: an effect-like draw (UBO 10 %llu bytes) whose camera is not the effect layout: left where it is",
                 (unsigned long long)cam.size);
        return false;
    }
    float move[12];
    if (arrow ? !vr::arrow_effect_move(m, info.eye, move)
              : hook ? !vr::hook_effect_move(m, info.eye, move)
              : stick ? !vr::stick_effect_move(m, info.eye, move) : !vr::sword_effect_move(m, info.eye, move)) return false;
    if (hook) return true;   // (the hookshot's laser: not drawn; the game's reticle, at the hit point, stays. Player's choice, 17 Nov:
                             // moved whole, the laser ran off the aim)
    const float opacity = stick || sword ? 1.0f : std::clamp(opts.vr_arrow_fx_opacity, 0, 100) * .01f;
    if (opacity <= 0) return true;   // (off)
    // (the effect's depth in front of the camera, moved onto the VR arrow: its place moved, through the view's third row)
    float at[3];
    for (int r = 0; r < 3; r++) at[r] = move[r * 4] * m[12] + move[r * 4 + 1] * m[13] + move[r * 4 + 2] * m[14] + move[r * 4 + 3];
    const float* v = c + vr::arrow_fx::kView;
    const float depth = -(v[2] * at[0] + v[6] * at[1] + v[10] * at[2] + v[14]);
    vr_arrow_fx.camera = cam.addr;
    std::memcpy(vr_arrow_fx.move, move, sizeof move);
    vr_arrow_fx.info = info;
    vr_arrow_fx.stretch = vr::arrow_fx::fade_stretch(depth, opacity);
    ++s.bind_gen;   // (this draw's descriptors are rebuilt with the moved camera)
    return false;
}
#include "../items/ocarina/song_effects.inc"   // (a song's effects: the VR tab's Song effects)
// (OoT VR, body v2) What a draw of Link's becomes: *what says which of his draws it was (vr::LinkDraw::Count: not one of
// his), for the counts behind the "Link blink" lines (BODY-12). Returns true to leave the draw out.
static bool vr_link_skin_pick(NvnState& s, const Program& p, const DrawArgs& d, const Targets& at, vr::LinkDraw* what) {
    if (vr_bones.palette) ++s.bind_gen; // discard descriptors that referenced the previous draw's private palette
    if (vr_arrow_fx.camera) ++s.bind_gen, vr_arrow_fx.camera = 0;   // (and the previous arrow effect's moved camera)
    vr_bones.addr = 0;
    vr_bones.palette.reset();
    static thread_local bool reticle_was_hidden=false;
    const bool hide_reticle=vr::native_bow_reticle_hidden();
    if (hide_reticle!=reticle_was_hidden) {
        ++s.bind_gen;
        descriptors_disturbed();
        bound().desc.valid = false;
        reticle_was_hidden=hide_reticle;
    }
    if (!opts.vr_arms || opts.vr_mode != 1) return false;
    if (vr_arrow_effect(s, p)) return true;   // (Elemental arrow effects: 0, off)
    if (vr_song_effect_hidden(s, p)) return true;   // (Song effects: Hidden)
    const nvn_p_bind_buf& bones = s.ubo[0][3];
    const nvn_p_bind_buf& object = s.ubo[0][1];
    const uint64_t vb0 = s.vbuf[0].size;
    // (Link's own draws use per-shape object buffers next to each other: "near" is within a few of the last one)
    static std::atomic<uint64_t> link_object{0};
    const uint64_t lo = link_object.load(std::memory_order_relaxed);
    const bool near = lo && object.addr && (object.addr > lo ? object.addr - lo : lo - object.addr) < 0x2000;
    static std::atomic<int> told{0};
    const auto trace = [&](const char* what, bool reads) {
        static thread_local std::unordered_set<uint64_t> seen;
        const uint64_t key = vb0 ^ ((uint64_t)d.count << 24) ^ (p.key << 7) ^ ((uint64_t)(at.n && at.color[0]) << 62) ^
                             ((uint64_t)(what[0]) << 54);
        if (told.load(std::memory_order_relaxed) >= 200 || !seen.insert(key).second || told.fetch_add(1) >= 200) return;
        logf("vr: %s vb0 %llu count %d prog %016llx palette %llu bytes%s: %s%s, %d instance(s)%s%s", what, (unsigned long long)vb0,
             d.count, (unsigned long long)p.key, (unsigned long long)bones.size, reads ? " (read)" : "",
             at.n && at.color[0] ? "colour" : "no colour", at.depth ? " + depth" : "", d.instances, d.mesh ? ", mesh" : "",
             d.indirect ? ", indirect" : "");
    };
    // (the head and hair only where there is a first-person body: a cutscene at the director's camera shows the game's Link
    // whole, head and all; the worn items and the recent pose below look at the frame's body already. The two hair buffers
    // known by size are left out when not near only in the world's own passes (into the scene's depth, or depth only: a
    // shadow map) and where the player's Link stands: the pause menu's Equipment screen draws its own Link into its own
    // targets, and a cinematic its own model of him; they keep their hair (headset 22 Nov, "a buzzcut")
    const auto world_pass = [&] {   // (asked only for those)
        return (!(at.n && at.color[0]) || vr::scene_depth_is(at.depth)) &&
               vr::link_draw_near_body(object.addr, object.size, texture_frame_no());
    };
    if (vr::hair_draw(vb0, (uint32_t)d.count) && (near || ((vb0 == 2912496 || vb0 == 1577752) && world_pass())) &&
        vr::link_body_shown(texture_frame_no())) {
        trace("hair left out", false);
        *what = vr::LinkDraw::HeadHidden;
        return true;
    }
    if (near && vr::head_draw(vb0) && vr::link_body_shown(texture_frame_no())) {   // (the head's own shadow pass binds the hair's palette, not the body's)
        trace("head left out (near)", false);
        *what = vr::LinkDraw::HeadHidden;
        return true;
    }
    const bool reads = vr_reads_palette(p);
    // (the fired hookshot chain's colour pass binds its palette in uniform buffer 4, not 3: vr/items/hookshot/held_hookshot.inc;
    // so do the ocarina's and its holder's: vr/items/ocarina/held_ocarina.inc; a bottle's is tried there too, taken only when
    // it puts the bottle in Link's game hand: vr/items/bottle/held_bottle.inc; and, while an item get plays, any model's, for
    // its item only (vr::item_get_draw): a worn item's draw goes on to its hiding below, vr/items/item_get/item_get.inc)
    const bool item_get = vr::item_get_live() && s.ubo[0][4].size && s.ubo[0][4].size <= 8 * 48;   // (a small model's palette)
    const bool held = vr::hook_chain_draw(vb0) || vr::ocarina_draw(vb0) || vr::bottle_draw(vb0);
    if (reads && (held || item_get)) {
        const nvn_p_bind_buf& u4 = s.ubo[0][4];
        if (auto chain = held ? vr::link_equipment_skin(u4.addr, u4.size, object.addr, object.size, vb0, texture_frame_no())
                              : vr::item_get_draw(u4.addr, u4.size, object.addr, object.size, vb0, texture_frame_no())) {
            vr_bones.addr = u4.addr;
            vr_bones.palette = std::move(chain);
            ++s.bind_gen;
            *what = vr::LinkDraw::Equipment;
            return false;
        }
    }
    if (reads) {
        auto equipment = vr::link_equipment_skin(bones.addr, bones.size, object.addr, object.size, vb0, texture_frame_no());
        if (equipment) {
            trace("equipment draw", true);
            vr_bones.addr = bones.addr;
            vr_bones.palette = std::move(equipment);
            ++s.bind_gen;
            *what = vr::LinkDraw::Equipment;
            return false;
        }
        // (some colour passes bind the palette in vertex UBO 4, a larger shared buffer in 3: the ocarina, the hookshot chain,
        // headset logs 17 Nov; a worn item's palette is looked for in either)
        const nvn_p_bind_buf& u4 = s.ubo[0][4];
        if (vr::link_worn_hidden(bones.addr, bones.size, object.addr, object.size, vb0, texture_frame_no()) ||
            (u4.addr && vr::link_worn_hidden(u4.addr, u4.size, object.addr, object.size, vb0, texture_frame_no()))) {
            trace("worn item left out", true);
            *what = vr::LinkDraw::HeadHidden;
            return true;
        }
    }
    if (!reads || !bones.addr || bones.size < 48 * 280 || bones.size > 48 * 288 || bones.size % 48) {
        if (near) trace("near-Link draw", reads);
        if (near && reads) *what = vr::LinkDraw::NearNative;
        return false;
    }
    bool hide = false;
    std::shared_ptr<const std::vector<float>> pal =
        vr::link_skin(bones.addr, bones.size, object.addr, object.size, vb0, &hide, texture_frame_no());
    const bool refused = !pal && vr::link_skin_refused();
    if (!pal && near) {   // (one of Link's own draws, beside the others, whose upload was not taken: the frame's VR pose)
        pal = vr::link_skin_recent(vb0, &hide, texture_frame_no());
        if (pal) trace(hide ? "head left out (recent pose)" : "body draw (recent pose)", true);
        if (pal) *what = hide ? vr::LinkDraw::HeadHidden : vr::LinkDraw::RecentPose;
    }
    if (!pal) {
        if (near) trace("near-Link draw", reads);
        *what = refused ? vr::LinkDraw::Refused : near ? vr::LinkDraw::NearNative : vr::LinkDraw::Count;
        return false;
    }
    if (object.addr) link_object.store(object.addr, std::memory_order_relaxed);
    trace(hide ? "head left out" : "body draw", true);
    if (hide) return *what = vr::LinkDraw::HeadHidden, true;
    if (*what == vr::LinkDraw::Count) *what = vr::LinkDraw::Posed;
    vr_bones.addr = bones.addr;
    vr_bones.palette = std::move(pal);
    ++s.bind_gen;   // the pose changes even when the game's bindings do not: rebuild this draw's descriptors
    return false;
}

// (OoT VR) Link's own shadow in first person (VR tab "Link's shadow (first person)": Hidden by default). A shadow map pass
// draws depth only, into a square target of its own; the scene's depth prepass draws depth only too, but into the scene's
// own depth (as the scene's colour draws use it: vr::scene_depth_is), and Link must stay in that one or his colour would be
// hidden behind the world's prepass depth or lose its depth test. So: no colour target, a square depth target that is not
// the scene's depth, and the scene's depth known (unknown: shown, the safe side). The headset logs (18 Nov) had Link's
// depth-only draws at 115x115 (a level of a shadow map) and shadow passes of 1600x1600, 1920x1920 and 384x384; the scene
// 2880x2304.
static bool vr_shadow_pass(const Targets& at) {
    if (!at.depth || (at.n && at.color[0])) return false;
    if (at.width != at.height || at.depth->width != at.depth->height) return false;
    return vr::scene_depth_known() && !vr::scene_depth_is(at.depth);
}

static bool vr_link_skin(NvnState& s, const Program& p, const DrawArgs& d, const Targets& at) {   // (OoT VR, body v2)
    vr::LinkDraw what = vr::LinkDraw::Count;
    bool skip = vr_link_skin_pick(s, p, d, at, &what);
    const bool colour = at.n && at.color[0];
    // Link's own posed draws (his body, the gear posed with it) leave the shadow passes, after their pose is made: the frame's
    // one pose stays the one every pass would use (BODY-03); his head and hair are already left out of every pass
    const bool posed = what == vr::LinkDraw::Posed || what == vr::LinkDraw::RecentPose || what == vr::LinkDraw::Equipment;
    if (!skip && posed && vr_bones.palette && opts.vr_own_shadow == 0 && vr_shadow_pass(at)) {
        static std::mutex told_mu;
        static std::unordered_set<uint64_t> told;
        {
            std::lock_guard lk(told_mu);
            const uint64_t key = (uint64_t)at.width << 32 | (uint64_t)at.depth_format << 16 | (uint64_t)at.depth->width;
            if (told.size() < 16 && told.insert(key).second)
                logf("vr: Link's shadow left out (first person, Link's shadow Hidden): depth-only pass %ux%u of a %ux%u depth "
                     "target, format %d", at.width, at.height, at.depth->width, at.depth->height, (int)at.depth_format);
        }
        vr_bones.addr = 0;   // (the pose stays in the cache for the passes that draw it; this draw is left out)
        vr_bones.palette.reset();
        skip = true;
        what = vr::LinkDraw::ShadowHidden;
    } else if (!skip && posed && at.depth && !colour && opts.vr_own_shadow == 0) {
        // (research, bounded) a depth-only pass that kept Link: the scene's depth prepass, or a shadow pass the rule above
        // missed. Once per kind, so a shadow still seen in the headset names its pass in the log.
        static std::mutex kept_mu;
        static std::unordered_set<uint64_t> kept;
        const bool scene = vr::scene_depth_is(at.depth);
        std::lock_guard lk(kept_mu);
        const uint64_t key = (uint64_t)at.width << 40 | (uint64_t)at.height << 20 | (uint64_t)at.depth->width << 1 | scene;
        if (kept.size() < 16 && kept.insert(key).second)
            logf("vr: Link kept in a depth-only pass %ux%u of a %ux%u depth target (%s)", at.width, at.height, at.depth->width,
                 at.depth->height, scene ? "the scene's depth: its prepass" : vr::scene_depth_known() ?
                 "not square, not the scene's depth" : "the scene's depth not known yet");
    }
    if (what != vr::LinkDraw::Count) vr::note_link_draw(what, colour, texture_frame_no());
    return skip;
}
