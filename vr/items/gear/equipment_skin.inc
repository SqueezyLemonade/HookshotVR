// Hookshot VR: equipment draws (sword, shield, bow, quiver...): which item a draw is, and its VR placement.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.

// ---- child Link's Goron Bracelet --------------------------------------------------------------------------------------------
// The bracelet (Item_Gauntlet_Goron) is its own small rigid model, not skinned to Link: the game puts its one palette matrix
// on one of child Link's arm bones every frame. Unrecognised, it was drawn where the GAME's arm is, which in first person is
// not the VR arm (the native arms hang at the hips): "I did not see it on my wrist" (player, 18 Nov). Like the gauntlets it
// is now moved by its bone's change from the game's body upload to the frame's one VR pose, so it sits on the VR forearm or
// wrist, the same in every pass.
// Which bone (rig::bracelet_bones in vk_vr_worn_items.h, made by vr/tools/generate/worn_items.py from the player's
// Player_Child and Item_Gauntlet_Goron, as are its draws' vb0 sizes: each forearm and wrist): the one it holds still
// against (smoothed, with hysteresis), within kBraceletReach of it. Its attachment to that bone is taken from the game once
// it has held still there for kBraceletCalmFrames frames and then kept, so the game's arm animation (swings, running, a
// pass's other animation snapshot) never moves it on the VR arm. A draw near no arm bone (a cutscene's or a shop's bracelet)
// is not Link's worn one: left to the game.
// RX_VR_NO_BRACELET=1 turns this off (the game's pose, as before), for comparison.
constexpr float kBraceletReach = .20f;        // (m) the bracelet's centre this close to a game forearm/wrist bone: worn there
constexpr float kBraceletKeptReach = .60f;    // (m) ... or this close to its own bone, once its attachment is taken and it was
                                              // on the VR arm lately: a sword swing's fast arm put the draw 0.20 m from the
                                              // body upload, and the bracelet vanished from the VR wrist (headset 18 Nov)
constexpr float kBraceletCalm = .006f;        // (m, and about radians) a frame's change of its attachment: holding still
constexpr int kBraceletCalmFrames = 10;       // frames in a row holding still before the attachment is taken
constexpr int kBraceletSwitchFrames = 8;      // frames in a row another bone must be clearly stiller before it is the bone
constexpr float kBraceletStiller = .4f;       // "clearly stiller": its smoothed change under this fraction of the current one's
constexpr uint64_t kBraceletWornFrames = 120; // frames after its last VR pose that a failed pose still hides it near Link
constexpr int kBraceletBones = int(sizeof rig::bracelet_bones / sizeof rig::bracelet_bones[0]);
static const char* const kBraceletBoneName[4] = {"left forearm", "left wrist", "right forearm", "right wrist"};
static_assert(kBraceletBones == 4, "worn_items.py writes the two forearms and the two wrists");

bool bracelet_model(uint64_t vb0, uint64_t bytes) {
    if (bytes != rig::bracelet_matrices * 48ull) return false;
    for (uint64_t v : rig::bracelet_vb0) if (v == vb0) return true;
    return false;
}
static bool bracelet_enabled() {
    static const bool off = env_on("RX_VR_NO_BRACELET");
    return !off;
}
// The bracelet's state between draws (under cache_mu): its bone, the attachment taken, the frame computed last
struct BraceletState {
    int bone = -1, streak = 0, calm_streak = 0;
    bool have_last = false, fixed_have = false;
    Aff last[kBraceletBones]{}, fixed{};
    float still[kBraceletBones] = {1, 1, 1, 1};
    uint64_t frame = ~0ull, posed_frame = ~0ull;
    Aff frame_attach{};   // (this frame's attachment: every pass of a frame gets the same)
    V3 frame_at{};        // (and where this frame's worn bracelet is: a draw far from it is another bracelet)
};
static BraceletState bracelet_state;

