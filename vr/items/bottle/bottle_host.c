/* Hookshot VR: bottles in the VR hands (game side), first person (VR tab: Bottles in the hands, option 970).

   A bottle is the game's own item and actor, its contents the game's save (PlayerStatus.AssignableItem.BottleInfo):
   the VR mod draws the selected bottle into the VR hand that grips it and places it there, with no event, so none of
   the game's bottle cutscenes or animations play. The controls are vr/items/bottle/bottle_controls.inc, the held model
   vr/items/bottle/held_bottle.inc, where the hand holds it vr/items/bottle/bottle_pose.h.
   A research probe is at the end, off unless
   RX_VR_BOTTLE_PROBE is set (1 log, 3 draw/stow/content calls, 4 drawn and placed above Link, 5 the draw asked every
   update, 6 held from 6 to 14 s as if by a VR hand: the real draw, keep-drawn hook and put-away), on a copied save with
   bottles selected by the probe itself.

   Keeping it out: Link's own AI (its sheathed sub-action, ExecutePlayerSheathed, f_828275e0: enter 828266b0 stows handles
   0, 4, 1 and 3) asks every frame for handle 1 to be stowed (headless 18 Nov: its drawn bits 04 after each of our draws'
   02, the bottle never leaving state 2). The game hook hos_vr_bottle_keep_drawn (vr/vr_game_hooks.py, in the stow
   f_80e54c70 itself) turns those stows of handle 1 away while a VR hand holds a bottle, and only then.

   Two ways to use it: the simple bottle (the default, for release: the trigger uses it, by the game's calls or as the
   game's own bottle use; vr_bottle_simple_update) and the full one (RX_VR_BOTTLE_FULL=1: the cork, drinking at the
   face, tipping out; vr_bottle_use_update), on hold.

   Game-side code, included by host/hos_main.c (its VR HOOK block) after vr/items/deku_stick/deku_stick_host.c (it uses
   vr/items/gear/equipment_host.c's readers, vr/items/bomb/bomb_host.c's vr_bomb_place). Not compiled on its own. */
#include "bottle_pose.h"

/* The five bottles are assignable items 14, 15, 16, 17 and 50 (OneShotConsumeDrinkFromSelectedBottle, f_828e2ca0) */
static const int kVrBottleItems[5] = {14, 15, 16, 17, 50};

/* A guest call with up to seven integer arguments, on a stack frame below the caller's; returns x0 */
static uint64_t vr_bottle_call(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                               uint64_t a5, uint64_t a6) {
    A64Fn f = a64_lookup(fn);
    if (!f) return 0;
    const Ctx saved = *c;
    c->x[31] = (c->x[31] - 0x100) & ~0xfULL;
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3, c->x[4] = a4, c->x[5] = a5, c->x[6] = a6;
    f(c);
    const uint64_t r = c->x[0];
    *c = saved;
    return r;
}

/* The inventory (the pouch object, *(*0x84c9e0d0)), or 0 */
static uint64_t vr_bottle_inventory(Ctx *c) {
    const uint64_t slot = recomp_load64(c, 0x84c9e0d0ULL);
    const uint64_t pouch = slot >= 0x80000000ULL && !(slot & 7) && hos_guest_range(slot, 8) ? recomp_load64(c, slot) : 0;
    return pouch >= HOS_HEAP_BASE ? pouch : 0;
}

/* Link's EquipmentUser component (player +0x270 components, +0x200), or 0 */
static uint64_t vr_bottle_equipment_user(Ctx *c, uint64_t player) {
    if (!oot_vr_equipment_ptr(player, 0x278)) return 0;
    const uint64_t components = recomp_load64(c, player + 0x270);
    const uint64_t user = oot_vr_equipment_ptr(components, 0x208) ? recomp_load64(c, components + 0x200) : 0;
    return oot_vr_equipment_ptr(user, 0x13c4) ? user : 0;
}

/* Bottle b (0 A .. 4 E) is owned: the save's PlayerStatus.AssignableItem.BottleInfo.Bottle<X>.IsValid (murmur3 keys) */
static int vr_bottle_owned(Ctx *c, int b) {
    static const uint32_t keys[5] = {0xf39ce410u, 0x000ff6eau, 0xf63b1194u, 0xb7f052ceu, 0xe27b136du};
    int owned = 0;
    return b >= 0 && b < 5 && oot_gamedata_bool(c, keys[b], &owned) && owned;
}

/* Bottle b's content as the game's code reads it (f_80424b80): -1 empty, 0 RedPotion .. 11 HalfMilk (the save's order
   without Empty), or -2 when it cannot be read */
static int vr_bottle_content(Ctx *c, int b) {
    const uint64_t inventory = vr_bottle_inventory(c);
    if (!inventory || b < 0 || b > 4) return -2;
    const int content = (int)(int32_t)vr_bottle_call(c, 0x80424b80ULL, inventory, (uint64_t)kVrBottleItems[b], 0, 0, 0, 0, 0);
    return content >= -1 && content <= 11 ? content : -2;
}

/* Plays an animation command on a bottle (its cork: CapOff, CapOn; the milk's CapOff_Milk, the letter's CapOff_Letter):
   the actor's animation controller (actor +624 -> +64 -> +24 -> +24, as ExecuteASSetAndSetFrame finds it) and
   f_80994c70(controller, &name, slot 0, 0) with s0 = s1 = -1 (as ExecuteASSetAndSetFrame calls it, f_826402f0; the slot
   is f_80aed6d0's for its Partial name, 0 for none). 1 when it was asked. */
static int vr_bottle_play(Ctx *c, uint64_t actor, const char *command) {
    static const int path[] = {624, 64, 24, 24};
    const uint64_t controller = vr_bomb_chain(c, actor, path, 4);
    A64Fn play = a64_lookup(0x80994c70ULL);
    if (!controller || !play || strlen(command) >= 48) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x100) & ~0xfULL;
    memcpy(A64_PTR(frame + 0x40), command, strlen(command) + 1);
    recomp_store64_sp(c, frame + 0x20, frame + 0x40);
    c->x[31] = frame;
    c->x[0] = controller, c->x[1] = frame + 0x20, c->x[2] = 0, c->x[3] = 0;
    const float minus_one = -1.0f;
    uint32_t bits;
    memcpy(&bits, &minus_one, 4);
    a64_vset64(c->vreg[0], bits), a64_vset64(c->vreg[1], bits);
    play(c);
    *c = saved;
    return 1;
}

/* Heals Link by quarter hearts through his Life component, as OneShotPlayerUseFairyRecover does (f_82931d80): the Life
   component (f_80853e00(actor)), its recover method (vtable +0x90) given a recovery record: the amount (u32) and 0, a
   float 8384 before them and a 1 after; the request: -1 and that float, -1, the record's address. 1 when asked. */
static int vr_bottle_heal(Ctx *c, uint64_t actor, int quarters) {
    const uint64_t life = vr_bottle_call(c, 0x80853e00ULL, actor, 0, 0, 0, 0, 0, 0);
    if (life < HOS_HEAP_BASE || quarters <= 0) return 0;
    const uint64_t method = recomp_load64(c, recomp_load64(c, life) + 0x90);
    A64Fn recover = a64_lookup(method);
    if (!recover) return 0;
    const Ctx saved = *c;
    const uint64_t frame = (c->x[31] - 0x100) & ~0xfULL;
    const uint64_t record = frame + 0x40, request = frame + 0x10;
    recomp_store32(c, record - 8, 0x46030000u), recomp_store32(c, record - 4, 0);
    recomp_store32(c, record, (uint32_t)quarters), recomp_store32(c, record + 4, 0), recomp_store32(c, record + 8, 1);
    recomp_store64_sp(c, request, 0x46030000ffffffffULL);
    recomp_store32(c, request + 8, 0xffffffffu), recomp_store32(c, request + 12, 0);
    recomp_store64_sp(c, request + 16, record);
    c->x[31] = frame;
    c->x[0] = life, c->x[1] = request;
    recover(c);
    *c = saved;
    return 1;
}

