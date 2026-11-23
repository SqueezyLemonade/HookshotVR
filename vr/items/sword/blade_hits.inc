/* Hookshot VR: sword hits decided by the VR blade, given to the game through its own attack sensor (game side). VR tab Sword
   hits (option 1005): Blade, the default; Button, a swing presses the attack button as before (vr/input/controllers_host.c
   vr_swing_step).

   Why (headset 22 Nov, child Link): with the button, a swing plays Link's own attack, whose sensor is live only in that
   attack's window: the VR sword went through a Deku Baba's stalk three times before one counted, the white trail of Link's
   slash showed where the VR blade was not, and quick ordinary moves attacked.
   How (the sword-hit calls research, its design A):
   1. The swing is ours (vr_stroke_step), measured in the room (vr_blade_room: Link's movement is no swing): the blade's
      tip (at most kVrStrokePoint along it) faster than the VR tab's Swing speed, having gone its Swing arc one way, not
      as a pull-back (along the blade, tip trailing) (a turn of more than 90 degrees from the way it was going starts
      over), its chord most of its path (a wiggle never is). It ends when the blade slows under kVrStrokeKeep or turns back;
      the next stroke may start at once (a backhand after a forehand). The hand's own speed is not asked: a wrist cut counts.
   2. The contact is ours (vr_blade_sweep): the weapon's attack sensor (its row: guard to tip along the actor's z, its
      thickness), swept from its pose at the weapon's last update to this one in sub-steps of at most kVrSweepStep at the
      tip, against the targets near Link (actors seen in their own updates: enemies, objects, animals, as vertical
      capsules, kVrTargetShapes). The game tests the real sensor only where the actor is at its update (it is warped there,
      never swept: research section 2), so a fast swing could pass a stalk between two updates.
   3. The hit is the game's: once a stroke begins with no target of ours in reach, the sensor is set up for the stroke (on
      until it ends, at most kVrHitMaxNs) as the blade goes, so what is not an actor near Link (grass) is found by the
      game's own sensor. On contact during a stroke, the weapon's actor is held at the swept pose
      that came closest to the target (a pose the VR blade really passed through), and on Link's next update the game's own
      attack is set up (or the stroke's is kept on) on Link's SensorHit, f_808f29c0(sensorHit, info) (what the AI node ExecuteSensorHitSetupAttack calls:
      the weapon row's AttackType, the Weapon At sensor), for kVrHitUpdates of Link's updates (at most kVrHitMaxNs), then
      switched off, f_808f56f0(sensorHit, Weapon). One a stroke. Damage, guards, stalk or head, switches, pots and grass stay
      the game's: the contact is real. Switched off early when the weapon's updates stop (put away, a scene change), when
      Link misses an update, or when Link's actor changes.
   No cooldown of ours and nothing tied to Link's animation: a stroke may follow the last at once (forehand, backhand). The
   limits are one setup a stroke, the re-arm when the stroke ends or turns back, and the game's own invulnerability after a
   target is hit (counted in the log: kVrRehitNs). A stab (the blade going along its own axis, tip first) is a thrust and
   takes the row's thrust AttackType (vr_blade_kind).
   4. Drawn without attacking: hits land only while Link stands in his drawn stance (action state 0x410). Taking the sword
      asks the game's own draw, f_80a63030(Link, 1, 1) (OneShotPlayerRequestEquipmentDrawn's call: kind 1 the sword and
      shield), on Link's next update; while a VR hand holds it, his sub action is kept on Drawn (his AI's requests made
      drawn in flight, the selector's forced change) and his own slash refused (its requests made his movement). No
      attack button, no attack of his, no slash trail.
   The log (bounded): each stroke that came near a target, how it ended ("hit set up", "no contact", "contact without a
   swing", "contact, sensor not set: why", "the game refused it"); the game's own setups and switch-offs on Link's SensorHit
   (the hooks, vr/vr_game_hooks.py), ours marked.
   RX_VR_SWORD_INFO=<direction>,<AttackType>,<attributes>,<rate>,<mask> replaces the weapon row's info (a test).
   A piece of vr/items/sword/sword_host.c (its helpers, and vr/items/bomb/bomb_host.c's), included before sword_test.inc.
   Not compiled on its own. */
#define kVrSetupAttack 0x808f29c0ULL        /* (f_808f29c0(sensorHit, info): an attack set up, as the AI node does) */
#define kVrAtSensorOff 0x808f56f0ULL        /* (f_808f56f0(sensorHit, kind): one At sensor switched off) */
#define kVrDrawEquipment 0x80a63030ULL      /* (f_80a63030(actor, kind, immediate): OneShotPlayerRequestEquipmentDrawn's call) */
#define kVrAtWeapon 0                       /* (the At sensor kinds' first: "Weapon"; the mask's bit 0) */
#define kVrStrokePoint 0.6f                 /* (m along the blade at most: its tip (the weapon's), capped, where the arc and
                                               the speed are measured) */
#define kVrStrokeArc 25                     /* (cm the tip goes one way before a stroke counts: VR tab Swing arc, option 1009) */
#define kVrStrokeSpeed 26                   /* (tenths of m/s at the tip: VR tab Swing speed, option 1013) */
#define kVrPullCos 0.6f                     /* (a run this close to the blade's own axis, tip trailing: a pull-back) */
#define kVrStrokeKeep 1.2f                  /* (m/s: slower, the stroke is over) */
#define kVrStrokeChord 0.6f                 /* (the stroke's chord at least this much of its path) */
#define kVrStrokeGapNs 200000000LL          /* (no update this long: start over) */
#define kVrSweepStep 0.04f                  /* (m at the tip between two sub-steps) */
#define kVrSweepSteps 16
#define kVrHitUpdates 3                     /* (Link's updates the sensor stays on) */
#define kVrHitMaxNs 300000000LL
#define kVrHitAskNs 150000000LL             /* (a setup asked and not made in this long: dropped) */
#define kVrWeaponFreshNs 100000000LL        /* (the weapon's updates: none this long, switched off) */
#define kVrLinkMissNs 100000000LL           /* (Link's: none this long, a missed update) */
#define kVrTargetReach 4.0f                 /* (m from Link: farther actors are not looked at) */
#define kVrTargetFreshNs 2000000000LL       /* (a target not updated this long is dropped: the dummy, Obj_AttentionRock, updates
                                               only every second or two, headless 22 Nov; a moving enemy updates each frame) */
#define kVrNearLog 0.6f
#define kVrExitMargin 0.10f                 /* (m: a target hit is re-armed only once the blade has been this far out of it and
                                               a new stroke has begun out there (its full Swing arc gone outside it): the
                                               player's rule, headset 22 Nov: a stab sliced out sideways hit twice; then quick
                                               wrist flicks out and back in still hit again and again, 10 cm out alone) */
#define kVrRehitNs 1000000000LL             /* (the same target set up again within this: may meet the game's invulnerability) */                     /* (m: a stroke that came this close to a target is logged) */
#define kVrTargets 48
#define kVrNotTargets 256

/* ---- the swung weapons: one row each, by the actor's name. AttackType: the Player's GameBalance AttackParam row for a slash
   and for a thrust (research section 4: the game's thrust uses the slash rows, there is no thrust-only row; a stab is told
   apart all the same, vr_blade_kind, so a row of its own can go here). Guard to tip and thickness: the blade AS DRAWN, along
   the actor's own z from its origin (the model's box, RSDB ActorInfo ModelAabbMax z; the half-width of the blade, not the
   guard's), so a hit is decided where the VR blade visibly touches. The game's attack sensor is longer (WeaponAtk: Kokiri
   0.8, Master 1.5, Giant's and Biggoron's 2.2, broken 0.5, Deku Stick 1.2 m) and is placed where the blade touched, so a
   contact we find is one it finds too. The actor is placed on the same pose the renderer draws the weapon at: the grip, the
   fist's axis leaned by the VR tab's Sword angle (sword_host.c vr_held_blade_pose, vr/body/arms_and_pose.inc). */
typedef struct { const char *name, *label; uint32_t attack, thrust; float guard, tip, thick; } VrBladeWeapon;
/* (a hit while Link is in the air is his jump slash's: the user's design, 22 Nov; its AttackType (x2 damage): 22 for the
   one-handed swords (the game's own jump slash, verb 0x72, Kokiri Sword, 22 Nov 07:09), the large swords' and the
   stick's not seen (0: their own row); RX_VR_JUMP_ATTACK=<small>,<large>,<stick> replaces them) */
#define kVrAirborneVy 1.5f   /* (m/s up or down: Link in the air) */
static uint32_t vr_jump_attack(const VrBladeWeapon *w) {
    static int read;
    static uint32_t v[3] = {22u, 0u, 0u};   /* (the one-handed: 22, the game's own jump slash with the Kokiri Sword, 22 Nov) */
    if (!read) {
        read = 1;
        const char *e = vr_research_getenv("RX_VR_JUMP_ATTACK");
        if (e) sscanf(e, "%u,%u,%u", &v[0], &v[1], &v[2]);
    }
    return w->attack == 101u ? v[2] : w->tip > 1.2f ? v[1] : v[0];   /* (the stick; the long swords; the rest) */
}
/* (AttackType is the game's enum index, not the rows' hash: the game's own slashes pass 13, 14, 15 (its combo, the last
   with direction 5) and 22, with direction 2 and mask 0, headset 22 Nov with the Kokiri Sword; the hashes were refused.
   The other weapons' indices are not seen yet: they take the one-handed slash's (13) until a log shows theirs, which is
   right for its sensor (the Weapon slot's) but not its damage kind; the Deku Stick's is 101, the SubWeapon slot's) */
