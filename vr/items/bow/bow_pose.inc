// Hookshot VR: the bow in the VR hand: its grip and nock, the string grabbed and drawn by the other hand,
// one committed bow solution per game frame.
//
// A piece of vr/body/vk_vr_body.cpp, included there in a fixed order: it uses the pieces before it. Not compiled on its own.
// ---- the bow: one committed solution per game frame -------------------------------------------------------------------------
// Made from the frame's frozen body snapshot (frame_body) and the canonical rig, the first time any of Link's draws asks for it
// in a game frame, and shared by the bow, its string, the nocked arrow, the reticle and the launch redirect. It does not
// depend on the bow model being drawn: a culled or skipped bow draw no longer drops the aim. The bow asset (the plain bow and
// the bow part of the combined bow+arrow models): +X points toward the string, +Z along the limbs; the grip is the measured
// central handle (Skl_Root rigid vertices), the nock the string's centre at rest.
constexpr V3 kBowGrip{-.0029648f, 0, -.0004349f};
constexpr V3 kBowNock{.13088851f, 0, -.000002233f};
// The plain bow's 14 palette slots at rest, relative to its root slot: the bow's skeleton in the game's own still pose
// for it (Weapon_Pose_Bow), made from the player's game files at build time (vr/tools/generate/bow_rest.py). The game's
// ZR is held while the VR bow is held ("Grip holds the bow"), and ZR is also the game's draw: its drawn string and limbs
// would show while nothing pulls them. The VR bow is always drawn from this rest; only the drawing hand bends its string
// (the limbs do not flex).
constexpr auto& kBowRest = rig::bow_rest;
// The string: a proximity-gated grab (the input pressed while the string hand is near the string's rest; pressed elsewhere,
// nothing until it is let go), held until the input drops below the lower threshold, the draw measured from the rest.
constexpr float kGrabRadius = .15f, kGrabPress = .55f, kGrabLetGo = .35f, kMinDraw = .12f, kMaxReach = 1.5f;
struct BowPose {
    bool ok = false;
    uint64_t frame = ~0ull;
    uint32_t pose_seq = 0;
    V3 palm{}, forward{}, up{};
    M3 target{};      // bow model axes -> world, scaled by the VR tab's bow size
    V3 rest{};        // the string's centre at rest (world)
    V3 nock{};        // the string's centre now: the drawing hand while held, else the rest
    bool held = false;
    float draw = 0;   // metres behind the rest along the aim, while held
    bool hidden = false;   // the bow's grip let go ("Grip holds the bow"): bow and nocked arrow gone at once, not after
                           // the game's lowering and put-away animation
};
struct BowString {
    bool held = false, pressed = false;
    bool committed = false;   // drawn past kMinDraw with "Bow fires on string release": the host holds the game's ZR
    float input = 0;          // the last string-hand reading while tracked (a tracking loss is never a let-go)
    uint64_t release_frame = 0;   // the last let-go that fired, and the drawn line at that moment: the game's shot (a
    V3 release_palm{}, release_forward{};   // frame or two later) and the reticle keep it, not the bow hand's own pointing
    float rumble_sent = 0;    // the string hand's last draw rumble, and when (sent ~10 times a second, not every frame)
    std::chrono::steady_clock::time_point rumble_at{};
    int hand = -1;
    uint64_t frame = 0;
    uint32_t releases = 0, cancels = 0;
    bool gripped = false, hidden = false;   // the bow hand's grip held the bow while out; let go since (until put away)
};
std::mutex bow_mu;
BowPose bow_frame;                          // (bow_mu) the committed solution of the latest game frame
BowString bow_string;                       // (bow_mu)
struct NativeBow { uint64_t frame = ~0ull; Aff world{}; } native_bow;   // (bow_mu) the game's own bow root this frame
std::atomic<uint64_t> bow_model_frame{0};   // the last game frame the bow model was drawn (the bow is out while not aiming)

V3 bow_point(const BowPose& b, V3 model) { return add(b.palm, apply(b.target, sub(model, kBowGrip))); }

