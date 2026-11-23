// Hookshot VR: Tracked controllers converted into the game world, equipment state and wrist calibration.
// Included by vr/headset/vk_vr.cpp in the order shown there; not a separate compilation unit.

// ---- (BODY-11) which game frame's body a replayed frame is posed with ----------------------------------------------------
// The game builds frame N's camera (and tells its body: hookshot_vr_body) and goes on to frame N+1 while the replay is still
// recording frame N, up to two frames behind. Link's VR pose is placed in the game world from the body and the camera pose
// (the eye, the hands through that camera); drawn with frame N's camera, a body from N+1 or N+2 sits ahead of the view by
// the distance Link moved meanwhile, a different amount from frame to frame: his body jittered while he walked and stood
// still when he did. So each replayed frame takes the body told with ITS camera: the scene's camera of that frame is matched
// to the drawn pose it was built with (note_scene_draw -> note_frame_camera, the same match the headset's warp uses for the
// head, drawn_pose), and that pose's body is used. Link's draws before the scene's first draw of a frame (its shadow
// passes) take the body one camera update after the last matched frame's per frame since (they come one per game frame);
// once the frame's own camera is seen, frame_body re-poses with it if the guess was wrong. Debug tab "Body pose timing"
// set to Latest: the latest body for every frame, as before.

/// The 10 s report's counts (body_timing_report)
static std::atomic<int64_t> timing_lag[3]{};   // matched frames whose body was 0, 1, 2 or more camera updates behind the newest
static std::atomic<int64_t> timing_how[3]{};   // bodies handed out: 0 the frame's own, 1 guessed from the last match, 2 the latest

static const BodySample* body_by_gen(uint64_t gen) {   // (s.pose_mu held)
    const BodySample& b = s.bodies[gen % kBodies];
    return gen && b.gen == gen ? &b : nullptr;
}

/// (frame_capture.inc note_scene_draw, once a frame) the frame's scene camera: the drawn pose it was built with, and its body
void note_frame_camera(uint64_t frame, const float vp[16], const float proj[16]) {
    XrPosef pose;
    uint32_t seq = 0;
    if (!drawn_pose(vp, proj, &pose, nullptr, &seq) || !seq) return;
    std::lock_guard lk(s.pose_mu);
    const BodySample* found = nullptr;
    for (const BodySample& b : s.bodies)
        if (b.gen && b.seq == seq && (!found || b.gen > found->gen)) found = &b;
    if (!found) return;
    s.matched_frame = frame, s.matched_gen = found->gen, s.matched_seq = seq;
    const uint64_t behind = s.body_gen - found->gen;
    timing_lag[behind < 2 ? behind : 2].fetch_add(1, std::memory_order_relaxed);
}

/// The body for a replayed frame (s.pose_mu held): how 0 its own (its camera matched), 1 guessed, 2 the latest
static const BodySample* body_for_frame(uint64_t frame, int* how) {
    constexpr uint64_t kGuessFrames = 8;   // (frames after the last match a guess is still made: beyond, a load or a cutscene)
    if (!opts.vr_body_latest && s.matched_frame != ~0ull) {
        if (s.matched_frame == frame) {
            if (const BodySample* b = body_by_gen(s.matched_gen)) return *how = 0, b;
        } else if (frame > s.matched_frame && frame - s.matched_frame <= kGuessFrames) {
            const uint64_t gen = std::min(s.matched_gen + (frame - s.matched_frame), s.body_gen);
            if (const BodySample* b = body_by_gen(gen)) return *how = 1, b;
        }
    }
    *how = 2;
    return body_by_gen(s.body_gen);
}

/// The body sample's LinkBody (s.pose_mu held)
static bool link_body_locked(const BodySample* b, LinkBody& out);