#define kVrAtkSmall 13u           /* (one-handed slash, the combo's first) */
#define kVrAtkLarge kVrAtkSmall   /* (two-handed: not seen yet) */
#define kVrAtkStick 101u          /* (the game's own stick swing: AttackType 101, direction 2, headset 22 Nov 06:34) */
#define kVrAtkHammer 0u
static const VrBladeWeapon kVrBladeWeapons[] = {
    {"Weapon_Sword_Kokiri", "Kokiri Sword", kVrAtkSmall, kVrAtkSmall, 0.08f, 0.53f, 0.04f},
    {"Weapon_Sword_Master", "Master Sword", kVrAtkSmall, kVrAtkSmall, 0.12f, 1.09f, 0.05f},
    {"Weapon_Sword_Giant", "Giant's Knife", kVrAtkLarge, kVrAtkLarge, 0.15f, 1.55f, 0.07f},
    {"Weapon_Sword_BrokenGiant", "broken Giant's Knife", kVrAtkLarge, kVrAtkLarge, 0.15f, 0.45f, 0.07f},
    {"Weapon_Sword_Biggoron", "Biggoron's Sword", kVrAtkLarge, kVrAtkLarge, 0.15f, 1.55f, 0.07f},
    {"Weapon_DekuStick", "Deku Stick", kVrAtkStick, kVrAtkStick, 0.05f, 0.82f, 0.05f},
    {"Weapon_Hammer", "Megaton Hammer", kVrAtkHammer, kVrAtkHammer, 1.10f, 1.45f, 0.30f},
};
#define kVrBladeWeaponCount ((int)(sizeof kVrBladeWeapons / sizeof *kVrBladeWeapons))
static int vr_blade_weapon_of(const char *name) {
    for (int i = 0; i < kVrBladeWeaponCount; i++) if (!strcmp(name, kVrBladeWeapons[i].name)) return i;
    return -1;
}

/* ---- the targets: by name, a vertical capsule from low to high above the actor's origin, this thick. First match wins */
typedef struct { const char *prefix; float low, high, radius; } VrTargetShape;
static const VrTargetShape kVrTargetShapes[] = {
    {"Enemy_Dekubaba", 0.0f, 1.3f, 0.30f},
    {"Enemy_Skullwalltula", -0.4f, 0.4f, 0.40f},
    {"Enemy_Skulltula", -0.5f, 0.5f, 0.45f},
    {"Enemy_", -0.2f, 1.6f, 0.50f},
    {"Animal_", 0.0f, 0.5f, 0.35f},
    {"Obj_", 0.0f, 0.8f, 0.40f},
    {"FldObj_", 0.0f, 0.8f, 0.40f},
};
#define kVrTargetShapeCount ((int)(sizeof kVrTargetShapes / sizeof *kVrTargetShapes))
static int vr_target_shape_of(const char *name) {
    /* (not targets: Link's own gear and held items, doors, walls and shutters, the invisible area and effect actors) */
    if (strstr(name, "Door") || strstr(name, "Shutter") || strstr(name, "Wall") || strstr(name, "Area") ||
        strstr(name, "_Dictionary")) return -1;
    for (int i = 0; i < kVrTargetShapeCount; i++)
        if (!strncmp(name, kVrTargetShapes[i].prefix, strlen(kVrTargetShapes[i].prefix))) return i;
    return -1;
}

/* ---- the swing: one stroke of the blade (a point kVrStrokePoint along it), sampled at the weapon's updates ----------- */
typedef struct {
    int have, active, number;            /* active: a stroke now (number: counted from 1) */
    int64_t then;
    float was[3], from[3], way[3];       /* (the last point; where this run of motion began; its recent way, unit) */
    float travel, peak, speed;
    int pull;                            /* (this run is a pull-back: never a stroke) */
} VrStroke;
/* one sample (p the blade's point, axis its way, unit, tip first; NULL not known): returns 1 when a stroke has just begun,
   -1 when one has just ended, else 0. A run that starts along the blade's own axis, tip trailing, is a pull-back and never
   a stroke (headset 22 Nov: a thrust counted twice, the pull-back the second); arc: the travel one way it needs (m) */
static int vr_stroke_step(VrStroke *s, const float p[3], const float *axis, int64_t now, float swing_speed, float arc) {
    const float dt = (float)(now - s->then) * 1e-9f;
    if (!s->have || now - s->then > kVrStrokeGapNs || dt < 0) {
        const int ended = s->active ? -1 : 0;
        memcpy(s->was, p, sizeof s->was), s->then = now, s->have = 1;
        s->active = 0, s->travel = s->peak = s->speed = 0, s->pull = 0;
        return ended;
    }
    if (dt < 0.001f) return 0;   /* (a second update in the same moment) */
    float d[3];
    for (int k = 0; k < 3; k++) d[k] = p[k] - s->was[k];
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    s->speed = dist / dt;
    int out = 0;
    if (s->speed < kVrStrokeKeep || dist < 1e-5f) {   /* (slow: any run of motion is over) */
        if (s->active) out = -1;
        s->active = 0, s->travel = s->peak = 0;
    } else {
        float u[3];
        for (int k = 0; k < 3; k++) u[k] = d[k] / dist;
        const float turn = u[0] * s->way[0] + u[1] * s->way[1] + u[2] * s->way[2];
        if (s->travel > 0 && turn < 0) {   /* (turned back: what went before is over, this starts a new run) */
            if (s->active) out = -1;
            s->active = 0, s->travel = s->peak = 0;
        }
        if (s->travel == 0) {   /* (a new run: a pull-back when it starts tip trailing) */
            memcpy(s->from, s->was, sizeof s->from), memcpy(s->way, u, sizeof s->way);
            s->pull = axis && u[0] * axis[0] + u[1] * axis[1] + u[2] * axis[2] <= -kVrPullCos;
        }
        else {   /* (the way follows an arc's turn) */
            float n = 0;
            for (int k = 0; k < 3; k++) s->way[k] = s->way[k] * 0.5f + u[k] * 0.5f, n += s->way[k] * s->way[k];
            n = sqrtf(n);
            for (int k = 0; k < 3; k++) s->way[k] = n > 1e-6f ? s->way[k] / n : u[k];
        }
        s->travel += dist, s->peak = fmaxf(s->peak, s->speed);
        float chord = 0;
        for (int k = 0; k < 3; k++) chord += (p[k] - s->from[k]) * (p[k] - s->from[k]);
        if (!s->active && !s->pull && s->travel >= arc && s->peak >= swing_speed && sqrtf(chord) >= kVrStrokeChord * s->travel)
            s->active = 1, s->number++, out = 1;
    }
    memcpy(s->was, p, sizeof s->was), s->then = now;
    return out;
}

/* a stroke's kind when it begins: a thrust (1) when the blade went mostly along its own axis, tip first (its way within
   kVrThrustCos of the blade's z), else a slash (0) */
#define kVrThrustCos 0.75f
static int vr_blade_kind(const VrStroke *s, const float axis[3]) {
    return s->way[0] * axis[0] + s->way[1] * axis[1] + s->way[2] * axis[2] >= kVrThrustCos;
}

/* ---- the blade in the room (the controller's tracking space, as vr/headset/bridges_host.c gives it): Link's own movement
   in the world, running or turned, is no swing (headset 22 Nov: the sword held still while running into an enemy hit) */
typedef struct { int valid; float grip_q[4], grip_p[3], aim_q[4], aim_p[3], lin_vel[3], ang_vel[3]; float trigger, squeeze,
                 stick[2]; uint32_t buttons; } VrBladeHand;   /* (HookshotVrHand, vr/input/controllers_host.c) */
typedef struct { uint32_t seq; VrBladeHand hand[2]; } VrBladeHands;
int rx_vr_hands(void *out);
/* the point `along` the held blade and the blade's way (unit, tip first), in the room: the grip's fist axis leaned by the
   VR tab's Sword angle, in the grip's frame (0, -sin a, -cos a), as controllers_host.c vr_blade_speed and the drawn hand */
static int vr_blade_room(int hand, float along, float p[3], float axis[3]) {
    VrBladeHands v;
    if (hand < 0 || hand > 1 || !rx_vr_hands(&v) || !v.hand[hand].valid) return 0;
    const VrBladeHand *h = &v.hand[hand];
    const float a = rx_option(914, 35) * 0.0174533f, b[3] = {0, -sinf(a), -cosf(a)};
    const float *q = h->grip_q;   /* (x y z w: b turned by q, b + 2w (q x b) + 2 q x (q x b)) */
    const float c[3] = {q[1] * b[2] - q[2] * b[1], q[2] * b[0] - q[0] * b[2], q[0] * b[1] - q[1] * b[0]};
    const float cc[3] = {q[1] * c[2] - q[2] * c[1], q[2] * c[0] - q[0] * c[2], q[0] * c[1] - q[1] * c[0]};
    float n = 0;
    for (int k = 0; k < 3; k++) axis[k] = b[k] + 2 * (q[3] * c[k] + cc[k]), n += axis[k] * axis[k];
    n = sqrtf(n);
    if (!(n > 1e-6f)) return 0;
    for (int k = 0; k < 3; k++) axis[k] /= n, p[k] = h->grip_p[k] + axis[k] * along;
    return 1;
}