/* ---- the held bottle ---------------------------------------------------------------------------------------------
   Selecting a bottle (the VR radial menu, or the game's own) makes the game create that bottle's own actor,
   Weapon_Bottle_<content> (Weapon_Bottle when empty), in Link's EquipmentUser slot 1, stowed (Equipment state 2):
   headless 18 Nov, the Deku Stick's slot. Its model shows the contents. A VR hand's grip (bottle_controls.inc) draws it
   with the game's own call (f_806d6820(user, 1): what OneShotEquipmentUserRequestChangeState and the full-bottle event
   ExecuteEventPlayerFullBottleDrawn call), every update it is placed on that hand (vr_bottle_pose, as the renderer draws
   it), and letting go stows it again (vr_bottle_put_away). No event plays, so no cutscene and no animation. */
#define kVrBottleOption 970              /* (VR tab: Bottles in the hands, 0 off, 1 on) */
#define kVrBottleSlot 1                  /* (Link's EquipmentUser handle the selected item's actor is in) */
#define kVrBottleRetryNs 300000000LL     /* (a draw or stow asked again this often until the game has done it) */
#define kVrBottleStowTries 4             /* (stows asked at most this many times after a let-go) */
int hos_vr_bottle_holder(void);          /* (vr/items/bottle/bottle_controls.inc, the VR hand holding it, or -1) */
int hos_oot_vr_first_person(void);
static volatile int vr_bottle_selected_now;   /* the selected item is a bottle */
static volatile int vr_bottle_away_now;       /* let go and the game still has it out (hidden from the hands meanwhile) */
static volatile uint64_t vr_bottle_actor_now; /* the selected bottle's actor while it is out and a VR hand holds it (0: none) */
static volatile uint64_t vr_bottle_user_now;  /* Link's EquipmentUser, while a VR hand holds a bottle in handle 1 (0: not) */
int hos_vr_bottle_selected(void) { return __atomic_load_n(&vr_bottle_selected_now, __ATOMIC_RELAXED); }
int hos_vr_bottle_away(void) { return __atomic_load_n(&vr_bottle_away_now, __ATOMIC_RELAXED); }

/* The selected item is a bottle: its assignable item number is one of the five (quick_select_host.c reads it) */
static volatile int vr_bottle_probe_hold;     /* (the research probe's modes 6 and 7, headless: bottle A held as if by a VR
                                                 hand; there the selection reader, run from the VR camera, has nothing) */
static int vr_bottle_is_selected(void) {
    if (__atomic_load_n(&vr_bottle_probe_hold, __ATOMIC_RELAXED)) return 1;
    const int item = __atomic_load_n(&oot_vr_selected_index, __ATOMIC_RELAXED);
    for (int b = 0; b < 5; b++) if (item == kVrBottleItems[b]) return 1;
    return 0;
}

/* The bottle actor in Link's slot 1 and its Equipment state (1 out, 2 stowed, -1 not read), or 0 when that slot holds no
   bottle; name: its actor name */
static uint64_t vr_bottle_find(Ctx *c, uint64_t user, int *state, char name[64]) {
    *state = -1, name[0] = 0;
    const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)kVrBottleSlot;
    const uint32_t generation = recomp_load32(c, handle + 0x10);
    const uint64_t entry = recomp_load64(c, handle + 8);
    if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) || recomp_load32(c, entry + 0x48) != generation) return 0;
    const uint64_t actor = recomp_load64(c, entry + 0x40);
    if (!oot_vr_equipment_ptr(actor, 0x340)) return 0;
    const uint32_t life = recomp_load32(c, actor + 0x2c);
    if (life >= 7 && life < 12) return 0;   /* (being destroyed: as the equipment reader) */
    oot_vr_equipment_name(c, actor, 0x260, name);
    if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
    if (strncmp(name, "Weapon_Bottle", 13)) return 0;
    const uint64_t parts = recomp_load64(c, actor + 0x270);
    const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
    if (oot_vr_equipment_ptr(equipment, 0xa0)) *state = (int)recomp_load32(c, equipment + 0x9c);
    return actor;
}

/* Puts the held bottle away: the game's own stow of handle 1 (f_80e54c70: OneShotEquipmentUserRequestChangeState's
   state 2). One small function, for the shared put-away call to take its place: hos_vr_put_away_held()
   (vr/items/gear/put_away_host.c, on the put-away branch, not merged here yet; it sheathes every drawn handle, as the
   game's B, handle 1 among them). The game hook below lets this stow through: by then no VR hand holds the bottle. */
static void vr_bottle_put_away(Ctx *c, uint64_t user) {
    vr_bottle_call(c, 0x80e54c70ULL, user, kVrBottleSlot, 0, 0, 0, 0, 0);
}

/* Whether a stow of one of Link's EquipmentUser handles is turned away (the game hook's rule, kept apart to be checked):
   only handle 1, only Link's own EquipmentUser, only while that handle holds a bottle a VR hand holds, the VR tab's row
   on, in first person, and no event playing (an event's own stow, as a talk's or a door's, goes through) */
static int vr_bottle_keep_drawn(uint64_t user, uint32_t slot, uint64_t holding_user, int event_playing) {
    return slot == kVrBottleSlot && user && user == holding_user && !event_playing;
}

/* (game hook, vr/vr_game_hooks.py: the stow f_80e54c70(user, slot), right after it loads the handle's generation into w8)
   a stow of the held bottle's handle is turned away: w8 = -1 takes the stow's own "no item in this handle" way out */
void hos_vr_bottle_keep_drawn(Ctx *c) {
    int hos_vr_event_playing(void);   /* (vr/camera/view_host.c) */
    const uint64_t holding = __atomic_load_n(&vr_bottle_user_now, __ATOMIC_RELAXED);
    if (!holding || !vr_bottle_keep_drawn(c->x[0], (uint32_t)c->x[1], holding, hos_vr_event_playing())) return;
    c->x[8] = 0xffffffffULL;
    static unsigned told;
    if (told < 3) told++, hos_log("vr: bottle: the game's stow of the held bottle turned away (a VR hand holds it)");
}

/* ---- the cork, drinking and releasing (first person, a VR hand holding an open or corked bottle) -----------------
   A filled bottle comes out corked (its actor plays CapOn when made: Weapon_Bottle_<content>'s ASInfo), an empty one
   open (Weapon_Bottle's CapOff). With no event, so no cutscene and no animation of Link:
   - Uncork: the holding hand's trigger, or the other hand's grip or trigger at the cork (kVrCorkReach of the mouth). The
     renderer is told (rx_vr_bottle_cork) and draws the cork popping out and falling to the floor (held_bottle.inc). The
     bottle's own CapOff does not hold on a drawn bottle (headless 18 Nov: played, the cork still in the neck; Link's
     equipment seems to set its animation), so the game's cork is left as it is and only the drawing changes.
   - A fairy flies out when uncorked: Link is healed as the game's fairy recovery heals (OneShotPlayerUseFairyRecover's
     own amount, RevivalFairy, and its call: headless 18 Nov, the call healed 4 quarter hearts on the next frame) and the
     bottle is empty.
   - Drink (potions, milk): the open mouth held at the headset (kVrDrinkReach, the bottle tipped) for kVrDrinkNs: the
     content as the game's drink leaves it (OneShotConsumeDrinkFromSelectedBottle: potions and half milk empty, milk
     half), and the heal (BottleHeal's amount through the same recovery call). Magic (green and blue potions) not yet.
   - Release (Poes, Big Poes): the open bottle upside down for kVrTipNs: the soul is gone, the bottle empty. A bug, a fish
     or Blue Fire is NOT let out yet: the game's own release of the animal or flame the bottle holds (its InBottle actor)
     is still to be found, and emptying without it would lose a bug a soft-soil spot needs; tipping them is only logged.
   - Put away with its content unused: corked again, so it comes out corked next time. */