bool link_body_for_frame(uint64_t frame, LinkBody& out, int* how) {
    out = {};
    *how = 2;
    if (!active() || !opts.vr_arms || opts.vr_mode != 1 || !ctl.attached ||
        !s.camera_owned.load(std::memory_order_acquire))
        return false;
    std::lock_guard lk(s.pose_mu);
    const BodySample* b = body_for_frame(frame, how);
    if (!link_body_locked(b, out)) return false;
    timing_how[*how].fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool link_body_shown(uint64_t frame) {
    if (!active() || !opts.vr_arms || opts.vr_mode != 1 || !ctl.attached || !s.camera_owned.load(std::memory_order_acquire))
        return false;
    std::lock_guard lk(s.pose_mu);
    int how = 2;
    const BodySample* b = body_for_frame(frame, &how);
    return b && b->active && b->seq;
}

/// (frame_loop.inc, every 10 s) how first-person bodies were paired with their frames, when there were any
void body_timing_report() {
    int64_t lag[3], how[3];
    for (int k = 0; k < 3; k++) lag[k] = timing_lag[k].exchange(0), how[k] = timing_how[k].exchange(0);
    if (!(lag[0] + lag[1] + lag[2] + how[0] + how[1] + how[2])) return;
    logf("vr: body timing (10 s): frames whose scene camera was matched to its pose: %lld, the game 0 frames ahead of the "
         "replay %lld, 1 ahead %lld, 2+ ahead %lld; frame bodies %lld its own, %lld guessed, %lld the latest%s",
         (long long)(lag[0] + lag[1] + lag[2]), (long long)lag[0], (long long)lag[1], (long long)lag[2], (long long)how[0],
         (long long)how[1], (long long)how[2], opts.vr_body_latest ? " (Debug tab: Body pose timing Latest)" : "");
}

bool link_body(LinkBody& out) {
    out = {};
    if (!active() || !opts.vr_arms || opts.vr_mode != 1 || !ctl.attached ||
        !s.camera_owned.load(std::memory_order_acquire))
        return false;
    std::lock_guard lk(s.pose_mu);
    return link_body_locked(body_by_gen(s.body_gen), out);   // (the game side's questions: the newest body)
}

static bool link_body_locked(const BodySample* b, LinkBody& out) {
    if (!b || !b->active || !b->seq) return false;
    const Pose& p = s.poses[b->seq % kPoses];
    if (p.seq != b->seq || !p.drawn) return false;

    out.active = true;
    out.pose_seq = b->seq;
    out.child = b->child;
    if (const EquipmentSample& e = s.equipment[b->equipment % kBodies]; b->equipment && e.gen == b->equipment) {
        out.equipment_known = e.known;
        out.equipment_drawn = e.drawn;
        std::memcpy(out.equipment_owner, e.owner, sizeof out.equipment_owner);
    }
    std::memcpy(out.root, b->root, sizeof out.root);
    std::memcpy(out.right, b->right, sizeof out.right);
    std::memcpy(out.back, b->back, sizeof out.back);
    const int wrist_deg[2][3] = {
        {opts.vr_wrist_l_pitch, opts.vr_wrist_l_yaw, opts.vr_wrist_l_roll},
        {opts.vr_wrist_r_pitch, opts.vr_wrist_r_yaw, opts.vr_wrist_r_roll},
    };
    for (int h = 0; h < 2; h++)
        for (int a = 0; a < 3; a++) out.wrist_deg[h][a] = (float)wrist_deg[h][a];
    // The rows of the game view matrix are the headset's right, up and back axes in game-world coordinates.
    const float eye[3] = {
        -(p.view[0] * p.view[3] + p.view[4] * p.view[7] + p.view[8] * p.view[11]),
        -(p.view[1] * p.view[3] + p.view[5] * p.view[7] + p.view[9] * p.view[11]),
        -(p.view[2] * p.view[3] + p.view[6] * p.view[7] + p.view[10] * p.view[11]),
    };
    for (int k = 0; k < 3; k++) out.head_back[k] = p.view[8 + k];   // (the view's third row: the headset's back axis)
    std::memcpy(out.eye, eye, sizeof out.eye);
    for (int k = 0; k < 3; k++) out.head_up[k] = p.view[4 + k];
    out.arm_scale = std::clamp(opts.vr_arm_length, 80, 160) * 0.01f;
    out.bow_hand = 0;   // (the left hand until a grip takes the bow, below)
    out.bow_scale = std::clamp(opts.vr_bow_scale, 80, 200) * .01f;
    out.bow_grip[0] = out.bow_grip[1] = out.bow_grip[2] = 0;   // (the bow's grip: the palm socket as authored)
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    out.bow_equipped = bow_active_at.load() && now-bow_active_at.load()<150 && !s.menu_visible.load();
    out.sling = out.bow_equipped && bow_is_sling.load(std::memory_order_relaxed);
    {   // The first grip squeezed while the bow is out takes it; the other hand draws. Kept until the bow is put away.
        const int64_t out_at = bow_active_at.load();
        int taken = bow_hand_now.load(std::memory_order_relaxed);
        if (!out_at || now - out_at > 1000) {
            if (taken >= 0) bow_hand_now.store(-1, std::memory_order_relaxed);
        } else if (taken < 0) {
            const bool l = ctl.shared[0].valid && ctl.shared[0].squeeze > .55f, r = ctl.shared[1].valid && ctl.shared[1].squeeze > .55f;
            if (l != r && bow_hand_now.compare_exchange_strong(taken, r ? 1 : 0)) {
                taken = r ? 1 : 0;
                logf("vr: bow taken in the %s hand", r ? "right" : "left");
            }
        }
        if (taken >= 0) out.bow_hand = taken;
    }
    {   // the hookshot's hand (the game side sends it every game frame while a VR hand holds it out)
        const int64_t at = hook_active_at.load();
        out.hook_hand = at && now - at < 250 && !s.menu_visible.load() ? hook_hand_now.load(std::memory_order_relaxed) : -1;
        // (out, and for 0.6 s after: the game shows the shield it held out with the hookshot for a moment after the put-away
        // starts; it flashed in the hand, 17 Nov)
        const int64_t seen = hook_seen_at.load();
        out.hook_out = seen && now - seen < 600 && !s.menu_visible.load();
    }
    {   // The shield the game draws beside an item a VR hand holds: kept out of the VR hands, as with the hookshot. The game
        // holds the Deku Stick and the Boomerang one-handed with the shield out (playthrough log 22 Nov: the stick's own draw
        // "drawn 0x2", put away "drawn slots 0x12"; the Boomerang's put-away "drawn slots 0x10", the shield's slot alone), and a
        // drawn shield goes to the right VR wrist: it popped up in a hand at the grip that took the item (the user, 22 Nov).
        // The shield goes to a VR hand only beside the sword taken from the back (held_gear.inc). Every held item counts, so
        // one the game holds beside the shield and not yet seen (a bottle's swing, the ocarina) does the same. Kept 0.6 s
        // after the hand lets go: the game puts the shield away with the item, a moment after (the hookshot's, above).
        static std::atomic<int64_t> held_seen{0};
        const bool sword = sword_hand() >= 0;
        if (!sword && (stick_hand() != -1 || boomerang_hand() != -1 || bottle_hand() != -1 || ocarina_hand() >= 0 ||
                       held_bomb_kind(0) || held_bomb_kind(1) || out.bow_equipped))
            held_seen.store(now, std::memory_order_relaxed);
        const int64_t held = held_seen.load(std::memory_order_relaxed);
        out.shield_away = out.hook_out || (!sword && held && now - held < 600 && !s.menu_visible.load());
    }
    out.menu = s.menu_visible.load();
    {   // (as host/hos_main.c places the first-person eye before the head's own movement: root + height + depth along back;
        // crawling, that much lower, as vr/camera/view_host.c lowers it)
        const int adult_cm = opts.vr_height > 0 ? opts.vr_height : 160;   // (child Link's scaled from it, as hos_main.c's)
        const int height_cm = b->child ? adult_cm * 125 / 160 : adult_cm;
        out.crawl = b->crawl, out.lowered = b->lowered;
        for (int k = 0; k < 3; k++)
            out.eye_rest[k] = b->root[k] + (k == 1 ? height_cm * 0.01f - b->lowered : 0.0f) + b->back[k] * opts.vr_body_depth * 0.01f;
    }
    const auto world_vec = [&](const float v[3], float w[3]) {
        for (int k = 0; k < 3; k++) w[k] = p.view[k] * v[0] + p.view[4 + k] * v[1] + p.view[8 + k] * v[2];
    };
    const XrQuaternionf head_inv = qinv(p.pose.orientation);
    for (int h = 0; h < 2; h++) {
        const HandState& in = ctl.shared[h];
        LinkHand& hand = out.hand[h];
        if (!in.valid) continue;
        const float dp[3] = {in.grip.position.x - p.pose.position.x, in.grip.position.y - p.pose.position.y,
                             in.grip.position.z - p.pose.position.z};
        float local[3], delta[3];
        qrot(head_inv, dp, local);
        world_vec(local, delta);
        for (int k = 0; k < 3; k++) hand.position[k] = eye[k] + delta[k];
        {   // the controller's velocity: the same turn into the game world as its position (no offset)
            float vlocal[3];
            qrot(head_inv, in.lin_vel, vlocal);
            world_vec(vlocal, hand.velocity);
            qrot(head_inv, in.ang_vel, vlocal);
            world_vec(vlocal, hand.angular_velocity);
        }

        const XrQuaternionf relative = qmul(head_inv, in.grip.orientation);
        for (int axis = 0; axis < 3; axis++) {
            float unit[3] = {}, turned[3], world[3];
            unit[axis] = 1.0f;
            qrot(relative, unit, turned);
            world_vec(turned, world);
            for (int k = 0; k < 3; k++) hand.basis[axis * 3 + k] = world[k];
        }
        {   // the aim pose, the same way (the hookshot's line, vr/items/hookshot)
            const XrQuaternionf aim = qmul(head_inv, in.aim.orientation);
            const float ahead[3] = {0, 0, -1}, up[3] = {0, 1, 0};
            const float da[3] = {in.aim.position.x - p.pose.position.x, in.aim.position.y - p.pose.position.y,
                                 in.aim.position.z - p.pose.position.z};
            float turned[3];
            qrot(aim, ahead, turned), world_vec(turned, hand.aim);
            qrot(aim, up, turned), world_vec(turned, hand.aim_up);
            qrot(head_inv, da, turned), world_vec(turned, delta);
            for (int k = 0; k < 3; k++) hand.aim_at[k] = eye[k] + delta[k];
        }
        hand.trigger = std::clamp(in.trigger, 0.0f, 1.0f);
        hand.squeeze = std::clamp(in.squeeze, 0.0f, 1.0f);
        hand.valid = true;
    }
    return true;   // head hiding remains stable through a momentary controller tracking loss
}

bool native_bow_reticle_hidden() {
    // The game's own screen reticle is hidden in first person whenever the bow is out (the host's bow-out state: taking it
    // out, raised, drawn, fired, idle in hand), whatever the headset reticle, tracking or the aim solution are doing. It is
    // drawn flat on the picture, never where the headset looks. (Before: only while the game aimed AND a fresh solution
    // existed, so the idle bow showed it.)
    // Third person: hidden while the headset's third-person reticle shows where the shot lands (vr/items/aim): the game's
    // sits at the middle of the picture, where the head looks, and the shot goes along the game camera's line instead.
    if (opts.vr_mode==0) {   // (the Aim reticle row off, or the Window view (the game camera's own picture): the game's own stays)
        float at[3];
        return opts.vr_bow_preview && !opts.vr_window && s.camera_owned.load() && !s.menu_visible.load() && third_person_aim(at) != 0;
    }
    if (opts.vr_mode!=1 || !opts.vr_arms || !s.camera_owned.load() || s.menu_visible.load()) return false;
    const int64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t seen=bow_active_at.load(), hook=hook_active_at.load();   // (the hookshot's too: the same reticle)
    return (seen && now-seen<250) || (hook && now-hook<250);
}

bool calibrate_wrists() {
    LinkBody body;
    if (!link_body(body) || !body.hand[0].valid || !body.hand[1].valid) {
        logf("vr: wrist calibration needs both tracked controllers in first person");
        return false;
    }
    const auto dot3 = [](const float a[3], const float b[3]) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; };
    const auto normalise = [&](float v[3], const float fallback[3]) {
        const float n = std::sqrt(std::max(dot3(v, v), 0.0f));
        if (n > 1e-6f) for (int k = 0; k < 3; k++) v[k] /= n;
        else std::memcpy(v, fallback, 3 * sizeof(float));
    };
    const auto cross3 = [](const float a[3], const float b[3], float out[3]) {
        out[0] = a[1]*b[2] - a[2]*b[1];
        out[1] = a[2]*b[0] - a[0]*b[2];
        out[2] = a[0]*b[1] - a[1]*b[0];
    };
    float right[3] = {body.right[0], 0, body.right[2]}, back[3] = {body.back[0], 0, body.back[2]};
    const float right_fallback[3] = {1, 0, 0}, back_fallback[3] = {0, 0, 1};
    normalise(right, right_fallback);
    const float rb = dot3(right, back);
    for (int k = 0; k < 3; k++) back[k] -= right[k] * rb;
    normalise(back, back_fallback);
    // (body v2) Link's right is the camera's right and he faces the camera's forward (vk_vr_body.cpp).

    int captured[2][3]{};
    constexpr float rad_to_deg = 57.29577951308232f;
    for (int h = 0; h < 2; h++) {
        // Neutral: arms forward, palms facing each other, thumbs up.
        float want[3][3]{};
        // OpenXR's grip: +x is the palm's normal (pointing right for either hand, palms in), -z runs through the closed fist
        // (pinky to thumb: up, thumbs up), +y = z cross x points back (the fingers wrap forward, along -y).
        float up[3];
        cross3(back, right, up);
        for (int k = 0; k < 3; k++) want[0][k] = right[k], want[2][k] = -up[k];
        cross3(want[2], want[0], want[1]);
        const float y_fallback[3] = {back[0], back[1], back[2]};
        normalise(want[1], y_fallback);

        // C = raw_basis^T * desired_basis. Applying raw_basis*C thereafter preserves all controller motion while making this
        // held pose the exact neutral Link wrist pose.
        float c[3][3]{};
        for (int row = 0; row < 3; row++)
            for (int col = 0; col < 3; col++)
                for (int k = 0; k < 3; k++) c[row][col] += body.hand[h].basis[row*3+k] * want[col][k];
        const float yaw = std::asin(std::clamp(-c[2][0], -1.0f, 1.0f));
        const float cy = std::cos(yaw);
        const float pitch = std::fabs(cy) > 1e-5f ? std::atan2(c[2][1], c[2][2]) : 0.0f;
        const float roll = std::fabs(cy) > 1e-5f ? std::atan2(c[1][0], c[0][0]) : std::atan2(-c[0][1], c[1][1]);
        captured[h][0] = (int)std::lround(pitch * rad_to_deg);
        captured[h][1] = (int)std::lround(yaw * rad_to_deg);
        captured[h][2] = (int)std::lround(roll * rad_to_deg);
    }
    opts.vr_wrist_l_pitch = captured[0][0], opts.vr_wrist_l_yaw = captured[0][1], opts.vr_wrist_l_roll = captured[0][2];
    opts.vr_wrist_r_pitch = captured[1][0], opts.vr_wrist_r_yaw = captured[1][1], opts.vr_wrist_r_roll = captured[1][2];
    logf("vr: wrists calibrated: left %d/%d/%d deg, right %d/%d/%d deg", captured[0][0], captured[0][1], captured[0][2],
         captured[1][0], captured[1][1], captured[1][2]);
    return true;
}