// Why a draw of the worn bracelet got no VR pose (it is then left out near Link: a flicker on the VR wrist), counted and
// logged every 10 s while any happen, so a headset log says which test drops it (player, 18 Nov: it flickered during sword
// swings, the spin attack and Deku Stick swings; the old once-only lines had run out by then)
enum BraceletDrop { kDropNoRig, kDropNoUpload, kDropReach, kDropOtherDraw, kDropBad, kDropKinds };
static std::atomic<uint32_t> bracelet_placed{0}, bracelet_drops[kDropKinds]{};
static std::atomic<int64_t> bracelet_told_ms{0};
static std::shared_ptr<const std::vector<float>> bracelet_dropped(BraceletDrop why) {
    bracelet_drops[why].fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
static void bracelet_tell() {   // (every 10 s, only if a draw was dropped meanwhile)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t was = bracelet_told_ms.load(std::memory_order_relaxed);
    if (now - was < 10000 || !bracelet_told_ms.compare_exchange_strong(was, now)) return;
    uint32_t d[kDropKinds], any = 0;
    for (int k = 0; k < kDropKinds; k++) any += d[k] = bracelet_drops[k].exchange(0);
    const uint32_t placed = bracelet_placed.exchange(0);
    if (any)
        logf("vr: Goron Bracelet (10 s): %u draws on the VR arm, %u left out: no VR rig %u, no body upload %u, far from "
             "the game's arm %u, another draw's place %u, a bad matrix %u", placed, any, d[kDropNoRig], d[kDropNoUpload],
             d[kDropReach], d[kDropOtherDraw], d[kDropBad]);
}

// The bone segment's distance from a point (m), in the world, from a body upload's palette
static float bracelet_reach(const Entry& e, const rig::BraceletBone& B, V3 at) {
    const float* m = e.raw.data() + B.slot * 12;
    const V3 a = add(e.origin, point(m, v3(B.joint))), b = add(e.origin, point(m, v3(B.end)));
    const V3 ab = sub(b, a);
    const float t = std::clamp(dot(sub(at, a), ab) / std::max(dot(ab, ab), 1e-8f), 0.0f, 1.0f);
    return length(sub(at, add(a, mul(ab, t))));
}

std::shared_ptr<const std::vector<float>> bracelet_skin(const std::vector<float>& raw, V3 o, uint64_t frame,
                                                       const LinkBody& body) {
    if (!body.child || !bracelet_enabled() || raw.size() < 12) return nullptr;   // (adult Link wears gauntlets, not this)
    bracelet_tell();
    std::lock_guard lk(cache_mu);
    const Entry* vr = canonical_rig(frame, body);
    if (!vr || vr->adult || vr->out->size() < (size_t)rig::child.slots * 12) return bracelet_dropped(kDropNoRig);
    BraceletState& st = bracelet_state;
    constexpr uint64_t kBraceletForgetFrames = 300;   // (a gap this long without a draw: a scene change; its bone is found again)
    if (st.frame != ~0ull && (frame < st.frame || frame - st.frame > kBraceletForgetFrames)) st = BraceletState{};
    const Aff T = shifted(aff(raw.data()), o);   // (its Skl_Root in the world; the model sits about its origin)
    if (st.frame != frame) {   // (once per game frame: the bone and the attachment)
        // the game's body upload of this frame (or the canonical rig's source) that keeps the attachment most continuous
        const Entry* orig = nullptr;
        Aff rel[kBraceletBones];
        float best = 1e9f;
        for (const Entry& e : cache) {
            if (!e.out || e.adult || e.frame > frame || frame - e.frame > 2 || e.raw.size() < (size_t)rig::child.slots * 12) continue;
            Aff r[kBraceletBones], inv;
            bool ok = true;
            for (int k = 0; k < kBraceletBones && ok; k++) {
                if (!invert(shifted(aff(e.raw.data() + rig::bracelet_bones[k].slot * 12), e.origin), &inv)) ok = false;
                else r[k] = mul(inv, T);
            }
            if (!ok) continue;
            const float score = (st.have_last && st.bone >= 0 ? aff_change(r[st.bone], st.last[st.bone]) : 0.0f) +
                                (e.frame == frame ? 0.0f : .05f);
            if (score < best) {
                best = score, orig = &e;
                for (int k = 0; k < kBraceletBones; k++) rel[k] = r[k];
            }
        }
        if (!orig) return bracelet_dropped(kDropNoUpload);
        float reach[kBraceletBones];
        int nearest = 0;
        for (int k = 0; k < kBraceletBones; k++) {
            reach[k] = bracelet_reach(*orig, rig::bracelet_bones[k], T.t);
            if (reach[k] < reach[nearest]) nearest = k;
        }
        // (worn and attached: kept on its bone through a swing's fast arm, which its fixed attachment never follows anyway)
        const bool kept = st.fixed_have && st.bone >= 0 && st.posed_frame != ~0ull && st.posed_frame <= frame &&
                          frame - st.posed_frame < kBraceletWornFrames && reach[st.bone] < kBraceletKeptReach;
        if (!(reach[nearest] < kBraceletReach) && !kept) {   // (not on Link's arm: a cutscene's or a shop's bracelet)
            static std::atomic<int> told{0};
            if (told.fetch_add(1) < 6)
                logf("vr: Goron Bracelet draw %.2f m from the nearest game forearm/wrist: not worn, the game's pose (frame %llu)",
                     reach[nearest], (unsigned long long)frame);
            return bracelet_dropped(kDropReach);
        }
        if (st.have_last)
            for (int k = 0; k < kBraceletBones; k++) st.still[k] += (aff_change(rel[k], st.last[k]) - st.still[k]) * .35f;
        // its bone: at first (or out of reach of the current one) the nearest; then another one only when, for
        // kBraceletSwitchFrames frames running, it holds clearly stiller against that one (and is within reach of it)
        const bool lost = st.bone < 0 || (!(reach[st.bone] < kBraceletReach) && !kept);
        int want = lost ? nearest : st.bone;
        if (!lost)
            for (int k = 0; k < kBraceletBones; k++)
                if (k != st.bone && reach[k] < kBraceletReach && st.still[k] < st.still[want] * kBraceletStiller) want = k;
        st.streak = want != st.bone ? st.streak + 1 : 0;
        if (lost || st.streak >= kBraceletSwitchFrames) {
            if (want != st.bone) {
                static std::atomic<int> told{0};
                if (told.fetch_add(1) < 20)
                    logf("vr: Goron Bracelet rides the VR %s (%.3f m from the game's, frame %llu)", kBraceletBoneName[want],
                         reach[want], (unsigned long long)frame);
                st.fixed_have = false, st.calm_streak = 0;
            }
            st.bone = want, st.streak = 0;
        }
        // the attachment, FIXED once it has held still on its bone for kBraceletCalmFrames frames (whole, not blended);
        // until then the game's current one
        const float calm = st.have_last ? aff_change(rel[st.bone], st.last[st.bone]) : 1.0f;
        st.calm_streak = calm < kBraceletCalm ? st.calm_streak + 1 : 0;
        if (st.calm_streak >= kBraceletCalmFrames) {
            if (!st.fixed_have) {
                static std::atomic<int> told{0};
                if (told.fetch_add(1) < 20)
                    logf("vr: Goron Bracelet attachment to the %s taken (%.3f m from its joint, frame %llu)", kBraceletBoneName[st.bone],
                         length(rel[st.bone].t), (unsigned long long)frame);
            }
            st.fixed = rel[st.bone], st.fixed_have = true;
        }
        for (int k = 0; k < kBraceletBones; k++) st.last[k] = rel[k];
        st.have_last = true, st.frame = frame;
        st.frame_attach = st.fixed_have ? st.fixed : rel[st.bone];
        st.frame_at = T.t;
    }
    // (another bracelet drawn in the same frame, a cutscene's beside the worn one: the game's. Once its attachment is taken,
    // within kBraceletKeptReach is the worn one still: a later pass of the same frame can draw it from another snapshot of
    // the game's arm, which a swing moves further than kBraceletReach. That draw is placed like the first one: on the VR arm)
    const float same = st.fixed_have ? kBraceletKeptReach : kBraceletReach;
    if (st.bone < 0 || !(length(sub(T.t, st.frame_at)) < same)) return bracelet_dropped(kDropOtherDraw);
    // its Skl_Root at the attachment on the VR bone; every matrix of the model moved alike
    const Aff placed = mul(shifted(aff(vr->out->data() + rig::bracelet_bones[st.bone].slot * 12), vr->origin), st.frame_attach);
    Aff inv;
    if (!invert(T, &inv)) return bracelet_dropped(kDropBad);
    const Aff delta = mul(placed, inv);
    auto out = std::make_shared<std::vector<float>>(raw.size());
    for (size_t i = 0; i < raw.size() / 12; i++)
        store(out->data() + i * 12, shifted(mul(delta, shifted(aff(raw.data() + i * 12), o)), mul(o, -1.0f)));
    for (float f : *out) if (!std::isfinite(f)) return bracelet_dropped(kDropBad);
    st.posed_frame = frame;
    bracelet_placed.fetch_add(1, std::memory_order_relaxed);
    return out;
}
// Whether the bracelet was on the VR arm lately: a draw of it whose VR pose failed is then left out near Link (never drawn
// on the game's arm); one that never was (a cutscene's) is the game's
static bool bracelet_worn_lately(uint64_t frame) {
    std::lock_guard lk(cache_mu);
    const uint64_t at = bracelet_state.posed_frame;
    return bracelet_enabled() && at != ~0ull && at <= frame && frame - at < kBraceletWornFrames;
}

static std::shared_ptr<const std::vector<float>> equipment_skin_uncached(uint64_t bones, uint64_t bytes, uint64_t object,
                                                             uint64_t object_size, uint64_t vb0, uint64_t frame) {
    const bool combined = vb0==129384 || vb0==576624 || vb0==126552 || vb0==562248 || vb0==84720 || vb0==55272 ||
        vb0==127920 || vb0==565368 || vb0==49680 || vb0==125784 || vb0==560304 || vb0==69912 || vb0==34944;
    if (((!combined && (vb0 == 81384 || vb0 == 86544) && bytes == 14 * 48) ||
         (combined && bytes==10*48)) && bones && object && object_size >= 0x4c) {
        LinkBody body;
        if (!frame_body(frame, body) || !body.hand[body.bow_hand].valid) return nullptr;
        float o[3];
        auto out = std::make_shared<std::vector<float>>(bytes/4);
        if (!ub_read(object+0x40,o,sizeof o) || !ub_read(bones,out->data(),bytes)) return nullptr;
        for (float f : *out) if (!std::isfinite(f)) return nullptr;
        if (!std::isfinite(o[0]+o[1]+o[2])) return nullptr;
        const float* root = out->data();
        const V3 old_grip = point(root,kBowGrip);
        if (length(sub(add(old_grip,v3(o)),v3(body.root))) > 2.5f) return nullptr;
        if (!combined) {   // (the game's own bow pose this frame: what tells its nocked arrow apart, held_arrow below)
            std::lock_guard lk(bow_mu);
            native_bow = {frame, shifted(aff(root), v3(o))};
        }
        uint64_t seen = bow_model_frame.load(std::memory_order_relaxed);
        while (seen < frame && !bow_model_frame.compare_exchange_weak(seen, frame, std::memory_order_relaxed)) {}
        const BowPose b = bow_pose(frame, body);
        if (!b.ok) return nullptr;
        if (b.hidden) { std::fill(out->begin(), out->end(), 0.0f); return out; }   // (every vertex at one point: not drawn)
        if (!combined) {   // (the bow at rest: kBowRest, whatever the game's draw is doing)
            const Aff r0 = aff(root);
            for (unsigned i = 1; i < 14; i++) store(out->data() + i * 12, mul(r0, aff(kBowRest[i])));
        }
        const V3 a{root[0],root[4],root[8]}, bb{root[1],root[5],root[9]}, c{root[2],root[6],root[10]};
        const float d=dot(a,cross(bb,c));
        if (!std::isfinite(d) || std::fabs(d)<1e-8f) return nullptr;
        const V3 r0=mul(cross(bb,c),1/d),r1=mul(cross(c,a),1/d),r2=mul(cross(a,bb),1/d);
        const M3 inv={{{r0.x,r0.y,r0.z},{r1.x,r1.y,r1.z},{r2.x,r2.y,r2.z}}};
        const M3 delta=mul(b.target,inv);
        const V3 grip=sub(b.palm,v3(o));
        for (unsigned i=0;i<bytes/48;i++) {
            float* m=out->data()+i*12;
            for (int col=0;col<3;col++) {
                const V3 v=apply(delta,{m[col],m[4+col],m[8+col]});
                m[col]=v.x;m[4+col]=v.y;m[8+col]=v.z;
            }
            const V3 t=add(grip,apply(delta,sub(V3{m[3],m[7],m[11]},old_grip)));
            m[3]=t.x;m[7]=t.y;m[11]=t.z;
        }
        if (b.held) {
            // The string held: keep each string half's limb tip fixed and bring its centre to the drawing hand. A visual
            // deformation of the committed solution; the game's firing state is not driven from a render palette.
            const V3 nock = sub(b.nock,v3(o));
            if (dot(sub(nock,grip),b.forward) < -.02f) {
                for (int side=0;side<2;side++) {
                    const int slot=side ? 7 : 3;
                    const V3 tip_bind{.12951246f,0,side ? .54742098f : -.54742098f};
                    const V3 tip=point(out->data()+slot*12,tip_bind);
                    const V3 old_nock=point(out->data()+(slot+1)*12,kBowNock);
                    const V3 old_line=sub(old_nock,tip),new_line=sub(nock,tip);
                    const float len=length(old_line);
                    if (len>.01f) {
                        const M3 stretch=stretch_along(unit(old_line,{0,0,1}),std::clamp(length(new_line)/len,.25f,3.0f));
                        const M3 change=mul(from_to(old_line,new_line),stretch);
                        turn(out->data()+slot*12,change,tip);
                        turn(out->data()+(slot+1)*12,change,tip);
                    }
                }
            }
        }
        if (combined) {
            // These are the equipped bow+arrow assets, NOT the projectile's Item_Arrow_* model. Their shaft and elemental
            // tips are rigid slot 9; the bow/string use smooth slots 0..8. Thus nocking need not move any flying arrows.
            const V3 tail = sub(b.nock,v3(o));
            float* arrow=out->data()+9*12;
            const V3 t=sub(tail,apply(b.target,{.38f,0,0}));
            for(int row=0;row<3;row++) for(int col=0;col<3;col++) arrow[row*4+col]=b.target.v[row][col];
            arrow[3]=t.x;arrow[7]=t.y;arrow[11]=t.z;
        }
        return out;
    }
    if ((arrow_model(vb0) && bytes == 48) || tunic_model(vb0, bytes) || anchored_model(vb0, bytes) || hookshot_model(vb0, bytes) ||
        hookchain_model(vb0, bytes) || sling_model(vb0, bytes) || bracelet_model(vb0, bytes) || ocarina_model(vb0, bytes) ||
        ocarina_holder_model(vb0, bytes) || boomerang_model(vb0, bytes)) {
        if (!bones || !object || object_size < 0x4c) return nullptr;
        LinkBody body;
        if (!frame_body(frame, body)) return nullptr;
        float o[3];
        std::vector<float> raw(bytes / 4);
        if (!ub_read(object + 0x40, o, sizeof o) || !ub_read(bones, raw.data(), bytes)) return nullptr;
        for (float f : raw) if (!std::isfinite(f)) return nullptr;
        if (!std::isfinite(o[0] + o[1] + o[2])) return nullptr;
        if (const rig::ArrowShape* a = arrow_model(vb0)) {
            return held_arrow(raw.data(), v3(o), frame, body);
        }
        if (anchored_model(vb0, bytes)) return anchored_skin(vb0, raw, v3(o), frame, body);
        if (hookshot_model(vb0, bytes)) return held_hookshot(raw, v3(o), frame, body);
        if (boomerang_model(vb0, bytes)) return held_boomerang(raw, v3(o), frame, body);   // (vr/items/boomerang)
        if (hookchain_model(vb0, bytes)) return held_hookchain(raw, v3(o), body);
        if (sling_model(vb0, bytes)) return held_slingshot(raw, v3(o), frame, body);
        if (bracelet_model(vb0, bytes)) return bracelet_skin(raw, v3(o), frame, body);
        if (ocarina_model(vb0, bytes) || ocarina_holder_model(vb0, bytes))   // (vr/items/ocarina/held_ocarina.inc)
            return held_ocarina(raw, v3(o), frame, body, ocarina_holder_model(vb0, bytes));
        return tunic_skin(*tunic_model(vb0, bytes), raw, v3(o), frame, body);
    }
    // These are wearable armor models, not the pickup/treasure models. Their palettes are independent of Player_Adult.
    const bool gold = vb0 == 6592 || vb0 == 803660 || vb0 == 81360 || vb0 == 196096 || vb0 == 304896;
    const bool silver = vb0 == 734760 || vb0 == 72800 || vb0 == 263452 || vb0 == 4648;
    if ((!gold && !silver) || bytes != (gold ? 52u : 50u) * 48 || !bones || !object || object_size < 0x4c) return nullptr;
    LinkBody body;
    if (!frame_body(frame, body)) return nullptr;
    float o[3];
    auto out = std::make_shared<std::vector<float>>(bytes / 4);
    if (!ub_read(object + 0x40, o, sizeof o) || !ub_read(bones, out->data(), bytes)) return nullptr;
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    if (!std::isfinite(o[0] + o[1] + o[2])) return nullptr;
    std::lock_guard lk(cache_mu);
    {
        // Some depth/LOD draws arrive before the body's matching upload. Never pair mismatched ORIGINAL animation deltas.
        // Instead build the current VR pose, then convert its bind space to the armor's own bind space by bone name.
        const Entry* candidate = canonical_rig(frame,body);
        if (!candidate) return nullptr;
        std::vector<float> target = *candidate->out;
        static const int shared_fallback[48] = {
            5,7,7,8,8,9,10,11,9,14,15,16,18,19,20,23,24,25,26,28,29,30,31,33,
            46,48,48,49,49,50,51,52,50,55,56,57,59,60,61,64,65,66,67,69,70,71,72,74};
        const Entry* original_body=nullptr;
        float error_limit=.01f;
        const V3 armor_joint[4]={{.4165570f,1.2495954f,-.0386526f},{.6615570f,1.2495954f,-.0386489f},
                                 {-.4165570f,1.2495954f,-.0386526f},{-.6615570f,1.2495954f,-.0386489f}};
        const int armor_slot[4]={0,5,24,29};
        for (const Entry& e:cache) {
            if (!e.out || !e.adult || e.frame>frame || frame-e.frame>2) continue;
            float error=0;
            for(int j=0;j<4;j++) {
                const rig::Arm& arm=rig::adult.arm[j/2];
                const int slot=j%2?arm.wrist:arm.fore;
                const V3 joint=j%2?v3(arm.wrist_pos):v3(arm.elbow_pos);
                error=std::max(error,length(sub(add(e.origin,point(e.raw.data()+slot*12,joint)),
                    add(v3(o),point(out->data()+armor_slot[j]*12,armor_joint[j])))));
            }
            if(error<error_limit) error_limit=error,original_body=&e;
        }
        // The game's runtime attachment of each armor slot to its body slot (world: body slot^-1 * armor slot), kept from the
        // last upload that had a matching body upload. Without a match (a depth/LOD upload of another animation snapshot,
        // often while moving) it is reused, rather than the bind-space reconstruction, which sits ~3.7 cm off: switching
        // between the two from frame to frame was the gauntlets' flashing while moving.
        static Aff attach[2][52];
        static uint64_t attach_frame[2] = {0, 0};
        static bool attach_valid[2] = {false, false};
        const int which = gold ? 0 : 1;
        const auto body_slot = [&](unsigned i) {
            const int rigid_gold[4]={7,9,48,50};
            return i<48?shared_fallback[i]:gold?rigid_gold[i-48]:i==48?9:50;
        };
        if (original_body) {
            Aff inv;
            bool all = true;
            for (unsigned i = 0; i < bytes / 48 && all; i++) {
                if (!invert(shifted(aff(original_body->raw.data() + body_slot(i) * 12), original_body->origin), &inv)) all = false;
                else attach[which][i] = mul(inv, shifted(aff(out->data() + i * 12), v3(o)));
            }
            attach_valid[which] = all, attach_frame[which] = frame;
        }
        if (original_body) {
            // Preserve the game's actual attachment and bone-axis offsets. Bind-pose-only reconstruction loses these.
            // The destination is still the one canonical VR pose; this does NOT reintroduce per-pass IK solving.
            const V3 origin_delta=sub(v3(o),original_body->origin);
            for(unsigned i=0;i<bytes/48;i++) {
                const int slot=body_slot(i);
                const float* old=original_body->raw.data()+slot*12;
                const float* now=target.data()+slot*12;
                const V3 a{old[0],old[4],old[8]},b{old[1],old[5],old[9]},c{old[2],old[6],old[10]};
                const float det=dot(a,cross(b,c));
                if(!std::isfinite(det)||std::fabs(det)<1e-8f) return nullptr;
                const V3 r0=mul(cross(b,c),1/det),r1=mul(cross(c,a),1/det),r2=mul(cross(a,b),1/det);
                const M3 inv={{{r0.x,r0.y,r0.z},{r1.x,r1.y,r1.z},{r2.x,r2.y,r2.z}}};
                const M3 n={{{now[0],now[1],now[2]},{now[4],now[5],now[6]},{now[8],now[9],now[10]}}};
                const M3 delta=mul(n,inv);
                float* m=out->data()+i*12;
                for(int col=0;col<3;col++) {
                    const V3 v=apply(delta,{m[col],m[4+col],m[8+col]});m[col]=v.x;m[4+col]=v.y;m[8+col]=v.z;
                }
                // target and original uploads can have different floating world origins.
                const V3 target_shift=sub(candidate->origin,original_body->origin);
                const V3 t=sub(add(add(V3{now[3],now[7],now[11]},target_shift),apply(delta,
                    sub(add(V3{m[3],m[7],m[11]},origin_delta),V3{old[3],old[7],old[11]}))),origin_delta);
                m[3]=t.x;m[7]=t.y;m[11]=t.z;
            }
            return out;
        }
        if (attach_valid[which] && frame >= attach_frame[which] && frame - attach_frame[which] < 1800) {
            for (unsigned i = 0; i < bytes / 48; i++)
                store(out->data() + i * 12, shifted(mul(shifted(aff(target.data() + body_slot(i) * 12), candidate->origin),
                                                        attach[which][i]), mul(v3(o), -1.0f)));
            static std::atomic<int> kept{0};
            if (kept.fetch_add(1) % 600 == 0)
                logf("vr: gauntlets: no matching body upload, the last runtime attachment kept (%d times so far)", kept.load());
            return out;
        }
        const std::vector<float> original=*out;
        for (int i=0;i<48;i++) {
            const float* n=target.data()+shared_fallback[i]*12;
            const float* b=rig::armor_bind[i];
            float* m=out->data()+i*12;
            for (int row=0;row<3;row++) for (int col=0;col<4;col++) {
                m[row*4+col]=n[row*4]*b[col]+n[row*4+1]*b[4+col]+n[row*4+2]*b[8+col];
                if (col==3) m[row*4+col]+=n[row*4+3];
            }
            m[3]+=candidate->origin.x-o[0]; m[7]+=candidate->origin.y-o[1]; m[11]+=candidate->origin.z-o[2];
        }
        for (unsigned i=48;i<bytes/48;i++) {
            const int parent=gold ? (i==48 ? 1 : i==49 ? 8 : i==50 ? 25 : 32) : (i==48 ? 8 : 32);
            const float* old=original.data()+parent*12;
            const float* now=out->data()+parent*12;
            const V3 a{old[0],old[4],old[8]},b{old[1],old[5],old[9]},c{old[2],old[6],old[10]};
            const float det=dot(a,cross(b,c));
            if (std::fabs(det)<1e-8f || !std::isfinite(det)) return nullptr;
            const V3 r0=mul(cross(b,c),1/det),r1=mul(cross(c,a),1/det),r2=mul(cross(a,b),1/det);
            const M3 inv={{{r0.x,r0.y,r0.z},{r1.x,r1.y,r1.z},{r2.x,r2.y,r2.z}}};
            const M3 n={{{now[0],now[1],now[2]},{now[4],now[5],now[6]},{now[8],now[9],now[10]}}};
            const M3 delta=mul(n,inv);
            float* m=out->data()+i*12;
            for (int col=0;col<3;col++) {
                const V3 v=apply(delta,{m[col],m[4+col],m[8+col]}); m[col]=v.x;m[4+col]=v.y;m[8+col]=v.z;
            }
            const V3 t=add(V3{now[3],now[7],now[11]},apply(delta,sub(V3{m[3],m[7],m[11]},V3{old[3],old[7],old[11]})));
            m[3]=t.x;m[7]=t.y;m[11]=t.z;
        }
        static std::atomic<int> fallback_count{0};
        if (fallback_count.fetch_add(1)<12) logf("vr: gauntlet bind-space fallback frame %llu source %llu",(unsigned long long)frame,(unsigned long long)candidate->frame);
        return out;
    }
}

// A bomb drawn at the VR tab's Bomb size: its palette (Obj_Bomb: 2 smooth and 1 rigid matrix; its body 120288 and metal
// 110448 vertex bytes) scaled about the bomb's centre (Item_Bomb_Body_0's, 0.2187 m above its origin). The game's own
// bomb (its collision, its physics) keeps its size; vr/items/bomb/bomb_host.c holds it by the drawn size.
// A bomb in a VR hand is also drawn exactly at its seat against that hand's palm, as this frame's VR body has it (the
// same seat the ball grip's fingers rest on, arms_and_pose.inc). The game side places it a frame behind: walking, the
// game's bomb trailed the hand by about a frame's walk at any size (user report). Held, it collides with nothing, so only
// the picture needs to be in step.
// While Link's lift has the hand's bomb (taken out of the bag: bomb_host.c says so), the one drawn over his head is drawn
// in the palm: it shows in the hand at once, not over his head first (player report, 19 Nov). The bomb the gravity pull
// would take pulses (a tenth larger and back, twice a second): the pull's highlight is on the bomb itself.
// Not a Bomb Flower's fruit: its body is a rigid shape drawn without this palette, so changing the palette moved and sized
// only its fuse, the stem floating off the fruit (headset, 19 Nov). The game side places a held fruit itself, by its size
// (flower_shape, below).
constexpr float kBombCentre = .2187f;   // (m) above the bomb's origin
constexpr float kBombLock = .4f;        // (m) a bomb draw this close to a holding hand's seat is that hand's bomb,
constexpr float kBombFound = .35f;      // (m) or this close to where the game has the held bomb (anywhere: overhead too),
constexpr float kBombLifted = 1.0f;     // (m) or, while Link's lift has it, this close to his eyes (overhead)
constexpr float kBombPulled = .35f;     // (m) a bomb draw this close to the pull's target is that bomb (it pulses)
static std::shared_ptr<const std::vector<float>> bomb_drawn(uint64_t bones, uint64_t bytes, uint64_t object,
                                                           uint64_t object_size, uint64_t frame) {
    const float s0 = std::clamp(opts.vr_bomb_scale, 50, 150) * .01f;
    if (!bones || bytes != 3 * 48) return nullptr;
    float target[3];
    const bool pulse_maybe = pull_target(target) == 1;   // (a bomb, not a Bomb Flower: see above)
    // the seat of a held bomb, in the world (none: only the size changes)
    V3 seat{}, game_at{};
    bool held = false, known = false, lifted = false;
    float o[3];
    LinkBody body;
    const bool placed = object && object_size >= 0x4c && ub_read(object + 0x40, o, sizeof o) && std::isfinite(o[0] + o[1] + o[2]) &&
                        frame_body(frame, body);
    if (placed)
        for (int side = 0; side < 2 && !held; side++) {
            const float r = held_ball_radius(side);
            const LinkHand& in = body.hand[side];
            if (!(r > 0) || !in.valid || held_bomb_kind(side) != 1) continue;
            const float drawn_r = r;
            const M3 basis = mul(columns(v3(in.basis), v3(in.basis + 3), v3(in.basis + 6)), local_euler(body.wrist_deg[side]));
            seat = add(v3(in.position), mul(column(basis, 0), (side ? -1.0f : 1.0f) * (.03f + drawn_r)));   // (as Grip::ball)
            float g[3];
            if (held_ball_centre(side, g) && std::isfinite(g[0] + g[1] + g[2])) game_at = v3(g), known = true;
            lifted = held_bomb_lifted(side);
            held = true;
        }
    if (s0 == 1.0f && !held && !pulse_maybe) return nullptr;
    auto out = std::make_shared<std::vector<float>>(bytes / 4);
    if (!ub_read(bones, out->data(), bytes)) return nullptr;
    for (float f : *out) if (!std::isfinite(f)) return nullptr;
    float* p = out->data();
    const V3 centre = point(p, {0, kBombCentre, 0});   // (the first matrix moves the whole bomb: it is rigid)
    V3 shift{};
    float s = s0;
    if (held) {
        const V3 at = add(centre, v3(o)), by = sub(seat, at);
        if (length(by) < kBombLock || (known && length(sub(at, game_at)) < kBombFound) ||
            (lifted && length(sub(at, v3(body.eye))) < kBombLifted)) {
            shift = by;
            static std::atomic<int> told{0};
            if (lifted && length(by) > kBombLock && told.fetch_add(1) < 6)
                logf("vr: bombs: a bomb in Link's lift drawn in the palm (%.2f m from it)", length(by));
        }
    }
    if (pulse_maybe && placed && length(sub(add(centre, v3(o)), v3(target))) < kBombPulled) {   // (the pull's target pulses)
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        s *= 1.0f + .1f * (float)(.5 + .5 * std::sin(t * 4 * 3.14159265));
    }
    const int n = (int)(bytes / 48);
    for (int i = 0; i < n; i++) {
        float* m = p + 12 * i;
        for (int r = 0; r < 3; r++) {
            for (int k = 0; k < 3; k++) m[4 * r + k] *= s;
            const float c = r == 0 ? centre.x : r == 1 ? centre.y : centre.z, d = r == 0 ? shift.x : r == 1 ? shift.y : shift.z;
            m[4 * r + 3] = m[4 * r + 3] * s + c * (1 - s) + d;
        }
    }
    return out;
}
// A Bomb Flower's fruit (vk_vr_bomb_flower.h, from your game's model), for the game side's hold: its body's centre above
// the actor's origin (x y z, m) and its radius (host_api.inc hookshot_vr_flower_shape -> vr/items/bomb/bomb_host.c)
bool flower_shape(float out[4]) {
    for (int k = 0; k < 3; k++) out[k] = rig::flower_centre[k];
    out[3] = rig::flower_radius;
    return true;
}

// Which of Link's equipment models a draw is (0: none): 1 bow, 2 and 3 the other gear sets, 4 bow and quiver combined,
// 5/6 the Goron/Zora tunic, 7 an arrow, 8 a weapon anchored to a hand, 9/10 the hookshot and its chain, 11 the slingshot,
// 12 child Link's Goron Bracelet, 13 the ocarina, 14 its holder on the belt, 15 child Link's Boomerang (vr/items/boomerang:
// without it here its draws never reached held_boomerang, headset 18 Nov: "Link doesn't hold it in his hand").
// And the palette size that kind's draws bind.
static int equipment_kind(uint64_t vb0, uint64_t bytes) {
    int kind = arrow_model(vb0) ? 7 : anchored_model(vb0, bytes) ? 8 : hookshot_model(vb0, bytes) ? 9 :
               hookchain_model(vb0, bytes) ? 10 : sling_model(vb0, bytes) ? 11 : bracelet_model(vb0, bytes) ? 12 :
               ocarina_model(vb0, bytes) ? 13 : ocarina_holder_model(vb0, bytes) ? 14 : boomerang_model(vb0, bytes) ? 15 : 0;
    if (const rig::Tunic* T = tunic_model(vb0, bytes)) kind = T == &rig::goron ? 5 : 6;
    if (!kind) switch (vb0) {
    case 81384: case 86544: kind=1; break;
    case 6592: case 803660: case 81360: case 196096: case 304896: kind=2; break;
    case 734760: case 72800: case 263452: case 4648: kind=3; break;
    case 129384: case 576624: case 126552: case 562248: case 84720: case 55272:
    case 127920: case 565368: case 49680: case 125784: case 560304: case 69912: case 34944: kind=4; break;
    }
    return kind;
}
static uint64_t equipment_palette(int kind, uint64_t bytes) {
    return kind>=8 && kind<=15 ? bytes : kind==1 ? 14*48 : kind==2 ? 52*48 : kind==4 ? 10*48 : kind==7 ? 48 :
           kind==5 ? rig::goron.slots*48 : kind==6 ? rig::zora.slots*48 : 50*48;
}
// (vk_vr.h) Only the item: a gear model's own pose is not drawn from here, so Link's sheath, shield and belt items are
// still left out in first person (headset 21 Nov: his sheath showed in small chests' item gets, drawn here in the gear
// code's pose and so never reaching skin_draw.inc's worn-item hiding).
std::shared_ptr<const std::vector<float>> item_get_draw(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size,
                                                        uint64_t vb0, uint64_t frame) {
    return item_get_skin(bones, bytes, object, object_size, vb0, frame, equipment_kind(vb0, bytes) != 0);
}
std::shared_ptr<const std::vector<float>> link_equipment_skin(uint64_t bones, uint64_t bytes, uint64_t object,
                                                             uint64_t object_size, uint64_t vb0, uint64_t frame) {
    if (vb0 == 120288 || vb0 == 110448) return bomb_drawn(bones, bytes, object, object_size, frame);
    const int kind = equipment_kind(vb0, bytes);
    if (!kind && bytes == rig::hookshot_matrices * 48ull) {   // (a model the hookshot's size not recognised: which, bounded)
        static std::mutex seen_mu;
        static std::vector<uint64_t> seen;
        std::lock_guard lk(seen_mu);
        if (seen.size() < 24 && std::find(seen.begin(), seen.end(), vb0) == seen.end())
            seen.push_back(vb0), logf("vr: a %u-matrix model not taken for the hookshot: vb0 %llu", rig::hookshot_matrices,
                                      (unsigned long long)vb0);
    }
    if (!kind && bytes == 48 && bones && object && object_size >= 0x4c && held_sword_effects_live()) {
        // (a copy of the held sword, its charge glow: held_gear.inc held_sword_copy)
        float o[3];
        std::vector<float> raw(12);
        if (ub_read(object + 0x40, o, sizeof o) && ub_read(bones, raw.data(), bytes) && std::isfinite(o[0] + o[1] + o[2]))
            if (auto copy = held_sword_copy(vb0, raw, v3(o))) return copy;
    }
    if (!kind) return item_get_skin(bones, bytes, object, object_size, vb0, frame);   // (vr/items/item_get: or nothing)
    if (kind >= 8 && item_get_live() && bytes > kItemGetMatrices * 48ull) item_get_refused("a held item's model with more than 8 matrices");
    else if (item_get_live())   // (an item get's item may be a gear model: vr/items/item_get)
        if (auto got = item_get_skin(bones, bytes, object, object_size, vb0, frame, true)) return got;
    LinkBody body;
    if (!frame_body(frame,body)) return nullptr;
    static std::mutex mu;
    struct EquipmentEntry {
        uint64_t frame=~0ull, used=0;
        int kind=0;
        uint32_t seq=0;
        V3 origin{};
        std::vector<float> raw;
        std::shared_ptr<const std::vector<float>> out;
    };
    static EquipmentEntry items[48];
    static uint64_t clock=0;
    static std::atomic<int> rejected{0};
    const uint64_t wanted = equipment_palette(kind, bytes);
    if (bytes!=wanted || !bones || !object || object_size<0x4c) {
        if (kind>=7 && kind<=15) return nullptr;   // (another representation: not the recorded one)
        if (rejected.fetch_add(1)<24) logf("vr: equipment signature rejected kind %d vb %llu palette %llu expected %llu frame %llu",
            kind,(unsigned long long)vb0,(unsigned long long)bytes,(unsigned long long)wanted,(unsigned long long)frame);
        return nullptr;
    }
    float o[3];
    std::vector<float> raw(bytes/4);
    if (!ub_read(object+0x40,o,sizeof o) || !ub_read(bones,raw.data(),bytes)) return nullptr;
    std::lock_guard lk(mu);
    for (EquipmentEntry& e:items)
        if (e.out && e.frame==frame && e.seq==body.pose_seq && e.kind==kind &&
            length(sub(e.origin,v3(o)))<1e-6f && e.raw==raw) {
            e.used=++clock;
            return e.out;
        }
    auto out=equipment_skin_uncached(bones,bytes,object,object_size,vb0,frame);
    if (!out) {
        if (kind>=7 && kind<=15) return nullptr;   // (an arrow not on Link's bow; a weapon, the hookshot, the bracelet or the
                                                   // ocarina with no VR pose this frame: the game's own, or left out by
                                                   // link_worn_hidden)
        if (rejected.fetch_add(1)<24) logf("vr: equipment pose unavailable kind %d frame %llu pose %u vb %llu",
                                         kind,(unsigned long long)frame,body.pose_seq,(unsigned long long)vb0);
        return nullptr;
    }
    EquipmentEntry* slot=&items[0];
    for (EquipmentEntry& e:items) if (e.used<slot->used) slot=&e;
    slot->frame=frame;slot->seq=body.pose_seq;slot->kind=kind;slot->origin=v3(o);
    slot->raw=std::move(raw);slot->out=out;slot->used=++clock;
    return out;
}

// A draw whose model stands where the frame's first-person body does (its object's origin within 1.5 m of his root): the
// player's own Link, not a cinematic's model of him or the pause menu's (vr/body/skin_draw.inc, his hair)
bool link_draw_near_body(uint64_t object, uint64_t object_size, uint64_t frame) {
    LinkBody body;
    float o[3];
    if (!object || object_size < 0x4c || !frame_body(frame, body) || !ub_read(object + 0x40, o, sizeof o) ||
        !std::isfinite(o[0] + o[1] + o[2]))
        return false;
    return length(sub(v3(o), v3(body.root))) < 1.5f;
}

// First person: a draw of something Link wears that has no VR pose this frame, near Link, is left out rather than drawn
// in the game's pose (where it floats off the VR body). That is Link's own gear when its VR pose failed (not arrows: an
// arrow away from the bow is a flying one; the Goron Bracelet only when it was on the VR arm lately, so a cutscene's is
// kept), and the belt items, boots, glasses and masks (rig::worn_hidden, generated from your game's models), which have no
// VR pose yet.
bool link_worn_hidden(uint64_t bones, uint64_t bytes, uint64_t object, uint64_t object_size, uint64_t vb0, uint64_t frame) {
    constexpr float kNearLink = 1.5f;   // metres from Link's root: further away it is not his (a shop's, a chest's)
    if (!bones || !object || object_size < 0x4c || bytes % 48) return false;
    const int kind = equipment_kind(vb0, bytes);
    // (not an arrow nor the Boomerang: one away from its hand is flying, or coming back to be caught)
    bool worn = kind && kind != 7 && kind != 15 && bytes == equipment_palette(kind, bytes);
    if (kind == 12) worn = bracelet_worn_lately(frame);   // (the Goron Bracelet: only one that was on the VR arm lately)
    if (!kind)
        for (const rig::Worn& w : rig::worn_hidden)
            if (w.vb0 == vb0 && (uint64_t)w.matrices * 48 == bytes) { worn = true; break; }
    if (!worn) return false;
    LinkBody body;
    float o[3];
    if (!frame_body(frame, body) || !ub_read(object + 0x40, o, sizeof o) || !std::isfinite(o[0] + o[1] + o[2])) return false;
    return length(sub(v3(o), v3(body.root))) < kNearLink;
}