/* ---- the contact: the blade (guard to tip) against a vertical capsule ------------------------------------------------ */
/* the closest distance between segments a0-a1 and b0-b1 (and where along a, 0..1) */
static float vr_segment_gap(const float a0[3], const float a1[3], const float b0[3], const float b1[3], float *along) {
    float u[3], v[3], w[3];
    for (int k = 0; k < 3; k++) u[k] = a1[k] - a0[k], v[k] = b1[k] - b0[k], w[k] = a0[k] - b0[k];
    const float a = u[0] * u[0] + u[1] * u[1] + u[2] * u[2], b = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    const float cc = v[0] * v[0] + v[1] * v[1] + v[2] * v[2], d = u[0] * w[0] + u[1] * w[1] + u[2] * w[2];
    const float e = v[0] * w[0] + v[1] * w[1] + v[2] * w[2], den = a * cc - b * b;
    float s = den > 1e-9f ? (b * e - cc * d) / den : 0, t;
    s = fminf(1, fmaxf(0, s));
    t = cc > 1e-9f ? (b * s + e) / cc : 0;
    if (t < 0 || t > 1) {   /* (clamped on b: a's point again) */
        t = fminf(1, fmaxf(0, t));
        s = a > 1e-9f ? fminf(1, fmaxf(0, (b * t - d) / a)) : 0;
    }
    float g = 0;
    for (int k = 0; k < 3; k++) { const float x = w[k] + u[k] * s - v[k] * t; g += x * x; }
    if (along) *along = s;
    return sqrtf(g);
}
/* a pose between two (origin, rot: the actor's axes as columns), at f: the origin and the blade's z mixed, x kept square to it */
static void vr_pose_mix(const float o0[3], const float r0[9], const float o1[3], const float r1[9], float f, float o[3], float r[9]) {
    float z[3], x[3], y[3], n = 0, m = 0;
    for (int k = 0; k < 3; k++) {
        o[k] = o0[k] + (o1[k] - o0[k]) * f;
        z[k] = r0[3 * k + 2] + (r1[3 * k + 2] - r0[3 * k + 2]) * f, n += z[k] * z[k];
    }
    n = sqrtf(n);
    if (n < 1e-6f) { memcpy(r, f < 0.5f ? r0 : r1, 9 * sizeof *r); return; }
    for (int k = 0; k < 3; k++) z[k] /= n, x[k] = r0[3 * k] + (r1[3 * k] - r0[3 * k]) * f;
    const float xz = x[0] * z[0] + x[1] * z[1] + x[2] * z[2];
    for (int k = 0; k < 3; k++) x[k] -= z[k] * xz, m += x[k] * x[k];
    m = sqrtf(m);
    if (m < 1e-6f) { memcpy(r, f < 0.5f ? r0 : r1, 9 * sizeof *r); return; }
    for (int k = 0; k < 3; k++) x[k] /= m;
    y[0] = z[1] * x[2] - z[2] * x[1], y[1] = z[2] * x[0] - z[0] * x[2], y[2] = z[0] * x[1] - z[1] * x[0];
    for (int k = 0; k < 3; k++) r[3 * k] = x[k], r[3 * k + 1] = y[k], r[3 * k + 2] = z[k];
}
/* the sweep from the last pose (o0, r0) to this one: the sub-step pose (o, r) closest to the capsule (at, low..high, its
   radius), its gap past the two thicknesses (<= 0: touching), and the sub-step's index (0: the last pose) */
static float vr_blade_sweep(const VrBladeWeapon *w, const float o0[3], const float r0[9], const float o1[3], const float r1[9],
                            const float at[3], const VrTargetShape *shape, float o[3], float r[9], int *step) {
    float tip0[3], tip1[3], dt = 0;
    for (int k = 0; k < 3; k++) {
        tip0[k] = o0[k] + r0[3 * k + 2] * w->tip, tip1[k] = o1[k] + r1[3 * k + 2] * w->tip;
        dt += (tip1[k] - tip0[k]) * (tip1[k] - tip0[k]);
    }
    int n = (int)ceilf(sqrtf(dt) / kVrSweepStep);
    if (n < 1) n = 1;
    if (n > kVrSweepSteps) n = kVrSweepSteps;
    const float b0[3] = {at[0], at[1] + shape->low, at[2]}, b1[3] = {at[0], at[1] + shape->high, at[2]};
    float best = 1e9f;
    for (int i = 0; i <= n; i++) {
        float po[3], pr[9], g0[3], g1[3];
        vr_pose_mix(o0, r0, o1, r1, (float)i / (float)n, po, pr);
        for (int k = 0; k < 3; k++) g0[k] = po[k] + pr[3 * k + 2] * w->guard, g1[k] = po[k] + pr[3 * k + 2] * w->tip;
        const float gap = vr_segment_gap(g0, g1, b0, b1, NULL) - w->thick - shape->radius;
        if (gap < best) best = gap, memcpy(o, po, 3 * sizeof *o), memcpy(r, pr, 9 * sizeof *r), *step = i;
    }
    return best;
}

/* ---- the targets near Link, from every actor's update (their own threads: under the lock) ----------------------------- */
typedef struct { uint64_t actor; int64_t seen; float at[3]; int shape, locked, left; char name[40]; } VrBladeTarget;
/* (locked: hit, and not re-armed since; left: the blade has been out of it since, a new stroke not yet begun outside it) */
static VrBladeTarget vr_blade_targets[kVrTargets];
static SRWLOCK vr_blade_lock = SRWLOCK_INIT;   /* (the targets, and the hit below) */
static int vr_blade_on(void) { return rx_option(1005, 1) == 1; }
static void vr_blade_target_note(Ctx *c, uint64_t actor) {
    static volatile uint64_t not_targets[kVrNotTargets];   /* (actors near Link already read and not targets, by address) */
    if (!vr_blade_on() || !hos_guest_range(actor + 796, 12)) return;
    float at[3], d[3];
    for (int k = 0; k < 3; k++) at[k] = vr_bomb_f(c, actor + 796 + 4 * k), d[k] = at[k] - vr_link_pos[k];
    if (!(d[0] * d[0] + d[2] * d[2] < kVrTargetReach * kVrTargetReach) || fabsf(d[1]) > kVrTargetReach) return;
    volatile uint64_t *known = &not_targets[(actor >> 4) % kVrNotTargets];
    if (__atomic_load_n(known, __ATOMIC_RELAXED) == actor) return;
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&vr_blade_lock);
    VrBladeTarget *t = NULL, *free_one = NULL;
    for (int i = 0; i < kVrTargets && !t; i++) {
        if (vr_blade_targets[i].actor == actor) t = &vr_blade_targets[i];
        else if (!free_one && (!vr_blade_targets[i].actor || now - vr_blade_targets[i].seen > kVrTargetFreshNs))
            free_one = &vr_blade_targets[i];
    }
    if (!t) {
        ReleaseSRWLockExclusive(&vr_blade_lock);
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        const int shape = vr_target_shape_of(name);
        if (shape < 0) {
            __atomic_store_n(known, actor, __ATOMIC_RELAXED);
            return;
        }
        AcquireSRWLockExclusive(&vr_blade_lock);
        if (!free_one || (free_one->actor && free_one->actor != actor && now - free_one->seen <= kVrTargetFreshNs)) {
            ReleaseSRWLockExclusive(&vr_blade_lock);
            return;
        }
        t = free_one, t->actor = actor, t->shape = shape, t->locked = t->left = 0;
        snprintf(t->name, sizeof t->name, "%s", name);
    }
    t->seen = now, memcpy(t->at, at, sizeof t->at);
    ReleaseSRWLockExclusive(&vr_blade_lock);
}

/* ---- the hit: asked by the weapon's update, made and ended on Link's (both the game's threads: under the lock) --------- */
enum { VR_HIT_NONE, VR_HIT_ASKED, VR_HIT_ON };
static struct {
    int state, weapon, stroke, kind, updates, ended, mine, hand;   /* (kind: 1 a thrust; ended: the stroke is over; mine: ours runs) */
    int late, sets;                                      /* (late: Hit test Late; sets: setups made this stroke) */
    uint64_t actor, target, link, sensor_hit, weapon_sh;  /* (weapon_sh: the held weapon's own SensorHit, suspended) */
    uint8_t info[0x20];
    int64_t asked_at, on_at, weapon_seen;
    float o[3], r[9], gap;
    char target_name[40];
} vr_hit;
static volatile int64_t vr_link_update_at;
int hos_vr_sword_drawn(void);   /* (vr/items/gear/equipment_host.c) */
static volatile int vr_blade_draw_wanted, vr_stick_draw_wanted;
int hos_vr_stick_holder(void);   /* (vr/input/controllers_host.c) */
/* (vr/input/controllers_host.c vr_stick, any thread) the Deku Stick's slot drawn by the game's own per-slot draw,
   f_806d6820(Link's EquipmentUser, 1) (what f_80a63030 calls for each slot it draws), on Link's next update; always 1 */
#define kVrDrawSlot 0x806d6820ULL
#define kVrStickSlot 1
int hos_vr_stick_draw(void) { __atomic_store_n(&vr_stick_draw_wanted, 1, __ATOMIC_RELEASE); return 1; }
/* (vr/input/controllers_host.c, any thread) the game's own draw asked, made on Link's next update; always 1 */
int hos_vr_sword_draw(void) { __atomic_store_n(&vr_blade_draw_wanted, 1, __ATOMIC_RELEASE); return 1; }

/* Link's SensorHit (f_808f6320: his components +0x3b8), 0 none */
static uint64_t vr_link_sensor_hit(Ctx *c, uint64_t link) {
    if (!link || !hos_guest_range(link + 0x270, 8)) return 0;
    const uint64_t parts = recomp_load64(c, link + 0x270);
    if (parts < HOS_HEAP_BASE || !hos_guest_range(parts + 0x3b8, 8)) return 0;
    const uint64_t sh = recomp_load64(c, parts + 0x3b8);
    return sh >= HOS_HEAP_BASE && hos_guest_range(sh, 0x100) ? sh : 0;
}

/* a game function called with x0, x1, x2, and (blob) n bytes copied to scratch memory below the caller's stack in x1 */
static uint64_t vr_blade_call(Ctx *c, uint64_t fn, uint64_t x0, uint64_t x1, uint64_t x2, const uint8_t *blob, int n) {
    A64Fn f = a64_lookup(fn);
    if (!f) return 0;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x200) & ~0xfULL;
    if (blob) {
        x1 = c->x[31] + 0x100;
        for (int i = 0; i < n; i++) recomp_store8(c, x1 + i, blob[i]);
    }
    c->x[0] = x0, c->x[1] = x1, c->x[2] = x2;
    f(c);
    const uint64_t answer = c->x[0];
    *c = saved;
    return answer;
}

static volatile int vr_blade_calling;   /* (our own call into the game running: the hooks mark it ours) */
static volatile int64_t vr_link_setup_at;
/* the weapon row's attack info for f_808f29c0 (0x20 bytes, as f_808f28c0 builds it from the node's inputs): +0 direction,
   +4 AttackType, +8 attributes, +0xc the damage rate, +0x10 the At sensors' mask (the game's own pass 0: the row's own
   sensors, the slash's Weapon and Elbow_L; ours 1, the Weapon sensor alone), +0x18 18, +0x1c 0, +0x1e 0 */
