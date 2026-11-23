/* Hookshot VR: where a VR hand holds a bottle. Shared by the game side (vr/items/bottle/bottle_host.c, which places the
   bottle actor there: its catch sensor and its mouth for drinking and tipping) and the renderer
   (vr/items/bottle/held_bottle.inc, which draws the bottle model there), so both put it in the same place.

   The hand is the controller's grip (calibrated, as hookshot_vr_hands_world gives it): its position, its x axis (the
   palm's normal: +x the left hand's palm, -x the right's) and its z axis (the fist runs along -z). The bottle is held by
   its neck, the fingers around it: the glass's axis runs along -z (the thumb's side), its mouth that way, kVrBottlePalm
   plus the neck's radius out from the grip toward the palm (where the renderer's ball grip closes the fingers, as on a
   bomb: arms_and_pose.inc), the neck kVrBottleNeck above the model's origin. Its model's x is the grip's x. Plain C,
   no state. */
#ifndef HOOKSHOT_VR_BOTTLE_POSE_H
#define HOOKSHOT_VR_BOTTLE_POSE_H

#define kVrBottleNeck 0.07f        /* (m: where the fingers hold the glass, up its axis from the model's origin: its
                                      narrowest, radius 0.061; the mouth is at 0.114, the cork from 0.065 to 0.143) */
#define kVrBottleNeckRadius 0.055f /* (m: the ball the fingers close on, a little inside the glass) */
#define kVrBottlePalm 0.03f        /* (m: the palm's surface from the grip, as the bomb's BOMB_PALM) */

/* the bottle model's place for a grip: origin[3], rot[9] (rot[3 * k + c]: column c is the model's x, y, z axis, z the
   mouth); side: 0 the left hand, 1 the right; gx: the grip's x, gz: its z (both unit length) */
static inline void vr_bottle_pose(int side, const float grip[3], const float gx[3], const float gz[3], float origin[3],
                                  float rot[9]) {
    const float palm = (side ? -1.0f : 1.0f) * (kVrBottlePalm + kVrBottleNeckRadius);
    float z[3], y[3];
    for (int k = 0; k < 3; k++) z[k] = -gz[k];
    y[0] = z[1] * gx[2] - z[2] * gx[1], y[1] = z[2] * gx[0] - z[0] * gx[2], y[2] = z[0] * gx[1] - z[1] * gx[0];   /* (z x x) */
    for (int k = 0; k < 3; k++) {
        rot[3 * k + 0] = gx[k], rot[3 * k + 1] = y[k], rot[3 * k + 2] = z[k];
        origin[k] = grip[k] + gx[k] * palm - z[k] * kVrBottleNeck;
    }
}

#endif