#define kVrBottleMouth 0.114f        /* (m up the glass from the model's origin: vk_vr_bottle.h bottle_mouth) */
#define kVrCorkReach 0.12f           /* (m: the other hand this close to the mouth pulls the cork) */
#define kVrDrinkReach 0.16f          /* (m: the mouth this close to the headset drinks) */
#define kVrDrinkTip 0.3f             /* (the glass's axis . up below this: tipped toward the mouth) */
#define kVrDrinkNs 600000000LL
#define kVrTipDown -0.6f             /* (the glass's axis . up below this: upside down) */
#define kVrTipNs 350000000LL
enum { kVrEmpty = -1, kVrRedPotion = 0, kVrGreenPotion, kVrBluePotion, kVrBlueFire, kVrFish, kVrLetter, kVrBigPoe, kVrPoe,
       kVrBug, kVrFairy, kVrMaxMilk, kVrHalfMilk };   /* (the game's code index: the save's order without Empty) */
static const char *const kVrBottleContentNames[13] = {"empty", "red potion", "green potion", "blue potion", "Blue Fire",
                                                      "fish", "Ruto's Letter", "Big Poe", "Poe", "bug", "fairy", "milk",
                                                      "half milk"};
int rx_vr_eye_world(float out[3]);   /* (vr/headset/bridges_host.c) */

/* BottleHeal's value for a field (Player's GameParameter: hearts; 20 a full heal), read as its getters read it: the value
   at +0x38 + 4 x field when its set flag (+0x57 + field) is, else the parent parameter's (f_826ded00(param + 0x10)).
   The fields are the parameter's names in order (headless 18 Nov, probe mode 7: 20 5 4 -3 -1 3 20 5 from +0x38, every
   flag set; OneShotPlayerUseFairyRecover's getter f_82931ef0 reads +0x54, flag +0x5e: RevivalFairy) */
enum { kVrHealBluePotion = 0, kVrHealFairy, kVrHealMilk, kVrHealPoeDamageBig, kVrHealPoeDamageSmall, kVrHealPoeHeal,
       kVrHealRedPotion, kVrHealRevivalFairy };
static int vr_bottle_heal_param(Ctx *c, uint64_t player, int field, int otherwise) {
    uint64_t param = vr_bottle_call(c, 0x826dead0ULL, player, 0, 0, 0, 0, 0, 0);
    for (int depth = 0; depth < 4 && param >= HOS_HEAP_BASE && hos_guest_range(param, 0x60); depth++) {
        if (recomp_load8(c, param + 0x57 + field) & 1) return (int)(int32_t)recomp_load32(c, param + 0x38 + 4 * field);
        param = vr_bottle_call(c, 0x826ded00ULL, param + 0x10, 0, 0, 0, 0, 0, 0);
    }
    return otherwise;
}

typedef struct {
    uint64_t actor;           /* the held bottle's actor (a new one when the content changes) */
    int b, content, corked;   /* which bottle (0 A .. 4 E), what it holds, whether its cork is in */
    int trigger_was[2], grip_was[2], tipped_told;
    int64_t at_mouth_since, tipped_since;
} VrBottleUse;
static VrBottleUse vr_bottle_use = {0, -1, -2, 0, {0, 0}, {0, 0}, 0, 0, 0};
static volatile int vr_bottle_at_cork_now = -1;   /* the other hand while it is at a corked bottle's cork (its buttons are
                                                     the cork's: bottle_controls.inc), or -1 */
int hos_vr_bottle_at_cork(void) { return __atomic_load_n(&vr_bottle_at_cork_now, __ATOMIC_RELAXED); }

/* the selected bottle (0 A .. 4 E), or -1 */
static int vr_bottle_selected_index(void) {
    if (__atomic_load_n(&vr_bottle_probe_hold, __ATOMIC_RELAXED)) {   /* (the probe's: the one it selects, RX_VR_BOTTLE_SELECT) */
        static int probe_b = -1;
        if (probe_b < 0) {
            const int item = vr_research_int("RX_VR_BOTTLE_SELECT", 14);
            probe_b = 0;
            for (int b = 0; b < 5; b++) if (item == kVrBottleItems[b]) probe_b = b;
        }
        return probe_b;
    }
    const int item = __atomic_load_n(&oot_vr_selected_index, __ATOMIC_RELAXED);
    for (int b = 0; b < 5; b++) if (item == kVrBottleItems[b]) return b;
    return -1;
}

/* the content set as a use leaves it, through the game's own bottle calls (f_833c45a0 sets, f_833c7280 empties) */
static void vr_bottle_set_content(Ctx *c, int b, int content) {
    const uint64_t inventory = vr_bottle_inventory(c);
    if (!inventory || b < 0) return;
    if (content == kVrEmpty) vr_bottle_call(c, 0x833c7280ULL, inventory, (uint64_t)kVrBottleItems[b], 1, 0, 0, 0, 0);
    else vr_bottle_call(c, 0x833c45a0ULL, inventory, (uint64_t)kVrBottleItems[b], (uint64_t)content, 0, 1, (uint64_t)-1, 0);
}