static void vr_blade_info(const VrBladeWeapon *w, int thrust, uint8_t info[0x20]) {
    uint32_t dir = 2, type = thrust == 2 && vr_jump_attack(w) ? vr_jump_attack(w) : thrust == 1 ? w->thrust : w->attack, attr = 0;
    /* (direction 2: the game's own slashes'; thrust: 0 a slash, 1 a thrust, 2 in the air) */
    float rate = 1;
    const int test = rx_option(1007, 0);   /* (VR tab Hit test: ... all, the attack's own sensors, as the game passes them) */
    uint64_t mask = test == 1 || test == 3 ? 0 : 1ULL << kVrAtWeapon;
    static int read;
    static char env[128];
    if (!read) {
        read = 1;
        const char *e = vr_research_getenv("RX_VR_SWORD_INFO");
        snprintf(env, sizeof env, "%s", e ? e : "");
        if (env[0]) hos_log("vr: blade: RX_VR_SWORD_INFO=%s (the weapon rows' info replaced)", env);
    }
    if (env[0]) {
        char *p = env, *q;
        dir = (uint32_t)strtoul(p, &q, 0); if (*q == ',') p = q + 1;
        type = (uint32_t)strtoul(p, &q, 0); if (*q == ',') p = q + 1;
        attr = (uint32_t)strtoul(p, &q, 0); if (*q == ',') p = q + 1;
        rate = strtof(p, &q); if (*q == ',') p = q + 1;
        mask = strtoull(p, &q, 0);
    }
    memset(info, 0, 0x20);
    const uint32_t eighteen = 18;
    memcpy(info + 0, &dir, 4), memcpy(info + 4, &type, 4), memcpy(info + 8, &attr, 4), memcpy(info + 0xc, &rate, 4);
    memcpy(info + 0x10, &mask, 8), memcpy(info + 0x18, &eighteen, 4);
}

/* ---- when in the game's frame (sword-sensor-dive.md, 22 Nov): Link's update runs the SensorHit pre-step (f_82ec3d60: +0x21
   = +0x20, +0x20 = 0), his AI, then the SensorHit calc (f_82ec3ef0: the attacks set up from his animation events, then
   every attack whose +0x20 bit dropped retired, 0x82ec415c), and only then the movement updates where sword_host.c runs.
   A setup made there was retired at the next calc every frame. So the setup is made at the calc's own entry (the hook,
   hos_vr_sensor_calc), every frame while ours is on, and ending it is only not setting it up again: the game retires it,
   as its own (it never calls f_808f56f0 on Link's SensorHit). While ours is on, the held weapon's own SensorHit is
   suspended as during the game's slash: reason bit 4 in its +0x26 (f_808f6340 sets, f_82ec6230 clears), its Weapon At
   sensor off (f_808f56f0(its SensorHit, 0)), so the hit goes through Link's. VR tab Hit test (option 1007, a test): Early
   (all of this, the Weapon sensor alone), Early all (the attack's own sensors, mask 0, as the game passes), No suspend,
   Late (the setup at Link's movement update, renewed there, and f_808f56f0 at the end: what did not hit). */
#define kVrSensorSuspend 0x808f6340ULL      /* (f_808f6340(sensorHit, bit): a reason its own bodies are suspended, set) */
#define kVrSensorResume 0x82ec6230ULL       /* (f_82ec6230(sensorHit, bit): cleared) */
#define kVrSensorBody 0x80236670ULL         /* (f_80236670(sensorHit, kind): the At sensor's body) */
#define kVrSuspendBit 4                     /* (the bit the game's sword-equipment state sets, f_808f6550) */
static int vr_hit_test(void) { const int t = rx_option(1007, 0); return t < 0 || t > 3 ? 0 : t; }
static volatile int vr_hit_live;   /* (ours on, made at the calc: read before taking the lock) */

static void vr_blade_off(Ctx *c, const char *why) {   /* (under the lock, on Link's update) */
    if (vr_hit.state == VR_HIT_ON && vr_hit.sensor_hit) {
        if (vr_hit.late) {
            __atomic_store_n(&vr_blade_calling, 1, __ATOMIC_RELAXED);
            vr_blade_call(c, kVrAtSensorOff, vr_hit.sensor_hit, kVrAtWeapon, 0, NULL, 0);
            __atomic_store_n(&vr_blade_calling, 0, __ATOMIC_RELAXED);
        }
        if (vr_hit.weapon_sh) vr_blade_call(c, kVrSensorResume, vr_hit.weapon_sh, kVrSuspendBit, 0, NULL, 0);
        hos_log("vr: blade: stroke %d: off after %d of Link's updates, %d ms, %d setups (%s)", vr_hit.stroke, vr_hit.updates,
                (int)((hos_now_ns() - vr_hit.on_at) / 1000000), vr_hit.sets, why);
    }
    vr_hit.state = VR_HIT_NONE, vr_hit.mine = 0, vr_hit.weapon_sh = 0;
    __atomic_store_n(&vr_hit_live, 0, __ATOMIC_RELEASE);
}

/* a short buzz in the hand that holds the weapon, for a hit (VR tab Hit buzz, option 1006) */
void rx_vr_rumble(int hand, float amplitude, float seconds);   /* (vr/headset/bridges_host.c) */
static void vr_blade_buzz(int hand) {
    if (hand >= 0 && hand <= 1 && rx_option(1006, 1)) rx_vr_rumble(hand, 0.7f, 0.05f);
}

/* (under the lock) the first setup of a stroke taken (before, after: its SensorHit +0x20): logged, counted, buzzed */
static void vr_blade_taken(int64_t now, unsigned before, unsigned after) {
    const VrBladeWeapon *w = &kVrBladeWeapons[vr_hit.weapon];
    /* (no cooldown of ours: a stroke may follow the last at once. The same target hit again within kVrRehitNs may be inside
       the game's own invulnerability after a hit, which then refuses it; counted, so a log shows how often fast swinging
       meets it) */
    static uint64_t last_target;
    static int64_t last_at, window_at;
    static unsigned setups, rehits;
    const int rehit = vr_hit.target && vr_hit.target == last_target && now - last_at < kVrRehitNs;
    setups++, rehits += rehit;
    uint32_t type;
    uint64_t mask;
    memcpy(&type, vr_hit.info + 4, 4), memcpy(&mask, vr_hit.info + 0x10, 8);
    hos_log("vr: blade: stroke %d: %s set up on %s (%s, AttackType %u, mask %#llx, Hit test %d, sword %s; SensorHit +0x20 "
            "%#x -> %#x), %d ms after the contact%s", vr_hit.stroke, vr_hit.kind == 2 ? "jump slash (in the air)" : vr_hit.kind ? "thrust" : "slash", vr_hit.target_name,
            w->label, type, (unsigned long long)mask, vr_hit_test(), hos_vr_sword_drawn() ? "drawn" : "not drawn", before,
            after, (int)((now - vr_hit.asked_at) / 1000000), rehit ? ", the same target hit again" : "");
    if (rehit) hos_log("vr: blade:   %d ms after its last hit (the game's invulnerability may refuse it)",
                       (int)((now - last_at) / 1000000));
    if (!window_at) window_at = now;
    if (now - window_at > 60000000000LL) {
        hos_log("vr: blade: in the last %d s, %u hits set up, %u of them on a target hit within %d ms before",
                (int)((now - window_at) / 1000000000LL), setups, rehits, (int)(kVrRehitNs / 1000000));
        window_at = now, setups = rehits = 0;
    }
    if (vr_hit.target) last_target = vr_hit.target, last_at = now, vr_blade_buzz(vr_hit.hand);
}

#define kVrActionRequest 0x800e08f0ULL
static uint64_t vr_action_target(Ctx *c, uint64_t link);
static volatile int vr_stance_want;          /* (a VR hand holds the sword, Blade: the stance asked at the calc) */
static volatile uint64_t vr_stance_sh;       /* (Link's SensorHit then: the calc hook's filter) */

/* ---- the stance kept inside Link's SensorHit calc, after his AI (sword-sensor-dive.md section 7): while a VR hand holds
   the sword, his sub selector (SelectorPlayerSubSchedulerMain, found by the hook at its request handler f_82b635b0's entry)
   is changed to its Drawn child (its byte +0x21; +0x1e the current one) by the game's own forced change,
   f_8076cf90(selector, child, 0), and his AI's own requests are made drawn in flight (hos_vr_action_request). Requests of
   ours, sent here or at the movement update, never reached the selector: f_800e08f0 keeps one record per slot and the
   AI's, written after ours, won. VR tab Draw test (option 1008, a test): Both (the default), Rewrite, Switch, Off */
#define kVrChangeChild 0x8076cf90ULL
static volatile uint64_t vr_sub_selector;   /* (Link's sub selector, as its request handler saw it) */
static void vr_blade_stance_calc(Ctx *c) {
    const int mode = rx_option(1008, 0);
    const uint64_t sel = __atomic_load_n(&vr_sub_selector, __ATOMIC_RELAXED);
    if ((mode == 0 || mode == 2) && sel && hos_guest_range(sel, 0x28)) {
        const unsigned cur = recomp_load8(c, sel + 0x1e), drawn = recomp_load8(c, sel + 0x21);
        if (cur != drawn) {
            static unsigned told;
            if (told < 40) told++, hos_log("vr: blade: Link's sub selector %#llx: child %u -> Drawn %u (forced change; action "
                                           "state %#x)", (unsigned long long)sel, cur, drawn, hos_vr_link_action());
            __atomic_store_n(&vr_blade_calling, 1, __ATOMIC_RELAXED);
            vr_blade_call(c, kVrChangeChild, sel, drawn, 0, NULL, 0);
            __atomic_store_n(&vr_blade_calling, 0, __ATOMIC_RELAXED);
        }
    }
}
/* (the hook at f_82b635b0's entry: SelectorPlayerSubSchedulerMain's request handler; x0 the selector, x1 the request) the
   selector kept, and the requests reaching it logged (bounded; a repeat of the last left out): whether ours arrive, and who
   sends the Sheathed (0x3a) after them */
void hos_vr_sub_request(Ctx *c) {
    const uint64_t sel = c->x[0], req = c->x[1];
    if (!hos_guest_range(sel, 0x28) || !hos_guest_range(req, 4)) return;
    if (sel != __atomic_exchange_n(&vr_sub_selector, sel, __ATOMIC_RELAXED))
        hos_log("vr: blade: Link's sub selector %#llx (Drawn child %u, now %u)", (unsigned long long)sel,
                recomp_load8(c, sel + 0x21), recomp_load8(c, sel + 0x1e));
    const uint32_t value = recomp_load32(c, req);
    const unsigned cur = recomp_load8(c, sel + 0x1e);
    const int ours = __atomic_load_n(&vr_blade_calling, __ATOMIC_RELAXED) != 0;
    static uint32_t last_value = ~0u;
    static unsigned last_cur = ~0u, told;
    if ((value == last_value && cur == last_cur) || told >= 150) return;
    last_value = value, last_cur = cur, told++;
    hos_log("vr: blade: sub request %#x (%s) to Link's sub selector: its child now %u, Drawn %u (return %#llx; action "
            "state %#x)", value, ours ? "ours" : "the game's", cur, recomp_load8(c, sel + 0x21), (unsigned long long)c->x[30],
            hos_vr_link_action());
}

