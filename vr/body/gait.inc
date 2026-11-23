// Hookshot VR: first-person legs that step the way Link travels (the VR tab's "Legs").
//
// First person turns Link's body to the VR direction, but the game still runs him forward along the stick: moving sideways
// or backwards, his legs ran forward while the body slid (and backwards they moonwalked). Here the hips and legs are posed
// from the game's own cycles instead (vk_vr_gait.h, made from the player's files by vr/tools/generate/leg_gait.py):
// Move_Walk/Jog/Run ahead, the lock-on side, diagonal and back steps the other ways, blended by the direction of travel
// relative to the VR torso and by speed. Every clip is played at one shared phase, lined up on the left foot's stance, and
// the phase advances by the ground actually covered, so the feet keep pace with the ground. Only the drawn legs change: the
// game's movement and speed are its own.
//
// A piece of vr/body/arms_and_pose.inc, included there (it uses math.inc). Not compiled on its own.
//
// Pure functions of their arguments, so vr/tools/test.py can check them without the game: gait_step advances a GaitState,
// gait_pose poses the hips and legs in Link's model space (+x his left, +y up, +z ahead; metres from his feet).

// How fast a cycle may play, as a multiple of its own pace: past this (running sideways at the game's full run speed, faster
// than the lock-on side-step was made for) the feet slide a little rather than the legs blurring.
constexpr float kGaitMaxPace = 1.35f;
// Speeds (m/s) between which the walking cycles give way to the running ones, and the forward jog to the run
constexpr float kGaitWalkTo = 1.4f, kGaitRunFrom = 2.6f, kGaitJogTo = 3.0f, kGaitRunTo = 4.8f;
// Everything here is measured on the game's own drawn body, never on Link's position in the game's physics: on slopes that
// jumps 0.35-0.4 m up and down against the body every stride or two (the third build's log: the game's pelvis 0.81, then
// 1.17, then 0.81 m above it, running downhill), so legs standing on it pulled apart from the torso, a leg up in the air,
// and its false "both feet off the ground" sent the legs back to the game's on every slope.
//
// (m) the game's lower foot (ankle) at least this far below its pelvis is a leg reaching the ground: its own walks and runs
// hold it 0.57-0.79 m below (Move_Run, the lock-on steps, Move_Run_D30/U30 on slopes); its running jump tucks to 0.46.
constexpr float kGaitReach = .5f;
// (s) the game's legs tucked this long is a jump or a flip, not a stride. (A longer grace, 0.3 s in the first build, let
// the stepping legs run through the first half of a jump.)
constexpr float kGaitAirborne = .15f;
// (s) after a landing, back on the ground this long before the legs step again (the game's own landing plays first)
constexpr float kGaitLanded = .2f;
// How fast the legs change over (per second): out at once, in gently. A half-way blend of two very different leg poses
// (a jump's tucked legs and a stride) bends a leg the wrong way, so it is kept off screen when leaving.
constexpr float kGaitLegsOut = 25.0f, kGaitLegsIn = 8.0f;
// Standing still, Link's position in the game this steady (m/s up or down, and over the ground) for this long (s): if the
// game's legs then look like no standing pose (turned over, or tucked: a backflip's pose kept after the landing, which only
// switching to third person used to clear), the still standing frame covers them.
// (Measured frame to frame, not on the smoothed speed, which takes about 0.4 s to settle after a 5 m/s backflip: the
// sixth build covered the stuck leg only about a second after the landing.)
constexpr float kGaitSteady = .3f, kGaitSteadyFor = .12f;
// What counts as no standing pose, standing still: the pelvis turned over (kGaitTiltMax), the lower foot tucked
// (kGaitReach), either foot raised this high (m below the pelvis: the game's Wait holds both 0.79 m down; the stuck
// pose of the fifth build held one 0.44 m down), or the game's leg pose unchanged this long (s): the game stops updating
// it after some backflips in first person (the log: unchanged for 1 s and on, no breathing), and switching to third
// person cleared it. The cover lets go only after kGaitCoverHold with none of these, so a pose alternating frame to
// frame (seen: 76 and 12 degrees, frame after frame) does not flicker it.
constexpr float kGaitFootRaised = .6f, kGaitFrozen = .15f, kGaitCoverHold = .5f;
// A backflip or a roll (the game's pelvis turned over, kGaitTiltMax): the whole lower body (hips, legs, skirt, belt) is the
// running jump's tucked frame, upright under the torso, switched in and out at this rate (per second: about 40 ms). The
// game's flip is right, but the view and the torso stay upright in first person, so its legs and skirt swinging over
// Link's head came up through the view ("a foot to the face"). Shrinking only the legs away (tried first) did not do:
// the game's leg still pushed the skirt up into the view.
constexpr float kGaitTuckRate = 25.0f;
// How deep the tuck is: this much of the running jump's knees-up frame, the rest the standing frame. All of it (the first
// tuck build) put the raised knee through the tunic: the skirt in that one frame is not shaped around a knee held there.
constexpr float kGaitTuckDepth = .6f;