/* (Link's update, while a VR hand holds the drawn bottle) the cork, a drink, a release */
static void vr_bottle_use_update(Ctx *c, uint64_t player, uint64_t bottle, int taken, int64_t now) {
    VrBottleUse *u = &vr_bottle_use;
    const int b = vr_bottle_selected_index();
    if (b < 0) return;
    if (taken || b != u->b) {   /* (a new take: a filled bottle comes out corked) */
        u->b = b, u->content = vr_bottle_content(c, b), u->corked = u->content >= 0, u->actor = bottle;
        u->at_mouth_since = u->tipped_since = 0, u->tipped_told = 0;
        hos_log("vr: bottle: holding bottle %c, %s%s", 'A' + b, u->content >= -1 ? kVrBottleContentNames[u->content + 1] : "?",
                u->corked ? ", corked" : "");
    } else if (bottle != u->actor) {   /* (the content changed: the game made the new content's actor, corked) */
        u->actor = bottle, u->content = vr_bottle_content(c, b);
        /* (still open: the renderer keeps drawing it so) */
    }
    float w[42];
    const int hand = hos_vr_bottle_holder();
    if (hand < 0 || !rx_vr_hands_world(w) || w[21 * hand] < .5f) return;
    const float *o = w + 21 * hand, *other = w + 21 * (1 - hand);
    float origin[3], rot[9], mouth[3], axis[3];
    vr_bottle_pose(hand, o + 1, o + 9, o + 15, origin, rot);
    for (int k = 0; k < 3; k++) axis[k] = rot[3 * k + 2], mouth[k] = origin[k] + axis[k] * kVrBottleMouth;
    const float up = axis[1];   /* (the game world's up is +y) */
    const int trigger = o[7] > .55f, other_trigger = other[0] > .5f && other[7] > .55f, other_grip = other[0] > .5f && other[8] > .55f;
    float d2 = 0;
    for (int k = 0; k < 3; k++) d2 += (other[1 + k] - mouth[k]) * (other[1 + k] - mouth[k]);
    const int at_cork = other[0] > .5f && d2 < kVrCorkReach * kVrCorkReach;
    const int pull = (trigger && !u->trigger_was[hand]) ||
                     (at_cork && ((other_trigger && !u->trigger_was[1 - hand]) || (other_grip && !u->grip_was[1 - hand])));
    u->trigger_was[hand] = trigger, u->trigger_was[1 - hand] = other_trigger, u->grip_was[1 - hand] = other_grip;
    __atomic_store_n(&vr_bottle_at_cork_now, u->corked && at_cork ? 1 - hand : -1, __ATOMIC_RELAXED);
    {   void rx_vr_bottle_cork(int corked);   /* (vr/headset/bridges_host.c: the renderer draws the cork in or popping) */
        rx_vr_bottle_cork(u->corked); }
    if (u->corked && pull) {
        u->corked = 0;
        hos_log("vr: bottle: uncorked (%s): the cork pops off", trigger ? "its own hand's trigger" : "the other hand at the cork");
        if (u->content == kVrFairy) {   /* (the fairy flies out and heals Link, as the game's fairy recovery) */
            const uint64_t heal = vr_bottle_call(c, 0x826dead0ULL, player, 0, 0, 0, 0, 0, 0);
            const uint64_t amount = heal >= HOS_HEAP_BASE ? vr_bottle_call(c, 0x82931ef0ULL, heal, 0, 0, 0, 0, 0, 0) : 0;
            const int hearts = amount >= HOS_HEAP_BASE ? (int)(int32_t)recomp_load32(c, amount) : 0;
            vr_bottle_heal(c, player, hearts * 4);
            vr_bottle_set_content(c, b, kVrEmpty);
            hos_log("vr: bottle: the fairy flew out: healed %d hearts, the bottle is empty", hearts);
        }
    }
    if (u->corked) return;
    const int drink = u->content == kVrRedPotion || u->content == kVrGreenPotion || u->content == kVrBluePotion ||
                      u->content == kVrMaxMilk || u->content == kVrHalfMilk;
    float eye[3], e2 = 1e9f;
    if (drink && rx_vr_eye_world(eye)) {
        e2 = 0;
        for (int k = 0; k < 3; k++) e2 += (eye[k] - mouth[k]) * (eye[k] - mouth[k]);
    }
    if (drink && e2 < kVrDrinkReach * kVrDrinkReach && up < kVrDrinkTip) {
        if (!u->at_mouth_since) u->at_mouth_since = now;
        if (now - u->at_mouth_since >= kVrDrinkNs) {
            static const int fields[12] = {kVrHealRedPotion, -1, kVrHealBluePotion, -1, -1, -1, -1, -1, -1, -1, kVrHealMilk, kVrHealMilk};
            const int hearts = fields[u->content] >= 0 ? vr_bottle_heal_param(c, player, fields[u->content], 0) : 0;
            if (hearts > 0) vr_bottle_heal(c, player, hearts * 4);
            const int left = u->content == kVrMaxMilk ? kVrHalfMilk : kVrEmpty;
            hos_log("vr: bottle: drank the %s: healed %d hearts%s, %s left", kVrBottleContentNames[u->content + 1], hearts,
                    u->content == kVrGreenPotion || u->content == kVrBluePotion ? " (its magic: not yet)" : "",
                    kVrBottleContentNames[left + 1]);
            vr_bottle_set_content(c, b, left);
            u->content = left, u->at_mouth_since = 0;
        }
    } else u->at_mouth_since = 0;
    const int release = u->content == kVrPoe || u->content == kVrBigPoe;
    const int held_in = u->content == kVrBug || u->content == kVrFish || u->content == kVrBlueFire;
    if ((release || held_in) && up < kVrTipDown) {
        if (!u->tipped_since) u->tipped_since = now;
        if (now - u->tipped_since >= kVrTipNs && release) {
            hos_log("vr: bottle: the %s let out: the bottle is empty", kVrBottleContentNames[u->content + 1]);
            vr_bottle_set_content(c, b, kVrEmpty);
            u->content = kVrEmpty, u->tipped_since = 0;
        } else if (now - u->tipped_since >= kVrTipNs && held_in && !u->tipped_told) {
            u->tipped_told = 1;
            hos_log("vr: bottle: tipped with a %s in it: letting it out is not done yet (kept in the bottle)",
                    kVrBottleContentNames[u->content + 1]);
        }
    } else u->tipped_since = 0, u->tipped_told = 0;
}

/* ---- the simple bottle (the default, for release) --------------------------------------------------------------------
   Grip holds the selected bottle, empty or filled, in either hand; the holding hand's trigger uses it, by its content:
   - A drink (red, green and blue potions, milk, half milk), a fairy or a Poe: used here at once with the game's own calls,
     no event: the heal (BottleHeal's amount, as above), the magic (green and blue: vr_bottle_magic), and the content as
     the game's drink leaves it (milk -> half milk, the rest empty).
   - Everything else (the empty bottle's catch, a bug, a fish, Blue Fire, Ruto's Letter, a Big Poe): the game's own bottle
     use, its swing or its event (Dm_SY_InsertBottle, Dm_SY_OutBottle, Dm_SY_LetterBottle...), from that same trigger
     passed to the game as its item button (bottle_controls.inc: the player's own press, as the hookshot's trigger). So a
     bug goes into soft soil, Blue Fire melts red ice and an NPC takes the letter or the Big Poe as the game has it.
   The cork stays in (no popping), nothing is drunk at the face or tipped out: that is the full bottle, kept whole above
   (vr_bottle_use_update) behind RX_VR_BOTTLE_FULL=1, on hold for release (Wishlist). */
int hos_vr_bottle_full(void) {
    static volatile int full = -1;
    if (full < 0) full = env_int("RX_VR_BOTTLE_FULL", 0) != 0;
    return full;
}
static volatile int vr_bottle_to_game_now;   /* the held bottle's content is the game's own use's (simple) */
static volatile int vr_bottle_use_asked;     /* the holding hand's trigger asked a use by calls (simple: bottle_controls.inc) */
int hos_vr_bottle_to_game(void) { return __atomic_load_n(&vr_bottle_to_game_now, __ATOMIC_RELAXED); }
void hos_vr_bottle_ask_use(void) { __atomic_store_n(&vr_bottle_use_asked, 1, __ATOMIC_RELAXED); }

/* The game's own use is handed the bottle first. With the bottle held out by the VR hand (drawn by us, kept drawn by the
   hook) the game's item button does nothing (headless 22 Nov, Zora's Domain's pond, empty bottle: no swing, no event;
   without our hold the same ZR swung and caught a fish at once). So a press for the game's use (bottle_controls.inc,
   hos_vr_bottle_ask_game_use) lets the bottle go back to the game first: the hook's mark cleared and the game's own stow
   asked; once it is stowed (hos_vr_bottle_game_ready) the trigger reaches the game as ZR, the game takes it out itself
   for its swing or its event, and the renderer keeps drawing it in the VR hand (any bottle in Link's game hand is moved
   there while a VR hand holds one: held_bottle.inc). Once its event is over (a catch, a release: Dm_SY_InsertBottle...),
   or kVrGameUseIdleNs after the press when none came (a swing that caught nothing leaves Link standing with the bottle
   out, action 0x408, until his next action: headless 22 Nov), it is drawn into the VR hand again. */
#define kVrGameUseGiveNs 1000000000LL   /* (the stow given this long before the press is dropped) */
#define kVrGameUseIdleNs 1500000000LL   /* (stowed, ZR given, no event this long: the swing is over or the game did nothing) */
#define kVrGameUseDoneNs 400000000LL    /* (its event over this long: the use is done) */
enum { kVrGameUseNone, kVrGameUseGiving, kVrGameUseUsing };
static volatile int vr_bottle_game_use_asked;   /* (controls -> game side: a press for the game's own use) */
static volatile int vr_bottle_game_ready_now;   /* (game side -> controls: the bottle is the game's again: ZR may go) */
void hos_vr_bottle_ask_game_use(void) { __atomic_store_n(&vr_bottle_game_use_asked, 1, __ATOMIC_RELAXED); }
int hos_vr_bottle_game_ready(void) { return __atomic_load_n(&vr_bottle_game_ready_now, __ATOMIC_RELAXED); }