/* (the hook at f_82ec3ef0's entry, vr/vr_game_hooks.py: every SensorHit's calc, x0 the SensorHit) Link's, while ours is on
   (Early): the setup made here, before the calc retires what was not set up this frame */
void hos_vr_sensor_calc(Ctx *c) {
    const uint64_t sh = c->x[0];
    if (__atomic_load_n(&vr_stance_want, __ATOMIC_RELAXED) && sh == __atomic_load_n(&vr_stance_sh, __ATOMIC_RELAXED))
        vr_blade_stance_calc(c);
    if (!__atomic_load_n(&vr_hit_live, __ATOMIC_ACQUIRE)) return;
    AcquireSRWLockExclusive(&vr_blade_lock);
    if (vr_hit.state == VR_HIT_ON && !vr_hit.late && sh == vr_hit.sensor_hit) {
        const int64_t now = hos_now_ns();
        const unsigned before = recomp_load8(c, sh + 0x20);
        __atomic_store_n(&vr_blade_calling, 1, __ATOMIC_RELAXED);
        vr_blade_call(c, kVrSetupAttack, sh, 0, 0, vr_hit.info, sizeof vr_hit.info);
        __atomic_store_n(&vr_blade_calling, 0, __ATOMIC_RELAXED);
        const unsigned after = recomp_load8(c, sh + 0x20);
        if (!vr_hit.sets++) {
            if (!after) {
                uint32_t type;
                memcpy(&type, vr_hit.info + 4, 4);
                hos_log("vr: blade: stroke %d: the game refused the attack at its calc (AttackType %u: SensorHit +0x20 %#x "
                        "-> %#x)", vr_hit.stroke, type, before, after);
                vr_hit.state = VR_HIT_NONE;
                __atomic_store_n(&vr_hit_live, 0, __ATOMIC_RELEASE);
            } else {
                vr_blade_taken(now, before, after);
                const uint64_t ws = vr_hit_test() != 2 ? vr_link_sensor_hit(c, vr_hit.actor) : 0;
                if (ws && ws != sh) {   /* (the weapon's own SensorHit suspended, its Weapon sensor off: as the game's slash) */
                    vr_blade_call(c, kVrSensorSuspend, ws, kVrSuspendBit, 0, NULL, 0);
                    vr_blade_call(c, kVrAtSensorOff, ws, kVrAtWeapon, 0, NULL, 0);
                    vr_hit.weapon_sh = ws;
                }
            }
        }
    }
    ReleaseSRWLockExclusive(&vr_blade_lock);
}

/* ---- Link's drawn stance (sword-sensor-dive.md section 5, 22 Nov, partly inferred): hits land only while Link's sub
   action is Player.sub drawn (ExecutePlayerDrawn); with the sub on Sheathed, its update (f_80811df0) puts drawn slots back
   each frame, which undoes the draw call two frames later. OneShotPlayerSetBattleMode (execute f_8292f550) writes the
   player component (Link's components +0x358) +0x1540 = +0x1544 = its float and +0x1548 = -1.0; written so while a VR
   hand held the sword, they did nothing for the stance (headset 22 Nov 04:57). Link's own slash (X) sets +0x1528 to 200,
   counting down about one an update, and his action state goes 0 -> 0x10 -> 0x410 (0x400 the drawn stance), and stays.
   So while a VR hand holds the sword (Sword hits: Blade), +0x1528 is kept at kVrDrawnTimer (RX_VR_SWORD_BATTLE=<value>,
   0 off: a test); the stance is logged when it changes */
#define kVrDrawnTimer 200.0f
static uint64_t vr_player_comp(Ctx *c, uint64_t link) {
    if (!link || !hos_guest_range(link + 0x270, 8)) return 0;
    const uint64_t parts = recomp_load64(c, link + 0x270);
    if (parts < HOS_HEAP_BASE || !hos_guest_range(parts + 0x358, 8)) return 0;
    const uint64_t pc = recomp_load64(c, parts + 0x358);
    return pc >= HOS_HEAP_BASE && hos_guest_range(pc + 0x1528, 0x24) ? pc : 0;
}
static void vr_blade_stance(Ctx *c, uint64_t link) {
    const uint64_t pc = vr_player_comp(c, link);
    if (!pc) return;
    static float value = -1;
    if (value < 0) {
        const char *e = vr_research_getenv("RX_VR_SWORD_BATTLE");
        value = e && e[0] ? (float)atof(e) : kVrDrawnTimer;
    }
    const int held = vr_blade_on() && (hos_vr_sword_holder() >= 0 || hos_vr_stick_holder() >= 0);
    if (value > 0 && held && vr_bomb_f(c, pc + 0x1528) < value) vr_bomb_put_f(c, pc + 0x1528, value);
    /* (the stance itself is asked at Link's SensorHit calc, after his AI: vr_blade_stance_calc) */
    __atomic_store_n(&vr_stance_want, held, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_stance_sh, held ? vr_link_sensor_hit(c, link) : 0, __ATOMIC_RELAXED);
    static float was[5] = {-9, -9, -9, -9, -9};
    static uint32_t action_was = ~0u;
    static int drawn_was = -1, held_was = -1;
    static unsigned told;
    const int off[5] = {0x1528, 0x1534, 0x1540, 0x1544, 0x1548};
    float now_v[5];
    const int drawn = hos_vr_sword_drawn();
    int changed = hos_vr_link_action() != action_was || drawn != drawn_was || held != held_was;
    for (int i = 0; i < 5; i++) now_v[i] = vr_bomb_f(c, pc + off[i]), changed |= i && fabsf(now_v[i] - was[i]) > 0.25f;
    changed |= (now_v[0] > 0) != (was[0] > 0);   /* (+0x1528 counts down every update: logged only when it starts or ends) */
    if (changed && told < 300) {
        drawn_was = drawn, held_was = held;
        told++;
        hos_log("vr: blade: stance: action state %#x, sword %s, held %d; player +0x1528 %.2f +0x1534 %.2f +0x1540 %.2f "
                "+0x1544 %.2f +0x1548 %.2f", hos_vr_link_action(), hos_vr_sword_drawn() ? "drawn" : "not drawn",
                hos_vr_sword_holder(), now_v[0], now_v[1], now_v[2], now_v[3], now_v[4]);
        memcpy(was, now_v, sizeof was), action_was = hos_vr_link_action();
    }
}

/* ---- the drawn stance by Link's own request (sword-sensor-dive.md section 6): the sub action slot (SelectorPlayerSub-
   SchedulerMain) changes to Drawn only on a request (its handler f_82b635b0: 0x39 Drawn, 0x3a Sheathed, 0x38 PutAway),
   sent by the AI node ExecuteBSASetActionVerb as f_800e08f0(Link's components +0x188, request (0x60 bytes), 1): +0x48 the
   node's ActionSlot, +0x4c its ActionVerb, +0x38 an optional name (+0x40 its flag), +0x58 the node. The draw call alone
   sends none, so the slot stays on Sheathed and puts the sword back. The hook at f_800e08f0's entry logs Link's requests
   (bounded; the routine ones, slot 0 verb 0x28 and slots 1-2 verb 0 each frame, are left out), and while a VR hand holds
   the sword makes the AI's own drawn: slot 1 verb 0x39, and its slash (slot 0 0x43, 0x70) its movement (0x28). Headset
   22 Nov 06:13: drawn at every take, no slash of Link's, no lunge, no trail, the VR strokes hitting the dummy */
#define kVrVerbDrawn 0x39u
#define kVrVerbDrawnMain 0x70u   /* (the main slot's: his slash's follow-through) */
#define kVrVerbSlash 0x43u       /* (the main slot's: his slash) */
#define kVrVerbMove 0x28u        /* (the main slot's: his movement, drawn or not) */
#define kVrVerbSpin 0x77u        /* (the main slot's: his spin, after the attack held (0x43) or from his movement) */
/* ---- the spin on the sword hand's trigger (the user's design, 22 Nov): held, Link's own charge (his AI's main-slot
   request made the charge verb, the game's own charge state and timer); let go, his spin (the spin verb, kVrSpinFrames of
   his requests), the game choosing the quick or the charged one by its own charge. The verbs, from the user's Button-mode
   capture (22 Nov 07:09): the attack held is slot 0 0x43 (the game's charge runs while it lasts; let go, 0x70 a slash or
   0x77 the spin); the spin 0x77, its AttackType the game's own: 34 quick or stage 1, 88 stage 2; the jump slash 0x72,
   AttackType 22. RX_VR_MOVE_VERBS=<charge>,<spin> replaces them (a test; 0,0 the trigger does nothing) */