BowPose bow_pose(uint64_t frame, const LinkBody& body) {
    {
        std::lock_guard lk(bow_mu);
        if (bow_frame.frame == frame && bow_frame.pose_seq == body.pose_seq) return bow_frame;
    }
    BowPose b;
    b.frame = frame, b.pose_seq = body.pose_seq;
    const LinkHand& hand = body.hand[body.bow_hand];
    const LinkHand& string = body.hand[1 - body.bow_hand];
    if (!hand.valid) {
        std::lock_guard lk(bow_mu);
        if (bow_string.held) bow_string.held = false, bow_string.cancels++, logf("vr: bow string cancelled (bow hand lost), no shot");
        bow_string.frame = frame;
        return b;
    }
    const M3 calibrated = mul(columns(v3(hand.basis), v3(hand.basis + 3), v3(hand.basis + 6)), local_euler(body.wrist_deg[body.bow_hand]));
    V3 palm = v3(hand.position);
    {
        std::lock_guard lk(cache_mu);
        if (const Entry* source = canonical_rig(frame, body)) {
            const rig::Model& model = source->adult ? rig::adult : rig::child;
            const rig::Arm& arm = model.arm[body.bow_hand];
            // Metacarpal landmark stays stable as the fingers curl. Use the final wrist skin transform so IK reach limits
            // and calibration affect the visible palm and the bow together.
            const V3 socket = add(v3(arm.wrist_pos), mul(sub(v3(arm.middle), v3(arm.wrist_pos)), .58f));
            const float* wrist = source->out->data() + arm.wrist * 12;
            const V3 fb = unit(sub(v3(arm.middle), v3(arm.wrist_pos)), {0, -1, 0});
            const V3 ab = unit(perp(sub(v3(arm.index), v3(arm.pinky)), fb), {0, 0, 1});
            const V3 surface = mul(cross(fb, ab), body.bow_hand ? -.012f : .012f);
            // The skeleton line is inside the hand. Offset to the palm surface, then into the handle by its radius.
            palm = add(source->origin, point(wrist, add(socket, surface)));
        }
    }
    // (the palm offsets were fitted on the left hand: across the palm, x, mirrors for the right; the OpenXR grip's +x is
    // the palm's normal for the left hand and the back of the hand for the right)
    const V3 grip_offset{body.bow_hand ? -body.bow_grip[0] : body.bow_grip[0], body.bow_grip[1], body.bow_grip[2]};
    b.palm = add(palm, apply(calibrated, grip_offset));
    // The slingshot (vr/items/slingshot/sling_rig.inc): its fork is the handle the shot leaves from, its pouch at rest the
    // string's centre, and at rest it aims from the pouch through the fork
    Aff sling{};
    const bool slingshot = body.sling && sling_place(frame, body, &sling);
    const V3 sling_rest = slingshot ? sling_point(sling, rig::sling_pouch) : V3{};
    if (slingshot) b.palm = sling_point(sling, rig::sling_fork);
    const auto orient = [&](V3 forward) {
        V3 up = unit(perp(mul(column(calibrated, 2), -1), forward), {0, 1, 0});
        up = unit(perp(up, forward), {0, 1, 0});
        const V3 x = mul(forward, -1), y = unit(cross(up, x), {0, 0, 1});
        up = unit(cross(x, y), up);
        b.forward = forward, b.up = up;
        // In the right hand the bow turns half a turn about its own string axis (limbs swapped, sides swapped, a symmetric
        // bow looks the same): the arrow then rides on the side away from the hand, as it does in the left. (Not a mirror:
        // that would turn the mesh's faces inside out.)
        const float flip = body.bow_hand ? -1.0f : 1.0f;
        b.target = columns(mul(x, body.bow_scale), mul(y, body.bow_scale * flip), mul(up, body.bow_scale * flip));
        b.rest = slingshot ? sling_rest : bow_point(b, kBowNock);
    };
    const V3 hand_forward = slingshot ? unit(sub(b.palm, sling_rest), {0, 0, -1}) : unit(mul(column(calibrated, 1), -1), {0, 0, -1});
    orient(hand_forward);
    const V3 sp = v3(string.position);
    std::lock_guard lk(bow_mu);
    if (bow_frame.frame == frame && bow_frame.pose_seq == body.pose_seq) return bow_frame;   // (another pass got here first)
    BowString& st = bow_string;
    if (st.frame != frame) {   // (once per game frame, never once per pass or eye)
        const bool gap = st.frame && (frame < st.frame || frame - st.frame > 4);   // (no first-person body in between)
        st.frame = frame;
        // String-hand input: either the grip or the trigger (VR tab "Bow string hand input"), independent of remapping
        if (string.valid)
            st.input = opts.vr_bow_string_trigger == 1 ? string.trigger : opts.vr_bow_string_trigger == 2 ? string.squeeze
                                                                                                         : std::max(string.trigger, string.squeeze);
        const float input = st.input;
        const bool pressed = st.pressed ? input > kGrabLetGo : input > kGrabPress;
        const bool press = pressed && !st.pressed;
        st.pressed = pressed;
        const uint64_t seen = bow_model_frame.load(std::memory_order_relaxed);
        const bool present = body.bow_equipped || (seen && frame >= seen && frame - seen <= 3);
        const bool fires = opts.vr_bow_string_fires != 0;
        {   // "Grip holds the bow": its grip let go puts it away (the host taps B); hide it now, not after the game's animation
            const bool grip_mode = fires && opts.vr_bow_grip_hold;
            const float g = hand.squeeze;
            if (!present || !grip_mode) st.gripped = st.hidden = false;
            else if (g > kGrabPress) st.gripped = true, st.hidden = false;
            else if (st.gripped && g < kGrabLetGo && !st.held && !st.committed) st.hidden = true;
        }
        const auto rumble = [&](float amplitude, float seconds) { hookshot_vr_rumble(1 - body.bow_hand, amplitude, seconds); };
        // Firing goes through the game: once drawn past kMinDraw the host holds the game's ZR (its own draw, sound and
        // nocking), and the real let-go releases it, so the game fires its own shot (ammo, magic, damage). A let-go before
        // that never pressed ZR: no shot. Once ZR is held, only a real let-go releases it (the game fires on any ZR
        // release): the menu, a lost string hand or a handedness change take the string out of the hand but keep the
        // game's draw held until the input is let go; the bow put away or first person interrupted (the game no longer
        // aiming) ends it.
        if (st.held || st.committed) {
            const float draw = st.held ? std::max(0.0f, dot(sub(b.rest, sp), hand_forward)) : 0.0f;
            if (st.held && !st.committed && fires && draw >= kMinDraw) {
                st.committed = true;
                logf("vr: bow drawn past %.2f m: the game's draw (ZR) held for the string", kMinDraw);
            }
            const char* end = gap ? "first person interrupted" : !present ? "bow put away" : nullptr;
            const char* pause = body.menu ? "menu opened" : !string.valid ? "string hand lost" :
                                st.hand != body.bow_hand ? "bow hand changed" :
                                length(sub(sp, b.palm)) > kMaxReach ? "string hand too far (tracking)" : nullptr;
            if (end) {
                st.cancels++;
                logf("vr: bow string cancelled (%s), no shot%s", end, st.committed ? " (the game's draw let go while not aiming)" : "");
                st.held = st.committed = false;
            } else if (!pressed && string.valid && !body.menu) {   // a real let-go
                if (st.committed) {
                    st.releases++;
                    // (the line the arrow was drawn along: the last frame's committed solution, while the string was held)
                    st.release_frame = frame;
                    st.release_palm = bow_frame.held ? bow_frame.palm : b.palm;
                    st.release_forward = bow_frame.held ? bow_frame.forward : b.forward;
                    logf("vr: bow string released at %.2f m draw: the game's ZR let go, its shot (release %u)", draw, st.releases);
                } else if (st.held) {
                    st.cancels++;
                    logf("vr: bow string let go at %.2f m (under %.2f m%s): no shot", draw, kMinDraw, fires ? "" : ", or string firing off");
                    if (!fires && draw >= kMinDraw) st.cancels--, st.releases++;
                }
                st.held = st.committed = false;
            } else if (pause && st.held) {
                st.held = false;
                if (!st.committed) st.cancels++;
                logf("vr: bow string out of the hand (%s)%s", pause,
                     st.committed ? ": the game's draw stays held; the next let-go fires" : ", no shot");
            }
            // The string hand feels the draw: nothing at rest, stronger as it comes back
            const float amplitude = st.held && draw > .03f ? .08f + .72f * std::clamp((draw - .03f) / .45f, 0.0f, 1.0f) : 0.0f;
            const auto now = std::chrono::steady_clock::now();
            if (amplitude > 0 && (std::fabs(amplitude - st.rumble_sent) > .06f || now - st.rumble_at > std::chrono::milliseconds(90))) {
                rumble(amplitude, .12f);   // (overlapping 120 ms pulses ~10 a second: a steady feel without flooding the runtime)
                st.rumble_sent = amplitude, st.rumble_at = now;
            } else if (amplitude == 0) st.rumble_sent = 0;
        } else if (press) {
            if (gap || !present || body.menu || !string.valid || length(sub(sp, b.rest)) > kGrabRadius) {
                // (pressed away from the string: no grab until the input is let go and pressed again near it)
            } else {
                st.held = true, st.hand = body.bow_hand;
                logf("vr: bow string grabbed (%.2f m from its rest)", length(sub(sp, b.rest)));
            }
        }
    }
    b.held = st.held;
    b.hidden = st.hidden;
    if (b.held && length(sub(v3(hand.position), sp)) > .15f) orient(unit(sub(b.palm, sp), hand_forward));
    b.nock = b.held ? sp : b.rest;
    b.draw = b.held ? std::max(0.0f, dot(sub(b.rest, sp), b.forward)) : 0.0f;
    b.ok = true;
    bow_frame = b;
    {
        std::lock_guard sk(bow_solution_mu);
        // (the handle/palm socket and the aim; for 20 frames after a firing let-go, the line it was drawn along)
        const bool released = bow_string.release_frame && frame >= bow_string.release_frame && frame - bow_string.release_frame <= 20;
        const V3 palm = released ? bow_string.release_palm : b.palm, forward = released ? bow_string.release_forward : b.forward;
        latest_bow[0] = palm.x, latest_bow[1] = palm.y, latest_bow[2] = palm.z;
        latest_bow[3] = forward.x, latest_bow[4] = forward.y, latest_bow[5] = forward.z;
        std::memcpy(latest_bow + 6, string.position, 3 * sizeof(float));
        latest_bow[9] = b.draw;
        // flags: 1 string held, 2 the game's ZR wanted (a committed draw), 4 bow in the right hand, 8 string firing on,
        // 16 flips with every firing let-go (the host's "Grip holds the bow" fires on that, its ZR being held throughout)
        latest_bow[10] = (float)((b.held ? 1 : 0) | (bow_string.committed ? 2 : 0) | (body.bow_hand ? 4 : 0) |
                                 (opts.vr_bow_string_fires ? 8 : 0) | (bow_string.releases & 1 ? 16 : 0));
        latest_bow[11] = (float)body.pose_seq;
        latest_bow_at = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    return b;
}