/* (Link's update) the handover's phase: kVrGameUseNone, or the bottle being given (stowed) or used by the game */
static int vr_bottle_game_use_step(Ctx *c, uint64_t user, uint64_t bottle, int state, int want, int64_t now) {
    static int phase = kVrGameUseNone, saw;
    static int64_t at;
    int hos_vr_event_playing(void);
    if (__atomic_exchange_n(&vr_bottle_game_use_asked, 0, __ATOMIC_RELAXED) && want && phase == kVrGameUseNone) {
        phase = kVrGameUseGiving, at = now, saw = 0;
        __atomic_store_n(&vr_bottle_user_now, 0, __ATOMIC_RELAXED);   /* (the hook lets the stows through from now) */
        if (bottle && state == 1) vr_bottle_put_away(c, user);
        hos_log("vr: bottle: handed to the game for its own use (stowed first)");
    }
    if (phase == kVrGameUseGiving) {
        if (!bottle || state != 1) {
            phase = kVrGameUseUsing, at = now;
            __atomic_store_n(&vr_bottle_game_ready_now, 1, __ATOMIC_RELAXED);
        } else if (now - at > kVrGameUseGiveNs) {
            phase = kVrGameUseNone;
            hos_log("vr: bottle: the game did not take the bottle back in %lld ms: the press dropped", (long long)(kVrGameUseGiveNs / 1000000));
        }
    } else if (phase == kVrGameUseUsing) {
        const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint32_t action = object >= HOS_HEAP_BASE ? recomp_load32(c, object + 0x1f60) : 0;
        static uint32_t action_told;
        if (action != action_told && action) hos_log("vr: bottle: the game's own use: Link's action %#x", action);
        action_told = action;
        if (hos_vr_event_playing()) {
            if (!saw) hos_log("vr: bottle: the game's own use plays an event");
            saw = 1, at = now;
        }
        if ((saw && now - at > kVrGameUseDoneNs) || (!saw && now - at > kVrGameUseIdleNs)) {
            hos_log("vr: bottle: the game's own use is over (%s): back in the VR hand", saw ? "its event ended" : "no event");
            phase = kVrGameUseNone, action_told = 0;
        }
    }
    if (phase != kVrGameUseUsing) __atomic_store_n(&vr_bottle_game_ready_now, 0, __ATOMIC_RELAXED);
    return phase;
}

/* a content the game's own bottle use takes (its catch, or a release, a pour or a give it may play an event for) */
static int vr_bottle_used_by_game(int content) {
    return content == kVrEmpty || content == kVrBlueFire || content == kVrFish || content == kVrLetter ||
           content == kVrBigPoe || content == kVrBug;
}

/* Fills Link's magic as OneShotPlayerHealMPMax does (f_8292b810): f_82ea0f80 on Link's player component (his components
   +0x358, what the node's f_80f40f90 finds from its owner). 1 when asked. */
static int vr_bottle_magic(Ctx *c, uint64_t player) {
    const uint64_t parts = recomp_load64(c, player + 0x270);
    const uint64_t component = parts >= HOS_HEAP_BASE && hos_guest_range(parts + 0x358, 8) ? recomp_load64(c, parts + 0x358) : 0;
    if (component < HOS_HEAP_BASE || !hos_guest_range(component, 0x1c40)) return 0;
    vr_bottle_call(c, 0x82ea0f80ULL, component, 0, 0, 0, 0, 0, 0);
    return 1;
}

/* a use by the game's calls: the heal, the magic, the content left (simple; any other content: nothing) */
static void vr_bottle_use_by_calls(Ctx *c, uint64_t player, int b, int content) {
    int field = -1, magic = 0, left = kVrEmpty;
    switch (content) {
    case kVrRedPotion: field = kVrHealRedPotion; break;
    case kVrGreenPotion: magic = 1; break;
    case kVrBluePotion: field = kVrHealBluePotion, magic = 1; break;
    case kVrMaxMilk: field = kVrHealMilk, left = kVrHalfMilk; break;
    case kVrHalfMilk: field = kVrHealMilk; break;
    case kVrFairy: field = kVrHealRevivalFairy; break;   /* (OneShotPlayerUseFairyRecover's own amount, getter f_82931ef0) */
    case kVrPoe: field = kVrHealPoeHeal; break;
    default: return;
    }
    const int hearts = field >= 0 ? vr_bottle_heal_param(c, player, field, 0) : 0;
    if (hearts > 0) vr_bottle_heal(c, player, hearts * 4);
    const int magic_asked = magic && vr_bottle_magic(c, player);
    vr_bottle_set_content(c, b, left);
    hos_log("vr: bottle: used the %s (bottle %c): healed %d hearts%s, %s left", kVrBottleContentNames[content + 1], 'A' + b,
            hearts, magic ? magic_asked ? ", magic filled" : ", magic not found" : "", kVrBottleContentNames[left + 1]);
}

/* (Link's update, while a VR hand holds the drawn bottle) which way the trigger uses it; a use by calls when asked */
static void vr_bottle_simple_update(Ctx *c, uint64_t player, int taken) {
    const int b = vr_bottle_selected_index();
    const int content = b >= 0 ? vr_bottle_content(c, b) : -2;
    if (taken || b != vr_bottle_use.b) {
        vr_bottle_use.b = b, vr_bottle_use.content = content;
        if (b >= 0) hos_log("vr: bottle: holding bottle %c, %s: the trigger %s", 'A' + b, content >= -1 ? kVrBottleContentNames[content + 1] : "?",
                            content == -2 ? "does nothing (not read)" : vr_bottle_used_by_game(content) ? "is the game's bottle use" : "uses it");
    }
    __atomic_store_n(&vr_bottle_to_game_now, content >= -1 && vr_bottle_used_by_game(content), __ATOMIC_RELAXED);
    if (__atomic_exchange_n(&vr_bottle_use_asked, 0, __ATOMIC_RELAXED) && b >= 0 && content >= 0 && !vr_bottle_used_by_game(content))
        vr_bottle_use_by_calls(c, player, b, content);
}

/* (Link's update) the bottle drawn while a VR hand holds it, stowed once none does */
static void vr_bottle_link_update(Ctx *c, uint64_t player) {
    static int64_t asked_at;
    static int drawn_by_us, tries;   /* (tries: draws or stows asked without the game doing it, for the log) */
    static char logged_name[64];
    const int selected = vr_bottle_is_selected();
    __atomic_store_n(&vr_bottle_selected_now, selected, __ATOMIC_RELAXED);
    const uint64_t user = vr_bottle_equipment_user(c, player);
    int state = -1;
    char name[64] = "";
    const uint64_t bottle = selected && user ? vr_bottle_find(c, user, &state, name) : 0;
    const int64_t now = hos_now_ns();
    const int want = ((rx_option(kVrBottleOption, 0) && hos_oot_vr_first_person() && hos_vr_bottle_holder() >= 0) ||
                      __atomic_load_n(&vr_bottle_probe_hold, __ATOMIC_RELAXED)) && bottle;
    if (bottle && strcmp(name, logged_name)) {
        hos_log("vr: bottle: the selected bottle is %s (actor %#llx, state %d)", name, (unsigned long long)bottle, state);
        snprintf(logged_name, sizeof logged_name, "%s", name);
    }
    int hos_vr_event_playing(void);   /* (vr/camera/view_host.c: an event puts it away and takes its own: not drawn meanwhile) */
    const int full = hos_vr_bottle_full();
    const int given = !full && vr_bottle_game_use_step(c, user, bottle, state, want, now) != kVrGameUseNone;   /* (the game's) */
    if (want && state != 1 && now - asked_at > kVrBottleRetryNs && !hos_vr_event_playing() && !given) {
        if (!drawn_by_us) tries = 0;
        asked_at = now, drawn_by_us = 1;
        vr_bottle_call(c, 0x806d6820ULL, user, kVrBottleSlot, 0, 0, 0, 0, 0);
        if (tries++ < 3) hos_log("vr: bottle: %s drawn into the VR hand (it was in state %d)%s", name, state, tries > 1 ? ": again" : "");
    }
    static int want_was;
    if (want && state == 1 && !given) {
        tries = 0;
        if (full) vr_bottle_use_update(c, player, bottle, !want_was || !drawn_by_us, now);
        else vr_bottle_simple_update(c, player, !want_was || !drawn_by_us);
    }
    if (!want || state != 1 || given) __atomic_store_n(&vr_bottle_to_game_now, 0, __ATOMIC_RELAXED);
    if (!want || given) __atomic_store_n(&vr_bottle_use_asked, 0, __ATOMIC_RELAXED);
    if (full && !want && want_was && bottle && state == 1 && vr_bottle_use.content >= 0 && !vr_bottle_use.corked) {
        vr_bottle_use.corked = 1;   /* (put away unused: corked again) */
        hos_log("vr: bottle: corked again (put away with its %s)", kVrBottleContentNames[vr_bottle_use.content + 1]);
    }
    if (!want || state != 1) __atomic_store_n(&vr_bottle_at_cork_now, -1, __ATOMIC_RELAXED);
    if (want && state == 1) want_was = 1;
    if (!want) want_was = 0;
    /* (the hook keeps handle 1 drawn only while this is set: cleared as soon as no VR hand wants it, before any stow) */
    __atomic_store_n(&vr_bottle_user_now, want && !given ? user : 0, __ATOMIC_RELAXED);
    if (!want && !given && drawn_by_us && bottle && state == 1 && now - asked_at > kVrBottleRetryNs && tries < kVrBottleStowTries) {
        asked_at = now;
        vr_bottle_put_away(c, user);
        tries++;
        hos_log("vr: bottle: %s put away%s", name, tries > 1 ? ": again" : "");
    }
    if (!want && !given && drawn_by_us && bottle && state == 1 && tries >= kVrBottleStowTries && now - asked_at > kVrBottleRetryNs) {
        hos_log("vr: bottle: %s still out after %d stows: given up (grip and let go to try again)", name, tries);
        drawn_by_us = 0;
    }
    if (drawn_by_us && (!bottle || state != 1) && !want) drawn_by_us = 0;
    __atomic_store_n(&vr_bottle_away_now, drawn_by_us && !want, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bottle_actor_now, want && state == 1 && !given ? bottle : 0, __ATOMIC_RELAXED);
}