#define kVrSpinFrames 6
static volatile int vr_charge_held, vr_spin_frames;
static volatile uint32_t vr_main_verb;   /* (the AI's last main-slot request, before ours: 0x43 its attack held) */
static uint32_t vr_move_verb(int which) {
    static int read;
    static uint32_t v[2] = {kVrVerbSlash, kVrVerbSpin};
    if (!read) {
        read = 1;
        const char *e = vr_research_getenv("RX_VR_MOVE_VERBS");
        if (e) sscanf(e, "%i,%i", (int *)&v[0], (int *)&v[1]);
    }
    return v[which & 1];
}
/* (vr/input/controllers_host.c vr_sword, each read) the sword hand's trigger held while a VR hand holds the sword (Blade) */
void hos_vr_sword_trigger(int held) {
    const int was = __atomic_exchange_n(&vr_charge_held, held, __ATOMIC_RELAXED);
    if (was && !held && vr_move_verb(1)) __atomic_store_n(&vr_spin_frames, kVrSpinFrames, __ATOMIC_RELAXED);
    if (was != held && vr_move_verb(0)) hos_log("vr: blade: the sword hand's trigger %s", held ? "held: Link's charge" : "let go: his spin");
}
static uint64_t vr_action_target(Ctx *c, uint64_t link) {   /* (Link's components +0x188) */
    if (!link || !hos_guest_range(link + 0x270, 8)) return 0;
    const uint64_t parts = recomp_load64(c, link + 0x270);
    return parts >= HOS_HEAP_BASE && hos_guest_range(parts + 0x188, 8) ? recomp_load64(c, parts + 0x188) : 0;
}
void hos_vr_action_request(Ctx *c) {
    const uint64_t link = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    const uint64_t to = vr_action_target(c, link), req = c->x[1];
    if (!to || c->x[0] != to || !hos_guest_range(req, 0x60)) return;
    const int ours = __atomic_load_n(&vr_blade_calling, __ATOMIC_RELAXED) != 0;
    const uint32_t slot = recomp_load32(c, req + 0x48), verb = recomp_load32(c, req + 0x4c);
    char name[48] = "";
    const uint64_t np = recomp_load64(c, req + 0x38);
    if ((recomp_load8(c, req + 0x40) & 1) && np >= 0x80000000ULL && hos_guest_range(np, sizeof name)) {
        for (unsigned i = 0; i + 1 < sizeof name; i++) {
            const char ch = (char)recomp_load8(c, np + i);
            if (ch < 32 || ch > 126) break;
            name[i] = ch, name[i + 1] = 0;
        }
    }
    static unsigned told[2];
    static uint32_t last_verb[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};   /* (the game's, per slot: logged on a change) */
    const int routine = !ours && (slot >= 8 || verb == last_verb[slot]);
    if (!ours && slot < 8) last_verb[slot] = verb;
    if (!ours && slot == 0) __atomic_store_n(&vr_main_verb, verb, __ATOMIC_RELAXED);   /* (the probe's: the game's own charge) */
    if (told[ours] < (ours ? 30 : 600) && !routine)
        told[ours]++, hos_log("vr: blade: %s action request to Link: slot %u, verb %#x, +0x50 %#x, %s (action state %#x)",
                        ours ? "our" : "the game's", slot, verb, recomp_load16(c, req + 0x50), name[0] ? name : "no name",
                        hos_vr_link_action());
    /* (while a VR hand holds the sword: the AI's own requests to the sub slot made Drawn, in flight. Sent from the calc
       ours never reached the sub selector, the AI's written after them each frame won, and the forced change was undone
       the next frame by its request 0: headset 22 Nov 05:29. The main slot is left alone: 0x28 is Link's movement drawn
       or not, and 0x70 his slash's follow-through; made 0x70 he slashed and lunged, the trail with it: headset 05:39.
       But the stance's change from Sheathed is answered by his AI with his slash (slot 0 0x43, then 0x70: one draw slash
       a take, headset 05:42), so while a VR hand holds the sword those two are made his movement (0x28): no slash of
       his, the Blade's own hits only. Draw test Both or Rewrite) */
    const int mode = rx_option(1008, 0);
    if (!ours && (mode == 0 || mode == 1) && __atomic_load_n(&vr_stance_want, __ATOMIC_RELAXED)) {
        const uint32_t charge = vr_move_verb(0), spin = vr_move_verb(1);
        const int held = __atomic_load_n(&vr_charge_held, __ATOMIC_RELAXED);
        const int spinning = __atomic_load_n(&vr_spin_frames, __ATOMIC_RELAXED) > 0;
        const uint32_t want = slot == 1 && verb != kVrVerbDrawn ? kVrVerbDrawn :
                              slot == 0 && held && charge ? charge :
                              slot == 0 && spinning && spin ? spin :
                              slot == 0 && (verb == kVrVerbSlash || verb == kVrVerbDrawnMain) ? kVrVerbMove : verb;
        if (want != verb) {
            recomp_store32(c, req + 0x4c, want);
            static unsigned rewrote;
            if (rewrote++ < 20) hos_log("vr: blade: the game's request to Link, slot %u verb %#x, made verb %#x (action state %#x)",
                                        slot, verb, want, hos_vr_link_action());
        }
    }

}

static void vr_sensor_watch(Ctx *c, uint64_t link, int ours_on);
/* (sword_host.c, Link's update) a hit asked: readied for the calc (Early) or made (Late); one on: counted and ended; the
   draw asked: made */
static void vr_blade_link_update(Ctx *c, uint64_t link) {
    const int64_t now = hos_now_ns(), was = __atomic_exchange_n(&vr_link_update_at, now, __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&vr_stick_draw_wanted, 0, __ATOMIC_ACQUIRE) && hos_guest_range(link + 0x270, 8)) {
        const uint64_t parts = recomp_load64(c, link + 0x270);
        const uint64_t user = parts >= HOS_HEAP_BASE && hos_guest_range(parts + 0x200, 8) ? recomp_load64(c, parts + 0x200) : 0;
        static unsigned told;
        if (user >= HOS_HEAP_BASE) {
            if (told++ < 20) hos_log("vr: blade: the Deku Stick drawn by the game's own call: f_806d6820(EquipmentUser, %d), "
                                     "action state %#x", kVrStickSlot, hos_vr_link_action());
            vr_blade_call(c, kVrDrawSlot, user, kVrStickSlot, 0, NULL, 0);
        }
    }
    if (__atomic_exchange_n(&vr_blade_draw_wanted, 0, __ATOMIC_ACQUIRE)) {
        static int64_t drew_at;
        const int out = hos_vr_sword_drawn();
        static int flag = -1;   /* (RX_VR_SWORD_DRAW: the call's third argument, a test; 1 by default) */
        if (flag < 0) flag = vr_research_int("RX_VR_SWORD_DRAW", 1);
        if (!out && now - drew_at > 250000000LL) {
            drew_at = now;
            hos_log("vr: blade: the game's own draw (no attack): f_80a63030(Link, 1, %d), action state %#x", flag,
                    hos_vr_link_action());
            vr_blade_call(c, kVrDrawEquipment, link, 1, (uint64_t)flag, NULL, 0);
        }
    }
    vr_blade_stance(c, link);
    if (__atomic_load_n(&vr_spin_frames, __ATOMIC_RELAXED) > 0) __atomic_fetch_sub(&vr_spin_frames, 1, __ATOMIC_RELAXED);
    {   /* (a probe, while the trigger holds the charge: Link's player component +0x1000..+0x1800, the 4-byte fields that
           change, logged (bounded) every 250 ms: where the game keeps its charge and its stage) */
        static uint8_t snap[0x800];
        static int have;
        static int64_t looked, began;
        static unsigned lines;
        const uint64_t pc = vr_player_comp(c, link);
        const int ours = __atomic_load_n(&vr_charge_held, __ATOMIC_RELAXED);
        const int held = ours || __atomic_load_n(&vr_main_verb, __ATOMIC_RELAXED) == kVrVerbSlash;   /* (or the game's own) */
        if (!held || !pc || !hos_guest_range(pc + 0x1000, sizeof snap)) have = 0;
        else if (!have) {
            for (unsigned i = 0; i < sizeof snap; i++) snap[i] = recomp_load8(c, pc + 0x1000 + i);
            have = 1, looked = began = now;
        } else if (now - looked >= 250000000LL && lines < 120) {
            looked = now;
            char line[480];
            int n = snprintf(line, sizeof line, "vr: blade: charge probe (%s), %d ms held:", ours ? "our trigger" : "the game's button",
                             (int)((now - began) / 1000000));
            unsigned found = 0;
            for (unsigned off = 0; off < sizeof snap && found < 14; off += 4) {
                uint32_t a, b = recomp_load32(c, pc + 0x1000 + off);
                memcpy(&a, snap + off, 4);
                if (a == b) continue;
                float fa, fb;
                memcpy(&fa, &a, 4), memcpy(&fb, &b, 4);
                found++;
                n += snprintf(line + n, sizeof line - n, " +%#x %#x->%#x (%.2f->%.2f)", 0x1000 + off, a, b, fa, fb);
                if (n > (int)sizeof line - 60) break;
            }
            if (found) lines++, hos_log("%s", line);
        }
    }
    vr_sensor_watch(c, link, vr_hit.state == VR_HIT_ON);
    AcquireSRWLockExclusive(&vr_blade_lock);
    if (vr_hit.state != VR_HIT_NONE && vr_hit.link && vr_hit.link != link) {   /* (a new Link: the old SensorHit is gone) */
        hos_log("vr: blade: stroke %d: Link's actor changed: the hit dropped", vr_hit.stroke);
        vr_hit.state = VR_HIT_NONE, vr_hit.weapon_sh = 0;
        __atomic_store_n(&vr_hit_live, 0, __ATOMIC_RELEASE);
    }
    if (vr_hit.state == VR_HIT_ON) {
        vr_hit.updates++;
        const int open = !vr_hit.target;   /* (a stroke's sensor on with no contact of ours yet: on until the stroke ends) */
        const char *why = was && now - was > kVrLinkMissNs ? "Link missed an update" :
                          now - vr_hit.weapon_seen > kVrWeaponFreshNs ? "the weapon is not held out" :
                          now - vr_hit.on_at > kVrHitMaxNs ? "the time cap" :
                          !open && vr_hit.updates >= kVrHitUpdates ? "its window" :
                          vr_hit.ended && vr_hit.updates >= (open ? 1 : 2) ? "the stroke ended" : NULL;
        if (why) vr_blade_off(c, why);
        else if (vr_hit.late) {   /* (Hit test Late: renewed here, as before the calc hook) */
            __atomic_store_n(&vr_blade_calling, 1, __ATOMIC_RELAXED);
            vr_blade_call(c, kVrSetupAttack, vr_hit.sensor_hit, 0, 0, vr_hit.info, sizeof vr_hit.info);
            __atomic_store_n(&vr_blade_calling, 0, __ATOMIC_RELAXED);
            vr_hit.sets++;
        }
    }
    if (vr_hit.state == VR_HIT_ASKED) {
        const VrBladeWeapon *w = &kVrBladeWeapons[vr_hit.weapon];
        const uint64_t sh = vr_link_sensor_hit(c, link);
        if (!vr_hit.target_name[0]) snprintf(vr_hit.target_name, sizeof vr_hit.target_name, "%s", "no target of ours (the stroke)");
        const char *why = !sh ? "Link has no SensorHit" : now - vr_hit.asked_at > kVrHitAskNs ? "asked too long ago" :
                          now - vr_hit.weapon_seen > kVrWeaponFreshNs ? "the weapon is not held out" : NULL;
        if (why) {
            hos_log("vr: blade: stroke %d: contact with %s, the sensor not set: %s", vr_hit.stroke, vr_hit.target_name, why);
            vr_hit.state = VR_HIT_NONE;
        } else {
            vr_blade_info(w, vr_hit.kind, vr_hit.info);
            vr_hit.late = vr_hit_test() == 3, vr_hit.sets = 0, vr_hit.weapon_sh = 0;
            vr_hit.mine = 1, vr_hit.on_at = now, vr_hit.updates = 0, vr_hit.link = link, vr_hit.sensor_hit = sh;
            if (!vr_hit.late) {   /* (Early: made at Link's next SensorHit calc) */
                vr_hit.state = VR_HIT_ON;
                __atomic_store_n(&vr_hit_live, 1, __ATOMIC_RELEASE);
            } else {
                const unsigned before = recomp_load8(c, sh + 0x20);
                __atomic_store_n(&vr_blade_calling, 1, __ATOMIC_RELAXED);
                vr_blade_call(c, kVrSetupAttack, sh, 0, 0, vr_hit.info, sizeof vr_hit.info);
                __atomic_store_n(&vr_blade_calling, 0, __ATOMIC_RELAXED);
                const unsigned after = recomp_load8(c, sh + 0x20);
                vr_hit.sets = 1;
                if (!after) {
                    uint32_t type;
                    memcpy(&type, vr_hit.info + 4, 4);
                    hos_log("vr: blade: stroke %d: contact with %s, the game refused the attack (AttackType %u, %s: its "
                            "SensorHit +0x20 %#x -> %#x)", vr_hit.stroke, vr_hit.target_name, type, w->label, before, after);
                    vr_hit.state = VR_HIT_NONE;
                } else vr_blade_taken(now, before, after), vr_hit.state = VR_HIT_ON;
            }
        }
    }
    ReleaseSRWLockExclusive(&vr_blade_lock);
}