// (m/s, the game's pelvis) rising faster than this is a jump's take-off; falling faster, a fall. Running down a slope is
// neither: the game's full run (5.3 m/s) down 45 degrees falls 3.7 m/s. (The first build stopped at 2.5 m/s either way, and
// measured it on the jumping physics position.)
constexpr float kGaitRiseMax = 2.5f, kGaitFallMax = 4.5f;

// (radians) the game's pelvis leaning further than this from upright (its up axis against the world's) is a roll or a flip.
// Measured on the up axis: the first builds measured it against the pelvis's level forward, which flips over with Link
// upside down in a backflip, reading 168, 4, 173 degrees on three frames running, and the 4 let the legs step mid-flip.
constexpr float kGaitTiltMax = 55 * kDeg;

// Whether Link is on the ground at a walk or a run, from the game's own drawn body: reach is how far its lower foot is
// below its pelvis (m), rise its pelvis's vertical speed (m/s, up), tilt how far its pelvis leans from upright (radians).
// After anything that is not a stride (a jump, a fall, a roll or a flip) it is a stride again only after kGaitLanded of
// strides: one frame that looks like one, mid-flip, is not enough. why: what stops it, or "on the ground".
struct GaitGround { float off = 1, landed = 0; };
inline bool gait_on_ground(GaitGround& g, float reach, float rise, float tilt, float dt, const char** why) {
    g.off = reach > kGaitReach ? 0.0f : std::min(g.off + dt, 1.0f);
    const char* away = g.off > kGaitAirborne ? "legs tucked (a jump)"
                       : rise > kGaitRiseMax ? "rising fast (a jump)"
                       : rise < -kGaitFallMax ? "falling fast"
                       : tilt > kGaitTiltMax ? "pelvis turned over (a roll or a flip)"
                       : nullptr;
    g.landed = away ? 0.0f : std::min(g.landed + dt, 1.0f);
    const char* stop = away ? away : g.landed < kGaitLanded ? "just landed" : nullptr;
    *why = stop ? stop : "on the ground";
    return !stop;
}

struct GaitState {
    float phase = 0;   // 0..1: where every clip is in its cycle, counted from the left foot's mid-stance
    struct Use { int clip; float weight; } use[6]{};
    int uses = 0;
};

struct GaitPose {
    Aff slot[rig::kGaitSlots];   // each hips-and-legs palette slot (rig::gait_slots order), model space
    V3 spine, waist;             // the torso's and the legs' joints, model space
    V3 ankle[2];                 // left, right (model space)
};