/* (the bottle's own update) placed on the VR hand that holds it, where the renderer draws it. The simple bottle leaves the
   actor where the game has it (its model is moved to the VR hand by the renderer all the same) and only keeps it from the
   near test: placed by the physics call, the game's own swing afterwards never ended (headless 22 Nov: Link held in the
   swing's action 0x408, nothing caught; without our hold the same swing caught a fish). The full bottle's cork, mouth and
   tipping read the VR hand, not the actor, but it keeps its placement as it was. */
static void vr_bottle_place(Ctx *c, uint64_t actor) {
    if (!hos_vr_bottle_full()) {
        hos_vr_held_keep(c, actor);
        return;
    }
    const int hand = hos_vr_bottle_holder();
    float w[42];
    if (hand < 0 || !rx_vr_hands_world(w) || w[21 * hand] < .5f) return;
    const float *o = w + 21 * hand;
    float origin[3], rot[9];
    vr_bottle_pose(hand, o + 1, o + 9, o + 15, origin, rot);
    vr_bomb_place(c, actor, origin, rot);
}

#ifdef HOOKSHOT_VR_RESEARCH   /* (the research probe and its stand-in hand: research builds only) */
/* ---- the probe ---- */
static volatile uint64_t vr_bottle_probe_above;   /* (mode 4: this bottle placed 1.3 m above Link each update) */
static volatile int vr_bottle_probe_hand = -1;    /* (mode 8: the stand-in VR hand holding bottle A, -1 none) */
static volatile int vr_bottle_probe_pressed;      /* (mode 8: its trigger held) */
int hos_vr_bottle_probe_holder(void) { return __atomic_load_n(&vr_bottle_probe_hand, __ATOMIC_RELAXED); }
int hos_vr_bottle_probe_trigger(void) { return __atomic_load_n(&vr_bottle_probe_pressed, __ATOMIC_RELAXED); }

/* mode 8, the simple bottle (headless, first person on the stand-in headset): the selected bottle (RX_VR_BOTTLE_SELECT) held by the right hand from the
   start, filled by the content calls with each content in turn (life lowered first, a test's poke) and its trigger
   pressed through the real controls (bottle_controls.inc): drinks, the fairy and the Poe by calls, the bug, the empty
   bottle's catch and the fish as the game's own bottle use; each press logged with what followed */