/* the stroke's log, once it ends: how near it came, and what came of it (bounded) */
static struct { int number, logged; float nearest; char name[40]; int outcome; } vr_stroke_log;   /* (outcome: 0 none, 1 hit asked) */
static unsigned vr_blade_told;
#define kVrBladeTell 400
static void vr_blade_stroke_ended(const VrStroke *s) {
    if (!vr_stroke_log.outcome && vr_stroke_log.nearest < kVrNearLog && vr_blade_told < kVrBladeTell) {
        vr_blade_told++;
        hos_log("vr: blade: stroke %d (peak %.1f m/s, %.0f cm): no contact, it passed %.2f m from %s", s->number, s->peak,
                s->travel * 100, vr_stroke_log.nearest, vr_stroke_log.name);
    }
    vr_stroke_log.outcome = 0, vr_stroke_log.nearest = 1e9f;
}

/* (under the lock) a target hit: no new hit on it until the blade has been out of it by kVrExitMargin */
static void vr_blade_lock_target(uint64_t actor) {
    for (int i = 0; i < kVrTargets; i++)
        if (vr_blade_targets[i].actor == actor) vr_blade_targets[i].locked = 1, vr_blade_targets[i].left = 0;
}

/* (sword_host.c and vr/items/deku_stick/deku_stick_host.c: the weapon's own update while a VR hand holds it out, or the
   headless test's blade) the swing and the contact from this pose; origin and rot become the pose to place it at (the
   contact's while a hit is asked or on). */
static void vr_blade_weapon_update(Ctx *c, uint64_t actor, int hand, float origin[3], float rot[9]) {
    static VrStroke stroke;
    static uint64_t actor_was;
    static int weapon = -1;
    static float o0[3], r0[9];
    static int64_t pose_at;
    if (!vr_blade_on()) return;
    const int64_t now = hos_now_ns();
    if (actor != actor_was) {
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        weapon = vr_blade_weapon_of(name);
        actor_was = actor, pose_at = 0, stroke.have = 0;
        hos_log("vr: blade: %s %#llx held (%s)", name[0] ? name : "?", (unsigned long long)actor,
                weapon < 0 ? "no row: not a swung weapon, no hits" : kVrBladeWeapons[weapon].label);
    }
    if (weapon < 0) return;
    const VrBladeWeapon *w = &kVrBladeWeapons[weapon];
    if (!w->attack) {   /* (no AttackType known for it yet) */
        static int told_weapon = -1;
        if (told_weapon != weapon) told_weapon = weapon, hos_log("vr: blade: %s: its AttackType not known yet: no hits", w->label);
        return;
    }
    /* (the stroke from the blade in the room, when the hand is known: the headless test's blade is the world's) */
    const float along = fminf(w->tip, kVrStrokePoint);
    float p[3], axis[3];
    if (!vr_blade_room(hand, along, p, axis))
        for (int k = 0; k < 3; k++) axis[k] = rot[3 * k + 2], p[k] = origin[k] + axis[k] * along;
    const float arc = (float)rx_option(1009, kVrStrokeArc) * 0.01f, speed = (float)rx_option(1013, kVrStrokeSpeed) * 0.1f;
    const VrStroke before = stroke;
    const int changed = vr_stroke_step(&stroke, p, axis, now, speed, arc);
    if (changed < 0) vr_blade_stroke_ended(&before);   /* (a stroke over: as it was) */
    static int stroke_kind;
    if (changed > 0) vr_stroke_log.nearest = 1e9f, vr_stroke_log.outcome = 0, stroke_kind = vr_blade_kind(&stroke, axis);
    const int fresh = pose_at && now - pose_at < kVrWeaponFreshNs;
    float best_gap = 1e9f, bo[3], br[9];
    char best_name[40] = "";
    uint64_t best_actor = 0;
    char inside[40] = "";   /* (a target hit and the blade not out of it yet: no new hit, kVrExitMargin) */
    if (fresh) {
        AcquireSRWLockExclusive(&vr_blade_lock);
        const uint64_t on_target = vr_hit.state != VR_HIT_NONE ? vr_hit.target : 0;
        for (int i = 0; i < kVrTargets; i++) {
            VrBladeTarget *t = &vr_blade_targets[i];
            if (!t->actor || now - t->seen > kVrTargetFreshNs) continue;
            float o[3], r[9];
            int step;
            const float gap = vr_blade_sweep(w, o0, r0, origin, rot, t->at, &kVrTargetShapes[t->shape], o, r, &step);
            if (t->locked && t->actor != on_target) {
                if (gap <= kVrExitMargin) t->left = 0;   /* (in it, or back in it before a new stroke) */
                else if (!t->left) t->left = 1;
                if (t->left && changed > 0) {   /* (a stroke begun out of it, its arc gone: re-armed) */
                    t->locked = t->left = 0;
                    static unsigned told;
                    if (told++ < 100) hos_log("vr: blade: %s re-armed: out of it, and stroke %d begun out there", t->name,
                                              stroke.number);
                } else {
                    if (gap <= kVrExitMargin) snprintf(inside, sizeof inside, "%s", t->name);
                    if (gap <= 0) {   /* (touching it again, not re-armed: refused, logged at most once a stroke) */
                        static int told_stroke = -1;
                        static unsigned told;
                        if (told_stroke != stroke.number && told < 300)
                            told_stroke = stroke.number, told++,
                            hos_log("vr: blade: stroke %d: %s refused: since its last hit the blade has not %s", stroke.number,
                                    t->name, t->left ? "begun a new stroke out of it" : "been out of it");
                    }
                    continue;   /* (no new hit on it) */
                }
            }
            if (gap < best_gap) best_gap = gap, memcpy(bo, o, sizeof bo), memcpy(br, r, sizeof br), best_actor = t->actor,
                                snprintf(best_name, sizeof best_name, "%s", t->name);
        }
        ReleaseSRWLockExclusive(&vr_blade_lock);
    }
    memcpy(o0, origin, sizeof o0), memcpy(r0, rot, sizeof r0), pose_at = now;
    if (stroke.active && best_gap < vr_stroke_log.nearest)
        vr_stroke_log.nearest = best_gap, snprintf(vr_stroke_log.name, sizeof vr_stroke_log.name, "%s", best_name);
    const int touching = best_actor && best_gap <= 0;
    AcquireSRWLockExclusive(&vr_blade_lock);
    vr_hit.weapon_seen = now;
    static int contact_stroke, open_stroke;   /* (the last strokes that asked: with a contact, and with none yet) */
    if (vr_hit.state != VR_HIT_NONE && vr_hit.actor == actor) {   /* (asked or on) */
        if (touching && (!vr_hit.target || best_actor == vr_hit.target) && best_gap < vr_hit.gap) {
            if (!vr_hit.target) {   /* (the stroke's sensor already on: now held where it touches, its window from here) */
                vr_hit.target = best_actor, contact_stroke = stroke.number, vr_stroke_log.outcome = 1;
                vr_blade_lock_target(best_actor);
                snprintf(vr_hit.target_name, sizeof vr_hit.target_name, "%s", best_name);
                if (vr_hit.state == VR_HIT_ON) vr_hit.updates = 0, vr_hit.on_at = now, vr_blade_buzz(hand);
                if (vr_blade_told < kVrBladeTell)
                    vr_blade_told++, hos_log("vr: blade: stroke %d: contact with %s (%.2f m in), the sensor %s: held there",
                                             stroke.number, best_name, -best_gap, vr_hit.state == VR_HIT_ON ? "on" : "asked");
            }
            vr_hit.gap = best_gap, memcpy(vr_hit.o, bo, sizeof bo), memcpy(vr_hit.r, br, sizeof br);
        }
        if (!stroke.active || stroke.number != vr_hit.stroke) vr_hit.ended = 1;
        if (vr_hit.target) memcpy(origin, vr_hit.o, sizeof vr_hit.o), memcpy(rot, vr_hit.r, sizeof vr_hit.r);   /* (held) */
    } else if (stroke.active && (__atomic_load_n(&vr_charge_held, __ATOMIC_RELAXED) ||
                                 __atomic_load_n(&vr_spin_frames, __ATOMIC_RELAXED) > 0) && vr_move_verb(0)) {
        open_stroke = contact_stroke = stroke.number;   /* (Link's own charge or spin under way: its hits are its own) */
    } else if (stroke.active && contact_stroke != stroke.number && !touching && open_stroke != stroke.number && inside[0]) {
        open_stroke = stroke.number;   /* (the blade still in a target it hit: this stroke sets nothing up) */
        if (vr_blade_told < kVrBladeTell)
            vr_blade_told++, hos_log("vr: blade: stroke %d: begun in %s, hit last: no new hit until the blade is out of it and "
                                     "a new stroke begins out there", stroke.number, inside);
    } else if (stroke.active && contact_stroke != stroke.number && (touching || open_stroke != stroke.number)) {
        /* a stroke: the game's attack asked, held where it touches a target of ours; with none in reach, on for the stroke
           as the blade goes (grass, and what is not an actor near Link: the game's own sensor finds it) */
        vr_hit.state = VR_HIT_ASKED, vr_hit.weapon = weapon, vr_hit.stroke = stroke.number, vr_hit.ended = 0;
        vr_hit.kind = fabsf(vr_link_vy) > kVrAirborneVy ? 2 : stroke_kind, vr_hit.hand = hand, vr_hit.actor = actor;
        vr_hit.asked_at = now;
        vr_hit.target = touching ? best_actor : 0, vr_hit.gap = touching ? best_gap : 1e9f;
        if (touching) contact_stroke = stroke.number, vr_stroke_log.outcome = 1, vr_blade_lock_target(best_actor);
        else open_stroke = stroke.number;
        memcpy(vr_hit.o, bo, sizeof bo), memcpy(vr_hit.r, br, sizeof br);
        snprintf(vr_hit.target_name, sizeof vr_hit.target_name, "%s", touching ? best_name : "");
        if (vr_blade_told < kVrBladeTell) {
            vr_blade_told++;
            if (touching) hos_log("vr: blade: stroke %d, a %s (%.1f m/s at the blade, %.0f cm so far): contact with %s (%.2f m "
                                  "in): the game's attack asked", stroke.number, stroke_kind ? "thrust" : "slash", stroke.speed,
                                  stroke.travel * 100, best_name, -best_gap);
            else hos_log("vr: blade: stroke %d, a %s (%.1f m/s at the blade, %.0f cm so far): no target of ours in reach "
                         "(nearest %s %.2f m): the game's attack asked for the stroke", stroke.number,
                         stroke_kind ? "thrust" : "slash", stroke.speed, stroke.travel * 100, best_name[0] ? best_name : "none",
                         best_gap < 1e8f ? best_gap : -1.0f);
        }
        if (touching) memcpy(origin, bo, sizeof bo), memcpy(rot, br, sizeof br);
    } else if (touching) {
        static int64_t told_at;
        if ((stroke.active || stroke.speed >= kVrStrokeKeep) && now - told_at > 1000000000LL &&
            vr_blade_told < kVrBladeTell) {   /* (moving: a blade at rest in the grass is no news) */
            told_at = now, vr_blade_told++;
            if (stroke.active) hos_log("vr: blade: stroke %d: contact with %s, the sensor not set: this stroke has hit already",
                                       stroke.number, best_name);
            else hos_log("vr: blade: contact with %s without a swing (%.1f m/s at the blade, %.0f cm one way): no hit", best_name,
                         stroke.speed, stroke.travel * 100);
        }
    }
    ReleaseSRWLockExclusive(&vr_blade_lock);
}

