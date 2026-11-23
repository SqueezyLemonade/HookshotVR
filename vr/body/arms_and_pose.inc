// Hookshot VR: Link's first-person pose: the torso upright under the headset, the legs from the game (or, walking and
// running, stepping the way he travels: gait.inc),
// the head folded away, the arms reaching the controllers (two-bone IK).
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// ---- body state kept between frames (only touched under cache_mu) ---------------------------------------------------------
struct Motion {
    bool have = false;
    std::chrono::steady_clock::time_point at;
    V3 root{};
    float speed = 0;         // metres per second, smoothed
    float heading = 0;       // the body's turn from the camera heading (radians): follows the headset past a dead zone
    float twist = 0;         // the hips' turn toward the direction of travel, smoothed
    float hang = 0;          // the torso's base above Link's position (m), learned on the ground: the body hangs this far below
                             // the view, which stands on his position
    float root_rise = 0;     // Link's position: metres per second up, smoothed
    V3 forward{};            // the way the torso faces (world, level), as last posed: body_heading()
    V3 velocity{};           // metres per second over the ground (world, level), smoothed
    float moved = 0;         // metres per second over the ground, this frame (not smoothed)
    float rise = 0;          // the game's pelvis: metres per second up, smoothed
    float pelvis_at = 0;     // the game's pelvis height (world) when last posed, for rise
    float feet_at = 0;       // Link's position's height (world) when last posed
    float steady = 0;        // seconds Link has stood still, his position neither moving nor rising (gait.inc kGaitSteady)
    float stand = 0;         // how much the legs show the still standing frame instead (a wrong game pose standing), 0..1
    float frozen = 0;        // seconds the game's own leg pose has not changed
    float since_odd = 1;     // seconds since the game's legs last looked like no standing pose
    float tuck = 0;          // how much the lower body is the held tuck (a backflip or a roll), 0..1
    V3 game_legs[2]{};       // the game's ankles against its pelvis when last posed (for frozen)
    float legs = 0;          // how much the legs follow travel (gait.inc) instead of the game's animation, 0..1
};
Motion motion;

// When child Link's Hylian Shield was last drawn (held_gear.inc, milliseconds): the game keeps it on his back, so a sword
// taken from the back leaves the other hand free (kept_shield)
std::atomic<int64_t> kept_shield_at{-1000};
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool kept_shield() { return now_ms() - kept_shield_at.load(std::memory_order_relaxed) < 250; }

// Crawling (vr/camera/crawl_host.c): the view has come down into the tunnel, and the body becomes the game's own crawl, on
// hands and knees, instead of the upright torso and the tracked arms (the game takes no items while Link crawls, and a
// standing player's hands are below the tunnel's floor). Across, its neck is held under the view's neck as the standing
// body's is: under the head as it moves (10 cm under the eyes, 8 cm behind them, in the head's own axes), so looking down
// shows his shoulders and arms ahead, never the inside of his collar. Up and down it stays where the game has it, so his
// hands and knees stay on the tunnel's floor. The head is folded away as standing. Blended in by how far into the crawl
// Link is (body.crawl), so going down and getting up are smooth. game: his pose as the game made it.
void crawl_under_view(float* p, const rig::Model& M, const LinkBody& body, V3 origin, const std::vector<float>& game) {
    if (game.size() != size_t(M.slots) * 12 || !(length(v3(body.head_up)) > .5f && length(v3(body.head_back)) > .5f)) return;
    const V3 U{0, 1, 0};
    const float* g = game.data();
    const V3 game_neck = point(g + M.neck * 12, v3(M.neck_pos));
    const V3 neck = add(add(v3(body.eye), mul(unit(v3(body.head_up), U), -.10f)), mul(unit(v3(body.head_back), U), .08f));
    V3 slide = flat(sub(sub(neck, origin), game_neck));
    if (!(length(slide) < 1.0f)) slide = {0, 0, 0};   // (a warp or a wrong pose: where the game has him)
    const M3 S = rotation(g + M.neck * 12);
    const V3 hidden = add(sub(game_neck, mul(U, .03f * M.neck_pos[1])), slide);   // (as standing: just under the neck)
    const float k = std::clamp(body.crawl, 0.0f, 1.0f);
    for (int i = 0; i < M.slots; i++) {
        float crawl[12];
        std::memcpy(crawl, g + i * 12, sizeof crawl);
        if (M.region[i] == 2) set(crawl, S, hidden, .001f);
        else crawl[3] += slide.x, crawl[11] += slide.z;
        float* m = p + i * 12;
        for (int j = 0; j < 12; j++) m[j] += (crawl[j] - m[j]) * k;
    }
}

#include "gait.inc"
GaitState stride;     // the stepping legs' phase and blend (only touched under cache_mu)
GaitGround ground;    // whether the game's feet are on the ground, the same
bool stepping = false;

// How a hand's fingers are posed. Empty, they follow the controller (grip, trigger, thumb). Holding something, the item
// decides and the buttons do not: one-handed gear the game itself holds (the bow, a drawn sword, a drawn shield) takes the
// game's own fingers from its animation, wrapped on that item's grip; a bomb, which the game only holds overhead in both
// hands, gets the mod's own grip: every finger curled until it rests on the ball, never through it.
enum class Grip { buttons, game, ball };

struct Hand {
    bool tracked;
    V3 wrist;                // the wrist joint's target (palette space)
    V3 f, a, c;              // the hand's frame: fingers, thumb side, cross(f, a)
    float trigger, squeeze;
    Grip grip;
    const M3 (*game)[3];     // Grip::game: each finger segment's rotation relative to the game's own hand ([5][3])
    V3 ball;                 // Grip::ball: the ball's centre (palette space)
    float ball_radius;       // (m)
};

