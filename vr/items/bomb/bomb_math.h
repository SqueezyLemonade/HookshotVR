/* Hookshot VR: the bombs' aiming arithmetic, with no game in it (vr/items/bomb/bomb_host.c uses it; vr/tools/test.py
   checks it on its own).

   - The throw arc (PARKED with Bomb aim's Arc to target: vr/items/bomb/bomb_host.c says why): where a bomb thrown from a point with a velocity is after t seconds under gravity g (m/s2, down), and
     when it comes down to a height (the ground, taken as flat at Link's feet: the game is not asked for the ground).
   - The throw onto a target: the velocity that lands a bomb on a point, leaving at an angle near the one asked for.
   - The gravity pull: which bomb a hand points at, inside a cone and a reach.

   Everything is in the game world: metres, y up. Plain C, included by bomb_host.c (and by the tester). */
#ifndef VR_BOMB_MATH_H
#define VR_BOMB_MATH_H

#include <math.h>

#define BOMB_ARC_MAX_S 3.0f          /* (s) an arc is followed at most this long: past that it has left the map */
#define BOMB_ARC_LOWEST_DEG 10.0f    /* (degrees) a throw onto a target leaves at least this far above level... */
#define BOMB_ARC_HIGHEST_DEG 75.0f   /* ...and at most this far: steeper is a lob straight up */

/* where the bomb is t seconds after leaving from with velocity v under gravity g */
static inline void bomb_arc_at(const float from[3], const float v[3], float g, float t, float out[3]) {
    out[0] = from[0] + v[0] * t;
    out[1] = from[1] + v[1] * t - 0.5f * g * t * t;
    out[2] = from[2] + v[2] * t;
}

/* when the arc comes down through height y (its later crossing: on the way down), or -1 if it never does before
   BOMB_ARC_MAX_S. A throw from below y that rises past it lands on the way down too. */
static inline float bomb_arc_land_time(const float from[3], const float v[3], float g, float y) {
    if (!(g > 0)) return -1;
    /* from[1] + v[1] t - g t^2 / 2 = y  ->  (g/2) t^2 - v[1] t + (y - from[1]) = 0 */
    const float a = 0.5f * g, b = -v[1], c = y - from[1];
    const float disc = b * b - 4 * a * c;
    if (disc < 0) return -1;   /* (it never reaches that height: thrown from below it, too slowly) */
    const float t = (-b + sqrtf(disc)) / (2 * a);
    return t > 0 && t <= BOMB_ARC_MAX_S ? t : -1;
}

/* The launch speed (m/s) for a throw at angle `pitch` (radians above level) that passes through a point `ahead` metres
   away level and `up` metres higher (negative: lower), or -1 if no speed at that angle can reach it (aimed below it). */
static inline float bomb_arc_speed_for(float ahead, float up, float pitch, float g) {
    const float c = cosf(pitch), below = ahead * tanf(pitch) - up;
    if (!(ahead > 0.01f) || !(c > 0.01f) || !(below > 0.001f) || !(g > 0)) return -1;
    return sqrtf(g * ahead * ahead / (2 * c * c * below));
}

/* The velocity that lands a bomb leaving `from` on `target`, leaving as near as it can to angle `pitch` (radians above
   level), and no faster than max_speed. It tries the asked angle first, then steeper ones (up to BOMB_ARC_HIGHEST_DEG),
   then shallower ones (down to BOMB_ARC_LOWEST_DEG). Returns 1 with v set, or 0 when the target is out of reach. A target
   nearly straight below (within 10 cm level) is a drop: the velocity is zero. */
static inline int bomb_arc_onto(const float from[3], const float target[3], float pitch, float g, float max_speed, float v[3]) {
    const float dx = target[0] - from[0], dz = target[2] - from[2], up = target[1] - from[1];
    const float ahead = sqrtf(dx * dx + dz * dz);
    if (ahead < 0.1f) { v[0] = v[1] = v[2] = 0; return up <= 0; }
    const float lowest = BOMB_ARC_LOWEST_DEG * 0.0174533f, highest = BOMB_ARC_HIGHEST_DEG * 0.0174533f;
    float p = pitch < lowest ? lowest : pitch > highest ? highest : pitch;
    const float step = 2.5f * 0.0174533f;
    for (int i = 0; i < 64; i++) {   /* (the asked angle, then alternately steeper and shallower ones) */
        const int k = (i + 1) / 2;
        const float tryp = p + (i % 2 ? 1.0f : -1.0f) * k * step;
        if (tryp < lowest - 1e-4f || tryp > highest + 1e-4f) continue;
        const float s = bomb_arc_speed_for(ahead, up, tryp, g);
        if (s > 0 && s <= max_speed) {
            v[0] = dx / ahead * s * cosf(tryp), v[2] = dz / ahead * s * cosf(tryp), v[1] = s * sinf(tryp);
            return 1;
        }
    }
    return 0;
}

/* The gravity pull's choice: how well a hand pointing from `origin` along unit `dir` points at a ball of radius r centred
   at `centre`. Returns the angle (radians) between the pointing line and the ball's centre, less the angle the ball itself
   covers (so a near bomb is as easy to point at as its size makes it), or -1 when it is outside the cone (cone_rad either
   side of the line), nearer than near_m or further than reach_m, or behind the hand. Smaller is better. */
static inline float bomb_pull_aim(const float origin[3], const float dir[3], const float centre[3], float r,
                                  float near_m, float reach_m, float cone_rad) {
    const float d[3] = {centre[0] - origin[0], centre[1] - origin[1], centre[2] - origin[2]};
    const float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(dist > 1e-3f) || dist < near_m || dist > reach_m) return -1;
    float along = (d[0] * dir[0] + d[1] * dir[1] + d[2] * dir[2]) / dist;
    if (along <= 0) return -1;   /* (behind the hand) */
    along = along > 1 ? 1 : along;
    float off = acosf(along) - atanf(r / dist);
    if (off < 0) off = 0;
    return off <= cone_rad ? off : -1;
}

#endif