static void vr_bottle_probe_simple(Ctx *c, uint64_t player, uint64_t user, float t) {
    static const struct { float at; int what; int content; } plan[] = {   /* (what: 1 fill, 2 press, 3 log) */
        {2, 1, kVrRedPotion}, {4, 2, 0}, {5.5f, 3, 0},
        {7, 1, kVrMaxMilk}, {9, 2, 0}, {10.5f, 3, 0}, {11, 1, -9}, {12, 2, 0}, {13.5f, 3, 0},
        {15, 1, kVrFairy}, {17, 2, 0}, {18.5f, 3, 0},
        {20, 1, kVrPoe}, {22, 2, 0}, {23.5f, 3, 0},
        {25, 1, kVrGreenPotion}, {27, 2, 0}, {28.5f, 3, 0},
        {30, 1, kVrBug}, {32, 2, 0}, {33, 3, 0}, {35, 3, 0}, {40, 3, 0},
        {45, 2, 0}, {46, 3, 0}, {48, 3, 0}, {51, 3, 0},
        {53, 1, kVrFish}, {55, 2, 0}, {56, 3, 0}, {58, 3, 0}, {62, 3, 0},
        {66, 4, 0}, {69, 3, 0}};
    static int done[sizeof plan / sizeof plan[0]];
    static float pressed_at = -100;
    __atomic_store_n(&vr_bottle_probe_hold, t < 66, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bottle_probe_hand, t < 66 ? 1 : -1, __ATOMIC_RELAXED);
    __atomic_store_n(&vr_bottle_probe_pressed, t - pressed_at < 0.3f, __ATOMIC_RELAXED);
    const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
    const uint64_t life_at = object >= HOS_HEAP_BASE ? recomp_load64(c, object + 7184) + 8 : 0;
    const int life = life_at > HOS_HEAP_BASE ? (int)recomp_load32(c, life_at) : -1;
    for (int k = 0; k < (int)(sizeof plan / sizeof plan[0]); k++) {
        if (done[k] || t < plan[k].at) continue;
        done[k] = 1;
        int state;
        char name[64];
        const uint64_t bottle = vr_bottle_find(c, user, &state, name);
        int hos_vr_event_playing(void);
        const uint32_t action = object >= HOS_HEAP_BASE ? recomp_load32(c, object + 0x1f60) : 0;
        const int b = vr_bottle_selected_index() >= 0 ? vr_bottle_selected_index() : 0;
        const int content = vr_bottle_content(c, b);
        if (plan[k].what == 1 && plan[k].content != -9) {
            vr_bottle_set_content(c, b, plan[k].content);
            if (life > 8) recomp_store32(c, life_at, (uint32_t)(life - 8));
        }
        if (plan[k].what == 2) pressed_at = t;
        hos_log("vr: bottle probe: t %.2f: %s; bottle %c %s, %s state %d, life %d, event %d, action %#x, to the game %d", t,
                plan[k].what == 1 ? plan[k].content == -9 ? "(half milk next)" : "filled, life lowered by 8" :
                plan[k].what == 2 ? "press" : plan[k].what == 4 ? "let go" : "after", 'A' + b,
                content >= -1 ? kVrBottleContentNames[content + 1] : "?", bottle ? name : "(none in handle 1)", state, life,
                hos_vr_event_playing(), action, hos_vr_bottle_to_game());
        if (plan[k].what == 1 && plan[k].content >= -1) hos_log("vr: bottle probe: filled with the %s", kVrBottleContentNames[plan[k].content + 1]);
    }
    (void)player;
}
static void vr_bottle_probe_slots(Ctx *c, uint64_t user, const char *why) {
    char line[600];
    size_t used = 0;
    for (int slot = 0; slot < 8; slot++) {
        const uint64_t handle = user + 0x20 + 0x18 * (uint64_t)slot;
        const uint32_t generation = recomp_load32(c, handle + 0x10);
        const uint64_t entry = recomp_load64(c, handle + 8);
        if (generation == UINT32_MAX || !oot_vr_equipment_ptr(entry, 0x50) || recomp_load32(c, entry + 0x48) != generation) continue;
        const uint64_t actor = recomp_load64(c, entry + 0x40);
        if (!oot_vr_equipment_ptr(actor, 0x340)) continue;
        char name[64];
        oot_vr_equipment_name(c, actor, 0x260, name);
        if (!name[0]) oot_vr_equipment_name(c, actor, 0x20, name);
        const uint64_t parts = recomp_load64(c, actor + 0x270);
        const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
        int state = -1, enabled = -1;
        if (oot_vr_equipment_ptr(equipment, 0xa0)) state = (int)recomp_load32(c, equipment + 0x9c), enabled = recomp_load8(c, equipment + 0x98) & 1;
        float p[3];
        for (int k = 0; k < 3; k++) p[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        used += (size_t)snprintf(line + used, sizeof line - used, " [%d %s state %d on %d drawn-bits %02x at %.2f %.2f %.2f actor %#llx]",
                                 slot, name[0] ? name : "?", state, enabled, recomp_load8(c, user + 0x13bc + slot), p[0], p[1], p[2],
                                 (unsigned long long)actor);
        if (used > sizeof line - 120) break;
    }
    hos_log("vr: bottle probe: %s: Link's equipment:%s", why, used ? line : " (none)");
}

static void vr_bottle_probe(Ctx *c, uint64_t player) {
    static int mode = -1;
    static int64_t first;
    static int step;
    if (mode < 0) mode = vr_research_int("RX_VR_BOTTLE_PROBE", 0);
    if (!mode) return;
    const uint64_t inventory = vr_bottle_inventory(c), user = vr_bottle_equipment_user(c, player);
    if (!inventory || !user) return;
    const int64_t now = hos_now_ns();
    if (!first) first = now;
    const float t = (float)((now - first) / 1e9);
    {   /* (every 0.25 s: the slots, when they change) */
        static int64_t looked;
        static uint64_t sig_was;
        if (now - looked > 250000000LL) {
            looked = now;
            uint64_t sig = 0;
            for (int slot = 0; slot < 8; slot++) {
                sig = sig * 31 + recomp_load64(c, user + 0x28 + 0x18 * (uint64_t)slot) + recomp_load8(c, user + 0x13bc + slot);
                int state;
                char name[64];
                if (slot == kVrBottleSlot && vr_bottle_find(c, user, &state, name)) sig = sig * 31 + (uint64_t)(state + 7);
            }
            const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
            const uint32_t action = object >= HOS_HEAP_BASE ? recomp_load32(c, object + 0x1f60) : 0;
            static uint32_t action_was = 1;
            if (sig != sig_was || action != action_was) {
                char why[64];
                snprintf(why, sizeof why, "t %.2f action %#x", t, action);
                vr_bottle_probe_slots(c, user, why);
            }
            sig_was = sig, action_was = action;
        }
    }
    if (step == 0 && t > 2) {   /* the contents, as the drink node reads them */
        step = 1;
        for (int b = 0; b < 5; b++) {
            const int content = (int)(int32_t)vr_bottle_call(c, 0x80424b80ULL, inventory, (uint64_t)kVrBottleItems[b], 0, 0, 0, 0, 0);
            const int drinkable = (int)(vr_bottle_call(c, 0x80a97000ULL, inventory, (uint64_t)kVrBottleItems[b], 0, 0, 0, 0, 0) & 1);
            hos_log("vr: bottle probe: bottle %c (item %d): content index %d, f_80a97000 %d", 'A' + b, kVrBottleItems[b], content, drinkable);
        }
        const uint64_t holder = recomp_load64(c, 0x84e9cfa8ULL);
        hos_log("vr: bottle probe: selection byte %d, selected item %d", holder >= HOS_HEAP_BASE ? recomp_load8(c, holder + 0x5c0) : -1,
                __atomic_load_n(&oot_vr_selected_index, __ATOMIC_RELAXED));
    }
    if (step == 1 && t > 3) {   /* select bottle A (RX_VR_BOTTLE_SELECT: another item number) */
        step = 2;
        const int item = vr_research_int("RX_VR_BOTTLE_SELECT", 14);
        int child = 1;
        if (!oot_gamedata_bool(c, 0xcea220d3u, &child)) child = 1;
        vr_bottle_call(c, 0x81231c00ULL, inventory, child ? 1 : 0, (uint64_t)item, 0, 1, 0, 0);
        hos_log("vr: bottle probe: selected item %d through pouch_assignableitem_change (%s)", item, child ? "child" : "adult");
    }
    /* mode 3: the selected bottle's own actor (slot 1) drawn and stowed with the game's calls (f_806d6820 draw, f_80e54c70
       stow: what OneShotEquipmentUserRequestChangeState calls), then its content changed (f_833c45a0) */
    static const struct { float at; int what; } plan[] = {{6, 1}, {11, 2}, {14, 3}, {17, 1}, {22, 4}, {25, 2}};
    if (mode == 4 && t > 6 && t < 14) {   /* mode 4: drawn, and placed above Link's head by its own update (vr_bottle_place) */
        int state;
        char name[64];
        const uint64_t bottle = vr_bottle_find(c, user, &state, name);
        static int drew;
        if (bottle && !drew) drew = 1, vr_bottle_call(c, 0x806d6820ULL, user, kVrBottleSlot, 0, 0, 0, 0, 0),
                             hos_log("vr: bottle probe: t %.2f: %s drawn (state was %d)", t, name, state);
        __atomic_store_n(&vr_bottle_probe_above, bottle, __ATOMIC_RELAXED);
    } else if (mode == 4 && t >= 14) {
        static int stowed;
        if (!stowed) stowed = 1, __atomic_store_n(&vr_bottle_probe_above, 0, __ATOMIC_RELAXED), vr_bottle_put_away(c, user),
                     hos_log("vr: bottle probe: t %.2f: stowed", t);
    }
    if (mode == 6) __atomic_store_n(&vr_bottle_probe_hold, t > 6 && t < 14, __ATOMIC_RELAXED);   /* (mode 6: held 6-14 s) */
    if (mode == 9) __atomic_store_n(&vr_bottle_probe_hold, t > 6, __ATOMIC_RELAXED);   /* (mode 9: held from 6 s, as by a VR hand) */
    if (mode == 8) {   /* (mode 8: from 3 s after the selection, once no event plays) */
        static float start = -1;
        int hos_vr_event_playing(void);
        if (start < 0 && step == 2 && t > 6 && !hos_vr_event_playing()) start = t, hos_log("vr: bottle probe: the simple bottle's run starts");
        if (start >= 0) vr_bottle_probe_simple(c, player, user, t - start);
    }
    if (mode == 10) {   /* (mode 10: the stand-in right hand holds the selected bottle; its trigger pressed at 4, 14, 24, 34 s) */
        static float start = -1, pressed_at = -100;
        static int done[4];
        int hos_vr_event_playing(void);
        if (start < 0 && step == 2 && t > 6 && !hos_vr_event_playing()) start = t, hos_log("vr: bottle probe: mode 10 starts");
        const float s = start >= 0 ? t - start : -1;
        __atomic_store_n(&vr_bottle_probe_hold, s >= 0 && s < 44, __ATOMIC_RELAXED);
        __atomic_store_n(&vr_bottle_probe_hand, s >= 0 && s < 44 ? 1 : -1, __ATOMIC_RELAXED);
        for (int k = 0; k < 4; k++)
            if (!done[k] && s >= 4 + 10 * k) {
                done[k] = 1, pressed_at = t;
                const int content = vr_bottle_content(c, vr_bottle_selected_index() >= 0 ? vr_bottle_selected_index() : 0);
                hos_log("vr: bottle probe: t %.2f: press %d (the selected bottle holds %s)", t, k + 1,
                        content >= -1 ? kVrBottleContentNames[content + 1] : "?");
            }
        __atomic_store_n(&vr_bottle_probe_pressed, t - pressed_at < 0.3f, __ATOMIC_RELAXED);
    }
    if (mode == 7) {   /* mode 7: held 6-24 s, placed above Link from 7 s; the cork off at 10, on at 14; the heal at 16 */
        __atomic_store_n(&vr_bottle_probe_hold, t > 6 && t < 24, __ATOMIC_RELAXED);
        int state;
        char name[64];
        const uint64_t bottle = vr_bottle_find(c, user, &state, name);
        __atomic_store_n(&vr_bottle_probe_above, t > 7 && t < 24 && state == 1 ? bottle : 0, __ATOMIC_RELAXED);
        static int done[6];
        const uint64_t object = __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED);
        const uint64_t life_at = object >= HOS_HEAP_BASE ? recomp_load64(c, object + 7184) + 8 : 0;
        if (!done[0] && t > 8) {
            done[0] = 1;
            const uint64_t heal = vr_bottle_call(c, 0x826dead0ULL, player, 0, 0, 0, 0, 0, 0);
            char line[300];
            size_t used = 0;
            for (int k = 0; heal >= HOS_HEAP_BASE && k < 9; k++)
                used += (size_t)snprintf(line + used, sizeof line - used, " +%#x %d", 0x38 + 4 * k, (int32_t)recomp_load32(c, heal + 0x38 + 4 * k));
            hos_log("vr: bottle probe: BottleHeal %#llx:%s flags %016llx; life %d", (unsigned long long)heal, used ? line : " ?",
                    heal >= HOS_HEAP_BASE ? (unsigned long long)recomp_load64(c, heal + 0x58) : 0ULL,
                    life_at > HOS_HEAP_BASE ? (int)recomp_load32(c, life_at) : -1);
        }
        if (bottle && state == 1 && ((!done[1] && t > 10) || (!done[2] && t > 14))) {
            const int off = !done[1];
            done[off ? 1 : 2] = 1;
            hos_log("vr: bottle probe: t %.2f: %s on %s: %s", t, off ? "CapOff" : "CapOn", name,
                    vr_bottle_play(c, bottle, off ? "CapOff" : "CapOn") ? "played" : "no animation controller");
        }
        if (!done[3] && t > 16 && life_at > HOS_HEAP_BASE) {
            done[3] = 1;
            const uint32_t before = recomp_load32(c, life_at);
            recomp_store32(c, life_at, before > 8 ? before - 8 : before);
            const int healed = vr_bottle_heal(c, player, 4);
            hos_log("vr: bottle probe: life %u, lowered to %u, healed by 4 (%s): %u", before, recomp_load32(c, life_at) + 0,
                    healed ? "asked" : "no life component", recomp_load32(c, life_at));
        }
        if (!done[4] && t > 17 && life_at > HOS_HEAP_BASE) done[4] = 1, hos_log("vr: bottle probe: life a second later %u", recomp_load32(c, life_at));
    }
    if (mode == 5 && t > 6 && t < 14) {   /* mode 5: the draw asked every update; what the bottle's state does */
        int state;
        char name[64];
        const uint64_t bottle = vr_bottle_find(c, user, &state, name);
        const uint64_t parts = bottle ? recomp_load64(c, bottle + 0x270) : 0;
        const uint64_t equipment = oot_vr_equipment_ptr(parts, 0x1e0) ? recomp_load64(c, parts + 0x1d8) : 0;
        const int before = recomp_load8(c, user + 0x13bc + kVrBottleSlot);
        const int busy = (int)vr_bottle_call(c, 0x80d07ab0ULL, user, kVrBottleSlot, 0, 0, 0, 0, 0);
        vr_bottle_call(c, 0x806d6820ULL, user, kVrBottleSlot, 0, 0, 0, 0, 0);
        const int after = recomp_load8(c, user + 0x13bc + kVrBottleSlot);
        const int on = oot_vr_equipment_ptr(equipment, 0xa0) ? recomp_load8(c, equipment + 0x98) & 1 : -1;
        const uint32_t action = recomp_load32(c, __atomic_load_n(&oot_vr_player_object, __ATOMIC_RELAXED) + 0x1f60);
        static int sig_was = -1;
        const int sig = state * 1000 + before * 100 + after * 10 + on + busy * 7919 + (int)(action & 0xffff) * 13;
        static unsigned told;
        if (sig != sig_was && told++ < 40)
            hos_log("vr: bottle probe: t %.2f: %s state %d on %d, bits %02x -> %02x after the draw, f_80d07ab0 %d, action %#x", t,
                    name, state, on, before, after, busy, action);
        sig_was = sig;
    }
    for (int k = 0; mode == 3 && k < (int)(sizeof plan / sizeof plan[0]); k++) {
        static int done[8];
        if (done[k] || t < plan[k].at) continue;
        done[k] = 1;
        uint64_t r = 0;
        if (plan[k].what == 1) r = vr_bottle_call(c, 0x806d6820ULL, user, 1, 0, 0, 0, 0, 0);
        if (plan[k].what == 2) r = vr_bottle_call(c, 0x80e54c70ULL, user, 1, 0, 0, 0, 0, 0);
        if (plan[k].what == 3) r = vr_bottle_call(c, 0x833c45a0ULL, inventory, 14, 0, 0, 1, (uint64_t)-1, 0);   /* (A: RedPotion) */
        if (plan[k].what == 4) r = vr_bottle_call(c, 0x833c7280ULL, inventory, 14, 1, 0, 0, 0, 0);   /* (A: emptied) */
        const int content = (int)(int32_t)vr_bottle_call(c, 0x80424b80ULL, inventory, 14, 0, 0, 0, 0, 0);
        hos_log("vr: bottle probe: t %.2f: %s returned %#llx; bottle A now holds content %d", t,
                plan[k].what == 1 ? "draw slot 1 (f_806d6820)" : plan[k].what == 2 ? "stow slot 1 (f_80e54c70)" :
                plan[k].what == 3 ? "set A to RedPotion (f_833c45a0)" : "empty A (f_833c7280)", (unsigned long long)r, content);
    }
}
#else
static void vr_bottle_probe(Ctx *c, uint64_t player) { (void)c, (void)player; }
#endif