// The most a tracked arm stretches past its length to keep the hand on the controller (2.5: about a metre and a quarter for
// adult Link): when the game drops his shoulders (a roll, a slope, a landing) the hand stays where the player's is, instead of
// being pulled down to his reach. Past this the tracking is taken as wrong and the hand stops at the reach.
constexpr float kMaxArmStretch = 2.5f;

// The ball grip: how far finger f curls (the same scale as a button's curl, 0 open to 1 a fist; a little below 0 opens it
// back) before any of it would pass inside the ball. The finger's joints and an estimated tip, a few points along each
// bone, are turned as the curl would turn them; the largest curl that keeps all of them kFingerSkin outside the surface is
// taken, so the fingers lie on the ball. The same ball in the same hand gives the same pose every time: the bomb is always
// seated at one spot in the palm (bomb_host.c), so this is a fixed grip per bomb size.
constexpr float kFingerSkin = .012f;   // (m) a finger's half-thickness: its bone line stays this far off the surface
float ball_curl(const float* p, const rig::Arm& A, int f, V3 axis, const float (&amount)[3], V3 centre, float radius) {
    V3 joint[4];
    int n = 0;
    for (int k = 0; k < 3; k++) {
        const rig::Segment& g = A.finger[f][k];
        if (g.slot < 0) break;
        joint[n++] = point(p + g.slot * 12, v3(g.joint));
    }
    if (n < 2) return 0;
    joint[n] = add(joint[n - 1], mul(sub(joint[n - 1], joint[n - 2]), .9f));   // (the tip: the last bone, about as long again)
    const float clear = radius + kFingerSkin, scale = f == 0 ? .6f : 1.0f;
    // (how far the finger, past its root, stays outside: the root itself does not move with the curl, so it is not counted)
    const auto margin = [&](float curl) {
        V3 q[4];
        for (int i = 0; i <= n; i++) q[i] = joint[i];
        for (int k = 0; k < n; k++) {   // (each segment turns about its own joint, carrying the rest of the finger)
            const M3 r = axis_angle(axis, curl * amount[k] * scale);
            for (int i = k + 1; i <= n; i++) q[i] = add(q[k], apply(r, sub(q[i], q[k])));
        }
        float m = 1e9f;
        for (int i = 0; i < n; i++)
            for (float t : {.33f, .67f, 1.0f})
                m = std::min(m, length(sub(add(q[i], mul(sub(q[i + 1], q[i]), t)), centre)) - clear);
        return m;
    };
    // The most curl that stays outside, found from the open hand inwards. A finger already inside even open (the ball
    // pressed into the knuckles) takes the curl that keeps it furthest out, not the most open one (it stuck straight out).
    float best = -2, closest = -.3f, closest_m = -1e9f;
    for (float curl = -.3f; curl <= 1.0f + 1e-4f; curl += .02f) {
        const float m = margin(curl);
        if (m >= 0) best = curl;
        else if (best > -2) break;
        if (m > closest_m) closest_m = m, closest = curl;
    }
    return best > -2 ? best : closest;
}