// Which clips, and how much of each, for a travel direction and speed. left/ahead: the ground velocity in Link's model
// axes (m/s). Running, the eight directions each have a cycle; walking, ahead and both sides do, and backwards is the run's
// back-step slowed down (the game has no backward walk).
inline void gait_weights(GaitState& g, float left, float ahead) {
    const float speed = std::sqrt(left * left + ahead * ahead);
    const float angle = speed > 1e-4f ? std::atan2(left, ahead) : 0.0f;   // (0 ahead, +90 degrees left, 180 back)
    g.uses = 0;
    const auto add_use = [&](int clip, float w) {
        if (w <= 1e-4f) return;
        for (int i = 0; i < g.uses; i++)
            if (g.use[i].clip == clip) { g.use[i].weight += w; return; }
        if (g.uses < 6) g.use[g.uses++] = {clip, w};
    };
    // Between the two directions either side of the travel: weighted by how close each is
    const auto ring = [&](const int* clips, int n, float w) {
        const float step = 2 * kPi / n;
        float at = angle / step;
        if (at < 0) at += n;
        const int a = int(at) % n, b = (a + 1) % n;
        const float f = at - std::floor(at);
        add_use(clips[a], w * (1 - f));
        add_use(clips[b], w * f);
    };
    const float run = smoothstep(kGaitWalkTo, kGaitRunFrom, speed);
    const float to_run = smoothstep(kGaitJogTo, kGaitRunTo, speed);
    // (counter-clockwise from ahead, seen from above: ahead, left, back, right)
    const int walking[4] = {rig::gait_walk, rig::gait_side_walk_left, rig::gait_back, rig::gait_side_walk_right};
    ring(walking, 4, 1 - run);
    if (run > 0) {
        // ahead is the jog becoming the run; the jog's share is moved there after the ring
        const int running[8] = {rig::gait_run, rig::gait_ahead_left, rig::gait_side_left, rig::gait_back_left, rig::gait_back,
                                rig::gait_back_right, rig::gait_side_right, rig::gait_ahead_right};
        ring(running, 8, run);
        for (int i = 0; i < g.uses; i++)
            if (g.use[i].clip == rig::gait_run && to_run < 1) {
                const float jog = g.use[i].weight * (1 - to_run);
                g.use[i].weight -= jog;
                add_use(rig::gait_jog, jog);
                break;
            }
    }
}

// The moving cycles give way to one held frame (rig::gait_stand: Wait; rig::gait_tuck: the running jump's knees-up
// moment), by amount 0..1
inline void gait_hold(GaitState& g, int clip, float amount) {
    if (amount <= 0) return;
    for (int i = 0; i < g.uses; i++) g.use[i].weight *= 1 - amount;
    if (g.uses < 6) g.use[g.uses++] = {clip, amount};
    else g.use[5] = {clip, amount};
}

// Advance the shared phase by the ground covered: one cycle per (weighted) stride, no faster than kGaitMaxPace times the
// clips' own pace.
inline void gait_step(GaitState& g, float left, float ahead, float dt) {
    gait_weights(g, left, ahead);
    float stride = 0, pace = 0;
    for (int i = 0; i < g.uses; i++) {
        const rig::GaitClip& c = rig::gait_clips[g.use[i].clip];
        stride += g.use[i].weight * std::sqrt(c.travel_left * c.travel_left + c.travel_ahead * c.travel_ahead);
        pace += g.use[i].weight * 60.0f / float(c.frames);   // (cycles per second at the clip's own 60 fps)
    }
    if (stride <= 1e-4f || !(dt > 0)) return;
    const float speed = std::sqrt(left * left + ahead * ahead);
    const float cycles = std::min(speed / stride, pace * kGaitMaxPace);
    g.phase = std::fmod(g.phase + cycles * dt, 1.0f);
}

struct GaitLocal { float q[4]; V3 t, s; };