/* (host/hos_main.c hos_oot_lift_player, VR HOOK: every actor's movement update) Link's: the bottle drawn or stowed;
   the held bottle's: placed on the VR hand */
void hos_vr_bottle_actor_update(Ctx *c, uint64_t actor) {
    if (!actor) return;
    if (actor == __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED)) {
        vr_bottle_link_update(c, actor);
        vr_bottle_probe(c, actor);
        return;
    }
    if (actor == __atomic_load_n(&vr_bottle_actor_now, __ATOMIC_RELAXED)) vr_bottle_place(c, actor);
#ifdef HOOKSHOT_VR_RESEARCH
    if (actor == __atomic_load_n(&vr_bottle_probe_above, __ATOMIC_RELAXED)) {   /* (the probe's mode 4) */
        const uint64_t link = __atomic_load_n(&oot_vr_player_actor, __ATOMIC_RELAXED);
        float origin[3], rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, was[3];
        for (int k = 0; k < 3; k++) origin[k] = vr_bomb_f(c, link + 796 + 4 * k) + (k == 1 ? 1.3f : 0), was[k] = vr_bomb_f(c, actor + 796 + 4 * k);
        static int64_t logged;
        if (hos_now_ns() - logged > 1000000000LL)
            logged = hos_now_ns(), hos_log("vr: bottle probe: the bottle found at %.2f %.2f %.2f, placed at %.2f %.2f %.2f", was[0], was[1],
                                           was[2], origin[0], origin[1], origin[2]);
        vr_bomb_place(c, actor, origin, rot);
    }
#endif
}