void solve_arm(float* p, const rig::Arm& A, const M3& S, V3 t_up, int side, const Hand& h, V3 R, V3 F, float k) {
    const V3 U{0, 1, 0};
    const V3 out = side == 0 ? mul(R, -1.0f) : R;   // (_L is on Link's left)
    const V3 sh = point(p + A.upper * 12, v3(A.shoulder));   // (the shoulder as the torso now stands)
    (void)S, (void)t_up;
    const float b1 = length(sub(v3(A.elbow_pos), v3(A.shoulder))), b2 = length(sub(v3(A.wrist_pos), v3(A.elbow_pos)));
    if (!(b1 > .02f && b2 > .02f)) return;
    // (the VR tab's Arm length: both bones stretched along their length; a tracked hand out of that reach stretches them more)
    if (h.tracked) k = std::max(k, std::min(length(sub(h.wrist, sh)) / ((b1 + b2) * .996f), kMaxArmStretch));
    const float l1 = b1 * k, l2 = b2 * k;

    // The wrist's target, inside the arm's reach
    V3 target = h.tracked ? h.wrist : add(add(sh, mul(U, -.93f * (l1 + l2))), add(mul(out, .07f), mul(F, .03f)));
    V3 line = sub(target, sh);
    float d = length(line);
    const V3 dir = unit(line, mul(U, -1.0f));
    d = std::clamp(d, std::fabs(l1 - l2) + .01f, l1 + l2 - .002f);
    target = add(sh, mul(dir, d));

    // The elbow: down, out and back from the shoulder-wrist line, as a person's does
    const V3 pole = add(add(mul(U, -1.0f), mul(out, .6f)), mul(F, -.45f));
    const V3 bend = unit(perp(pole, dir), unit(perp(mul(U, -1.0f), dir), out));
    const float along = (l1 * l1 - l2 * l2 + d * d) / (2 * d);
    const float rise = std::sqrt(std::max(l1 * l1 - along * along, 0.0f));
    const V3 elbow = add(add(sh, mul(dir, along)), mul(bend, rise));

    // Upper arm (and all below it) about the shoulder
    const V3 el0 = point(p + A.fore * 12, v3(A.elbow_pos));
    const M3 q1 = from_to(sub(el0, sh), sub(elbow, sh));
    turn_set(p, A.upper_set, q1, sh);
    turn_set(p, A.fore_set, q1, sh);
    turn_set(p, A.hand_set, q1, sh);
    if (k != 1.0f) {   // the upper arm stretched along itself; the forearm and hand carried out to the new elbow
        const V3 u = unit(sub(elbow, sh), dir);
        turn_set(p, A.upper_set, stretch_along(u, k), sh);
        const V3 out_by = mul(u, b1 * (k - 1));
        move_set(p, A.fore_set, out_by);
        move_set(p, A.hand_set, out_by);
    }

    // Forearm about the elbow; the elbow cap takes half the bend
    const V3 el1 = point(p + A.fore * 12, v3(A.elbow_pos));
    const V3 wr1 = point(p + A.wrist * 12, v3(A.wrist_pos));
    const M3 q2 = from_to(sub(wr1, el1), sub(target, el1));
    turn_set(p, A.fore_set, q2, el1, A.elbow);
    turn_set(p, A.hand_set, q2, el1);
    turn(p + A.elbow * 12, from_to(sub(wr1, el1), sub(target, el1), .5f), el1);
    if (k != 1.0f) {
        const V3 v = unit(sub(target, el1), dir);
        turn_set(p, A.fore_set, stretch_along(v, k), el1, A.elbow);
        move_set(p, A.hand_set, mul(v, b2 * (k - 1)));
    }

    // The hand's wanted rotation (bind hand frame -> wanted world frame)
    const V3 fb = unit(sub(v3(A.middle), v3(A.wrist_pos)), {0, -1, 0});
    const V3 ab = unit(perp(sub(v3(A.index), v3(A.pinky)), fb), {0, 0, 1});
    const M3 bind = columns(fb, ab, cross(fb, ab));
    const M3 want = mul(columns(h.f, h.a, h.c), transpose(bind));

    // Forearm roll: shared along the forearm (its twist bones), so the gauntlet turns with the hand instead of the wrist snapping
    const V3 axis = unit(sub(target, el1), dir);
    const float roll = std::clamp(twist_about(mul(want, transpose(rotation(p + A.wrist * 12))), axis), -2.6f, 2.6f);
    for (int16_t s : A.fore_set) {
        if (s < 0) break;
        if (s == A.elbow) continue;
        const float share = s == A.assist[0] ? .5f : s == A.assist[1] ? .75f : .25f;
        turn(p + s * 12, axis_angle(axis, roll * share), el1);
    }
    turn_set(p, A.hand_set, axis_angle(axis, roll * .25f), el1);

    // The hand itself: exactly the wanted rotation, at the wrist
    const V3 wr = point(p + A.wrist * 12, v3(A.wrist_pos));
    turn_set(p, A.hand_set, mul(want, transpose(rotation(p + A.wrist * 12))), wr);

    if (h.grip == Grip::game) {   // the game's own fingers on its own grip, relative to the VR hand
        const M3 hand = rotation(p + A.wrist * 12);
        for (int f = 0; f < 5; f++)
            for (int k = 0; k < 3; k++) {   // (base to tip: each segment set, carrying the ones after it)
                const rig::Segment& g = A.finger[f][k];
                if (g.slot < 0) continue;
                const V3 pivot = point(p + g.slot * 12, v3(g.joint));
                turn_set(p, g.set, mul(mul(hand, h.game[f][k]), transpose(rotation(p + g.slot * 12))), pivot);
            }
        return;
    }
    // Fingers: curl toward the palm. Thumb A, index B (trigger), middle..little C-E (grip)
    const V3 curl_axis = side == 0 ? mul(h.a, -1.0f) : h.a;   // (a physical left palm faces cross(f, a), a right one -cross(f, a))
    const float relaxed[5] = {.10f, .10f, .14f, .18f, .22f};
    const float amount[3] = {.55f, .80f, .60f};
    for (int f = 0; f < 5; f++) {
        const float pressed = f == 0 ? h.squeeze * .45f : f == 1 ? h.trigger : h.squeeze;
        float curl = relaxed[f] + pressed * (1.0f - relaxed[f]);
        if (h.grip == Grip::ball) curl = ball_curl(p, A, f, curl_axis, amount, h.ball, h.ball_radius);
        for (int k = 0; k < 3; k++) {
            const rig::Segment& g = A.finger[f][k];
            if (g.slot < 0) continue;
            const V3 pivot = point(p + g.slot * 12, v3(g.joint));
            turn_set(p, g.set, axis_angle(curl_axis, curl * amount[k] * (f == 0 ? .6f : 1.0f)), pivot);
        }
    }
}