// One clip's bone at a phase: between its two nearest samples (rotations normalised-lerped)
inline GaitLocal gait_sample(int clip, float phase, int bone) {
    const rig::GaitClip& c = rig::gait_clips[clip];
    float at = std::fmod(phase + c.left_planted, 1.0f) * float(c.count);
    const int a = int(at) % c.count, b = (a + 1) % c.count;
    const float f = at - std::floor(at);
    const int16_t* qa = rig::gait_rotation[c.first + a][bone];
    const int16_t* qb = rig::gait_rotation[c.first + b][bone];
    const float sign = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3] < 0 ? -1.0f : 1.0f;
    GaitLocal out;
    for (int k = 0; k < 4; k++) out.q[k] = (qa[k] * (1 - f) + sign * qb[k] * f) / 32767.0f;
    const rig::GaitBone& B = rig::gait_bones[bone];
    if (B.moves < 0) {
        out.t = v3(B.rest), out.s = v3(B.scale);
    } else {
        const float* ma = rig::gait_moving[c.first + a][B.moves];
        const float* mb = rig::gait_moving[c.first + b][B.moves];
        out.t = add(mul(v3(ma), 1 - f), mul(v3(mb), f));
        out.s = add(mul(v3(ma + 3), 1 - f), mul(v3(mb + 3), f));
    }
    return out;
}

inline M3 gait_rotation_of(const float (&q)[4]) {
    float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (n < 1e-6f) return identity();
    const float x = q[0] / n, y = q[1] / n, z = q[2] / n, w = q[3] / n;
    return {{{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
             {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
             {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
}

inline M3 gait_scaled(const M3& r, V3 col, V3 row_inv) {   // diag(row_inv) * r * diag(col)
    M3 out = r;
    const float c[3] = {col.x, col.y, col.z}, ri[3] = {row_inv.x, row_inv.y, row_inv.z};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out.v[i][j] *= c[j] * ri[i];
    return out;
}

// The hips and legs at the state's phase and blend, in model space
inline void gait_pose(const GaitState& g, GaitPose& out) {
    static Aff model[rig::kGaitBones];   // (only touched under cache_mu, like the rest of the pose)
    static V3 scale[rig::kGaitBones];
    float total = 0;
    for (int i = 0; i < g.uses; i++) total += g.use[i].weight;
    for (int k = 0; k < rig::kGaitBones; k++) {
        GaitLocal blended{{0, 0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        for (int i = 0; i < g.uses && total > 0; i++) {
            const float w = g.use[i].weight / total;
            const GaitLocal l = gait_sample(g.use[i].clip, g.phase, k);
            // (each quaternion turned to the same side as the first clip's, so opposite signs do not cancel)
            float d = 0;
            for (int c = 0; c < 4; c++) d += l.q[c] * blended.q[c];
            const float sign = i > 0 && d < 0 ? -1.0f : 1.0f;
            for (int c = 0; c < 4; c++) blended.q[c] += sign * w * l.q[c];
            blended.t = add(blended.t, mul(l.t, w));
            blended.s = add(blended.s, mul(l.s, w));
        }
        if (!(total > 0)) {   // (no clip: the rest pose)
            const rig::GaitBone& B = rig::gait_bones[k];
            blended.q[3] = 1, blended.t = v3(B.rest), blended.s = v3(B.scale);
        }
        const int parent = rig::gait_bones[k].parent;
        const V3 inv_parent = parent < 0 ? V3{1, 1, 1}
                                         : V3{1 / scale[parent].x, 1 / scale[parent].y, 1 / scale[parent].z};
        const Aff local{gait_scaled(gait_rotation_of(blended.q), blended.s, inv_parent), blended.t};
        model[k] = parent < 0 ? local : mul(model[parent], local);
        scale[k] = blended.s;
    }
    for (int i = 0; i < rig::kGaitSlots; i++) {
        const rig::GaitSlot& s = rig::gait_slots[i];
        out.slot[i] = mul(model[s.bone], aff(s.inv_bind));
    }
    out.spine = add(apply(model[0].r, v3(rig::gait_spine)), model[0].t);
    out.waist = model[1].t;
    out.ankle[0] = model[rig::gait_ankle[0]].t, out.ankle[1] = model[rig::gait_ankle[1]].t;
}
