/* Hookshot VR: putting away what a VR hand lets go of, the controls' side: one put-away, from the let-go until the game
   has it away. The sword, the Deku Stick, the bow and the slingshot, the hookshot and the boomerang all use it.

   It only ever asks for the game's own put-away (vr/items/gear/put_away_host.c hos_vr_put_away_held: f_804c8690, what
   events use, made on Link's next update) and never presses a button: the game's B rolls while Link moves and talks, opens
   or reads beside a person, a chest or a door (review finding 5, 20 Nov). Asked when it begins and again every
   kVrStowRetryNs while the item's own reader still says it is out (a request made while the game is still drawing it can be
   undone by the game), at most kVrStowTries times; still out then, the put-away has failed: it is logged
   ("vr: <item>: put away FAILED ...") and nothing else is done, the item left in Link's game hand.
   Taken again meanwhile (vr_stow_cancel), a request not yet made is withdrawn. One already made on the game's thread may
   still put the item away just after the grip took it again: the item's own take-out brings it back
   (vr_stow_regrip_lost says when).

   A piece of the controls, included where they need it (vr/input/controllers_host.c in host/nn_hid.c; the bow's, the
   hookshot's and the boomerang's files in host/hos_main.c); once per translation unit. Not compiled on its own. */
#ifndef VR_PUT_AWAY_STOW_INC
#define VR_PUT_AWAY_STOW_INC
#define kVrStowRetryNs 300000000LL
#define kVrStowTries 4
#define kVrStowRegripNs 300000000LL   /* (taken again this soon after a request: the game may still put it away) */
int hos_vr_put_away_held(void);       /* (vr/items/gear/put_away_host.c: asks; the call is made on Link's next update) */
void hos_vr_put_away_cancel(void);    /* (the same: a request not yet made, withdrawn) */
enum { kVrStowIdle, kVrStowWaiting, kVrStowDone, kVrStowFailed };
typedef struct { int active, tries; int64_t next, asked_at; } VrStow;

static void vr_stow_start(VrStow *w, int64_t now) { w->active = 1, w->tries = 0, w->next = now, w->asked_at = 0; }
static int vr_stow_busy(const VrStow *w) { return w->active; }
/* taken again before the game had it away: no more requests, the last one withdrawn if not yet made */
static void vr_stow_cancel(VrStow *w, const char *what) {
    if (!w->active) return;
    w->active = 0;
    hos_vr_put_away_cancel();
    hos_log("vr: %s: taken again before it was away: the put-away withdrawn", what);
}
/* after vr_stow_cancel: the item went away anyway (out now 0) soon after the last request, so the game made it */
static int vr_stow_regrip_lost(const VrStow *w, int out, int64_t now) {
    return !out && w->asked_at && now - w->asked_at < kVrStowRegripNs;
}
/* one read: what the put-away is doing (kVrStow*). out: the game still has the item out */
static int vr_stow_step(VrStow *w, int out, int64_t now, const char *what) {
    if (!w->active) return kVrStowIdle;
    if (!out) {
        hos_log("vr: %s: the game has put it away (%d request%s)", what, w->tries, w->tries == 1 ? "" : "s");
        w->active = 0;
        return kVrStowDone;
    }
    if (now < w->next) return kVrStowWaiting;
    if (w->tries >= kVrStowTries) {
        hos_log("vr: %s: put away FAILED: still out after %d of the game's put-away calls (no button pressed: left in Link's "
                "hand)", what, w->tries);
        w->active = 0;
        return kVrStowFailed;
    }
    hos_vr_put_away_held();
    w->tries++, w->next = now + kVrStowRetryNs, w->asked_at = now;
    if (w->tries == 1) hos_log("vr: %s: put away by the game's own call", what);
    else hos_log("vr: %s: still out: the put-away asked again (%d)", what, w->tries);
    return kVrStowWaiting;
}
#endif