bool pose(float* p, const rig::Model& M, const LinkBody& body, V3 origin) {
    const V3 U{0, 1, 0};
    const auto now = std::chrono::steady_clock::now();
    const V3 root = v3(body.root);
    float dt = motion.have ? std::chrono::duration<float>(now - motion.at).count() : 0.0f;
    if (dt > .5f) dt = 0;   // (a pause: no velocity from it)
    if (dt > 1e-4f) {
        const float v = length(flat(sub(root, motion.root))) / dt;
        motion.speed += (std::min(v, 20.0f) - motion.speed) * std::min(1.0f, dt * 8);
        V3 vel = mul(flat(sub(root, motion.root)), 1 / dt);
        motion.moved = length(vel);
        if (length(vel) > 20) vel = {0, 0, 0};   // (a warp, not a run)
        motion.velocity = add(motion.velocity, mul(sub(vel, motion.velocity), std::min(1.0f, dt * 8)));
        const float up = std::clamp((root.y - motion.root.y) / dt, -20.0f, 20.0f);
        motion.root_rise += (up - motion.root_rise) * std::min(1.0f, dt * 8);
    }
    motion.have = true, motion.at = now, motion.root = root;
    // (crawling: the game's own pose, before the first-person pose below rebuilds it, for the crawl at the end)
    static std::vector<float> game_pose;
    if (body.crawl > 0) game_pose.assign(p, p + M.slots * 12);

    // Body heading: the game camera's, turned toward the headset when the head looks more than 50 degrees away (or while moving)
    const V3 Rc = unit(flat(v3(body.right)), {1, 0, 0});
    const V3 Fc = mul(unit(perp(flat(v3(body.back)), Rc), cross(Rc, U)), -1.0f);
    const V3 head_f = mul(flat(v3(body.head_back)), -1.0f);
    if (length(head_f) > .2f && dt > 0) {
        const float dead = 50 * kDeg;
        float off = signed_angle(apply(axis_angle(U, motion.heading), Fc), head_f, U);
        if (std::fabs(off) > dead) motion.heading += off - std::copysign(dead, off), off = std::copysign(dead, off);
        if (motion.speed > .4f) motion.heading += off * std::min(1.0f, dt * 2.5f);   // (walking: the body settles under the head)
        motion.heading = std::remainder(motion.heading, 2 * kPi);
    }
    const M3 yaw = axis_angle(U, motion.heading);
    const V3 R = apply(yaw, Rc), F = apply(yaw, Fc);
    motion.forward = F;
    // Model axes -> world: +x is Link's left, +y up, +z the way he faces
    const M3 S = columns(mul(R, -1.0f), U, F);

    // The game's own pelvis
    const V3 gw = point(p + M.waist * 12, v3(M.waist_pos));
    V3 gs = point(p + M.spine * 12, v3(M.spine_pos));   // (moved onto the stepping legs' hips below, as much as they are used)
    const M3 game_hips = rotation(p + M.waist * 12);

    // Hips and legs: the game's animation, the pelvis turned to face the torso (plus a little twist toward travel). Most of the
    // game's own pelvis tilt is kept (a run's forward lean): with an upright pelvis the stride swung out in front of the view
    // ("the leg dance"). Large tilts (rolls, flips) fade out, so the legs stay under the body.
    const V3 game_f = flat(apply(game_hips, {0, 0, 1}));
    float want_twist = 0;
    M3 tilt = identity();
    float game_tilt = 0;   // how far the game's pelvis is pitched or rolled (radians): a roll or a flip
    if (length(game_f) > .3f) {
        const V3 Fg = unit(game_f, F);
        const float rel = signed_angle(F, Fg, U);
        const float folded = std::atan2(std::sin(2 * rel), std::cos(2 * rel)) * .5f;   // backwards counts as forwards
        want_twist = std::clamp(folded, -30 * kDeg, 30 * kDeg) * (1 - smoothstep(55 * kDeg, 88 * kDeg, std::fabs(folded))) *
                     smoothstep(.3f, 1.5f, motion.speed);
        const M3 Sg = columns(mul(cross(Fg, U), -1.0f), U, Fg);
        const M3 residual = mul(transpose(Sg), game_hips);   // (model space: the animation's pitch and roll of the pelvis)
        V3 axis;
        const float angle = rotation_angle(residual, &axis);
        tilt = axis_angle(axis, angle * .75f * (1 - smoothstep(40 * kDeg, 70 * kDeg, angle)));
        game_tilt = angle;
    }

    // Legs that step the way Link travels (gait.inc), on the ground at a walk or run; the game's legs for anything else
    // (standing, rolls and flips, jumps and falls, climbing, the child). On the ground: gait_on_ground, from the game's own
    // feet. The legs leave at once and come back gently (kGaitLegsOut, kGaitLegsIn).
    const bool gait_ok = opts.vr_gait && &M == &rig::adult;
    bool cover = false;   // standing still with the game's legs in no standing pose: the still standing frame instead
    bool flipping = false;   // the game's pelvis turned over (a backflip or a roll): the lower body tucked, upright
    if (gait_ok && dt > 0) {
        static V3 ankle_joint[2];   // (the game's ankle joints in bind space, from their slots' inverse binds)
        static int ankle_slot[2] = {-1, -1};
        if (ankle_slot[0] < 0)
            for (int side = 0; side < 2; side++)
                for (const rig::GaitSlot& g : rig::gait_slots) {
                    Aff bind;
                    if (g.bone == rig::gait_ankle[side] && invert(aff(g.inv_bind), &bind)) {
                        ankle_slot[side] = g.slot, ankle_joint[side] = bind.t;
                        break;
                    }
                }
        float foot = 1e9f;
        V3 legs[2]{};
        for (int side = 0; side < 2; side++)
            if (ankle_slot[side] >= 0) {
                legs[side] = sub(point(p + ankle_slot[side] * 12, ankle_joint[side]), gw);
                foot = std::min(foot, legs[side].y + gw.y);
            }
        // (the game's leg pose unchanged: frozen, logged once it lasts a second)
        const bool same = length(sub(legs[0], motion.game_legs[0])) < 1e-4f && length(sub(legs[1], motion.game_legs[1])) < 1e-4f;
        motion.game_legs[0] = legs[0], motion.game_legs[1] = legs[1];
        const float was_frozen = motion.frozen;
        motion.frozen = same ? motion.frozen + dt : 0.0f;
        if (was_frozen < 1 && motion.frozen >= 1)
            logf("vr: legs: the game's leg pose has not changed for 1 s (ankles %.2f and %.2f m below its pelvis)",
                 -legs[0].y, -legs[1].y);
        // (Link's own position standing still: neither moving over the ground nor up or down)
        const float feet_y = root.y;
        const float feet_rise = motion.feet_at != 0 ? (feet_y - motion.feet_at) / dt : 0.0f;
        motion.feet_at = feet_y;
        motion.steady = std::fabs(feet_rise) < kGaitSteady && motion.moved < kGaitSteady ? motion.steady + dt : 0.0f;
        // (the pelvis's rise: its own height, not Link's position in the game's physics, which jumps on slopes)
        const float pelvis_y = gw.y + origin.y;
        if (motion.pelvis_at == 0) motion.pelvis_at = pelvis_y;
        motion.rise += (std::clamp((pelvis_y - motion.pelvis_at) / dt, -20.0f, 20.0f) - motion.rise) * std::min(1.0f, dt * 8);
        motion.pelvis_at = pelvis_y;
        const float feet = root.y - origin.y;
        const char* why = "";
        const bool was = stepping;
        // (how far the game's pelvis leans from upright, by its up axis: unlike game_tilt, never fooled by Link upside down)
        const float lean = std::acos(std::clamp(dot(unit(apply(game_hips, U), U), U), -1.0f, 1.0f));
        stepping = gait_on_ground(ground, gw.y - foot, motion.rise, lean, dt, &why);
        flipping = lean > kGaitTiltMax;
        // (no standing pose: turned over, a foot tucked or raised, or the pose frozen; gait.inc kGaitFootRaised)
        const float raised = std::min(-legs[0].y, -legs[1].y);   // (the higher foot, below the pelvis)
        const bool odd = lean > kGaitTiltMax || gw.y - foot < kGaitReach || raised < kGaitFootRaised ||
                         motion.frozen > kGaitFrozen;
        motion.since_odd = odd ? 0.0f : std::min(motion.since_odd + dt, 1.0f);
        static bool covered;
        cover = motion.steady > kGaitSteadyFor && motion.since_odd < kGaitCoverHold;
        if (cover != covered)
            logf("vr: legs: %s (game pelvis leans %.0f deg, its feet %.2f and %.2f m below it, the pose unchanged for %.1f s)",
                 cover ? "standing still in no standing pose: the still standing frame covers the game's legs"
                       : "the game's legs back", lean / kDeg, -legs[0].y, -legs[1].y, motion.frozen);
        covered = cover;
        // (what the stepping legs see, for the log: each change, and a few times a second while they show)
        static auto told = now;
        if (stepping != was || (motion.legs > 0 && now - told > std::chrono::milliseconds(250))) {
            told = now;
            const V3 v = motion.velocity;
            logf("vr: legs %s (%.2f): %s; game foot %.2f m below its pelvis, pelvis %.2f m above Link's position, %.1f m/s at "
                 "%+.0f deg, pelvis rise %+.1f m/s, tilt %.0f deg", stepping != was ? (stepping ? "step" : "back to the game's") :
                 "stepping", motion.legs, why, gw.y - foot, gw.y - feet, motion.speed,
                 std::atan2(dot(v, mul(R, -1.0f)), dot(v, F)) / kDeg, motion.rise, lean / kDeg);
        }
        // (standing with the game's own legs: what the older first-person path does to them, every 2 s, for a leg left
        // looking wrong after a flip)
        static auto idle_told = now;
        if (motion.legs == 0 && motion.speed < .2f && now - idle_told > std::chrono::seconds(2)) {
            idle_told = now;
            logf("vr: legs: the game's, standing: pelvis leans %.0f deg (by its forward %.0f deg), hips twisted %.0f deg, "
                 "game foot %.2f m below its pelvis", lean / kDeg, game_tilt / kDeg, motion.twist / kDeg, gw.y - foot);
        }
        const V3 v = motion.velocity;
        gait_step(stride, dot(v, mul(R, -1.0f)), dot(v, F), dt);
    }
    const float want_legs = cover || flipping ? 1.0f : gait_ok && stepping ? smoothstep(.2f, .6f, motion.speed) : 0.0f;
    motion.stand += ((cover ? 1.0f : 0.0f) - motion.stand) * (dt > 0 ? std::min(1.0f, dt * kGaitLegsOut) : 1.0f);
    motion.tuck += ((flipping ? 1.0f : 0.0f) - motion.tuck) * (dt > 0 ? std::min(1.0f, dt * kGaitTuckRate) : 1.0f);
    const float rate = want_legs < motion.legs ? kGaitLegsOut : flipping ? kGaitTuckRate : kGaitLegsIn;
    motion.legs += (want_legs - motion.legs) * (dt > 0 ? std::min(1.0f, dt * rate) : 1.0f);
    if (motion.legs < 1e-3f) motion.legs = 0;
    static GaitPose gait;   // (only touched under cache_mu)
    Aff placed{};           // Link's model space -> palette space for the stepping legs: the torso's facing, on his feet
    if (motion.legs > 0) {
        GaitState shown = stride;
        gait_hold(shown, rig::gait_stand, std::max(motion.stand, motion.tuck));
        gait_hold(shown, rig::gait_tuck, motion.tuck * kGaitTuckDepth);
        gait_pose(shown, gait);
        // (on the game's drawn pelvis: its height, not Link's physics position, which jumps on slopes; across, his position.
        // Covering a wrong pose standing still, or tucked in a flip, the pelvis is no guide: on his position, which the view
        // also stands on. Mid-flip the game's pelvis is 1.2-1.5 m above it instead of 0.9, and a tuck hung from it came up
        // to the eyes: the knee in the face.)
        placed = {S, sub(root, origin)};
        const float on_feet = std::max(motion.stand, motion.tuck);
        placed.t.y = (gw.y - gait.waist.y) * (1 - on_feet) + (root.y - origin.y) * on_feet;
        gs = add(mul(gs, 1 - motion.legs), mul(add(apply(S, gait.spine), placed.t), motion.legs));
    }

    motion.twist += (want_twist * (1 - motion.legs) - motion.twist) * (dt > 0 ? std::min(1.0f, dt * 6) : 1.0f);
    const M3 hips = mul(mul(axis_angle(U, motion.twist), S), tilt);
    const M3 hips_turn = mul(hips, transpose(game_hips));

    // The game's own fingers, relative to its hand (before the torso below rebuilds the arms): Grip::game
    M3 game_fingers[2][5][3];
    for (int side = 0; side < 2; side++) {
        const rig::Arm& A = M.arm[side];
        const M3 hand_t = transpose(rotation(p + A.wrist * 12));
        for (int f = 0; f < 5; f++)
            for (int k = 0; k < 3; k++)
                game_fingers[side][f][k] = A.finger[f][k].slot < 0 ? identity() :
                                           mul(hand_t, rotation(p + A.finger[f][k].slot * 12));
    }

    // Torso: upright, joined to the game's pelvis
    const V3 t_up = sub(gs, apply(S, v3(M.spine_pos)));
    // Head: everything above the neck folded to a point just under the neck joint. (Body v2's first build folded it 25 cm
    // down into the chest: skin weighted partly to the head - collar, hood - was dragged down with it.)
    const float drop = .03f * M.neck_pos[1];
    const V3 hidden = add(apply(S, sub(v3(M.neck_pos), {0, drop, 0})), t_up);
    for (int i = 0; i < M.slots; i++) {
        float* m = p + i * 12;
        // A matrix the game collapsed (scale ~0: a part it hides) stays as the game made it
        const M3 g = {{{m[0], m[1], m[2]}, {m[4], m[5], m[6]}, {m[8], m[9], m[10]}}};
        const float det = g.v[0][0] * (g.v[1][1] * g.v[2][2] - g.v[1][2] * g.v[2][1]) -
                          g.v[0][1] * (g.v[1][0] * g.v[2][2] - g.v[1][2] * g.v[2][0]) +
                          g.v[0][2] * (g.v[1][0] * g.v[2][1] - g.v[1][1] * g.v[2][0]);
        if (std::fabs(det) < .05f && M.region[i] != 2) continue;
        switch (M.region[i]) {
        case 1: turn(m, hips_turn, gw); break;
        case 2: set(m, S, hidden, .001f); break;
        default: set(m, S, t_up); break;
        }
    }
    // The stepping legs, over the game's as much as they are used (a part the game hides stays hidden)
    if (motion.legs > 0)
        for (int i = 0; i < rig::kGaitSlots; i++) {
            float* m = p + rig::gait_slots[i].slot * 12;
            if (std::fabs(det(aff(m).r)) < .05f) continue;
            float step[12];
            store(step, mul(placed, gait.slot[i]));
            for (int k = 0; k < 12; k++) m[k] += (step[k] - m[k]) * motion.legs;
        }

    // The body follows the real head, as VR avatars do. The neck is estimated from the headset (10 cm under the eyes and 8 cm
    // behind them, in the head's own axes), so tilting or nodding the head barely moves it; how far it is from where it was at the
    // last recentre moves Link: sideways and forwards the whole body steps along under it, and a drop (slumping, ducking) bends
    // him forward from the hips. His body is also held under the view sideways instead of at the game's animated pelvis, which
    // runs and rolls swing forward and back (the "neck hole" coming into view while running).
    V3 head{0, 0, 0};
    if (length(v3(body.eye_rest)) > 0 && length(v3(body.head_up)) > .5f) {
        const V3 Bc = mul(Fc, -1.0f);
        const V3 neck_now = add(add(v3(body.eye), mul(unit(v3(body.head_up), U), -.10f)), mul(unit(v3(body.head_back), Bc), .08f));
        const V3 neck_rest = add(add(v3(body.eye_rest), mul(U, -.10f)), mul(Bc, .08f));
        head = sub(neck_now, neck_rest);
        if (!(length(head) < 1.2f)) head = {0, 0, 0};
    }
    // bend: as much as the neck dropped, the spine folds forward (at most 60 degrees)
    const float spine = std::max(length(sub(add(apply(S, v3(M.neck_pos)), t_up), gs)), .1f);
    const float dropped = std::clamp(-head.y, 0.0f, spine * .5f);
    const float bend = std::acos(std::clamp((spine - dropped) / spine, -1.0f, 1.0f));
    // where the pelvis goes: under the view (the game root plus the head's offset), less what the bend puts ahead
    const V3 root_local = sub(root, origin);
    V3 shift = sub(add(root_local, flat(head)), gs);
    shift = sub(flat(shift), mul(F, spine * std::sin(bend)));
    // Height: the body hangs a fixed distance under the view, never from the game's animated pelvis. The view stands on Link's
    // position (eye = position + height) and the hands are tracked from it, while the game's pelvis moves against that
    // position in its jump, fall, landing, run and bow-stance animations: the shoulders rose and dropped under hands that
    // stayed put, the arms stretching and snapping back (tester, 17 Nov). The distance is learned slowly (about 2 s) from the
    // game's pelvis while his position is neither rising nor falling and he is not flipping, and held as it was otherwise.
    // Crawling, the view comes down into the tunnel (body.lowered) and the body with it; the distance is not learned from the
    // game's crawling pelvis then, so it is the standing one when he gets up.
    {
        const float y = gs.y + origin.y;
        const float above = y - root.y;
        const bool calm = std::fabs(motion.root_rise) < .5f && !flipping;
        if (body.crawl > 0 && motion.hang != 0) {}   // (crawling: kept)
        else if (dt <= 0 || motion.hang == 0 || (calm && std::fabs(above - motion.hang) > .6f)) motion.hang = above;   // (a warp, the age)
        else if (calm) motion.hang += (above - motion.hang) * std::min(1.0f, dt * .5f);
        shift.y -= y - (root.y + motion.hang - body.lowered);
    }
    for (int i = 0; i < M.slots; i++) {
        float* m = p + i * 12;
        m[3] += shift.x, m[7] += shift.y, m[11] += shift.z;
    }
    const V3 pelvis = add(gs, shift);
    if (bend > 1e-3f) {
        const M3 lean = axis_angle(R, -bend);   // (about Link's right: the chest goes forward)
        for (int i = 0; i < M.slots; i++)
            if (M.region[i] != 1) turn(p + i * 12, lean, pelvis);
    }

    // Arms
    M3 mirrored_fingers[5][3];
    M3 gear_fingers[2][5][3];   // (the sword or shield taken from the back, in a hand the game does not hold it in: mirrored)   // (the left hand's grip mirrored, for the hookshot in the right hand)
    for (int side = 0; side < 2; side++) {
        const LinkHand& in = body.hand[side];   // (0 left, 1 right, as rig::Model::arm)
        Hand h{};
        h.tracked = in.valid;
        if (in.valid) {
            const M3 basis = mul(columns(v3(in.basis), v3(in.basis + 3), v3(in.basis + 6)), local_euler(body.wrist_deg[side]));
            const V3 gx = column(basis, 0), gy = column(basis, 1), gz = column(basis, 2);
            // OpenXR's grip: +x is the palm's normal (to the right for either hand when the palms face each other), -z runs
            // through the closed fist from the little finger to the thumb, and the fingers wrap around it along -y.
            h.a = unit(mul(gz, -1.0f), U);
            h.c = unit(perp(gx, h.a), R);
            h.f = cross(h.a, h.c);
            (void)gy;
            // the wrist joint sits behind the fist, along the forearm
            h.wrist = sub(sub(v3(in.position), origin), mul(h.f, .075f));
            h.trigger = in.trigger, h.squeeze = in.squeeze;
            h.grip = Grip::buttons, h.game = game_fingers[side];
            // Held gear keeps its grip without a continuous real-world squeeze; the other hand stays free. The bow in its
            // hand, a native drawn sword in the left, a native drawn shield (its strap) in the right: the game's fingers.
            // Not while the sword taken from the back is let go and the game still puts it away (sword_hand -2: the sword and
            // shield are hidden then, held_gear.inc): the hands are empty at once (headset 18 Nov: they kept gripping nothing).
            if ((body.bow_equipped && side == body.bow_hand) ||
                (side == 0 && (body.equipment_known & body.equipment_drawn & 1u) && body.equipment_owner[0] && sword_hand() != -2) ||
                (side == 1 && (body.equipment_known & body.equipment_drawn & 2u) && body.equipment_owner[1] && !body.shield_away &&
                 sword_hand() != -2))
                h.grip = opts.vr_held_grip == 0 ? Grip::game : Grip::buttons, h.trigger = .85f, h.squeeze = 1;
            // The hookshot in this hand (vr/items/hookshot): the game's own grip on it, from Link's left hand, which holds it
            // in the game. In the right hand, that grip mirrored across Link's body (x = 0): Link's skeleton is mirrored, so a
            // finger's rotation relative to its hand mirrors as x R x, x the mirror. (The left's rotations unmirrored bent the
            // right fingers backwards, headset test 17 Nov.) The slingshot the same: the game holds it in Link's left hand too
            // (its Weapon_L socket, vr/items/slingshot).
            // The Boomerang the same (vr/items/boomerang: held on the game's Weapon_L socket, boomerang_hand).
            if (side == body.hook_hand || (body.sling && body.bow_equipped && side == body.bow_hand) || side == boomerang_hand()) {
                h.grip = opts.vr_held_grip == 0 ? Grip::game : Grip::buttons, h.game = game_fingers[0], h.trigger = .85f, h.squeeze = 1;
                if (side == 1) {
                    for (int f = 0; f < 5; f++)
                        for (int k = 0; k < 3; k++) {
                            M3 r = game_fingers[0][f][k];
                            r.v[0][1] = -r.v[0][1], r.v[0][2] = -r.v[0][2], r.v[1][0] = -r.v[1][0], r.v[2][0] = -r.v[2][0];
                            mirrored_fingers[f][k] = r;
                        }
                    h.game = mirrored_fingers;
                }
            }
            // The sword taken from the back (vr/input/controllers_host.c, sword_hand): the sword in that hand, the shield in the
            // other. Once the game has drawn them (the first swing), the game's own grip on each, from Link's left hand (the
            // sword) or right (the shield), mirrored when the VR hand is the other one; before, the fingers close around the
            // handle (or strap) as around a small ball. VR tab Held gear grip Fist: the buttons' fist.
            // The game's grips are remembered from the last time it held each (seen_grip): used from the take, before the
            // first swing draws them in the game.
            static M3 seen_grip[2][5][3];
            static bool seen[2];
            for (int k = 0; k < 2; k++)
                if (body.equipment_known & body.equipment_drawn & (k ? 2u : 1u) && body.equipment_owner[k])
                    std::memcpy(seen_grip[k], game_fingers[k], sizeof seen_grip[k]), seen[k] = true;
            // (the other hand takes the shield's grip only while the game has the shield out: with a two-handed sword
            // (Biggoron's, the Giant's Knife) the game puts it away and holds the hilt with both hands, and the empty VR hand
            // took that grip too, the user, 22 Nov; it is the controller's own then)
            const bool shield_out = (body.equipment_known & body.equipment_drawn & 2u) != 0;
            if (const int vs = sword_hand(); vs >= 0 && (side == vs || (!kept_shield() && shield_out))) {
                const bool sword_side = side == vs;
                const int from = sword_side ? 0 : 1;   // (the game hand that holds this item)
                const bool drawn = body.equipment_known & body.equipment_drawn & (sword_side ? 1u : 2u);
                h.trigger = .85f, h.squeeze = 1;
                if (sword_side) {   // (VR tab Sword angle: the hand that holds the sword turned about its palm, the fist's
                    // axis (the blade) leaned forward, away from the forearm; the sword stays in the game's grip in it)
                    const float t = (float)opts.vr_sword_angle * 0.0174533f;
                    const V3 a2 = add(mul(h.a, std::cos(t)), mul(h.f, std::sin(t)));
                    h.f = sub(mul(h.f, std::cos(t)), mul(h.a, std::sin(t))), h.a = a2;
                    h.wrist = sub(sub(v3(in.position), origin), mul(h.f, .075f));
                }
                if (opts.vr_held_grip != 0) h.grip = Grip::buttons;
                else if (drawn || seen[from]) {
                    h.grip = Grip::game, h.game = drawn ? game_fingers[from] : seen_grip[from];
                    if (side != from) {
                        for (int f = 0; f < 5; f++)
                            for (int k = 0; k < 3; k++) {
                                M3 r = (drawn ? game_fingers[from] : seen_grip[from])[f][k];
                                r.v[0][1] = -r.v[0][1], r.v[0][2] = -r.v[0][2], r.v[1][0] = -r.v[1][0], r.v[2][0] = -r.v[2][0];
                                gear_fingers[side][f][k] = r;
                            }
                        h.game = gear_fingers[side];
                    }
                } else {
                    const float r = sword_side ? .016f : .02f;
                    const V3 palm = mul(gx, side ? -1.0f : 1.0f);
                    h.grip = Grip::ball, h.ball_radius = r, h.trigger = 0;
                    h.ball = add(sub(v3(in.position), origin), mul(palm, .02f + r));
                }
            }
            // The Deku Stick in this hand (vr/items/deku_stick, stick_hand: told by vr/input/controllers_host.c vr_stick): held
            // as the game holds a sword, so its fingers are the game's own left-hand grip (mirrored in the right hand, as the
            // hookshot's), and the hand leaned by the VR tab's Sword angle like the sword's: the game side places the stick's
            // actor (its attack and fire sensors) along that same leaned fist (vr/items/sword/sword_host.c vr_held_blade_pose).
            if (stick_hand() == side) {
                h.trigger = .85f, h.squeeze = 1;
                const float t = (float)opts.vr_sword_angle * 0.0174533f;
                const V3 a2 = add(mul(h.a, std::cos(t)), mul(h.f, std::sin(t)));
                h.f = sub(mul(h.f, std::cos(t)), mul(h.a, std::sin(t))), h.a = a2;
                h.wrist = sub(sub(v3(in.position), origin), mul(h.f, .075f));
                h.grip = opts.vr_held_grip == 0 ? Grip::game : Grip::buttons, h.game = game_fingers[0];
                if (side == 1) {
                    for (int f = 0; f < 5; f++)
                        for (int k = 0; k < 3; k++) {
                            M3 r = game_fingers[0][f][k];
                            r.v[0][1] = -r.v[0][1], r.v[0][2] = -r.v[0][2], r.v[1][0] = -r.v[1][0], r.v[2][0] = -r.v[2][0];
                            gear_fingers[side][f][k] = r;
                        }
                    h.game = gear_fingers[side];
                }
            }
            // A bottle in this hand (vr/items/bottle, bottle_hand: told by bottle_host.c): the fingers close on its neck, where
            // vr/items/bottle/bottle_pose.h puts it (a ball as a bomb's, the neck's radius)
            if (bottle_hand() == side) {
                const V3 palm = mul(gx, side ? -1.0f : 1.0f);
                h.grip = Grip::ball, h.ball_radius = kVrBottleNeckRadius, h.trigger = 0, h.squeeze = 1;
                h.ball = add(sub(v3(in.position), origin), mul(palm, kVrBottlePalm + kVrBottleNeckRadius));
            }
            // A bomb in this hand (the game side says so, bomb_host.c): seated against the palm, BOMB_PALM (3 cm) out from
            // the grip along the palm's direction (OpenXR grip +x for the left hand, -x for the right), its radius away.
            // (A Bomb Flower's fruit: the game side sends its own radius, from vk_vr_bomb_flower.h through flower_shape.)
            if (const float r = held_ball_radius(side); r > 0) {
                const V3 palm = mul(gx, side ? -1.0f : 1.0f);   // (calibrated, as hookshot_vr_hands_world sends it)
                h.grip = Grip::ball, h.ball_radius = r, h.trigger = 0, h.squeeze = 1;
                h.ball = add(sub(v3(in.position), origin), mul(palm, .03f + r));
            }
        } else {
            h.grip = Grip::buttons;
            h.f = mul(U, -1.0f), h.c = R, h.a = cross(h.c, h.f);   // hanging, palms to the thighs
        }
        solve_arm(p, M.arm[side], S, t_up, side, h, R, F, std::clamp(body.arm_scale, .8f, 1.6f));
    }
    if (body.crawl > 0) crawl_under_view(p, M, body, origin, game_pose);
    return true;
}