/* ---- the hooks (vr/vr_game_hooks.py): Link's and the sword's SensorHit, their attacks set up and At sensors switched off -- */
#define kVrSensorTell 60   /* (lines each: the game's on Link's, the game's on the sword's, ours on the sword's) */
#define kVrSensorOursTell 8   /* (ours on Link's: each also has its own line) */
/* which SensorHit this is: 0 Link's, 1 the sword's (its actor's own, +0x270 +0x3b8), -1 neither */
static int vr_sensor_whose(Ctx *c, uint64_t sh) {
    const uint64_t link = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
    if (link && sh == vr_link_sensor_hit(c, link)) return 0;
    const uint64_t sword = __atomic_load_n(&oot_vr_sword_actor_seen, __ATOMIC_RELAXED);
    return sword && sh == vr_link_sensor_hit(c, sword) ? 1 : -1;
}
static unsigned *vr_sensor_budget(int ours, int whose) {
    static unsigned told[2][2];
    return &told[ours][whose];
}
void hos_vr_sensor_setup_seen(Ctx *c) {
    const int ours = __atomic_load_n(&vr_blade_calling, __ATOMIC_RELAXED) != 0;
    const uint64_t sh = c->x[0], info = c->x[1];
    const int whose = vr_sensor_whose(c, sh);
    if (whose < 0 || !hos_guest_range(info, 0x20)) return;
    const int64_t now = hos_now_ns();
    if (!whose && !ours) {   /* (the game's own attack: logged when its AttackType or direction changes, or after a pause) */
        static uint64_t last;
        static int64_t last_at;
        const uint64_t key = (uint64_t)recomp_load32(c, info) << 32 | recomp_load32(c, info + 4);
        const int same = key == last && now - __atomic_load_n(&vr_link_setup_at, __ATOMIC_RELAXED) < 500000000LL;
        __atomic_store_n(&vr_link_setup_at, now, __ATOMIC_RELAXED);
        last = key, last_at = now, (void)last_at;
        if (same) return;
    }
    unsigned *told = vr_sensor_budget(ours, whose);
    if (*told >= (ours && !whose ? kVrSensorOursTell : !whose && !ours ? 300 : kVrSensorTell)) return;
    (*told)++;
    float rate;
    const uint32_t raw_rate = recomp_load32(c, info + 0xc);
    memcpy(&rate, &raw_rate, 4);
    hos_log("vr: blade: %s attack set up on %s SensorHit: direction %u, AttackType %u, attributes %#x, rate %.2f, At mask "
            "%#llx, %u %u %u (return %#llx; action state %#x; its +0x20..0x25 %02x %02x %02x %02x %02x %02x)",
            ours ? "our" : "the game's", whose ? "the sword's" : "Link's", recomp_load32(c, info), recomp_load32(c, info + 4),
            recomp_load32(c, info + 8), rate, (unsigned long long)recomp_load64(c, info + 0x10), recomp_load32(c, info + 0x18),
            recomp_load16(c, info + 0x1c), recomp_load8(c, info + 0x1e), (unsigned long long)c->x[30], hos_vr_link_action(),
            recomp_load8(c, sh + 0x20), recomp_load8(c, sh + 0x21), recomp_load8(c, sh + 0x22), recomp_load8(c, sh + 0x23),
            recomp_load8(c, sh + 0x24), recomp_load8(c, sh + 0x25));
}
void hos_vr_sensor_off_seen(Ctx *c) {
    const int ours = __atomic_load_n(&vr_blade_calling, __ATOMIC_RELAXED) != 0;
    const int whose = vr_sensor_whose(c, c->x[0]);
    if (whose < 0) return;
    unsigned *told = vr_sensor_budget(ours, whose);
    if (*told >= (ours && !whose ? kVrSensorOursTell : kVrSensorTell)) return;
    (*told)++;
    const int64_t at = __atomic_load_n(&vr_link_setup_at, __ATOMIC_RELAXED);
    hos_log("vr: blade: %s At sensor %u off on %s SensorHit, %d ms after the game's last setup on Link's (return %#llx)",
            ours ? "our" : "the game's", (unsigned)c->x[1], whose ? "the sword's" : "Link's",
            at ? (int)((hos_now_ns() - at) / 1000000) : -1, (unsigned long long)c->x[30]);
}

/* (Link's update) for 300 ms after the game's own setup on Link's SensorHit (its slash: the X button), and while ours is
   on, Link's and the sword's SensorHit bytes +0x20..0x27 (bounded): what the game's slash sets that ours does not */
static void vr_sensor_watch(Ctx *c, uint64_t link, int ours_on) {
    static unsigned told;
    const int64_t at = __atomic_load_n(&vr_link_setup_at, __ATOMIC_RELAXED);
    const int game = at && hos_now_ns() - at < 300000000LL;
    if (told >= 120 || (!game && !ours_on)) return;
    const uint64_t sword = __atomic_load_n(&oot_vr_sword_actor_seen, __ATOMIC_RELAXED);
    const uint64_t a = vr_link_sensor_hit(c, link), b = sword ? vr_link_sensor_hit(c, sword) : 0;
    char line[2][40] = {"none", "none"};
    for (int i = 0; i < 2; i++) {
        const uint64_t sh = i ? b : a;
        if (sh) snprintf(line[i], sizeof line[i], "%02x %02x %02x %02x %02x %02x %02x %02x", recomp_load8(c, sh + 0x20),
                         recomp_load8(c, sh + 0x21), recomp_load8(c, sh + 0x22), recomp_load8(c, sh + 0x23),
                         recomp_load8(c, sh + 0x24), recomp_load8(c, sh + 0x25), recomp_load8(c, sh + 0x26),
                         recomp_load8(c, sh + 0x27));
    }
    told++;
    /* (the Weapon At sensor's body by each SensorHit, f_80236670(sh, 0), and its +0x70 bit 6: which body the hit goes
       through, sword-sensor-dive.md 3c) */
    uint64_t body[2] = {0, 0};
    for (int i = 0; i < 2; i++) if (i ? b : a) body[i] = vr_blade_call(c, kVrSensorBody, i ? b : a, kVrAtWeapon, 0, NULL, 0);
    hos_log("vr: blade: %s: Link's SensorHit +0x20.. %s; the sword's %s (action state %#x, sword %s); Weapon bodies: "
            "Link's %#llx (+0x70 bit 6 %d), the sword's %#llx (%d)", game ? "the game's slash" : "ours on", line[0], line[1],
            hos_vr_link_action(), hos_vr_sword_drawn() ? "drawn" : "not drawn", (unsigned long long)body[0],
            body[0] && hos_guest_range(body[0] + 0x70, 1) ? (recomp_load8(c, body[0] + 0x70) >> 6) & 1 : -1,
            (unsigned long long)body[1], body[1] && hos_guest_range(body[1] + 0x70, 1) ? (recomp_load8(c, body[1] + 0x70) >> 6) & 1 : -1);
}
